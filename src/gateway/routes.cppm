module front;
import std;
import native;
import constants;
import reflection;
import coroutine;
import meta;
import http;
import mcp;
import configuration;
import usage;
import registry;
import fleet;
import commands;
import tools;
import protocol;
import world;
import client;
import nodes;
import codec;
import enrollment;

// The REST endpoints, each with the access it needs, and the table that routes a request to one.
namespace front {
    // The role an endpoint needs; each role may do what the ones before it may.
    enum class access { open, member, owner, host };

    template <meta::fixed_string Method, meta::fixed_string Pattern, access Need = access::owner> struct endpoint {
        static constexpr std::string_view method = Method;
        static constexpr access need = Need;
        using path = web::path<Pattern>;
        using params = typename path::values;
    };

    template <class T> auto ok(const T& value, http::status status = 200) -> web::reply { return web::json_reply(status, rfl::json::write(value)); }

    struct message {
        std::string message;
    };

    namespace api {
        template <class T> struct section_save {
            std::optional<std::uint64_t> revision;
            T value;
        };

        template <class T> auto read_save(std::string_view body, std::string_view what) -> std::pair<std::uint64_t, T> {
            auto s = config::parse_as<section_save<T>>(body, what);
            if (!s.revision)
                throw coded_error(400, "missing_revision", std::format("{}: 'revision' is required; read the section first", what));
            return {*s.revision, std::move(s.value)};
        }
    } // namespace api

    struct me {
        std::string issuer, user, caller, role;
        std::optional<std::string> email;
    };

    // `bytes` of a project is the size of its main source, as max_bytes (the project limit) counts it; `bytes` of the workspace is what all
    // its projects store, history included, as its storage limit counts it.
    struct project_usage {
        std::string name;
        std::uint64_t bytes = 0;
        std::uint64_t max_bytes = 0;
        std::uint32_t builds_last_hour = 0;
        std::uint32_t builds_per_hour = 0;
    };

    struct workspace_usage {
        std::uint64_t bytes = 0;
        std::uint64_t max_bytes = 0;
        std::uint32_t projects = 0;
        std::uint32_t max_projects = 0;
        std::uint32_t running_builds = 0;
        std::uint32_t concurrent_builds = 0;
        double build_seconds_today = 0;
        std::uint32_t build_seconds_per_day = 0;
        double build_seconds_month = 0;
        std::uint32_t build_seconds_per_month = 0;
        std::vector<project_usage> per_project;
    };

    struct health {
        std::string status = "ok";
        // Whether the source answered a read-only probe taken for this request; absent when the gateway cannot probe.
        std::optional<bool> source;
        std::size_t workers = 0;
        std::size_t online_workers = 0;
        std::uint32_t protocol = 0;
    };

    namespace api {

