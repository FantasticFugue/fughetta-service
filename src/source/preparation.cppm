export module preparation;
import std;
import native;
import coroutine;
import language;
import replica;
import protocol;
import runner;
import durability;
import host;
import reflection;
import isolation;

export namespace preparation {
    class service {
        struct receipt {
            content::hash input;
            language::result result;
            // A preparation a worker runs is preparing until this time (Unix seconds), interrupted after it.
            std::int64_t until = 0;
        };
        static auto now() -> std::int64_t {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

    public:
        service(cores::context<native::scheduler>& rt, std::filesystem::path root, execution::thread_pool& pool, builds::toolset tools = {})
            : rt_(rt), pool_(pool), store_(std::move(root)), tools_(std::move(tools)) {}

        // A preparation a worker ran: `after` publishes what it found; then its receipt, written as preparing by `begin`, is final.
        template <class After>
        auto finish(std::size_t core, language::request request, language::result result, After after) -> coroutine::task<language::result> {
            if (core == 0)
                co_return co_await completed(std::move(request), std::move(result), std::move(after));
            co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(0),
                                           [this, request = std::move(request), result = std::move(result), after]() mutable {
                                               return completed(std::move(request), std::move(result), std::move(after));
                                           });
        }

        // `gather` stages the source; `after` runs once the preparation succeeded, before its receipt is final. Action `begin` only writes
        // the preparing receipt of a preparation a worker will run, or answers the receipt there is.
        template <class Gather, class After>
        auto run(std::size_t core, language::request request, Gather gather, After after) -> coroutine::task<language::result> {
            if (core == 0)
                co_return co_await serial(std::move(request), std::move(gather), std::move(after));
            co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(0), [this, request = std::move(request), gather, after]() mutable {
                return serial(std::move(request), std::move(gather), std::move(after));
            });
        }

        // Stages one worldline version for preparation: its manifest, and `read(digest, offset, count)` for the bytes of its files.
        template <class Read>
        static auto stage(replica::store& cache, const language::request& request, std::string version, content::manifest tree, Read read) -> language::result {
            auto plan = language::prepare(request.config);
            language::check(plan, tree.files);
            auto missing = cache.offer({request.project, tree});
            for (auto& hash : missing.missing) {
                auto file = std::ranges::find(tree.files, hash, &content::entry::digest);
                std::uint64_t offset = 0;
                do {
                    auto n = std::min<std::uint64_t>(file->size - offset, 1u << 20);
                    auto text = read(hash, offset, n);
                    auto bytes = std::as_bytes(std::span{text});
                    cache.put({{{hash, offset, file->size, {bytes.begin(), bytes.end()}}}});
                    offset += n;
                } while (offset < file->size);
            }
            return checkout(cache, request, std::move(version), tree, plan);
        }

