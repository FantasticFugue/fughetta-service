export module helper;
import std;
import native;
import host;
import coroutine;
import reflection;
import records;
import constants;
export import evaluation;
import toolchain;

export namespace language::helper {
    // The helper session wire contract. Managed platform hosts implement exactly these shapes;
    // `language schema` publishes them so hosts can be checked without reading this module.
    struct host_inputs {
        std::string objects;
        std::map<std::string, std::string> sdks;
    };
    struct opening {
        session_state session;
        rfl::Generic initialization;
        host_inputs host;
    };
    struct object_reference {
        semantic::shard_id id;
        std::string path;
    };
    struct installation {
        session_state session;
        std::vector<document> documents;
        std::vector<std::string> changed;
        std::map<std::string, std::string> sources, artifacts, generated;
        std::map<std::string, std::vector<object_reference>> facts;
    };
    struct selection {
        std::uint64_t epoch;
        semantic::extent demand;
    };
    struct evaluation {
        evaluation_request request;
        semantic::unit unit;
    };
    // producer_keys maps each observed read key to the units that produce it.
    struct evaluation_result {
        rfl::Flatten<evaluated_unit> result;
        std::optional<std::map<std::string, std::vector<semantic::unit_key>>> producer_keys;
    };
    struct observation {
        session_state session;
        std::vector<semantic::dependency> reads;
    };
    template <class Params> struct message {
        std::uint64_t request_id;
        std::string operation;
        Params params;
    };
    template <class Result> struct response {
        std::uint64_t request_id;
        std::optional<Result> result;
        std::optional<std::string> error;
    };
    template <class T> auto schema() -> rfl::Generic { return rfl::json::read<rfl::Generic>(rfl::json::to_schema<T>()).value(); }
    template <class Params, class Result> auto operation() -> rfl::Generic {
        rfl::Generic::Object out;
        out["params"] = schema<Params>();
        out["result"] = schema<Result>();
        return out;
    }
    inline auto schemas() -> rfl::Generic {
        rfl::Generic::Object operations;
        operations["open"] = operation<opening, rfl::Generic>();
        operations["update"] = operation<installation, rfl::Generic>();
        operations["units"] = operation<selection, std::vector<semantic::unit_key>>();
        operations["evaluate"] = operation<evaluation, evaluation_result>();
        operations["observe"] = operation<observation, std::vector<semantic::input_object>>();
        rfl::Generic::Object out;
        out["message"] = schema<message<rfl::Generic>>();
        out["response"] = schema<response<rfl::Generic>>();
        out["operations"] = std::move(operations);
        return out;
    }
} // namespace language::helper

export namespace language {
    // Each request and response is one bounded JSON line. The slot serializes calls;
    // cancellation terminates the helper and the failed epoch cannot be reused.
    class helper_adapter : public adapter {
    public:
        static constexpr adapter_kind transport = adapter_kind::helper;
        using adapter::adapter;
        void dependency_reads(bool enabled) { dependency_reads_ = enabled; }

    protected:
        auto do_open(rfl::Generic initialization) -> coroutine::task<> override {
            helper::host_inputs host{.objects = command_.env.value().at("FUGHETTA_OBJECTS")};
            for (const auto& sdk : runtime_.inputs) {
                if (sdk.roots.empty())
                    throw std::invalid_argument("helper SDK has no installed root: " + sdk.name);
                host.sdks.emplace(sdk.name, sdk.roots.front());
            }
            auto command = command_;
            command.interactive = true;
            child_.emplace(io::spawn_process(io_, command));
            (void)co_await call<rfl::Generic>("open", helper::opening{state_, std::move(initialization), std::move(host)});
        }
        auto do_update(language::installation input, const std::vector<std::string>& changed) -> coroutine::task<> override {
            installed_ = std::move(input);
            const auto& documents = installed_.documents;
            std::map<std::string, std::vector<helper::object_reference>> inputs;
            for (const auto& [port, objects] : installed_.source_facts)
                for (const auto& object : objects)
                    inputs[port].push_back({object->data->id, object->path.string()});
            (void)co_await call<rfl::Generic>(
                "update", helper::installation{state_, documents, changed, installed_.source_files, installed_.model, installed_.generated, inputs});
        }
        auto do_units(const semantic::extent& demand) -> coroutine::task<std::vector<semantic::unit_key>> override {
            co_return co_await call<std::vector<semantic::unit_key>>("units", helper::selection{state_.epoch, demand});
        }
        auto do_evaluate(const evaluation_request& request) -> coroutine::task<evaluated_unit> override {
            auto response = co_await call<helper::evaluation_result>("evaluate", helper::evaluation{request, template_unit(request)});
            auto result = std::move(response.result.get());
            if (!dependency_reads_ || !response.producer_keys)
                co_return result;
            const auto& producers = *response.producer_keys;
            for (auto& shard : result.shards)
                for (auto& read : shard.unit.observed_dependencies)
                    if (const auto keys = producers.find(read.key); keys != producers.end()) {
                        for (const auto& key : keys->second)
                            read.producers.push_back(semantic::unit_identifier(key));
                        std::ranges::sort(read.producers);
                        read.producers.erase(std::unique(read.producers.begin(), read.producers.end()), read.producers.end());
                    }
            co_return result;
        }
        auto observed_inputs(const std::vector<semantic::candidate>& candidates) -> coroutine::task<std::vector<semantic::input_object>> override {
            if (!dependency_reads_)
                co_return std::vector<semantic::input_object>{};
            std::map<std::string, semantic::dependency> selected;
            for (const auto& candidate : candidates)
                for (const auto& unit : candidate.units)
                    for (const auto& read : unit.observed_dependencies)
                        if (!read.producer && read.channel &&
                            (read.kind == semantic::dependency_kind::declaration || read.kind == semantic::dependency_kind::lookup ||
                             read.kind == semantic::dependency_kind::constant || read.kind == semantic::dependency_kind::body))
                            selected.try_emplace(read.key, read);
            if (selected.empty())
                co_return std::vector<semantic::input_object>{};
            helper::observation request{state_};
            for (const auto& [_, read] : selected)
                request.reads.push_back(read);
            auto inputs = co_await call<std::vector<semantic::input_object>>("observe", request);
            for (const auto& input : inputs)
                if (input.kind != semantic::input_kind::channel)
                    throw std::runtime_error("helper observations must name semantic channels");
            co_return inputs;
        }
        auto do_close() -> coroutine::task<> override {
            if (child_) {
                child_->kill();
                (void)co_await io::async_wait(*child_);
                child_.reset();
            }
        }

