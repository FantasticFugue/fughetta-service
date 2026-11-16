export module tools;
import utility;
import constants;
import std;
import knowledge;
import reflection;
import coroutine;
import meta;
import protocol;
import mcp;
import configuration;
import usage;
import world;
import codec;
import archive;
import registry;
import fleet;
import commands;
export import templates;
export import publication;
import native;

export import :analysis;

export namespace tools {
    struct configurations : tool<"configurations", "List build and run configurations and the operations each build configuration provides."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
        };
        struct config_view {
            std::string name;
            std::vector<std::string> adapters;
            std::vector<std::string> operations;
            bool enabled = true;
            bool is_default = false;
        };
        struct run_view {
            std::string name;
            std::string operation;
            std::vector<std::string> configurations;
        };
        struct output {
            std::vector<config_view> configurations;
            std::vector<run_view> run_configurations;
        };
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto e = config::resolve(*s.snapshot(), project_of(in));
            output out;
            for (auto& [name, c] : e.configs) {
                config_view v{.name = name, .enabled = c.enabled, .is_default = std::ranges::contains(e.default_configurations, name)};
                for (auto& [_, b] : c.build.builders)
                    if (auto adapter = builds::adapter_of(b); !std::ranges::contains(v.adapters, adapter))
                        v.adapters.emplace_back(adapter);
                for (auto& [op, _] : c.build.operations)
                    v.operations.push_back(op);
                out.configurations.push_back(std::move(v));
            }
            for (auto& [name, r] : e.runs)
                out.run_configurations.push_back({name, r.spec.operation, r.configurations});
            co_return out;
        }
    };

    struct build : tool<"build", "Build a fixed server-side worldline version with one or more configurations: send its files "
                                 "to an eligible worker (matching each configuration's requirements), "
                                 "execute its build steps, and return each configuration's "
                                 "steps, output and diagnostics in one result."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"Configurations to build at once; the defaults if absent; [\"*\"] for all enabled", std::optional<std::vector<std::string>>>
                configurations;
            rfl::Description<"Targets accepted by the selected build operation; configured targets if absent", std::optional<std::vector<std::string>>> targets;
            rfl::Description<"Worldline to build; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
            rfl::Description<"Rebuild from a clean working tree, bypassing cached and in-flight build results", std::optional<bool>> fresh;
        };
        using output = results;

        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            auto targets = in.targets.value().value_or(std::vector<std::string>{});
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(in.configurations.value().value_or(std::vector<std::string>{}), "build"))
                jobs.push_back(detail::operation(*c, {.operation = "build", .parameters = {.targets = targets}}));
            co_return co_await execute_all(s, context, project, "build", true, in.fresh.value().value_or(false), std::move(jobs), note,
                                           execution_line(actor, in.worldline.value()), in.version.value().value_or(""), actor);
        }
    };

    struct test : tool<"test", "Execute the test operation and its configured dependencies on one or more configurations; "
                               "returns steps, diagnostics and output, plus a test summary when the test runner is one the "
                               "service reads (CTest, Bazel, pytest)."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"Configurations to test on at once; the defaults if absent; [\"*\"] for all enabled", std::optional<std::vector<std::string>>>
                configurations;
            rfl::Description<"Test filter, if the selected operation accepts one", std::optional<std::string>> filter;
            rfl::Description<"Test labels, if the selected operation accepts them", std::optional<std::string>> labels;
            rfl::Description<"Worldline to build; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
        };
        using output = results;

        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(in.configurations.value().value_or(std::vector<std::string>{}), "test")) {
                jobs.push_back(detail::operation(*c, {.operation = "test", .parameters = {.filter = in.filter.value(), .labels = in.labels.value()}}));
            }
            co_return co_await execute_all(s, context, project, "test", false, false, std::move(jobs), note, execution_line(actor, in.worldline.value()),
                                           in.version.value().value_or(""), actor);
        }
    };

    struct launch : tool<"run", "Invoke a saved run configuration's operation on its selected build configurations."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"The run configuration", std::string> configuration;
            rfl::Description<"Build configurations to run on; the run configuration's own if absent", std::optional<std::vector<std::string>>> configurations;
            rfl::Description<"Worldline to build; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
        };
        using output = results;

        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            auto& r = e.run(in.configuration.value());
            auto names = in.configurations.value().value_or(r.configurations);
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(names, r.spec.operation)) {
                auto build = *c;
                for (auto& [key, value] : r.spec.env)
                    build.build.env[key] = value;
                config::detail::apply(build.limits, r.spec.limits);
                jobs.push_back(detail::operation(std::move(build), {.operation = r.spec.operation, .parameters = r.spec.parameters}));
            }
            co_return co_await execute_all(s, context, project, r.spec.operation, false, false, std::move(jobs), note,
                                           execution_line(actor, in.worldline.value()), in.version.value().value_or(""), actor);
        }
    };

    struct generation_result {
        templates::plan preview;
        std::optional<mutation> receipt;
    };
    // Generated files are created in main in one observed batch; existing files are never overwritten.
    auto generate_files(services& s, const std::string& project, templates::parameters parameters, const worlds::actor& actor, std::string operation_id,
                        bool preview) -> coroutine::task<generation_result> {
        generation_result out{templates::render(parameters)};
        if (preview)
            co_return out;
        if (operation_id.empty())
            throw tool_error(nodes::code::invalid_record, "generation names an operation_id");
        std::string input = content::hash_of(rfl::json::write(parameters)).str();
        auto apply_once = [&]() -> coroutine::task<mutation> {
            auto observed = co_await main_tree(s, project, actor);
            std::vector<change> changes;
            for (const auto& file : out.preview.files) {
                if (std::ranges::contains(observed.files, file.path, &nodes::file_view::path))
                    throw tool_error(nodes::code::source_conflict, file.path + " already exists");
                changes.push_back({.kind = std::string{"create"}, .path = file.path, .text = file.text});
            }
            auto r = co_await main_apply(s, project, actor, observed, std::move(changes), operation_id, "generate " + parameters.template_id,
                                         rfl::json::write(record{input}));
            co_return mutation{r.version, r.receipt, operation_id, r.replayed};
        };
        out.receipt = co_await once<mutation>(s, project, actor, operation_id, input, std::move(apply_once),
                                              [&](const auto& done) { return mutation{done.first.version, done.first.receipt, operation_id, true}; });
        co_return out;
    }
    // Console generation publishes one observed batch. There is purposely no MCP
    // `generate` tool: agents write files with the file tools, and the console's
    // generation flow goes through this REST helper only.
    auto generate_globally(services& s, const std::string& project, templates::parameters parameters, const worlds::actor& actor, std::string operation_id)
        -> coroutine::task<generation_result> {
        co_return co_await generate_files(s, project, std::move(parameters), actor, std::move(operation_id), false);
    }

    struct scope : tool<"scope", "For orchestrators that divide work before dispatching it: acquire an atomic set of exclusive scope leases using "
                                 "references returned by language(scopes). Pass tokens to worldline(apply); renew or release each token. "
                                 "Conflicts return immediately. A single agent never needs this: apply locks implicitly."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"acquire, renew, release or list", std::string> action;
            rfl::Description<"Task worldline; explicit selection required without execution context", std::optional<std::string>> worldline;
            rfl::Description<"acquire: scope references returned by language", std::optional<std::vector<std::string>>> scopes;
            rfl::Description<"acquire: whole files, including empty and binary files", std::optional<std::vector<std::string>>> files;
            rfl::Description<"acquire: path entries for create/remove/move", std::optional<std::vector<std::string>>> entries;
            rfl::Description<"acquire with files/entries: observed version", std::optional<std::string>> version;
            rfl::Description<"acquire: unique operation ID", std::optional<std::string>> operation_id;
            rfl::Description<"renew/release: lease token", std::optional<std::string>> token;
            rfl::Description<"Lease duration, 1 through 600 seconds; default 120", std::optional<std::uint64_t>> seconds;
        };
        struct output {
            std::string worldline, version;
            std::vector<nodes::scope_lease> leases;
        };
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in), action = in.action.value();
            if (action != "acquire" && action != "renew" && action != "release" && action != "list")
                throw tool_error(nodes::code::invalid_record, "unknown scope action");
            auto line = execution_line(actor, in.worldline.value());
            nodes::node_request request{.action = "scope_" + action, .worldline = line};
            request.lease_token = in.token.value().value_or("");
            request.lease_seconds = in.seconds.value().value_or(constants::scope_lease_seconds);
            if (action == "acquire") {
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "acquire requires operation_id");
                request.scope_refs = in.scopes.value().value_or(std::vector<std::string>{});
                request.scope_files = in.files.value().value_or(std::vector<std::string>{});
                request.scope_entries = in.entries.value().value_or(std::vector<std::string>{});
                request.revision = in.version.value().value_or("");
                request.operation = lines::operation_of(*in.operation_id.value());
            }
            auto result = co_await node_call(s, project, std::move(request), actor, action == "list");
            co_return output{line, result.version, std::move(result.leases)};
        }
    };

    struct artifact : tool<"artifact", "Retrieve a build/test/run export or execution log by its project and digest. "
                                       "Use the export metadata returned by execution tools. Read bounded base64 chunks, "
                                       "decode them in offset order into a local file, and preserve its executable flag. "
                                       "Continue at next_offset until eof; no source synchronization is required."> {
        struct input {
            rfl::Description<"Project that produced the artifact; supplied by execution context when absent", std::optional<std::string>> project;
            rfl::Description<"Digest returned in exports or a step log", std::string> digest;
            rfl::Description<"Byte offset; defaults to zero", std::optional<std::uint64_t>> offset;
            rfl::Description<"Bytes to retrieve, 1 through 65536; defaults to 65536", std::optional<std::uint64_t>> length;
        };
        struct output {
            std::string digest;
            std::uint64_t offset, total, next_offset;
            bool eof;
            std::string data_base64;
        };
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor&) -> coroutine::task<output> {
            auto project = project_of(in);
            auto length = in.length.value().value_or(constants::mcp_artifact_bytes);
            if (!length || length > constants::mcp_artifact_bytes)
                throw tool_error(nodes::code::invalid_record, "artifact length must be 1 through 65536 bytes");
            auto bytes = co_await s.files.read_export(s.core, project, {in.digest.value(), in.offset.value().value_or(0), length});
            const auto& part = bytes.parts.at(0);
            const auto next = part.offset + part.bytes.size();
            co_return output{part.digest.str(),  part.offset,
                             part.total,         next,
                             next == part.total, lines::base64({reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()})};
        }
    };

    struct task : tool<"task", "Create a task on an explicit worldline, fork a task worldline, start an execution, stop a run or close a task. Only the "
                               "orchestrator's account credential can administer tasks."> {
        struct input {
            rfl::Description<"Project", std::string> project;
            rfl::Description<"create, fork, start, stop or close", std::string> action;
            rfl::Description<"Task identifier", std::string> task;
            rfl::Description<"create/fork: task worldline", std::optional<std::string>> worldline;
            rfl::Description<"fork: source worldline", std::optional<std::string>> from;
            rfl::Description<"fork: source version", std::optional<std::string>> version;
            rfl::Description<"create/fork/start: unique operation ID", std::optional<std::string>> operation_id;
            rfl::Description<"stop: execution identifier", std::optional<std::string>> run;
            rfl::Description<"start: permit source writes; default true", std::optional<bool>> writable;
        };
        struct output {
            std::string task, worldline;
            std::optional<std::string> run, credential;
            bool active = true;
        };
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            if (!actor.run.empty())
                throw tool_error(nodes::code::permission_denied, "execution credentials cannot administer tasks");
            auto project = project_of(in), action = in.action.value();
            config::check_name("task", in.task.value());
            if (action != "create" && action != "fork" && action != "start" && action != "stop" && action != "close")
                throw tool_error(nodes::code::invalid_record, "unknown task action");
            nodes::node_request request{.action = "task_" + action};
            request.task_id = in.task.value();
            request.run_id = in.run.value().value_or("");
            request.execution_writable = in.writable.value().value_or(true);
            if (action == "create" || action == "fork" || action == "start") {
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "operation_id is required");
                request.operation = lines::operation_of(*in.operation_id.value());
            }
            if (action == "create" || action == "fork") {
                if (!in.worldline.value() || in.worldline.value()->empty())
                    throw tool_error("context_required", "pass a task worldline");
                request.worldline = *in.worldline.value();
            }
            if (action == "fork") {
                if (!in.from.value() || !in.version.value())
                    throw tool_error(nodes::code::invalid_record, "fork requires from and version");
                auto base = co_await selected(s, project, *in.from.value(), *in.version.value(), actor);
                request.action = "fork";
                request.source = base.frontier;
                request.result = rfl::json::write(std::tuple{in.task.value(), *in.from.value(), *in.version.value()});
            }
            auto result = co_await node_call(s, project, std::move(request), actor);
            if (!result.execution)
                co_return output{in.task.value(), result.worldline};
            const auto& e = *result.execution;
            co_return output{e.task, e.worldline, e.run.empty() ? std::nullopt : std::optional{e.run},
                             e.token.empty() ? std::nullopt : std::optional{"fugrun." + project + "." + e.token}, e.active};
        }
    };

    using standard = toolbox<task, configurations, worldline, scope, build, test, launch, language_query, artifact>;
} // namespace tools
