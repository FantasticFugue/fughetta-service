module build;

namespace builds {
    auto adapter_of(const builder& b) -> std::string_view {
        return b.visit([](const auto& w) { return std::remove_cvref_t<decltype(w)>::name; });
    }
    auto schemas() -> const std::map<std::string, rfl::Generic>& {
        static const auto out = [] {
            std::map<std::string, rfl::Generic> out;
            [&]<class... W>(std::type_identity<rfl::TaggedUnion<"adapter", W...>>) {
                ((out[std::string{W::name}] =
                      *rfl::json::read<rfl::Generic>(rfl::json::to_schema<typename W::implementation::options, rfl::DefaultIfMissing, rfl::NoExtraFields>())),
                 ...);
            }(std::type_identity<builder>{});
            return out;
        }();
        return out;
    }
    namespace {
        auto describe(const builder& b) -> std::map<std::string, operation_spec> {
            return b.visit([](const auto& w) {
                using implementation = typename std::remove_cvref_t<decltype(w)>::implementation;
                implementation::validate(w.options);
                return implementation::describe(w.options);
            });
        }
        auto expand(const builder& b, std::string_view operation, const parameters& p) -> std::vector<step> {
            return b.visit([&](const auto& w) { return std::remove_cvref_t<decltype(w)>::implementation::expand(w.options, operation, p); });
        }
        auto needs_of(const builder& b) -> const requirements& {
            return b.visit([](const auto& w) -> const requirements& { return w.needs.get(); });
        }
        auto env_of(const builder& b) -> const environment& {
            return b.visit([](const auto& w) -> const environment& { return w.env; });
        }
        void name(std::string_view n) {
            if (n.empty() || !std::ranges::all_of(n, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; }))
                throw std::invalid_argument("invalid build graph name");
        }
        using descriptions = std::map<std::string, std::map<std::string, operation_spec>>;
        auto describe(const configuration& c) -> descriptions {
            descriptions out;
            for (auto& [id, b] : c.builders) {
                name(id);
                validate(needs_of(b));
                validate_environment(env_of(b));
                out[id] = describe(b);
                for (auto& [op, spec] : out[id]) {
                    name(op);
                    validate(spec.needs);
                    for (auto& p : spec.parameters)
                        if (p != "targets" && p != "args" && p != "filter" && p != "labels" && p != "parallel")
                            throw std::invalid_argument("unknown operation parameter: " + p);
                    for (auto& [port, type] : spec.inputs) {
                        name(port);
                        if (type.empty())
                            throw std::invalid_argument("input type is empty");
                    }
                    for (auto& [port, artifact] : spec.outputs) {
                        name(port);
                        if (artifact.type.empty())
                            throw std::invalid_argument("output type is empty");
                        if (artifact.kind != "file" && artifact.kind != "directory")
                            throw std::invalid_argument("output kind must be file or directory");
                        artifacts::find(artifact.type, [&]<class F>(F) {
                            if (artifact.kind != F::kind)
                                throw std::invalid_argument("a " + artifact.type + " output must be a " + std::string{F::kind});
                        });
                        relative(artifact.path);
                    }
                    for (auto& p : spec.source_dirs)
                        relative(p, true);
                    for (auto& p : spec.build_dirs)
                        relative(p);
                }
            }
            return out;
        }
        auto spec_of(const configuration& c, const descriptions& d, const std::string& id) -> const operation_spec& {
            auto t = c.tasks.find(id);
            if (t == c.tasks.end())
                throw std::invalid_argument("unknown build task: " + id);
            auto b = d.find(t->second.builder);
            if (b == d.end())
                throw std::invalid_argument("unknown builder: " + t->second.builder);
            auto op = b->second.find(t->second.operation);
            if (op == b->second.end())
                throw std::invalid_argument("builder does not provide operation: " + id + "/" + t->second.operation);
            return op->second;
        }
        auto output(const configuration& c, const descriptions& d, const reference& r) -> const artifact& {
            auto& spec = spec_of(c, d, r.task);
            auto out = spec.outputs.find(r.output);
            if (out == spec.outputs.end())
                throw std::invalid_argument("task does not provide output: " + r.task + "/" + r.output);
            return out->second;
        }
        auto order(const configuration& c, const std::vector<std::string>& roots) -> std::vector<std::string> {
            std::map<std::string, int> visited;
            std::vector<std::string> ordered;
            auto visit = [&](this auto&& self, const std::string& id) -> void {
                auto it = c.tasks.find(id);
                if (it == c.tasks.end())
                    throw std::invalid_argument("unknown build task: " + id);
                if (visited[id] == 1)
                    throw std::invalid_argument("build task dependency cycle at: " + id);
                if (visited[id] == 2)
                    return;
                visited[id] = 1;
                for (auto& before : it->second.after)
                    self(before);
                for (auto& [_, input] : it->second.inputs)
                    self(input.task);
                visited[id] = 2;
                ordered.push_back(id);
            };
            for (auto& root : roots)
                visit(root);
            return ordered;
        }
        void bind_inputs(std::string& value, const std::map<std::string, reference>& inputs, const configuration& c, const descriptions& d) {
            for (auto& [port, input] : inputs) {
                auto key = "${input:" + port + "}";
                auto path = "${tree}/" + output(c, d, input).path;
                std::size_t pos = 0;
                while ((pos = value.find(key, pos)) != std::string::npos) {
                    value.replace(pos, key.size(), path);
                    pos += path.size();
                }
            }
            if (value.find("${input:") != std::string::npos)
                throw std::invalid_argument("unbound build input in step");
            if (value.find('\0') != std::string::npos)
                throw std::invalid_argument("build argument contains a null character");
        }
        void check(const configuration& c, const descriptions& d) {
            validate(c.needs.get());
            validate_environment(c.env);
            if ((c.limits.timeout_s && !*c.limits.timeout_s) || (c.limits.max_memory_bytes && !*c.limits.max_memory_bytes) ||
                (c.limits.max_cpu_seconds && !*c.limits.max_cpu_seconds))
                throw std::invalid_argument("build limits must be positive");
            std::vector<std::string> all;
            for (auto& [id, task] : c.tasks) {
                name(id);
                auto& spec = spec_of(c, d, id);
                require(spec, task.parameters);
                for (auto& [port, type] : spec.inputs)
                    if (!task.inputs.contains(port))
                        throw std::invalid_argument("required build input is unbound: " + id + "/" + port);
                for (auto& [port, input] : task.inputs) {
                    name(port);
                    auto expected = spec.inputs.find(port);
                    if (expected == spec.inputs.end() || output(c, d, input).type != expected->second)
                        throw std::invalid_argument("build input type mismatch: " + id + "/" + port);
                }
                all.push_back(id);
            }
            order(c, all);
            for (auto& [id, op] : c.operations) {
                name(id);
                for (const auto& [label, path] : op.exports) {
                    name(label);
                    content::checked_path(path);
                }
                if (op.tasks.empty())
                    throw std::invalid_argument("build operation has no tasks: " + id);
                auto tasks = order(c, op.tasks);
                if (op.receiver && !std::ranges::contains(tasks, *op.receiver))
                    throw std::invalid_argument("operation " + id + " receives parameters in a task it does not run: " + *op.receiver);
            }
            for (auto& [id, ref] : c.outputs) {
                name(id);
                output(c, d, ref);
            }
        }
        // A report file is a plain name in ${reports}, named once, read by a known protocol.
        void reports(const step& value, const std::string& task) {
            auto plain = [&](const std::string& file) {
                if (file.empty() || file == "." || file == ".." || file.find_first_of("/\\:") != std::string::npos || file.contains('\0') ||
                    file.contains("${"))
                    throw std::invalid_argument("a report of task " + task + " is a plain file name in ${reports}: " + file);
            };
            for (std::size_t i = 0; i < value.reports.size(); ++i) {
                auto& r = value.reports[i];
                plain(r.file);
                if (!report_protocols::known(r.protocol)) {
                    std::string names;
                    for (auto n : report_protocols::names)
                        names += (names.empty() ? "" : ", ") + std::string{n};
                    throw std::invalid_argument("unknown report protocol " + r.protocol + " of task " + task + " (protocols: " + names + ")");
                }
                for (std::size_t j = 0; j < i; ++j)
                    if (value.reports[j].file == r.file)
                        throw std::invalid_argument("task " + task + " names the report " + r.file + " twice");
            }
            if (value.standard_output)
                plain(*value.standard_output);
        }
        auto plan_of(const configuration& c, const descriptions& d, const request& request) -> plan {
            auto op = c.operations.find(request.operation);
            if (op == c.operations.end())
                throw std::invalid_argument("configuration does not provide operation: " + request.operation);
            auto tasks = order(c, op->second.tasks);
            std::optional<std::string> receiver;
            if (!requested(request.parameters).empty()) {
                if (op->second.receiver)
                    receiver = op->second.receiver;
                else if (op->second.tasks.size() == 1)
                    receiver = op->second.tasks.front();
                else
                    throw std::invalid_argument("operation " + request.operation +
                                                " has several final tasks; name the one that takes parameters as its receiver");
            }
            plan out{.needs = c.needs.get(), .env = c.env};
            auto add = [](auto& into, const auto& values) {
                for (auto& v : values)
                    if (!std::ranges::contains(into, v))
                        into.push_back(v);
            };
            for (auto& id : tasks) {
                auto& t = c.tasks.at(id);
                auto& b = c.builders.at(t.builder);
                auto& spec = spec_of(c, d, id);
                auto p = t.parameters;
                if (receiver == id) {
                    auto& r = request.parameters;
                    if (!r.targets.empty())
                        p.targets = r.targets;
                    p.args.insert(p.args.end(), r.args.begin(), r.args.end());
                    if (r.filter)
                        p.filter = r.filter;
                    if (r.labels)
                        p.labels = r.labels;
                    if (r.parallel)
                        p.parallel = r.parallel;
                }
                require(spec, p);
                combine(out.needs, needs_of(b));
                combine(out.needs, spec.needs);
                add(out.source_dirs, spec.source_dirs);
                add(out.build_dirs, spec.build_dirs);
                task_plan task{.name = id, .dependencies = t.after, .first_step = out.steps.size()};
                for (auto& [_, input] : t.inputs)
                    if (!std::ranges::contains(task.dependencies, input.task))
                        task.dependencies.push_back(input.task);
                for (auto& [port, artifact] : spec.outputs)
                    task.outputs.push_back({id + "/" + port, artifact.type, artifact.path, artifact.kind});
                auto steps = expand(b, t.operation, p);
                if (steps.empty())
                    throw std::invalid_argument("adapter produced no steps: " + id);
                for (auto& [port, artifact] : spec.outputs)
                    steps.back().outputs.push_back({id + "/" + port, artifact.type, artifact.path, artifact.kind});
                for (auto& step : steps) {
                    step.name = steps.size() == 1 ? id : id + "/" + step.name;
                    step.reports.insert(step.reports.end(), t.reports.begin(), t.reports.end());
                    reports(step, id);
                    if (step.argv.empty() || step.argv.front().empty())
                        throw std::invalid_argument("build step has no executable");
                    relative(step.cwd, true);
                    auto env = env_of(b);
                    for (auto& [key, value] : step.env)
                        env[key] = value;
                    step.env = std::move(env);
                    validate_environment(step.env);
                    for (auto& arg : step.argv)
                        bind_inputs(arg, t.inputs, c, d);
                    for (auto& [_, value] : step.env)
                        bind_inputs(value, t.inputs, c, d);
                    out.steps.push_back(std::move(step));
                }
                task.step_count = out.steps.size() - task.first_step;
                out.tasks.push_back(std::move(task));
            }
            for (auto& [id, ref] : c.outputs)
                if (std::ranges::contains(tasks, ref.task)) {
                    auto& artifact = output(c, d, ref);
                    out.contexts.push_back({id, artifact.type, artifact.path, artifact.kind});
                }
            return out;
        }
        void replace_all(std::string& text, std::string_view token, std::string_view with) {
            std::size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos) {
                text.replace(pos, token.size(), with);
                pos += with.size();
            }
        }
    } // namespace
    void validate(const configuration& c) {
        auto d = describe(c);
        check(c, d);
        for (auto& [op, _] : c.operations)
            plan_of(c, d, {.operation = op});
    }
    auto prepare(const configuration& c, const request& request) -> plan {
        auto d = describe(c);
        check(c, d);
        return plan_of(c, d, request);
    }
    auto prepare_inputs(const configuration& c, const std::vector<std::string>& types, const std::map<std::string, std::string>& selected_paths) -> plan {
        auto descriptions = describe(c);
        check(c, descriptions);
        if (!c.enabled)
            throw std::invalid_argument("analysis build configuration is disabled");
        if (types.empty())
            return {.needs = c.needs.get(), .env = c.env};
        std::vector<std::string> roots;
        std::vector<context> inputs;
        std::set<std::string> seen;
        for (auto& type : types) {
            if (!seen.insert(type).second)
                continue;
            std::vector<std::pair<std::string, context>> producers;
            for (auto& [id, task] : c.tasks)
                for (auto& [port, artifact] : spec_of(c, descriptions, id).outputs)
                    if (artifact.type == type && (!selected_paths.contains(type) || selected_paths.at(type) == artifact.path))
                        producers.push_back({id, {id + "/" + port, artifact.type, artifact.path, artifact.kind}});
            if (producers.empty())
                throw std::invalid_argument("analysis has no producer of " + type);
            std::map<std::string, std::string> paths;
            for (auto& [id, artifact] : producers) {
                if (auto existing = paths.find(artifact.path); existing != paths.end()) {
                    if (existing->second != id)
                        throw std::invalid_argument("analysis producers write the same artifact: " + artifact.path);
                    continue;
                }
                paths.emplace(artifact.path, id);
                if (!std::ranges::contains(roots, id))
                    roots.push_back(id);
                inputs.push_back(artifact);
            }
        }
        auto selected = c;
        selected.operations["__analysis"] = {.tasks = std::move(roots)};
        auto out = plan_of(selected, descriptions, {.operation = "__analysis"});
        out.contexts = std::move(inputs);
        return out;
    }
    auto locate(step value, const std::filesystem::path& tree, const toolset& tools, std::uint32_t jobs, const std::filesystem::path& reports,
                const std::filesystem::path& dependency_cache, const std::filesystem::path& dependency_upstream) -> step {
        auto root = std::filesystem::absolute(tree);
        auto bind = [&](std::string& text) {
            if (text.contains("${cache:")) throw std::invalid_argument("cache placeholders are reserved for managed binary sources");
            replace_all(text, "${tree}", root.string());
            replace_all(text, "${jobs}", std::to_string(jobs));
            if (!reports.empty())
                replace_all(text, "${reports}", std::filesystem::absolute(reports).string());
            for (std::size_t at; (at = text.find("${tool:")) != std::string::npos;) {
                auto end = text.find('}', at);
                if (end == std::string::npos)
                    throw std::invalid_argument("unterminated ${tool:...} in step");
                auto name = text.substr(at + 7, end - at - 7);
                tool_name(name);
                auto mapped = tools.find(name);
                text.replace(at, end + 1 - at, mapped == tools.end() ? name : mapped->second);
            }
        };
        for (auto& arg : value.argv)
            bind(arg);
        for (auto& [name, v] : value.env) {
            if (name == "VCPKG_BINARY_SOURCES" && v == "clear;files,${cache:vcpkg},readwrite") {
                if (dependency_cache.empty()) throw std::invalid_argument("vcpkg dependency cache is not configured");
                if (!dependency_upstream.empty()) {
                    const auto writable = std::filesystem::weakly_canonical(std::filesystem::absolute(dependency_cache / "vcpkg"));
                    const auto upstream = std::filesystem::weakly_canonical(std::filesystem::absolute(dependency_upstream / "vcpkg"));
                    const auto [left, right] = std::mismatch(writable.begin(), writable.end(), upstream.begin(), upstream.end());
                    if (left == writable.end() || right == upstream.end())
                        throw std::invalid_argument("writable dependency cache must not overlap the published upstream cache");
                }
                auto source = [](const std::filesystem::path& cache) {
                    std::string escaped;
                    for (char c : std::filesystem::absolute(cache / "vcpkg").string()) {
                        if (c == '`' || c == ',' || c == ';') escaped += '`';
                        escaped += c;
                    }
                    return ";files," + escaped;
                };
                v = "clear";
                if (!dependency_upstream.empty()) v += source(dependency_upstream) + ",read";
                v += source(dependency_cache) + ",readwrite";
            } else bind(v);
        }
        relative(value.cwd, true);
        value.cwd = (value.cwd.empty() ? root : root / value.cwd).string();
        if (!value.argv.empty() && value.argv[0].find_first_of("/\\") != std::string::npos && !std::filesystem::path(value.argv[0]).is_absolute())
            value.argv[0] = (root / content::checked_path(value.argv[0])).string();
        return value;
    }
    auto collect(const step& bound, const std::filesystem::path& reports, const std::filesystem::path& tree) -> reading {
        constexpr std::uintmax_t max_bytes = 16u << 20;
        auto root = std::filesystem::absolute(tree);
        reading out;
        for (auto& r : bound.reports) {
            auto path = reports / r.file;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)))
                continue;
            reading found;
            try {
                if (std::filesystem::file_size(path) > max_bytes)
                    throw std::runtime_error("larger than 16 MiB");
                std::ifstream input{path, std::ios::binary};
                found = report_protocols::read(r.protocol, std::string{std::istreambuf_iterator<char>{input}, {}});
            } catch (const std::exception& e) {
                out.diagnostics.push_back(
                    {.severity = "error", .message = std::format("the {} report {} could not be read: {}", r.protocol, r.file, e.what())});
                continue;
            }
            for (auto& d : found.diagnostics) {
                std::filesystem::path file{d.file};
                auto rel = (file.is_absolute() ? file : std::filesystem::path{bound.cwd} / file).lexically_normal().lexically_relative(root);
                if (!rel.empty() && *rel.begin() != "..")
                    d.file = rel.generic_string();
                out.diagnostics.push_back(std::move(d));
            }
            if (found.tests) {
                if (!out.tests)
                    out.tests.emplace();
                out.tests->total += found.tests->total;
                out.tests->failed += found.tests->failed;
                std::ranges::move(found.tests->failed_tests, std::back_inserter(out.tests->failed_tests));
            }
        }
        return out;
    }
    auto inspect(const context& artifact, const std::filesystem::path& tree) -> artifact_result {
        relative(artifact.path);
        auto path = std::filesystem::absolute(tree);
        for (const auto& part : std::filesystem::path(artifact.path)) {
            path /= part;
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
                throw std::runtime_error("build artifact goes through a symlink: " + artifact.name);
        }
        artifact_result out{.name = artifact.name, .type = artifact.type, .path = path.string(), .kind = artifact.kind};
        if (artifact.kind == "directory") {
            if (!std::filesystem::is_directory(path))
                throw std::runtime_error("missing build directory artifact: " + artifact.name);
            return out;
        }
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("missing build artifact: " + artifact.name);
        std::uint64_t max_bytes = 0;
        std::uint64_t (*check)(std::string_view) = nullptr;
        artifacts::find(artifact.type, [&]<class F>(F) {
            max_bytes = F::max_bytes;
            check = &F::check;
        });
        if (max_bytes && std::filesystem::file_size(path) > max_bytes)
            throw std::runtime_error(artifact.type + " artifact exceeds " + std::to_string(max_bytes >> 20) + " MiB: " + artifact.name);
        std::ifstream input{path, std::ios::binary};
        if (!input)
            throw std::runtime_error("cannot read build artifact: " + artifact.name);
        content::sha256 hash;
        std::string body;
        std::array<char, 16384> buffer;
        while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
            auto bytes = std::span{buffer}.first(static_cast<std::size_t>(input.gcount()));
            hash.update(std::as_bytes(bytes));
            if (check)
                body.append(bytes.data(), bytes.size());
        }
        if (input.bad())
            throw std::runtime_error("cannot read build artifact: " + artifact.name);
        out.digest = content::to_hex(hash.finish());
        if (check)
            out.units = check(body);
        return out;
    }
} // namespace builds
