export module evaluation;
import std;
import native;
import host;
import coroutine;
import json;
import codec;
export import provider;
import records;
export import objects;
import enumeration;
import runner;
import constants;
import toolchain;

export namespace language {
    using record_inputs = std::map<std::string, std::vector<std::shared_ptr<const semantic::shard_object>>>;
    struct document_loader {
        std::filesystem::path root;
        std::optional<native::scheduler> scheduler;
        execution::thread_pool* pool = nullptr;
        std::map<std::string, std::string> documents;
        std::uint64_t document_bytes = 4u << 20, bytes = 64u << 20;
        explicit operator bool() const { return pool || !documents.empty(); }
        auto read(std::set<std::string> paths) const -> coroutine::task<std::map<std::string, std::string>> {
            if (!pool) {
                std::map<std::string, std::string> result;
                for (const auto& path : paths)
                    result.emplace(path, documents.at(path));
                co_return result;
            }
            co_return co_await io::blocking(*scheduler, *pool, [root = root, paths = std::move(paths), maximum = document_bytes, budget = bytes] {
                std::uint64_t bytes = 0;
                std::map<std::string, std::string> result;
                for (const auto& path : paths) {
                    const auto size = std::filesystem::file_size(root / path);
                    if (size > maximum || size > budget - bytes)
                        throw std::length_error("reader source text budget exhausted");
                    bytes += size;
                    std::ifstream input{root / path, std::ios::binary};
                    result.emplace(path, std::string{std::istreambuf_iterator<char>{input}, {}});
                }
                return result;
            });
        }
    };
    struct object_loader {
        std::filesystem::path root;
        std::set<std::string> allowed;
        explicit operator bool() const { return !root.empty(); }
        auto path(const std::string& id) const -> std::filesystem::path {
            if (!allowed.contains(id))
                throw std::invalid_argument("reader object is not a declared artifact input");
            return std::filesystem::absolute(root / id);
        }
    };

    struct installation {
        std::string snapshot;
        std::vector<document> documents;
        // Gradle/MSBuild and future project models use model or projects; providers do not add installation fields.
        std::map<std::string, std::string> source_files, model, generated;
        record_inputs source_facts;
        semantic::input_vector projects;

        auto manifest() const -> std::map<std::pair<std::string, std::string>, std::string> {
            std::map<std::pair<std::string, std::string>, std::string> result;
            for (const auto& [name, value] : source_files)
                result[{"source", name}] = value;
            for (const auto& [name, value] : model)
                result[{"model", name}] = value;
            for (const auto& [name, value] : generated)
                result[{"generated", name}] = value;
            for (const auto& document : documents)
                result[{"document", document.path}] = document.language + "\n" + document.text;
            for (const auto& [port, objects] : source_facts) {
                std::vector<semantic::shard_id> ids;
                for (const auto& object : objects)
                    ids.push_back(object->data->id);
                result[{"records", port}] = json::write(ids);
            }
            for (const auto& [project, inputs] : projects)
                result[{"project", project}] = json::write(inputs);
            return result;
        }
    };

    // A slot owns one adapter. The common lifecycle controls epochs and cancellation;
    // each provider owns its unit enumeration, analysis and dependency observations.
    class adapter {
    public:
        adapter(native::scheduler io, io::command command, session_state identity, std::map<std::string, std::vector<std::string>> commands = {},
                toolchain::identity runtime = {}, document_loader loader = {})
            : io_(io), command_(command), commands_(std::move(commands)), runtime_(std::move(runtime)), root_(command.cwd), state_(std::move(identity)),
              loader_(std::move(loader)) {
            state_.id = "session-" + nodes::hex(nodes::random_bytes(16));
            state_.snapshot.clear();
            state_.inputs.clear();
            state_.epoch = 0;
            state_.phase = std::string{"created"};
        }

        virtual ~adapter() = default;
        void objects(object_loader loader) { objects_ = std::move(loader); }
        // The directory this session may write; the tree it reads belongs to the build.
        void cache(std::filesystem::path directory) { cache_ = std::move(directory); }
        auto open(json::value initialization) -> coroutine::task<session_state> {
            auto held = co_await lock_.lock();
            if (state_.phase != "created")
                throw std::logic_error("provider session has already been opened");
            operation active{state_, cancelled_, "opening"};
            co_await do_open(std::move(initialization));
            active.complete();
            co_return state_;
        }

