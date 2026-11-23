export module tools:common;
import utility;
import constants;
import std;
import knowledge;
import json;
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

export namespace tools {
    class backlog;
    struct execution_identity;

    struct services {
        const config::store::snapshot* config;
        routing::router& workers;
        storage::registry& files;
        std::size_t core;
        // The build, test and run queue of the workspace, shared by every core.
        tools::backlog* queue = nullptr;

        auto snapshot() const -> config::store::snapshot { return *config; }
    };

    template <class T>
    concept tool_for = requires(services& s, mcp::call_context context, typename T::input in, mcp::notifier& out, const worlds::actor& actor) {
        { T::name } -> std::convertible_to<std::string_view>;
        { T::description } -> std::convertible_to<std::string_view>;
        { T::run(s, context, std::move(in), out, actor) } -> std::same_as<coroutine::task<typename T::output>>;
    };

    template <class In>
    concept on_project = requires(const In& in) { in.project.value(); };

    template <on_project In> auto project_name(const In& in) -> std::string {
        if constexpr (requires { in.project.value().value_or(""); })
            return in.project.value().value_or("");
        else
            return in.project.value();
    }
    template <on_project In> auto project_of(const In& in) -> std::string {
        auto name = project_name(in);
        config::check_name("project", name);
        return name;
    }

    struct tool_refusal {
        std::string_view code;
        std::string message;
    };

    inline auto refuse_tool(tool_refusal r) -> mcp::tool_outcome {
        return {.text = json::write(r), .structured = json::to_value(r), .error = true};
    }

    template <class... Tools> struct toolbox {
        static_assert(meta::distinct(std::array<std::string_view, sizeof...(Tools)>{Tools::name...}), "two tools share a name");
        static_assert((tool_for<Tools> && ...), "every tool runs on the gateway's services");

        class bound {
        public:
            explicit bound(services& s) : s_(&s) {}

            auto list() const -> const std::vector<mcp::tool>& {
                static const std::vector<mcp::tool> all{describe<Tools>()...};
                return all;
            }

            auto call(std::string_view name, json::value args, mcp::notifier& out, const worlds::actor& who, mcp::call_context context = {})
                -> coroutine::task<mcp::tool_outcome> {
                std::optional<coroutine::task<mcp::tool_outcome>> run;
                (void)((name == Tools::name && (run.emplace(invoke<Tools>(*s_, context, std::move(args), out, who)), true)) || ...);
                if (!run)
                    co_return mcp::tool_outcome{.text = "unknown tool: " + std::string{name}, .error = true};
                co_return co_await std::move(*run);
            }

        private:
            template <class T> static auto describe() -> mcp::tool {
                return {std::string{T::name}, std::string{T::description}, json::schema<typename T::input>(),
                        json::schema<typename T::output>()};
            }

            template <class T>
            auto invoke(services& s, mcp::call_context context, json::value args, mcp::notifier& note, const worlds::actor& who)
                -> coroutine::task<mcp::tool_outcome> {
                auto started = std::chrono::steady_clock::now();
                if (!who.run.empty()) {
                    if constexpr (T::name == "task")
                        co_return refuse_tool({nodes::code::permission_denied, "execution cannot administer tasks"});
                    auto object = args.to_object();
                    if (!object)
                        co_return refuse_tool({nodes::code::invalid_record, "arguments must be an object"});
                    if (auto project = object->get("project")) {
                        auto value = project->to_string();
                        if (!value || *value != who.project)
                            co_return refuse_tool({nodes::code::permission_denied, "project differs from task binding"});
                    } else
                        (*object)["project"] = who.project;
                    if constexpr (requires(typename T::input input) { input.worldline; }) {
                        if (auto line = object->get("worldline")) {
                            auto value = line->to_string();
                            if (!value || *value != who.worldline)
                                co_return refuse_tool({nodes::code::permission_denied, "worldline differs from task binding"});
                        } else
                            (*object)["worldline"] = who.worldline;
                    }
                    args = *object;
                }
                auto in = json::from_value<typename T::input>(args);
                if (!in)
                    co_return refuse_tool({nodes::code::invalid_record, std::format("{}: bad arguments: {}", T::name, in.error().what())});
                std::optional<std::string> where;
                if constexpr (on_project<typename T::input>)
                    if (config::valid_name(project_name(*in)))
                        where = project_name(*in);
                auto& actor = who;

                std::optional<typename T::output> value;
                std::string error, code;
                try {
                    value.emplace(co_await T::run(s, context, std::move(*in), note, actor));
                } catch (const tool_error& e) {
                    error = e.what();
                    code = e.code;
                } catch (const transport::remote_error& e) {
                    error = e.what();
                    code = e.code;
                } catch (const config::not_found& e) {
                    error = e.what();
                    code = nodes::code::missing_objects;
                } catch (const std::invalid_argument& e) {
                    error = e.what();
                    code = nodes::code::invalid_record;
                } catch (const usage::quota_exceeded& e) {
                    error = e.what();
                    code = nodes::code::resource_limit;
                } catch (const std::exception& e) {
                    error = e.what();
                }
                mcp::tool_outcome outcome;
                if (value) {
                    outcome.text = json::write(*value);
                    outcome.structured = json::to_value(*value);
                    if constexpr (requires { value->failed(); })
                        outcome.error = value->failed();
                } else
                    outcome = refuse_tool({code.empty() ? "failed" : code, std::move(error)});
                if (where) {
                    usage::tool_event e{.project = *where,
                                        .tool = std::string{T::name},
                                        .error = outcome.error,
                                        .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
                    s.files.post(s.core, [e](usage::ledger& ledger) { ledger.record(e); });
                }
                co_return outcome;
            }

            services* s_;
        };
    };

    auto progress_note(const jobs::progress& p) -> mcp::notification {
        return {.method = "notifications/progress",
                .params = json::object({{"progress", static_cast<std::int64_t>(p.done)}, {"total", static_cast<std::int64_t>(p.total)}, {"message", p.stage}})};
    }

} // namespace tools
