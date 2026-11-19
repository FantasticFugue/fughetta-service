module language;
import json;

namespace language {
    auto timeout(const builds::limits& c) -> std::uint32_t { return c.timeout_s.value_or(1800); }
    auto output_limit(const builds::limits& c) -> std::uint64_t { return c.output_limit.value_or(1u << 20); }
    auto provider_of(const provider& p) -> std::string_view {
        return p.visit([](const auto& w) -> std::string_view {
            if constexpr (requires { w.options.registration; })
                return w.options.registration.id;
            else
                return std::remove_cvref_t<decltype(w)>::name;
        });
    }
    auto select_builds(const stage& c, const std::map<std::string, builds::configuration>& builds) -> std::map<std::string, builds::configuration> {
        std::set<std::string> names{c.build_configuration};
        for (const auto& [port, producers] : c.inputs)
            for (const auto& producer : producers)
                if (const auto* build = std::get_if<build_input>(&producer))
                    names.insert(build->configuration);
        std::map<std::string, builds::configuration> selected;
        for (const auto& name : names) {
            auto found = builds.find(name);
            if (name.empty() || found == builds.end())
                throw std::invalid_argument("unknown analysis build configuration: " + name);
            selected.emplace(name, found->second);
        }
        return selected;
    }
    auto fingerprint(const stage& c, const std::map<std::string, builds::configuration>& builds) -> content::hash {
        auto semantic = c;
        semantic.after.clear();
        semantic.execution = {};
        auto inputs = select_builds(c, builds);
        for (auto& [name, build] : inputs) {
            build.limits = {};
            build.relocatable_build_dirs.clear();
        }
        return content::hash_of(json::write(semantic) + "\n" + json::write(inputs));
    }
    namespace {
        template <class T>
        void overlay(std::optional<T>& into, const std::optional<T>& from, std::map<std::string, std::string>& origins, const std::string& key,
                     const std::string& source) {
            if (from) {
                into = from;
                origins[key] = source;
            }
        }
        void apply(execution& into, const execution& from, std::map<std::string, std::string>& origins, const std::string& source) {
            overlay(into.directory_group, from.directory_group, origins, "directory_group", source);
            auto& needs = into.needs.get();
            const auto& selected = from.needs.get();
            for (const auto& tool : selected.tools) {
                if (!std::ranges::contains(needs.tools, tool))
                    needs.tools.push_back(tool);
                origins["requires.tools." + tool] = source;
            }
            overlay(needs.platform.os, selected.platform.os, origins, "requires.platform.os", source);
            overlay(needs.platform.arch, selected.platform.arch, origins, "requires.platform.arch", source);
            overlay(into.limits.timeout_s, from.limits.timeout_s, origins, "limits.timeout_s", source);
            overlay(into.limits.output_limit, from.limits.output_limit, origins, "limits.output_limit", source);
            overlay(into.limits.max_memory_bytes, from.limits.max_memory_bytes, origins, "limits.max_memory_bytes", source);
            overlay(into.limits.max_cpu_seconds, from.limits.max_cpu_seconds, origins, "limits.max_cpu_seconds", source);
            overlay(into.budget.milliseconds, from.budget.milliseconds, origins, "budget.milliseconds", source);
            overlay(into.budget.units, from.budget.units, origins, "budget.units", source);
            overlay(into.budget.nodes, from.budget.nodes, origins, "budget.nodes", source);
        }
        template <class T> void cap(std::optional<T>& value, const std::optional<T>& ceiling) {
            if (ceiling)
                value = value ? std::min(*value, *ceiling) : *ceiling;
        }
    } // namespace
    auto resolve(const stage& c, const defaults& defaults) -> stage {
        auto result = c;
        result.execution = defaults.execution;
        std::map<std::string, std::string> origins;
        apply(result.execution, c.execution, origins, "stage");
        return result;
    }
    auto prepare(const stage& c, const std::map<std::string, builds::configuration>& configurations) -> plan {
        validate(c);
        const auto selected = select_builds(c, configurations);
        const auto& bound = selected.at(c.build_configuration);
        const auto descriptor = describe_provider(c.server);
        std::map<std::string, std::vector<builds::input>> bindings;
        for (const auto& port : descriptor.ports)
            if (port.direction == "input" && port.type != "source_snapshot" && port.required &&
                (!c.inputs.contains(port.name) || c.inputs.at(port.name).empty()))
                throw std::invalid_argument("required analysis input is unbound: " + port.name);
        for (const auto& [name, producers] : c.inputs) {
            const auto port = std::ranges::find_if(descriptor.ports, [&](const auto& value) { return value.direction == "input" && value.name == name; });
            if (port == descriptor.ports.end())
                throw std::invalid_argument("unknown analysis input: " + name);
            if (!port->multiple && producers.size() > 1)
                throw std::invalid_argument("analysis input takes one producer: " + name);
            for (const auto& producer : producers)
                if (const auto* input = std::get_if<build_input>(&producer))
                    bindings[input->configuration].push_back({{input->task, input->output}, port->type});
        }
        builds::plan build;
        builds::combine(build.needs, bound.needs.get());
        std::map<std::string, std::string> produced;
        auto add_paths = [](auto& into, const auto& from) {
            for (const auto& path : from)
                if (!std::ranges::contains(into, path))
                    into.push_back(path);
        };
        for (const auto& [configuration, producers] : bindings) {
            auto prepared = builds::prepare_inputs(selected.at(configuration), producers);
            const auto prefix = configuration + "/";
            auto qualify = [&](auto& outputs) {
                for (auto& output : outputs)
                    output.name = prefix + output.name;
            };
            for (auto& task : prepared.tasks) {
                task.name = prefix + task.name;
                for (auto& dependency : task.dependencies)
                    dependency = prefix + dependency;
                task.first_step += build.steps.size();
                qualify(task.outputs);
                for (const auto& output : task.outputs) {
                    auto [owner, inserted] = produced.emplace(output.path, configuration);
                    if (!inserted && owner->second != configuration)
                        throw std::invalid_argument("analysis producers write the same artifact: " + output.path);
                }
            }
            for (auto& step : prepared.steps) {
                step.name = prefix + step.name;
                auto environment = prepared.env;
                for (const auto& [key, value] : step.env)
                    environment[key] = value;
                step.env = std::move(environment);
                qualify(step.outputs);
                if (step.generation) {
                    step.generation->producer = prefix + step.generation->producer;
                    qualify(step.generation->outputs);
                }
            }
            qualify(prepared.contexts);
            builds::combine(build.needs, prepared.needs);
            add_paths(build.source_dirs, prepared.source_dirs);
            add_paths(build.build_dirs, prepared.build_dirs);
            std::ranges::move(prepared.tasks, std::back_inserter(build.tasks));
            std::ranges::move(prepared.steps, std::back_inserter(build.steps));
            std::ranges::move(prepared.contexts, std::back_inserter(build.contexts));
        }
        std::vector<builds::context> inputs;
        for (const auto& [port, producers] : c.inputs)
            for (const auto& producer : producers) {
                const auto* build_input = std::get_if<language::build_input>(&producer);
                if (!build_input)
                    continue;
                const auto name = build_input->configuration + "/" + build_input->task + "/" + build_input->output;
                if (std::ranges::none_of(inputs, [&](const auto& input) { return input.name == name; }))
                    inputs.push_back(*std::ranges::find(build.contexts, name, &builds::context::name));
            }
        build.contexts = inputs;
        auto server = c.server.visit([&](const auto& w) {
            using implementation = typename std::remove_cvref_t<decltype(w)>::implementation;
            implementation::validate(w.options);
            for (auto& input : inputs)
                if (std::ranges::none_of(descriptor.ports, [&](const auto& port) { return port.direction == "input" && port.type == input.type; }))
                    throw std::invalid_argument("the language provider does not read " + input.type + " output " + input.name);
            auto out = implementation::launch(w.options, inputs);
            auto environment = bound.env;
            for (const auto& [key, value] : out.env)
                environment[key] = value;
            out.env = std::move(environment);
            out.provider = std::string{provider_of(c.server)};
            return out;
        });
        builds::validate_environment(server.env);
        auto execution = c.execution;
        execution.directory_group = c.execution.directory_group.value_or(c.build_configuration);
        auto needs = build.needs;
        builds::combine(needs, execution.needs.get());
        builds::combine(needs, descriptor.needs);
        if (!server.argv.empty() && server.argv.front().find('/') == std::string::npos)
            builds::combine(needs, builds::tools({server.argv.front()}));
        for (const auto& [operation, argv] : server.commands)
            if (!argv.empty() && argv.front().find('/') == std::string::npos)
                builds::combine(needs, builds::tools({argv.front()}));
        for (const auto& executable : server.executables)
            if (!builds::absolute_path(executable) && executable.find_first_of("/\\") == std::string::npos)
                builds::combine(needs, builds::tools({executable}));
        execution.needs = needs;
        for (const auto& [name, dependency] : selected) {
            cap(execution.limits.timeout_s, dependency.limits.timeout_s);
            cap(execution.limits.output_limit, dependency.limits.output_limit);
            cap(execution.limits.max_memory_bytes, dependency.limits.max_memory_bytes);
            cap(execution.limits.max_cpu_seconds, dependency.limits.max_cpu_seconds);
        }
        execution.limits.timeout_s = timeout(execution.limits);
        execution.limits.output_limit = output_limit(execution.limits);
        return {.build = std::move(build),
                .server = std::move(server),
                .inputs = std::move(inputs),
                .fingerprint = fingerprint(c, selected),
                .execution = std::move(execution)};
    }
    auto expand(const configuration& c, const std::map<std::string, builds::configuration>& builds, const std::vector<std::string>& selected_outputs,
                const defaults& project) -> graph {
        auto name = [](const std::string& value) {
            if (value.empty() || value == "." || value == ".." || value.find_first_of("/\\") != std::string::npos || value.contains('\0'))
                throw std::invalid_argument("invalid analysis node name: " + value);
        };
        if (c.stages.empty() || c.outputs.empty())
            throw std::invalid_argument("analysis profile requires stages and outputs");
        std::map<std::string, std::string> registrations;
        for (const auto& [_, stage] : c.stages)
            stage.server.visit([&](const auto& value) {
                if constexpr (requires { value.options.registration; }) {
                    const auto definition = json::write(std::tuple{value.options.registration, value.options.documents});
                    const auto [previous, inserted] = registrations.emplace(value.options.registration.id, definition);
                    if (!inserted && previous->second != definition)
                        throw std::invalid_argument("conflicting external provider registration: " + value.options.registration.id);
                }
            });
        graph result;
        auto layer = [&](const defaults& values, const std::string& source) {
            overlay(result.defaults.fact_limit, values.fact_limit, result.origins, "fact_limit", source);
            overlay(result.defaults.relation_depth, values.relation_depth, result.origins, "relation_depth", source);
            apply(result.defaults.execution, values.execution, result.origins, source);
        };
        layer({.fact_limit = 50, .relation_depth = 1}, "service");
        layer(project, "project");
        layer(c.defaults, "profile");
        if (*result.defaults.fact_limit == 0)
            throw std::invalid_argument("analysis fact_limit must be positive");
        std::map<std::string, plan> plans;
        std::map<std::string, std::map<std::string, std::string>> produced;
        std::map<std::string, std::map<std::string, std::string>> output_types;
        std::map<std::string, std::vector<std::string>> emitted;
        for (const auto& [id, stage] : c.stages) {
            name(id);
            auto bound = builds.find(stage.build_configuration);
            if (bound == builds.end())
                throw std::invalid_argument("analysis stage " + id + " names an unknown build_configuration: " + stage.build_configuration);
            const auto descriptor = describe_provider(stage.server);
            for (const auto& port : descriptor.ports)
                if (port.direction == "output")
                    output_types[id].emplace(port.name, port.type);
            emitted.emplace(id, descriptor.emits);
        }
        for (const auto& [id, stage] : c.stages) {
            const auto descriptor = describe_provider(stage.server);
            for (const auto& [port, producers] : stage.inputs) {
                auto target = std::ranges::find_if(descriptor.ports, [&](const auto& p) { return p.direction == "input" && p.name == port; });
                if (target == descriptor.ports.end())
                    throw std::invalid_argument("unknown analysis input: " + id + "/" + port);
                if (!target->multiple && producers.size() > 1)
                    throw std::invalid_argument("analysis input takes one producer: " + id + "/" + port);
                for (const auto& producer : producers) {
                    if (const auto* dependency = std::get_if<project_input>(&producer)) {
                        for (const auto* value : {&dependency->project, &dependency->configuration, &dependency->output})
                            name(*value);
                        if (dependency->worldline.empty())
                            throw std::invalid_argument("project analysis input requires an explicit worldline");
                        if (target->type != "semantic_shard")
                            throw std::invalid_argument("project analysis inputs require a semantic_shard port: " + id + "/" + port);
                    }
                    if (const auto* ref = std::get_if<reference>(&producer)) {
                        auto owner = output_types.find(ref->stage);
                        if (owner == output_types.end() || !owner->second.contains(ref->output))
                            throw std::invalid_argument("unknown analysis output: " + ref->stage + "/" + ref->output);
                        if (owner->second.at(ref->output) != target->type)
                            throw std::invalid_argument("analysis input " + id + "/" + port + " requires " + target->type + "; " + ref->stage + "/" +
                                                        ref->output + " produces " + owner->second.at(ref->output));
                        if (descriptor.reads && port == "source_facts" && !std::ranges::contains(emitted.at(ref->stage), *descriptor.reads))
                            throw std::invalid_argument("analysis input " + id + "/" + port + " reads the " + *descriptor.reads + " domain; " + ref->stage +
                                                        " does not emit into it");
                    }
                }
            }
        }
        for (const auto& [port, producers] : c.outputs) {
            name(port);
            if (producers.empty())
                throw std::invalid_argument("analysis profile output is unbound: " + port);
            for (const auto& ref : producers)
                if (!c.stages.contains(ref.stage) || !output_types.at(ref.stage).contains(ref.output))
                    throw std::invalid_argument("unknown analysis output: " + ref.stage + "/" + ref.output);
        }
        std::map<std::string, int> visited;
        auto check_order = [&](this auto&& self, const std::string& id) -> void {
            if (!c.stages.contains(id))
                throw std::invalid_argument("unknown analysis stage: " + id);
            if (visited[id] == 1)
                throw std::invalid_argument("analysis stage dependency cycle: " + id);
            if (visited[id] == 2)
                return;
            visited[id] = 1;
            for (const auto& before : c.stages.at(id).after)
                self(before);
            for (const auto& [port, producers] : c.stages.at(id).inputs)
                for (const auto& producer : producers)
                    if (const auto* ref = std::get_if<reference>(&producer))
                        self(ref->stage);
            visited[id] = 2;
        };
        for (const auto& [id, _] : c.stages)
            check_order(id);
        for (const auto& [id, stage] : c.stages)
            plans.emplace(id, prepare(resolve(stage, result.defaults), builds));
        if (selected_outputs.empty())
            result.outputs = c.outputs;
        else
            for (const auto& port : selected_outputs) {
                if (!c.outputs.contains(port))
                    throw std::invalid_argument("unknown analysis profile output: " + port);
                result.outputs.emplace(port, c.outputs.at(port));
            }
        std::set<std::string> included;
        auto append = [&](this auto&& self, const std::string& id) -> void {
            if (!included.insert("stage/" + id).second)
                return;
            const auto& stage = c.stages.at(id);
            const auto& plan = plans.at(id);
            const auto& group = *plan.execution.directory_group;
            builds::combine(result.directory_groups[group], plan.execution.needs.get());
            node analysis{.id = "stage/" + id,
                          .kind = std::string{"analysis"},
                          .configuration = stage.build_configuration,
                          .inputs = stage.inputs,
                          .outputs = output_types.at(id),
                          .server = plan.server,
                          .execution = plan.execution,
                          .origins = result.origins};
            auto inherited = result.defaults.execution;
            apply(inherited, stage.execution, analysis.origins, "stage");
            auto limited = [&](const auto& ceiling, const auto& actual, const auto& chosen, const std::string& field, const std::string& configuration) {
                if (ceiling && ceiling == actual && (!chosen || *ceiling <= *chosen))
                    analysis.origins["limits." + field] = "build/" + configuration;
            };
            for (const auto& [configuration, bound] : select_builds(stage, builds)) {
                limited(bound.limits.timeout_s, plan.execution.limits.timeout_s, inherited.limits.timeout_s, "timeout_s", configuration);
                limited(bound.limits.output_limit, plan.execution.limits.output_limit, inherited.limits.output_limit, "output_limit", configuration);
                limited(bound.limits.max_memory_bytes, plan.execution.limits.max_memory_bytes, inherited.limits.max_memory_bytes, "max_memory_bytes",
                        configuration);
                limited(bound.limits.max_cpu_seconds, plan.execution.limits.max_cpu_seconds, inherited.limits.max_cpu_seconds, "max_cpu_seconds",
                        configuration);
            }
            analysis.origins.try_emplace("directory_group", "build/" + stage.build_configuration);
            analysis.origins.try_emplace("limits.timeout_s", "service");
            analysis.origins.try_emplace("limits.output_limit", "service");
            const auto& needs = plan.execution.needs.get();
            for (const auto& tool : needs.tools)
                analysis.origins.try_emplace("requires.tools." + tool,
                                             !plan.server.argv.empty() && tool == plan.server.argv.front() ? "provider" : "build/" + stage.build_configuration);
            if (needs.platform.os)
                analysis.origins.try_emplace("requires.platform.os", "build/" + stage.build_configuration);
            if (needs.platform.arch)
                analysis.origins.try_emplace("requires.platform.arch", "build/" + stage.build_configuration);

            for (const auto& before : stage.after) {
                self(before);
                analysis.dependencies.push_back("stage/" + before);
            }
            for (const auto& [port, producers] : stage.inputs)
                for (const auto& producer : producers)
                    if (const auto* ref = std::get_if<reference>(&producer)) {
                        self(ref->stage);
                        if (!std::ranges::contains(analysis.dependencies, "stage/" + ref->stage))
                            analysis.dependencies.push_back("stage/" + ref->stage);
                    }
            const auto prefix = "directory/" + group + "/build/";
            for (const auto& task : plan.build.tasks) {
                if (!included.insert(prefix + task.name).second)
                    continue;
                node build{.id = prefix + task.name,
                           .kind = std::string{"build"},
                           .configuration = task.name.substr(0, task.name.find('/')),
                           .execution = execution{.directory_group = group}};
                for (const auto& dependency : task.dependencies)
                    build.dependencies.push_back(prefix + dependency);
                for (const auto& output : task.outputs) {
                    auto [owner, inserted] = produced[group].emplace(output.path, build.configuration);
                    if (!inserted && owner->second != build.configuration)
                        throw std::invalid_argument("directory group " + group + " has multiple producers for " + output.path);
                    build.outputs.emplace(output.name.substr(task.name.size() + 1), output.type);
                }
                build.steps.assign(plan.build.steps.begin() + task.first_step, plan.build.steps.begin() + task.first_step + task.step_count);
                result.nodes.push_back(std::move(build));
            }
            for (const auto& [_, producers] : stage.inputs)
                for (const auto& producer : producers)
                    if (const auto* build = std::get_if<build_input>(&producer))
                        if (!std::ranges::contains(analysis.dependencies, prefix + build->configuration + "/" + build->task))
                            analysis.dependencies.push_back(prefix + build->configuration + "/" + build->task);
            result.nodes.push_back(std::move(analysis));
            result.stages.push_back(id);
        };
        for (const auto& [_, producers] : result.outputs)
            for (const auto& producer : producers)
                append(producer.stage);
        return result;
    }
    auto locate(program value, const std::filesystem::path& tree, const builds::toolset& tools, std::uint32_t jobs, const std::filesystem::path& cache)
        -> program {
        if (value.argv.empty()) {
            if (providers::dispatch(value.provider, []<class Implementation> { return frontend<Implementation>; }))
                throw std::invalid_argument("language server executable is required");
        } else {
            if (!builds::absolute_path(value.argv.front())) {
                builds::tools({value.argv.front()});
                value.argv.front() = builds::tool(value.argv.front());
            }
            auto bound = builds::locate({.argv = std::move(value.argv), .env = std::move(value.env)}, tree, tools, jobs);
            value.argv = std::move(bound.argv);
            value.env = std::move(bound.env);
        }
        for (auto& [operation, argv] : value.commands) {
            if (argv.empty())
                throw std::invalid_argument("language operation executable is required");
            if (!builds::absolute_path(argv.front()))
                argv.front() = builds::tool(argv.front());
            argv = builds::locate({.argv = std::move(argv)}, tree, tools, jobs).argv;
        }
        for (auto& executable : value.executables) {
            if (!builds::absolute_path(executable) && executable.find_first_of("/\\") == std::string::npos)
                executable = builds::tool(executable);
            executable = builds::locate({.argv = {executable}}, tree, tools, jobs).argv.front();
        }
        if (!value.registration)
            providers::dispatch(value.provider, [&]<class Implementation>() {
                if constexpr (requires { Implementation::configure(value, jobs, cache); })
                    Implementation::configure(value, jobs, cache);
            });
        return value;
    }
    void validate(const stage& c) {
        if (c.build_configuration.empty())
            throw std::invalid_argument("analysis build_configuration is required");
        if (json::write(c).find("\\u0000") != std::string::npos)
            throw std::invalid_argument("analysis configuration contains a null character");
        if (const auto& group = c.execution.directory_group;
            group && (group->empty() || *group == "." || *group == ".." || group->find_first_of("/\\") != std::string::npos))
            throw std::invalid_argument("invalid analysis directory_group: " + *group);
        builds::validate(c.execution.needs.get());
        const auto& limits = c.execution.limits;
        if ((limits.timeout_s && !*limits.timeout_s) || (limits.max_memory_bytes && !*limits.max_memory_bytes) ||
            (limits.max_cpu_seconds && !*limits.max_cpu_seconds))
            throw std::invalid_argument("analysis execution limits must be positive");
        c.server.visit([](const auto& w) { std::remove_cvref_t<decltype(w)>::implementation::validate(w.options); });
        c.server.visit([](const auto& w) {
            if constexpr (requires { w.options.registration; }) {
                const auto known = providers::describe();
                if (std::ranges::contains(known, w.options.registration.id, &description::id))
                    throw std::invalid_argument("external registration shadows a built-in provider");
            }
        });
    }
    void check(const plan& p, const std::vector<content::entry>& files) {
        for (auto& entry : files) {
            for (auto& dir : p.build.build_dirs)
                if (entry.path == dir || entry.path.starts_with(dir + "/"))
                    throw std::invalid_argument("analysis build directory overlaps tracked source files");
            for (auto& step : p.build.steps)
                for (auto& output : step.outputs)
                    if (entry.path == output.path || entry.path.starts_with(output.path + "/"))
                        throw std::invalid_argument("analysis output overlaps tracked source files");
        }
    }
} // namespace language