        struct get_config : endpoint<"GET", "/api/v1/config", access::member> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto doc = c.config.current(c.core);
                if (worlds::rank(c.who->role) >= worlds::rank("owner"))
                    co_return ok(*doc);
                // Credentials are for those who run workers and hand out tokens: the owners.
                auto mine = *doc;
                mine.server.auth.tokens.clear();
                mine.worker_token.reset();
                co_return ok(mine);
            }
        };

        struct get_server : endpoint<"GET", "/api/v1/config/server"> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto doc = c.config.current(c.core);
                co_return ok(section_view{doc->revisions.server, doc->server});
            }
        };

        struct put_server : endpoint<"PUT", "/api/v1/config/server"> {
            static auto handle(context& c, params, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto [revision, s] = read_save<config::server>(body, "server");
                // Who may call is not the callers' to change: the authentication settings are set where the core is run.
                if (!(s.auth == c.config.current(c.core)->server.auth))
                    throw coded_error(403, nodes::code::permission_denied, "server.auth is set in the configuration file or with `fughetta-service token`");
                co_await c.config.edit(c.core, [revision, s](config::store& st) { st.put_server(s, revision); });
                co_return ok(message{"saved; takes effect when the gateway restarts"});
            }
        };

        struct get_limits : endpoint<"GET", "/api/v1/config/limits", access::member> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto doc = c.config.current(c.core);
                co_return ok(section_view{doc->revisions.limits, doc->limits});
            }
        };

        struct put_limits : endpoint<"PUT", "/api/v1/config/limits"> {
            static auto handle(context& c, params, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto [revision, l] = read_save<config::limits>(body, "limits");
                co_await c.config.edit(c.core, [revision, l](config::store& st) { st.put_limits(l, revision); });
                auto doc = c.config.current(c.core);
                co_return ok(section_view{doc->revisions.limits, doc->limits});
            }
        };

        struct get_granted : endpoint<"GET", "/api/v1/config/granted", access::member> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto doc = c.config.current(c.core);
                co_return ok(section_view{doc->revisions.granted, doc->granted});
            }
        };

        struct put_granted : endpoint<"PUT", "/api/v1/config/granted", access::host> {
            static auto handle(context& c, params, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto [revision, g] = read_save<std::optional<config::limits>>(body, "granted");
                co_await c.config.edit(c.core, [revision, g](config::store& st) { st.put_granted(g, revision); });
                auto doc = c.config.current(c.core);
                co_return ok(section_view{doc->revisions.granted, doc->granted});
            }
        };

        struct get_runtime : endpoint<"GET", "/api/v1/config/runtime", access::host> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                if (!c.runtime)
                    throw coded_error(503, "runtime_unmanaged", "runtime control requires the managed service entry");
                auto doc = c.config.current(c.core);
                co_return ok(c.runtime->observe(doc->revisions.runtime, doc->runtime));
            }
        };
        struct put_runtime : endpoint<"PUT", "/api/v1/config/runtime", access::host> {
            static auto handle(context& c, params, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                if (!c.runtime)
                    throw coded_error(503, "runtime_unmanaged", "runtime control requires the managed service entry");
                auto [revision, value] = read_save<allocation::budget>(body, "runtime");
                co_await c.config.edit(c.core, [revision, value](config::store& st) { st.put_runtime(value, revision); });
                auto doc = c.config.current(c.core);
                auto observed = c.runtime->observe(doc->revisions.runtime, doc->runtime);
                co_return web::json_reply(observed.phase == "ready" ? http::status{200} : http::status{202}, rfl::json::write(observed));
            }
        };

        struct list_projects : endpoint<"GET", "/api/v1/projects", access::member> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                std::vector<std::string> out;
                for (auto& [name, _] : c.config.current(c.core)->projects)
                    out.push_back(name);
                co_return ok(out);
            }
        };

        struct get_project : endpoint<"GET", "/api/v1/projects/{project}", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto doc = c.config.current(c.core);
                auto* s = doc->find(p[0]);
                if (!s)
                    throw config::not_found(std::format("unknown project '{}'", p[0]));
                co_return ok(section_view{config::revision_of(*doc, p[0]), *s});
            }
        };

        struct put_project : endpoint<"PUT", "/api/v1/projects/{project}", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto [revision, s] = read_save<config::settings>(body, "settings");
                co_await c.config.edit(c.core, [project = p[0], revision, s](config::store& st) { st.put_project(project, s, revision); });
                co_return ok(section_view{config::revision_of(*c.config.current(c.core), p[0]), std::move(s)});
            }
        };

        struct delete_project : endpoint<"DELETE", "/api/v1/projects/{project}", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                co_await c.config.edit(c.core, [project = p[0]](config::store& st) { st.remove_project(project); });
                co_return ok(message{std::format("project '{}' removed from the configuration; its files are kept", p[0])});
            }
        };

        struct source_head {
            std::string head;
            std::vector<content::entry> files;
            bool truncated = false;
        };
        struct get_source : endpoint<"GET", "/api/v1/projects/{project}/source", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                // The files of the project's main worldline; `head` is its current version.
                auto& services = *c.services;
                auto tree = co_await tools::node_call(services, p[0], {.action = "tree", .worldline = "main"}, *c.who, true);
                auto most = config::resolve(*c.config.current(c.core), p[0]).limits.list_max_entries;
                source_head out{.head = tree.version};
                for (auto& f : tree.files)
                    out.files.push_back({f.path, f.digest, f.size, f.executable});
                std::ranges::sort(out.files, {}, &content::entry::path);
                out.truncated = out.files.size() > most;
                if (out.truncated)
                    out.files.resize(most);
                co_return ok(out);
            }
        };

        // The HTTP status of a worldline error code, for the node route and for console routes that act on worldlines.
        inline auto node_status(std::string_view code) -> http::status {
            static const std::set<std::string_view, std::less<>> conflicts{
                "wrong_project",       "stale_frontier",  nodes::code::source_conflict, nodes::code::identity_conflict,
                "dependency_conflict", "policy_conflict", "worldline_boundary"};
            return conflicts.contains(code)                                                       ? http::status{409}
                   : code == nodes::code::policy_denied || code == nodes::code::permission_denied ? http::status{403}
                   : code == nodes::code::missing_objects || code == nodes::code::outcome_unknown ? http::status{503}
                   : code == nodes::code::storage_failure                                         ? http::status{500}
                                                                                                  : http::status{400};
        }
        inline auto node_result(nodes::node_reply reply) -> nodes::node_reply {
            if (reply.code != nodes::code::ok) {
                coded_error error{node_status(reply.code), reply.code, reply.reason};
                error.missing = std::move(reply.missing);
                error.conflicts = std::move(reply.conflicts);
                error.frontier = std::move(reply.frontier);
                throw error;
            }
            return reply;
        }
        // Local clients use this route to synchronize files and select explicit worldlines.
        struct post_worldline : endpoint<"POST", "/api/v1/projects/{project}/worldline", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto in = rfl::json::read<tools::worldline::input, rfl::NoExtraFields>(body);
                if (!in || in->project.value() != p[0])
                    throw coded_error(400, nodes::code::invalid_record, "worldline request must name the project in the URL");
                mcp::notifier note;
                try {
                    co_return ok(co_await tools::worldline::run(*c.services, std::move(*in), note, *c.who));
                } catch (const tools::tool_error& error) {
                    throw coded_error(node_status(error.code), error.code, error.what());
                }
            }
        };
        struct mail_page {
            std::vector<nodes::mail_message> messages;
            std::uint64_t next = 0;
            bool more = false, replayed = false;
        };
        struct get_export : endpoint<"GET", "/api/v1/projects/{project}/exports/{digest}", access::member> {
            static auto handle(context& c, params p, const web::target& target, std::string_view) -> coroutine::task<web::reply> {
                auto bytes = co_await c.files.read_export(c.core, p[0], {p[1], target.number("offset", 0, constants::export_bytes),
                                                                         target.number("length", jobs::part_size, jobs::part_size)});
                const auto& part = bytes.parts.at(0);
                struct chunk { std::string digest; std::uint64_t offset, total; std::string data_base64; };
                co_return ok(chunk{part.digest.str(), part.offset, part.total,
                    tools::lines::base64({reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()})});
            }
        };
        struct post_mail : endpoint<"POST", "/api/v1/projects/{project}/d-mail", access::member> {
            struct input {
                std::string worldline = "main", operation_id, recipient, body;
            };
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto in = rfl::json::read<input, rfl::DefaultIfMissing, rfl::NoExtraFields>(body);
                if (!in || in->operation_id.empty() || in->operation_id.size() > 256)
                    throw coded_error(400, nodes::code::invalid_record, "d-mail requires an operation_id of 1 through 256 characters");
                auto& services = *c.services;
                auto result = co_await tools::node_call(services, p[0],
                                                        {.action = "mail_send",
                                                         .mail = nodes::mail_input{in->recipient, in->body},
                                                         .worldline = in->worldline,
                                                         .operation = tools::lines::operation_of(in->operation_id),
                                                         .caller_operation = in->operation_id},
                                                        *c.who);
                co_return ok(mail_page{std::move(result.messages), 0, false, result.replayed});
            }
        };
        struct get_mail : endpoint<"GET", "/api/v1/projects/{project}/d-mail", access::member> {
            static auto handle(context& c, params p, const web::target& target, std::string_view) -> coroutine::task<web::reply> {
                auto& services = *c.services;
                auto after = target.number("after", 0, std::numeric_limits<std::uint64_t>::max());
                auto result = co_await tools::node_call(services, p[0],
                                                        {.action = "mail_list",
                                                         .after = after,
                                                         .target = std::string{target.text("recipient").value_or("main")},
                                                         .limit = target.number("limit", 20, 100)},
                                                        *c.who, true);
                auto next = result.messages.empty() ? after : result.messages.back().id;
                co_return ok(mail_page{std::move(result.messages), next, result.truncated});
            }
        };
        struct post_node : endpoint<"POST", "/api/v1/projects/{project}/node", access::owner> {
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto in = rfl::json::read<nodes::node_request, rfl::DefaultIfMissing, rfl::NoExtraFields>(body);
                if (!in)
                    throw coded_error(400, nodes::code::invalid_record, in.error().what());
                in->caller_operation.clear();
                auto reply = co_await c.files.node(c.core, p[0], *in, *c.who, in->action == "receipt");
                co_return ok(node_result(std::move(reply)));
            }
        };
        struct post_node_capture : endpoint<"POST", "/api/v1/projects/{project}/node/capture", access::owner> {
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto in = rfl::json::read<node_capture, rfl::DefaultIfMissing, rfl::NoExtraFields>(body);
                if (!in)
                    throw coded_error(400, nodes::code::invalid_record, in.error().what());
                auto reply = co_await c.files.capture(c.core, p[0], in->operation, in->expected, in->intent, *c.who);
                co_return ok(node_result(std::move(reply)));
            }
        };
        inline auto selected_node(const web::target& target, bool file = false) -> nodes::node_request {
            return {.action = file ? "read" : "tree",
                    .worldline = std::string{target.text("worldline").value_or("main")},
                    .revision = std::string{target.text("revision").value_or("")},
                    .snapshot = std::string{target.text("snapshot").value_or("")},
                    .target = file ? std::string{target.text("path").value_or("")} : std::string{}};
        }
        struct get_node_source : endpoint<"GET", "/api/v1/projects/{project}/node/source", access::member> {
            static auto handle(context& c, params p, const web::target& target, std::string_view) -> coroutine::task<web::reply> {
                auto settings = config::resolve(*c.config.current(c.core), p[0]);
                auto state = node_result(co_await c.files.node(c.core, p[0], selected_node(target), *c.who, true));
                auto reply = [&] {
                    node_source_head out{state.source_digest,
                                         state.version,
                                         std::string{target.text("snapshot").value_or("")},
                                         std::string{target.text("worldline").value_or("main")},
                                         {},
                                         false,
                                         state.conflicts,
                                         state.frontier,
                                         state.worldlines,
                                         state.node};
                    for (auto& file : state.files)
                        out.files.push_back({file.path, file.digest, file.size, file.executable});
                    std::ranges::sort(out.files, {}, &content::entry::path);
                    if (out.files.size() > settings.limits.list_max_entries) {
                        out.files.resize(settings.limits.list_max_entries);
                        out.truncated = true;
                    }
                    return out;
                }();
                co_return ok(reply);
            }
        };
        struct get_node_file : endpoint<"GET", "/api/v1/projects/{project}/node/source/file", access::member> {
            static auto handle(context& c, params p, const web::target& target, std::string_view) -> coroutine::task<web::reply> {
                if (!target.text("revision") && !target.text("snapshot"))
                    throw coded_error(400, "missing_revision", "select a revision or snapshot before reading a file");
                auto state = node_result(co_await c.files.node(c.core, p[0], selected_node(target, true), *c.who, true));
                auto reply = [&] {
                    auto path = target.text("path").value_or("");
                    auto file = std::ranges::find(state.files, path, &nodes::file_view::path);
                    if (file == state.files.end())
                        throw coded_error(404, "file_not_found", "file is absent from this version");
                    node_source_file out{.revision = state.version,
                                         .snapshot = std::string{target.text("snapshot").value_or("")},
                                         .worldline = std::string{target.text("worldline").value_or("main")},
                                         .path = std::string{path}};
                    out.first_line = target.number("offset", 1, 10000000);
                    if (!out.first_line)
                        throw coded_error(400, "invalid_offset", "line offset starts at one");
                    out.binary = file->kind == "binary" || file->text.find('\0') != std::string::npos;
                    if (out.binary)
                        return out;
                    auto limit = target.number("limit", 200, 1000);
                    if (!limit)
                        throw coded_error(400, "invalid_limit", "line limit must be positive");
                    std::size_t pos = 0;
                    while (pos < file->text.size()) {
                        auto end = file->text.find('\n', pos);
                        auto next = end == std::string::npos ? file->text.size() : end + 1;
                        ++out.total_lines;
                        if (out.total_lines >= out.first_line && out.lines < limit) {
                            if (!out.truncated) {
                                auto available = 1024 * 1024 - out.content.size();
                                auto end = pos + std::min<std::size_t>(available, next - pos);
                                if (end < next) {
                                    while (end > pos && (static_cast<unsigned char>(file->text[end]) & 0xc0) == 0x80)
                                        --end;
                                    out.truncated = true;
                                }
                                out.content.append(file->text, pos, end - pos);
                                ++out.lines;
                            }
                        }
                        pos = next;
                    }
                    out.truncated = out.truncated || out.total_lines >= out.first_line + out.lines;
                    return out;
                }();
                co_return ok(reply);
            }
        };

        // Only the templates that render files into an existing project are offered; projects are made with PUT /api/v1/projects/{project}.
        struct get_templates : endpoint<"GET", "/api/v1/templates", access::member> {
            static auto handle(context&, params, const web::target&, std::string_view) -> coroutine::task<web::reply> { co_return ok(templates::catalog()); }
        };
        struct generation_input {
            templates::parameters parameters;
            std::string operation_id;
            bool preview = true;
        };
        struct post_generate : endpoint<"POST", "/api/v1/projects/{project}/generate", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto in = config::parse_as<generation_input>(body, "generation");
                auto plan = templates::render(in.parameters);
                std::string project{p[0]};
                config::check_name("project", project);
                if (!c.config.current(c.core)->find(project))
                    throw config::not_found(std::format("unknown project '{}'", project));
                if (in.preview)
                    co_return ok(tools::generation_result{std::move(plan)});
                if (in.operation_id.empty())
                    throw std::invalid_argument("Generation requires operation_id.");
                co_return ok(co_await tools::generate_globally(*c.services, project, in.parameters, *c.who, in.operation_id));
            }
        };

        struct get_effective : endpoint<"GET", "/api/v1/projects/{project}/effective", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                co_return ok(config::resolve(*c.config.current(c.core), p[0]));
            }
        };

        // Settles an operation whose reply was lost: it ran, or it never will. Clients call it, not agents, so it stays out of the tools.
        struct abandon_operation : endpoint<"POST", "/api/v1/projects/{project}/operations/{operation}/abandon", access::member> {
            struct settled {
                std::string outcome;
                std::optional<std::string> version;
                std::optional<std::string> receipt;
            };
            static auto handle(context& c, params p, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                if (p[1].empty() || p[1].size() > 256)
                    throw coded_error(400, nodes::code::invalid_record, "an operation_id of 1 through 256 characters is required");
                auto r = node_result(co_await c.files.node(
                    c.core, p[0], {.action = "abandon", .operation = tools::lines::operation_of(p[1]), .caller_operation = std::string{p[1]}}, *c.who, false));
                if (r.receipt.empty())
                    co_return ok(settled{.outcome = "abandoned"});
                co_return ok(settled{.outcome = "applied", .version = r.version, .receipt = r.receipt});
            }
        };

        struct get_build_adapters : endpoint<"GET", "/api/v1/build-adapters", access::member> {
            static auto handle(context&, params, const web::target&, std::string_view) -> coroutine::task<web::reply> { co_return ok(builds::schemas()); }
        };

        struct get_settings_schema : endpoint<"GET", "/api/v1/settings-schema", access::member> {
            static auto handle(context&, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                static const auto schema = config::schema<config::settings>();
                co_return ok(schema);
            }
        };

        struct plan_build : endpoint<"POST", "/api/v1/projects/{project}/build-configurations/{name}/plan", access::member> {
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto settings = config::resolve(*c.config.current(c.core), p[0]);
                auto request = config::parse_as<builds::request>(body, "build plan");
                co_return ok(builds::prepare(settings.config(p[1]).build, request));
            }
        };

        // What the workspace stores and spends.
        auto usage_of(context& c, worlds::actor actor) -> coroutine::task<workspace_usage> {
            auto stored = co_await c.files.usage(c.core, std::move(actor));
            co_return co_await c.files.with_ledger(c.core, [&stored](usage::ledger& ledger, const config::document& d) {
                auto limits = config::resolve_limits(d);
                workspace_usage u{.bytes = jobs::stored(stored),
                                  .max_bytes = limits.max_workspace_bytes,
                                  .max_projects = limits.max_projects,
                                  .running_builds = ledger.running(),
                                  .concurrent_builds = limits.concurrent_builds,
                                  .build_seconds_today = ledger.build_seconds_today(),
                                  .build_seconds_per_day = limits.build_seconds_per_day,
                                  .build_seconds_month = ledger.build_seconds_this_month(),
                                  .build_seconds_per_month = limits.build_seconds_per_month};
                for (auto& [name, _] : d.projects) {
                    auto found = std::ranges::find(stored.projects, name, &jobs::project_usage::project);
                    ++u.projects;
                    u.per_project.push_back({name, found == stored.projects.end() ? 0 : found->source_bytes, limits.max_project_bytes,
                                             ledger.builds_last_hour(name), limits.builds_per_hour});
                }
                return u;
            });
        }
        struct get_usage : endpoint<"GET", "/api/v1/usage", access::member> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                co_return ok(co_await usage_of(c, *c.who));
            }
        };

        inline auto project_filter(const web::target& t) -> std::optional<std::string> {
            return t.text("project").transform([](std::string_view v) { return std::string{v}; });
        }

        // The ledger's events after `after`, for whoever bills or audits the workspace.
        struct get_events : endpoint<"GET", "/api/v1/usage/events", access::owner> {
            static auto handle(context& c, params, const web::target& t, std::string_view) -> coroutine::task<web::reply> {
                auto after = t.number("after", 0, std::numeric_limits<std::uint64_t>::max());
                auto limit = t.number("limit", 1000, 10000);
                co_return ok(co_await c.files.with_ledger(
                    c.core, [after, limit](usage::ledger& ledger, const config::document&) { return ledger.events(after, limit); }));
            }
        };

        struct get_stats : endpoint<"GET", "/api/v1/stats", access::member> {
            static auto handle(context& c, params, const web::target& t, std::string_view) -> coroutine::task<web::reply> {
                auto days = static_cast<std::uint32_t>(t.number("days", 7, 32));
                co_return ok(co_await c.files.with_ledger(
                    c.core, [days, project = project_filter(t)](usage::ledger& ledger, const config::document&) { return ledger.days(days, project); }));
            }
        };

        struct get_builds : endpoint<"GET", "/api/v1/builds", access::member> {
            static auto handle(context& c, params, const web::target& t, std::string_view) -> coroutine::task<web::reply> {
                auto limit = t.number("limit", 50, 1000);
                co_return ok(co_await c.files.with_ledger(
                    c.core, [limit, project = project_filter(t)](usage::ledger& ledger, const config::document&) { return ledger.builds(project, limit); }));
            }
        };

        struct get_me : endpoint<"GET", "/api/v1/me", access::member> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                me out{c.who->issuer, c.who->user, c.who->caller, c.who->role};
                if (!c.who->email.empty())
                    out.email = c.who->email;
                co_return ok(out);
            }
        };

        auto all_workers(context& c) -> coroutine::task<std::vector<routing::worker_view>> { co_return co_await c.workers.describe(c.core); }

        inline auto identities(context& c, bool registering = false) -> enrollment::store& {
            auto doc = c.config.current(c.core);
            if (!c.identities || !c.pool || (registering && (!doc->server.remote_worker_enrollment || !c.remote_listener)))
                throw coded_error(503, "worker_enrollment_disabled", "Remote Worker enrollment is disabled");
            return *c.identities;
        }

        // An owner registers a machine; its client keeps the credential and connects the Worker through the tunnel.
        struct register_worker : endpoint<"POST", "/api/v1/workers"> {
            struct request {
                std::string name;
            };
            static auto handle(context& c, params, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto& store = identities(c, true);
                std::optional<request> input;
                try {
                    input = config::parse_as<request>(body, "Worker registration");
                } catch (const std::exception&) {
                    throw coded_error(400, "invalid_worker_name", "Worker registration request is invalid");
                }
                auto actor = worlds::identity_key(*c.who);
                auto result = co_await io::blocking(c.rt.get_scheduler(c.core), *c.pool,
                                                    [&store, actor, name = std::move(input->name)] { return store.add(name, actor); });
                logging::info(std::format("worker.identity registered workspace_id={} worker_id={}", store.workspace_id(), result.worker_id));
                auto out = ok(result, 201);
                out.head.fields.push_back({"cache-control", "no-store"});
                co_return out;
            }
        };

        struct delete_worker : endpoint<"DELETE", "/api/v1/workers/{worker_id}"> {
            static auto handle(context& c, params p, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto& store = identities(c);
                auto id = p[0];
                co_await io::blocking(c.rt.get_scheduler(c.core), *c.pool, [&store, id] { store.revoke(id); });
                logging::info(std::format("worker.identity revoked workspace_id={} worker_id={}", store.workspace_id(), id));
                co_await c.workers.update_identity(c.core, id);
                co_return ok(message{"Worker identity revoked"});
            }
        };

        struct rename_worker : endpoint<"POST", "/api/v1/workers/{worker_id}/rename"> {
            struct request {
                std::string name;
            };
            static auto handle(context& c, params p, const web::target&, std::string_view body) -> coroutine::task<web::reply> {
                auto& store = identities(c);
                std::optional<request> input;
                try {
                    input = config::parse_as<request>(body, "Worker name");
                } catch (const std::exception&) {
                    throw coded_error(400, "invalid_worker_name", "Worker name request is invalid");
                }
                auto id = p[0], name = input->name;
                co_await io::blocking(c.rt.get_scheduler(c.core), *c.pool, [&store, id, name] { store.rename(id, name); });
                co_await c.workers.update_identity(c.core, id, name);
                co_return ok(message{"Worker identity renamed"});
            }
        };

        struct get_workers : endpoint<"GET", "/api/v1/workers", access::member> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto out = co_await all_workers(c);
                for (auto& worker : out)
                    worker.managed = !worker.worker_id;
                if (c.identities && c.pool) {
                    auto records = co_await io::blocking(c.rt.get_scheduler(c.core), *c.pool, [store = c.identities] { return store->list(); });
                    for (const auto& record : records) {
                        auto found = std::ranges::find_if(out, [&](const auto& worker) { return worker.worker_id == std::optional{record.worker_id}; });
                        if (found == out.end()) {
                            out.emplace_back();
                            found = std::prev(out.end());
                            found->online = false;
                        }
                        found->worker_id = record.worker_id;
                        found->name = record.name;
                        found->managed = false;
                        found->created_by = record.created_by;
                        found->created_at = record.created_at;
                        found->last_connected_at = record.last_connected_at;
                        found->revoked_at = record.revoked_at;
                        if (record.revoked_at)
                            found->online = false;
                    }
                }
                co_return ok(out);
            }
        };

        inline auto label(std::string_view v) -> std::string {
            std::string out;
            for (char c : v) {
                if (c == '\\' || c == '"')
                    out += '\\';
                if (c == '\n') {
                    out += "\\n";
                    continue;
                }
                out += c;
            }
            return out;
        }

        struct get_metrics : endpoint<"GET", "/metrics"> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                // Project bytes are the size of each project's main source in the node repository, as the project limit counts it.
                jobs::node_usage_reply stored;
                try {
                    stored = co_await c.files.usage(c.core, worlds::service_actor("metrics"));
                } catch (const std::exception&) {
                }
                struct counted {
                    usage::totals totals;
                    std::uint32_t running = 0;
                    std::vector<std::tuple<std::string, std::uint64_t, std::uint64_t>> project_bytes;
                };
                auto ledger = co_await c.files.with_ledger(c.core, [&stored](usage::ledger& ledger, const config::document& d) {
                    counted m{.totals = ledger.counters(), .running = ledger.running()};
                    auto limit = config::resolve_limits(d).max_project_bytes;
                    for (auto& [p, _] : d.projects) {
                        auto found = std::ranges::find(stored.projects, p, &jobs::project_usage::project);
                        m.project_bytes.emplace_back(p, found == stored.projects.end() ? 0 : found->source_bytes, limit);
                    }
                    return m;
                });
                auto workers = co_await all_workers(c);

                auto metric = [](std::string_view suffix) { return std::string{constants::metrics_prefix} + std::string{suffix}; };
                std::string out;
                auto family = [&](std::string_view name, std::string_view type, std::string_view help) {
                    out += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
                };
                family(metric("up"), "gauge", "The gateway is serving.");
                out += std::format("{} 1\n", metric("up"));
                family(metric("workers"), "gauge", "Workers connected.");
                out += std::format("{} {}\n", metric("workers"), workers.size());
                family(metric("worker_inflight"), "gauge", "Requests served by workers with the same identity labels.");
                std::map<std::pair<std::string, std::string>, std::uint64_t> inflight;
                for (const auto& w : workers)
                    inflight[{w.name, w.worker_id.value_or("")}] += w.inflight;
                for (const auto& [identity, count] : inflight)
                    out += std::format("{}{{worker=\"{}\",worker_id=\"{}\"}} {}\n", metric("worker_inflight"), label(identity.first), label(identity.second),
                                       count);
                family(metric("builds_total"), "counter", "Builds, by result: ok, failed, not_run, unknown, cancelled.");
                for (auto& [p, t] : ledger.totals.projects)
                    for (auto [result, n] : {std::pair{"ok", t.builds_ok}, std::pair{"failed", t.builds_failed}, std::pair{"not_run", t.builds_not_run},
                                             std::pair{"unknown", t.builds_unknown}, std::pair{"cancelled", t.builds_cancelled}})
                        out += std::format("{}{{project=\"{}\",result=\"{}\"}} {}\n", metric("builds_total"), p, result, n);
                family(metric("build_seconds_total"), "counter", "Wall time spent in builds.");
                for (auto& [p, t] : ledger.totals.projects)
                    out += std::format("{}{{project=\"{}\"}} {}\n", metric("build_seconds_total"), p, t.build_seconds);
                family(metric("synced_bytes_total"), "counter", "Bytes sent to workers to sync projects.");
                for (auto& [p, t] : ledger.totals.projects)
                    out += std::format("{}{{project=\"{}\"}} {}\n", metric("synced_bytes_total"), p, t.bytes_synced);
                family(metric("tool_calls_total"), "counter", "MCP tool calls, by tool and result.");
                for (auto& [tool, t] : ledger.totals.tools) {
                    out += std::format("{}{{tool=\"{}\",result=\"ok\"}} {}\n", metric("tool_calls_total"), tool, t.calls - t.errors);
                    out += std::format("{}{{tool=\"{}\",result=\"error\"}} {}\n", metric("tool_calls_total"), tool, t.errors);
                }
                family(metric("running_builds"), "gauge", "Builds running now.");
                out += std::format("{} {}\n", metric("running_builds"), ledger.running);
                family(metric("project_bytes"), "gauge", "Bytes of a project's files.");
                for (auto& [p, bytes, _] : ledger.project_bytes)
                    out += std::format("{}{{project=\"{}\"}} {}\n", metric("project_bytes"), p, bytes);
                family(metric("project_bytes_limit"), "gauge", "A project's storage quota.");
                for (auto& [p, _, limit] : ledger.project_bytes)
                    out += std::format("{}{{project=\"{}\"}} {}\n", metric("project_bytes_limit"), p, limit);
                co_return web::reply{.head = {.status = 200, .fields = {{"content-type", "text/plain; version=0.0.4; charset=utf-8"}}}, .body = std::move(out)};
            }
        };

        // A read-only reachability probe of the source this gateway depends on: the connection, the handshake, and one usage read.
        auto probe_source(context& c) -> coroutine::task<std::optional<bool>> {
            // A gateway built without a blocking pool cannot reach the source off-core; it reports nothing instead of guessing.
            if (!c.pool)
                co_return std::nullopt;
            auto endpoint = io::parse_endpoint(c.config.current(c.core)->server.source);
            co_return co_await io::blocking(c.rt.get_scheduler(c.core), *c.pool, [endpoint] {
                try {
                    source::call(endpoint, jobs::node_usage{.actor = worlds::service_actor("probe")});
                    return true;
                } catch (const std::exception&) {
                    // An unreachable source, a refused handshake and a protocol mismatch all read as "not reachable now".
                    return false;
                }
            });
        };
        struct get_health : endpoint<"GET", "/healthz", access::open> {
            static auto handle(context& c, params, const web::target&, std::string_view) -> coroutine::task<web::reply> {
                auto workers = co_await all_workers(c);
                // Observed for this request, so a restart or the gap between two syncs cannot report a stale answer.
                health out{.source = co_await probe_source(c), .workers = workers.size(), .protocol = jobs::protocol_version};
                for (auto& w : workers)
                    if (w.online)
                        ++out.online_workers;
                co_return ok(std::move(out));
            }
        };
    } // namespace api

    template <class... Endpoints> struct routes {
        static auto handle(context& c, std::string_view method, const web::target& t, std::string_view body) -> coroutine::task<web::reply> {
            std::optional<coroutine::task<web::reply>> run;
            bool path_known = false;
            std::optional<web::reply> refused;
            auto attempt = [&]<class E>() {
                if (run || refused)
                    return;
                auto params = E::path::match(t.path);
                if (!params)
                    return;
                path_known = true;
                if (method != E::method)
                    return;
                if (auto why = denied<E>(c.who))
                    refused = std::move(*why);
                else
                    run.emplace(E::handle(c, *params, t, body));
            };
            (attempt.template operator()<Endpoints>(), ...);
            if (refused)
                co_return std::move(*refused);
            if (!run)
                co_return path_known ? coded_reply(coded_error{405, "method_not_allowed", "method not allowed"})
                                     : coded_reply(coded_error{404, "not_found", "not found"});

            std::optional<web::reply> out;
            http::status status = 500;
            std::string why;
            try {
                out.emplace(co_await std::move(*run));
            } catch (const config::not_found& e) {
                status = 404;
                why = e.what();
            } catch (const config::revision_conflict& e) {
                out.emplace(conflict_reply(e));
            } catch (const coded_error& e) {
                out.emplace(coded_reply(e));
            } catch (const tools::tool_error& e) {
                out.emplace(coded_reply(coded_error{api::node_status(e.code), e.code, e.what()}));
            } catch (const transport::coded_error& e) {
                http::status status = e.code == jobs::why::stale_head || e.code == jobs::why::receipt_conflict ? http::status{409}
                                      : e.code == nodes::code::permission_denied                               ? http::status{403}
                                                                                                               : http::status{400};
                out.emplace(coded_reply(coded_error{status, e.code, e.what()}));
            } catch (const transport::remote_error& e) {
                http::status status = e.code == jobs::why::stale_head || e.code == jobs::why::receipt_conflict ? http::status{409}
                                      : e.code.empty()                                                         ? http::status{502}
                                                                                                               : api::node_status(e.code);
                out.emplace(coded_reply(coded_error{status, e.code, e.what()}));
            } catch (const enrollment::refused& e) {
                out.emplace(
                    coded_reply(coded_error{http::status::from_number(static_cast<std::uint16_t>(e.status)).value_or(http::status{500}), e.code, e.what()}));
            } catch (const usage::quota_exceeded& e) {
                status = 429;
                why = e.what();
            } catch (const std::invalid_argument& e) {
                status = 400;
                why = e.what();
            } catch (const std::exception& e) {
                status = 500;
                why = e.what();
                logging::error(std::format("{} {}: {}", method, t.path, why));
            }
            if (out)
                co_return std::move(*out);
            std::string_view code = status == http::status{404}   ? std::string_view{"not_found"}
                                    : status == http::status{400} ? std::string_view{"invalid_request"}
                                    : status == http::status{429} ? nodes::code::resource_limit
                                                                  : std::string_view{"internal"};
            co_return coded_reply(coded_error{status, code, why});
        }

        template <class E> static auto denied(const worlds::actor* who) -> std::optional<web::reply> {
            if constexpr (E::need == access::open)
                return std::nullopt;
            if (!who)
                return refusal(401, "send a bearer token");
            if (worlds::rank(who->role) < static_cast<int>(E::need))
                return refusal(403, E::need == access::host ? "only the host may do this" : "only an owner may do this");
            return std::nullopt;
        }

        static auto refusal(http::status status, std::string why) -> web::reply {
            std::string_view code = status == http::status{401}   ? std::string_view{"unauthenticated"}
                                    : status == http::status{404} ? std::string_view{"not_found"}
                                                                  : nodes::code::permission_denied;
            return coded_reply(coded_error{status, code, std::move(why)});
        }
    };

    using admin_api = routes<api::get_config, api::get_server, api::put_server, api::get_limits, api::put_limits, api::get_granted, api::put_granted,
                             api::get_runtime, api::put_runtime, api::post_worldline, api::get_export, api::post_mail, api::get_mail, api::get_source, api::post_node,
                             api::post_node_capture, api::get_node_source, api::get_node_file, api::get_templates, api::post_generate, api::list_projects,
                             api::get_project, api::put_project, api::delete_project, api::get_effective, api::get_build_adapters, api::get_settings_schema,
                             api::plan_build, api::abandon_operation, api::get_builds, api::get_workers, api::register_worker, api::delete_worker,
                             api::rename_worker, api::get_usage, api::get_events, api::get_stats, api::get_me, api::get_health, api::get_metrics>;

    auto route(context& c, std::string_view method, const web::target& target, std::string_view body) -> coroutine::task<web::reply> {
        co_return co_await admin_api::handle(c, method, target, body);
    }
} // namespace front