    private:
        static auto checkout(replica::store& cache, const language::request& request, std::string version, const content::manifest& tree,
                             const language::plan& plan) -> language::result {
            language::result out{.operation_id = request.operation_id, .worldline = request.worldline, .revision = std::move(version)};
            auto env = host::environment();
            auto key = content::hash_of(std::format("{}:{}:{}", request.worldline, plan.recipe.view(), rfl::json::write(env)));
            auto path = cache.checkout(request.project, tree.id(), key);
            out.snapshot = tree.id();
            out.configuration = plan.fingerprint;
            out.tree = std::filesystem::absolute(path).string();
            out.build_dirs = plan.build.build_dirs;
            for (auto& dir : plan.build.source_dirs)
                if (!dir.empty())
                    safe(path, dir);
            for (auto& dir : plan.build.build_dirs)
                safe(path, dir);
            auto marker = cache.root() / "heads" / request.project / (std::string{key.view()} + ".json");
            durability::durable_directories(marker.parent_path());
            durability::durable_write(marker, rfl::json::write(tree));
            for (auto& context : plan.inputs) {
                safe(path, context.path);
                auto input = path / context.path;
                if (std::filesystem::exists(input)) {
                    auto status = std::filesystem::symlink_status(input);
                    if (context.kind == "directory" ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status))
                        throw std::invalid_argument("analysis input " + context.name + " must be a " + context.kind);
                    std::filesystem::remove_all(input);
                }
            }
            return out;
        }
        static void safe(const std::filesystem::path& root, const std::string& relative) {
            auto path = root;
            for (const auto& component : std::filesystem::path(relative)) {
                path /= component;
                if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
                    throw std::invalid_argument("analysis path goes through a symlink");
            }
        }
        static auto read(const std::filesystem::path& path, std::uint64_t limit) -> std::string {
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) || std::filesystem::file_size(path) > limit)
                throw std::runtime_error("analysis metadata is missing, unsafe, or too large");
            std::ifstream input{path, std::ios::binary};
            if (!input)
                throw std::runtime_error("cannot read analysis metadata");
            std::string text{std::istreambuf_iterator<char>{input}, {}};
            if (input.bad())
                throw std::runtime_error("cannot read analysis metadata");
            return text;
        }
        static auto source_intact(const std::filesystem::path& marker, const language::result& result) -> bool {
            auto manifest = rfl::json::read<content::manifest>(read(marker, 64u << 20));
            if (!manifest || manifest->id() != result.snapshot)
                return false;
            for (auto& file : manifest->files) {
                content::checked_path(file.path);
                safe(result.tree, file.path);
                auto path = std::filesystem::path(result.tree) / file.path;
                if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) || std::filesystem::file_size(path) != file.size)
                    return false;
                std::ifstream input{path, std::ios::binary};
                if (!input)
                    return false;
                content::sha256 digest;
                std::array<char, 16384> buffer;
                while (input.read(buffer.data(), buffer.size()) || input.gcount())
                    digest.update(std::as_bytes(std::span{buffer}.first(static_cast<std::size_t>(input.gcount()))));
                if (input.bad() || content::to_hex(digest.finish()) != file.digest)
                    return false;
            }
            return true;
        }
        static auto intact(const language::result& result) -> bool {
            for (auto& dir : result.build_dirs)
                safe(result.tree, dir);
            std::map<std::tuple<std::string, std::string, std::string>, builds::artifact_result> inspected;
            auto matches = [&](const builds::artifact_result& artifact) {
                auto path = std::filesystem::path(artifact.path).lexically_relative(result.tree).generic_string();
                auto key = std::tuple{path, artifact.type, artifact.kind};
                auto found = inspected.find(key);
                if (found == inspected.end())
                    found = inspected.emplace(std::move(key), builds::inspect({artifact.name, artifact.type, path, artifact.kind}, result.tree)).first;
                return found->second.digest == artifact.digest && found->second.units == artifact.units;
            };
            return !result.inputs.empty() && std::ranges::all_of(result.artifacts, matches) && std::ranges::all_of(result.inputs, matches);
        }
        static void inspect(language::result& result, const language::plan& plan) {
            std::map<std::tuple<std::string, std::string, std::string>, builds::artifact_result> inspected;
            auto measure = [&](const builds::context& artifact) {
                auto key = std::tuple{artifact.path, artifact.type, artifact.kind};
                auto found = inspected.find(key);
                if (found == inspected.end())
                    found = inspected.emplace(std::move(key), builds::inspect(artifact, result.tree)).first;
                auto value = found->second;
                value.name = artifact.name;
                return value;
            };
            result.artifacts.clear();
            result.inputs.clear();
            for (auto& step : plan.build.steps)
                for (auto& artifact : step.outputs)
                    result.artifacts.push_back(measure(artifact));
            for (auto& database : plan.inputs)
                result.inputs.push_back(measure(database));
        }
        template <class Gather, class After> auto serial(language::request request, Gather gather, After after) -> coroutine::task<language::result> {
            auto held = co_await lock_.lock();
            replica::store::checked_project(request.project);
            content::checked_name(request.operation_id);
            if (request.action != "prepare" && request.action != "receipt" && request.action != "begin")
                throw std::invalid_argument("unknown analysis action");
            auto path = receipt_path(request);
            auto input = input_of(request);
            auto saved = co_await disk([&]() -> std::optional<receipt> {
                if (!std::filesystem::exists(std::filesystem::symlink_status(path)))
                    return {};
                auto parsed = rfl::json::read<receipt>(read(path, 4u << 20));
                if (!parsed)
                    throw std::runtime_error("invalid analysis receipt; refusing recovery");
                return *parsed;
            });
            if (saved) {
                if (request.action != "receipt" && saved->input != input)
                    throw std::invalid_argument("analysis operation ID was reused with different input");
                auto result = saved->result;
                if (result.status == "preparing" && saved->until <= now()) {
                    result.status = "interrupted";
                    result.error = "preparation ended without a completion receipt";
                }
                // Only the source's own tree can be checked; a worker's stays where it was built.
                if (result.status == "prepared" && result.worker.empty()) {
                    auto current = co_await disk([&] {
                        try {
                            auto marker = store_.root() / "heads" / request.project / (std::filesystem::path(result.tree).filename().string() + ".json");
                            return source_intact(marker, result) && intact(result);
                        } catch (const std::exception&) {
                            return false;
                        }
                    });
                    if (!current) {
                        result.status = "stale";
                        result.error = "prepared files have changed; start a new preparation";
                    }
                }
                result.replayed = request.action != "receipt";
                co_return result;
            }
            if (request.action == "receipt")
                throw std::invalid_argument("analysis receipt not found");
            if (!request.config.build.enabled)
                throw std::invalid_argument("analysis configuration is disabled");
            auto plan = language::prepare(request.config);
            receipt record{.input = input,
                           .result = {.operation_id = request.operation_id,
                                      .status = "preparing",
                                      .worldline = request.worldline,
                                      .revision = request.revision,
                                      .configuration = plan.fingerprint}};
            auto persist = [&] {
                durability::durable_directories(path.parent_path());
                durability::durable_write(path, rfl::json::write(record));
            };
            if (request.action == "begin") {
                // The worker's steps get the configuration's time, and its sync and survey ten more minutes.
                record.until = now() + language::timeout(request.config) + 600;
                co_await disk(persist);
                co_return record.result;
            }
            co_await disk(persist);
            auto started = std::chrono::steady_clock::now();
            try {
                record.result = co_await gather(store_, request);
                record.result.status = "preparing";
                co_await disk(persist);
                auto io = rt_.get_scheduler(0);
                auto env = host::environment();
                for (auto& [key, value] : plan.build.env)
                    env[key] = value;
                if ((plan.build.needs.platform.os && *plan.build.needs.platform.os != host::os) ||
                    (plan.build.needs.platform.arch && *plan.build.needs.platform.arch != host::arch))
                    throw std::runtime_error("Source does not meet preparation platform requirements");
                auto tools = co_await disk([&] { return host::detect_tools(tools_); });
                for (auto& tool : plan.build.needs.tools)
                    if (!std::ranges::contains(tools, tool))
                        throw std::runtime_error("Source lacks required tool: " + tool);
                if (!resources_)
                    resources_ = std::make_unique<isolation::pool>(content::hash_of(std::filesystem::absolute(record.result.tree).string()).str());
                for (auto& step : plan.build.steps) {
                    auto left = std::chrono::seconds{language::timeout(request.config)} - (std::chrono::steady_clock::now() - started);
                    if (left <= std::chrono::steady_clock::duration::zero())
                        throw std::runtime_error("analysis preparation timed out");
                    // A preparation keeps no report, but its steps write them as anywhere else.
                    auto reports = store_.root() / "reports" / std::filesystem::path(record.result.tree).filename();
                    co_await disk([&] {
                        std::filesystem::remove_all(reports);
                        std::filesystem::create_directories(reports);
                    });
                    auto local = builds::locate(step, record.result.tree, tools_, 1, reports);
                    auto step_env = env;
                    for (auto& [key, value] : local.env)
                        step_env[key] = value;
                    step_env["OMP_NUM_THREADS"] = "1";
                    io::command command{.argv = std::move(local.argv), .cwd = local.cwd, .env = std::move(step_env)};
                    if (local.standard_output)
                        command.standard_output = reports / *local.standard_output;
                    auto report = co_await runner::prepared_step(io, std::move(command), std::chrono::duration_cast<std::chrono::milliseconds>(left),
                                                                 language::output_limit(request.config), step.name, *resources_);
                    record.result.steps.push_back({report.name, report.exit_code, report.timed_out, report.seconds, report.output, report.truncated});
                    if (report.exit_code || report.timed_out)
                        throw std::runtime_error("analysis preparation command failed");
                    co_await disk([&] {
                        for (auto& artifact : step.outputs)
                            builds::inspect(artifact, record.result.tree);
                    });
                }
                co_await disk([&] {
                    auto marker = store_.root() / "heads" / request.project / (std::filesystem::path(record.result.tree).filename().string() + ".json");
                    if (!source_intact(marker, record.result))
                        throw std::runtime_error("preparation changed retained source content");
                    inspect(record.result, plan);
                });
                record.result.status = "prepared";
            } catch (const std::exception& error) {
                record.result.status = "failed";
                record.result.error = error.what();
            }
            if (record.result.status == "prepared")
                co_await after(record.result, request, plan);
            co_await disk(persist);
            co_return record.result;
        }
        auto receipt_path(const language::request& request) const -> std::filesystem::path {
            return store_.root() / "receipts" / request.project / (request.operation_id + ".json");
        }
        // What a receipt is for: the request, whichever action holds it.
        static auto input_of(language::request request) -> content::hash {
            request.action = "prepare";
            return content::hash_of(rfl::json::write(request));
        }

        template <class After> auto completed(language::request request, language::result result, After after) -> coroutine::task<language::result> {
            auto held = co_await lock_.lock();
            replica::store::checked_project(request.project);
            content::checked_name(request.operation_id);
            auto path = receipt_path(request);
            auto saved = co_await disk([&]() -> std::optional<receipt> {
                if (!std::filesystem::exists(std::filesystem::symlink_status(path)))
                    return {};
                auto parsed = rfl::json::read<receipt>(read(path, 4u << 20));
                if (!parsed)
                    throw std::runtime_error("invalid analysis receipt; refusing recovery");
                return *parsed;
            });
            if (!saved || saved->result.status != "preparing" || saved->input != input_of(request))
                throw std::invalid_argument("no preparing receipt for this analysis operation");
            receipt record{.input = saved->input, .result = std::move(result)};
            record.result.operation_id = request.operation_id;
            record.result.worldline = request.worldline;
            record.result.configuration = saved->result.configuration;
            if (record.result.status == "prepared")
                co_await after(record.result, request, language::prepare(request.config));
            co_await disk([&] { durability::durable_write(path, rfl::json::write(record)); });
            co_return record.result;
        }

        template <class F> auto disk(F work) -> coroutine::task<std::invoke_result_t<F&>> {
            co_return co_await io::blocking(rt_.get_scheduler(0), pool_, std::move(work));
        }
        cores::context<native::scheduler>& rt_;
        std::unique_ptr<isolation::pool> resources_;
        execution::thread_pool& pool_;
        replica::store store_;
        builds::toolset tools_;
        coroutine::mutex lock_;
    };
} // namespace preparation