    private:
        auto template_unit(const evaluation_request& request) const -> semantic::unit {
            semantic::recipe recipe{request.unit, state_.options};
            auto out = semantic::make_unit(recipe, semantic::context_identifier(recipe));
            out.direct_inputs = request.inputs;
            out.coverage.requested = request.demand;
            return out;
        }
        template <class T, class Params> auto call(std::string operation, Params params) -> coroutine::task<T> {
            const auto id = ++next_;
            auto text = rfl::json::write(helper::message<Params>{id, std::move(operation), std::move(params)}) + "\n";
            if (text.size() > constants::semantic_object_bytes)
                throw std::length_error("helper request exceeds the semantic object limit");
            co_await io::async_write(*child_, std::as_bytes(std::span{text}));
            for (;;) {
                if (const auto end = buffer_.find('\n'); end != std::string::npos) {
                    auto reply = rfl::json::read<helper::response<T>>(std::string_view{buffer_}.substr(0, end)).value();
                    buffer_.erase(0, end + 1);
                    if (reply.request_id != id)
                        throw std::runtime_error("helper returned another request's result");
                    if (reply.error)
                        throw std::runtime_error(*reply.error);
                    if (!reply.result)
                        throw std::runtime_error("helper response has no result");
                    co_return std::move(*reply.result);
                }
                std::array<std::byte, 16384> chunk;
                const auto count = co_await io::async_read_some(*child_, net::buffer(chunk));
                if (!count)
                    throw std::runtime_error("helper closed its output");
                if (buffer_.size() + count > constants::semantic_object_bytes)
                    throw std::length_error("helper response exceeds the semantic object limit");
                buffer_.append(reinterpret_cast<const char*>(chunk.data()), count);
            }
        }
        language::installation installed_;
        std::optional<native::process> child_;
        std::string buffer_;
        std::uint64_t next_ = 0;
        bool dependency_reads_ = false;
    };
    template <> struct server<"external"> {
        using adapter = helper_adapter;
        struct options {
            description registration{
                .operations = {"evaluate"}, .negotiated = false, .adapter = adapter_kind::helper, .unit_kind = semantic::unit_kind::module};
            std::map<std::string, std::string> documents;
            std::vector<std::string> argv;
            std::vector<std::string> tools;
            builds::environment env;
            rfl::Generic initialization;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 0> documents{};
        static constexpr std::array<std::string_view, 0> inputs{}, facts{};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::module;
        static constexpr bool negotiated = false;
        static auto channels() -> std::vector<semantic::channel_definition> { return {}; }
        static auto requirements(const options& o) -> builds::requirements {
            auto needs = builds::tools(o.tools);
            if (!o.argv.empty())
                builds::combine(needs, builds::tools({o.argv.front()}));
            builds::combine(needs, o.registration.needs);
            return needs;
        }
        static void validate(const options& o) {
            if (o.registration.id.empty() || o.registration.id.contains('\0'))
                throw std::invalid_argument("external provider requires a registration ID");
            if (o.registration.adapter != adapter_kind::helper)
                throw std::invalid_argument("external providers use the helper adapter");
            if (o.argv.empty() || o.argv.front().empty())
                throw std::invalid_argument("external provider requires a helper command");
            if (o.registration.operations != std::vector<std::string>{"evaluate"})
                throw std::invalid_argument("external helpers support the evaluate operation");
            if (o.registration.facts.empty())
                throw std::invalid_argument("external provider requires exported facts");
            std::set<std::pair<std::string, std::string>> ports;
            bool output = false;
            for (const auto& port : o.registration.ports) {
                if (port.name.empty() || port.type.empty() || (port.direction != "input" && port.direction != "output") ||
                    !ports.emplace(port.direction, port.name).second)
                    throw std::invalid_argument("invalid external provider port");
                if (port.direction == "output") {
                    if (port.type != "semantic_shard")
                        throw std::invalid_argument("helper outputs use semantic_shard records");
                    output = true;
                }
            }
            if (!output)
                throw std::invalid_argument("external provider requires a semantic_shard output");
            for (const auto& [suffix, language] : o.documents)
                if (suffix.empty() || suffix.find_first_of("./\\") != std::string::npos || language.empty())
                    throw std::invalid_argument("invalid external provider document mapping");
            builds::validate_environment(o.env);
            (void)requirements(o);
        }
        static auto launch(const options& o, const std::vector<builds::context>&) -> program {
            return {.provider = o.registration.id,
                    .argv = o.argv,
                    .env = o.env,
                    .initialization = o.initialization,
                    .executables = o.tools,
                    .registration = o.registration,
                    .documents = o.documents};
        }
    };
} // namespace language
