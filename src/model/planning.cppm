module build;

namespace builds {
    auto adapter_of(const builder& b) -> std::string_view {
        return b.visit([](const auto& w) { return std::remove_cvref_t<decltype(w)>::name; });
    }
    auto schemas() -> const std::map<std::string, rfl::Generic>& {
        static const auto out = [] {
            std::map<std::string, rfl::Generic> out;
            [&]<class... W>(std::type_identity<rfl::TaggedUnion<"adapter", W...>>) {
                ((out[std::string{W::name}] = *rfl::json::read<rfl::Generic>(
                      rfl::json::to_schema<typename W::implementation::options, rfl::DefaultIfMissing, rfl::NoExtraFields>())),
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
                auto steps = expand(b, t.operation, p);
                if (steps.empty())
                    throw std::invalid_argument("adapter produced no steps: " + id);
                for (auto& [port, artifact] : spec.outputs)
                    steps.back().outputs.push_back({id + "/" + port, artifact.type, artifact.path, artifact.kind});
                for (auto& step : steps) {
                    step.name = steps.size() == 1 ? id : id + "/" + step.name;
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
    auto locate(step value, const std::filesystem::path& tree, const toolset& tools, std::uint32_t jobs) -> step {
        auto root = std::filesystem::absolute(tree);
        auto bind = [&](std::string& text) {
            replace_all(text, "${tree}", root.string());
            replace_all(text, "${jobs}", std::to_string(jobs));
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
        for (auto& [_, v] : value.env)
            bind(v);
        relative(value.cwd, true);
        value.cwd = (value.cwd.empty() ? root : root / value.cwd).string();
        if (!value.argv.empty() && value.argv[0].find_first_of("/\\") != std::string::npos && !std::filesystem::path(value.argv[0]).is_absolute())
            value.argv[0] = (root / content::checked_path(value.argv[0])).string();
        return value;
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