        auto update(std::uint64_t epoch, installation input) -> coroutine::task<session_state> {
            auto held = co_await lock_.lock();
            require(epoch, false);
            if (input.snapshot.empty())
                throw std::invalid_argument("provider inputs require a snapshot");
            operation active{state_, cancelled_, "updating"};
            auto next = input.manifest();
            std::vector<std::string> changed;
            for (const auto& [key, value] : next)
                if (!manifest_.contains(key) || manifest_.at(key) != value)
                    changed.push_back(key.second);
            for (const auto& [key, value] : manifest_)
                if (!next.contains(key))
                    changed.push_back(key.second);
            std::ranges::sort(changed);
            changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
            if (state_.epoch == 0 || state_.snapshot != input.snapshot || !changed.empty())
                ++state_.epoch;
            state_.snapshot = input.snapshot;
            state_.inputs = {{semantic::input_kind::source, state_.project, input.snapshot}};
            if (!runtime_.execution.empty())
                state_.inputs.push_back({semantic::input_kind::runtime, "toolchain", runtime_.execution});
            for (const auto& value : runtime_.inputs)
                state_.inputs.push_back({semantic::input_kind::sdk, value.name, value.object});
            for (const auto& [name, value] : input.model)
                state_.inputs.push_back({semantic::input_kind::artifact, name, value});
            for (const auto& [name, value] : input.generated)
                state_.inputs.push_back({semantic::input_kind::generated, name, value});
            projects_ = input.projects;
            if (const auto found = projects_.find(state_.project); found != projects_.end())
                for (const auto& value : found->second)
                    if (!std::ranges::contains(state_.inputs, value))
                        state_.inputs.push_back(value);
            co_await do_update(std::move(input), changed);
            active.complete();
            manifest_ = std::move(next);
            co_return state_;
        }

        auto units(std::uint64_t epoch, const semantic::extent& demand, const semantic::request& query = {})
            -> coroutine::task<std::vector<semantic::unit_key>> {
            auto held = co_await lock_.lock();
            require(epoch);
            operation active{state_, cancelled_, "evaluating"};
            co_await prepare(demand);
            auto result = co_await do_units(demand);
            if (resolves() && (query.action == "definitions" || query.action == "references"))
                result.push_back(
                    {state_.project, state_.stage, state_.provider, "resolve", semantic::unit_kind::group, semantic::resolution_identifier(query)});
            std::set<semantic::unit_id> identifiers;
            for (const auto& unit : result)
                if (!identifiers.insert(semantic::unit_identifier(unit)).second)
                    throw std::invalid_argument("provider returned duplicate unit IDs");
            active.complete();
            co_return result;
        }
        virtual auto deferred(const semantic::unit_key& key, const semantic::extent& demand, std::string reason = "semantic unit budget exhausted") const
            -> semantic::missing {
            auto scope = demand;
            if (key.kind == semantic::unit_kind::file)
                scope.files = {key.name};
            return {.scope = std::move(scope), .units = {semantic::unit_identifier(key)}, .reason = std::move(reason), .deferred = true};
        }
        auto current_inputs(const std::vector<semantic::candidate>& candidates) -> coroutine::task<semantic::input_vector> {
            auto held = co_await lock_.lock();
            auto inputs = state_.inputs;
            for (auto value : co_await observed_inputs(candidates))
                if (!std::ranges::contains(inputs, value))
                    inputs.push_back(std::move(value));
            for (const auto& [name, object] : runtime_.executables)
                inputs.push_back({semantic::input_kind::tool, name, object});
            for (const auto& candidate : candidates)
                for (const auto& unit : candidate.units)
                    for (const auto& input : unit.direct_inputs)
                        if (auto value = input_value(input)) {
                            semantic::input_object current{input.kind, input.name, *value, input.type};
                            if (!std::ranges::contains(inputs, current))
                                inputs.push_back(std::move(current));
                        }
            auto projects = projects_;
            projects[state_.project] = std::move(inputs);
            co_return projects;
        }
        auto evaluate(evaluation_request request, const std::vector<semantic::candidate>& candidates = {}) -> coroutine::task<evaluated_unit> {
            co_return co_await compute(std::move(request), candidates, {});
        }
        auto resolve(evaluation_request request, semantic::request query, const std::vector<semantic::candidate>& candidates = {})
            -> coroutine::task<evaluated_unit> {
            co_return co_await compute(std::move(request), candidates, std::move(query));
        }

    private:
        auto compute(evaluation_request request, const std::vector<semantic::candidate>& candidates, std::optional<semantic::request> query)
            -> coroutine::task<evaluated_unit> {
            auto held = co_await lock_.lock();
            require(request.epoch);
            if (request.slot_id != state_.slot)
                throw std::invalid_argument("provider request belongs to another slot");
            if (request.execution.request_id.empty() || request.execution.computation_id.empty() || request.execution.stage_id.empty())
                throw std::invalid_argument("provider evaluation requires request, computation and stage identities");
            if (request.inputs != state_.inputs)
                throw std::invalid_argument("provider request belongs to other installed inputs");
            if (request.unit.project != state_.project || request.unit.stage != state_.stage || request.unit.provider != state_.provider ||
                request.unit.operation != (query ? "resolve" : "evaluate"))
                throw std::invalid_argument("provider request belongs to another logical unit");
            operation active{state_, cancelled_, "evaluating"};
            evaluated_unit result{.request = std::move(request)};
            co_await prepare(result.request.demand);
            for (const auto& candidate : candidates)
                if ((!result.request.retained_only || candidate.retained) && candidate.recipe == semantic::recipe{result.request.unit, state_.options}) {
                    result.reused = candidate.id;
                    active.complete();
                    result.session = state_;
                    co_return result;
                }
            if (result.request.retained_only) {
                result.deferred = true;
                active.complete();
                result.session = state_;
                co_return result;
            }
            evaluated_unit produced;
            try {
                produced = query ? co_await do_resolve(result.request, *query) : co_await do_evaluate(result.request);
            } catch (const resolution_error&) {
                active.complete();
                throw;
            }
            result.shards = std::move(produced.shards);
            result.deferred = produced.deferred;
            active.complete();
            result.session = state_;
            co_return result;
        }

    public:
        auto close() -> coroutine::task<> {
            auto held = co_await lock_.lock();
            if (state_.phase == "closed")
                co_return;
            state_.phase = std::string{"closing"};
            if (!closed_) {
                closed_ = true;
                co_await do_close();
            }
            manifest_.clear();
            projects_.clear();
            state_.phase = std::string{"closed"};
        }

        auto state() const -> const session_state& { return state_; }

        void request_cancel() noexcept { cancelled_.store(true); }

        auto cancel() -> coroutine::task<> {
            request_cancel();
            auto held = co_await lock_.lock();
            if (state_.phase == "closed")
                co_return;
            state_.phase = std::string{"failed"};
            if (!closed_) {
                closed_ = true;
                co_await do_close();
            }
        }

    protected:
        virtual auto do_open(json::value initialization) -> coroutine::task<> = 0;
        virtual auto do_update(installation input, const std::vector<std::string>& changed) -> coroutine::task<> = 0;
        virtual auto prepare(const semantic::extent&) -> coroutine::task<> { co_return; }
        virtual auto do_units(const semantic::extent& demand) -> coroutine::task<std::vector<semantic::unit_key>> = 0;
        virtual auto do_evaluate(const evaluation_request& request) -> coroutine::task<evaluated_unit> = 0;
        virtual auto do_resolve(const evaluation_request&, const semantic::request&) -> coroutine::task<evaluated_unit> {
            throw resolution_error("this provider supplies query records during evaluation");
            co_return evaluated_unit{};
        }
        virtual auto resolves() const -> bool { return false; }
        virtual auto do_close() -> coroutine::task<> = 0;
        virtual auto input_value(const semantic::input_object&) const -> std::optional<std::string> { return std::nullopt; }
        virtual auto observed_inputs(const std::vector<semantic::candidate>&) -> coroutine::task<std::vector<semantic::input_object>> {
            co_return std::vector<semantic::input_object>{};
        }
        struct operation {
            session_state& state;
            const std::atomic<bool>& cancelled;
            bool completed = false;
            operation(session_state& state, const std::atomic<bool>& cancelled, std::string phase) : state(state), cancelled(cancelled) {
                state.phase = std::move(phase);
            }
            void complete() {
                if (cancelled.load() || state.phase == "failed" || state.phase == "closing" || state.phase == "closed")
                    throw std::runtime_error("provider operation was cancelled");
                state.phase = std::string{"ready"};
                completed = true;
            }
            ~operation() {
                if (!completed)
                    state.phase = std::string{"failed"};
            }
        };
        void require(std::uint64_t epoch, bool installed = true) const {
            if (cancelled_.load() || state_.phase != "ready" || (installed && state_.epoch == 0))
                throw std::logic_error("provider session has no ready inputs");
            if (state_.epoch != epoch)
                throw std::invalid_argument("provider request belongs to another input epoch");
        }
        native::scheduler io_;
        io::command command_;
        std::map<std::string, std::vector<std::string>> commands_;
        toolchain::identity runtime_;
        std::filesystem::path root_, cache_;
        session_state state_;
        coroutine::mutex lock_;
        bool closed_ = false;
        std::atomic<bool> cancelled_{false};
        document_loader loader_;
        object_loader objects_;
        std::map<std::pair<std::string, std::string>, std::string> manifest_;
        semantic::input_vector projects_;
    };

    // Readers share the slot allocation and object inputs without owning a language process.
    class reader_adapter : public adapter {
    public:
        static constexpr adapter_kind transport = adapter_kind::reader;
        using adapter::adapter;

    protected:
        auto do_open(json::value initialization) -> coroutine::task<> override {
            initialization_ = std::move(initialization);
            co_return;
        }
        auto do_update(installation input, const std::vector<std::string>&) -> coroutine::task<> override {
            installed_ = std::move(input);
            co_return;
        }
        auto do_close() -> coroutine::task<> override {
            installed_ = {};
            co_return;
        }
        auto run(std::vector<std::string> argv) -> coroutine::task<std::string> {
            auto command = command_;
            command.argv = std::move(argv);
            command.interactive = true;
            auto result = co_await runner::run_step(io_, std::move(command), std::nullopt, constants::semantic_object_bytes, state_.provider);
            if (result.truncated || result.timed_out)
                throw std::length_error("artifact reader exhausted its output or time budget");
            if (result.exit_code)
                throw std::runtime_error("artifact reader failed: " + result.output);
            co_return std::move(result.output);
        }
        installation installed_;
        json::value initialization_;
    };
} // namespace language
