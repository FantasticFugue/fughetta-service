export module settings;
import std;
import reflection;
import coroutine;
import http;
import mcp;
import tools;
import meta;
import configuration;
import world;

export namespace settings {
    struct empty {};
    using project =
        rfl::Description<"Project name: 1-64 letters, digits, dots, underscores or hyphens; start with a letter or digit. Example: app", std::string>;
    struct location {
        settings::project project;
    };
    template <class T> struct save {
        rfl::Description<"Revision returned by the corresponding GET; 0 creates a new project. Example: 0", std::uint64_t> revision;
        T value;
    };
    // The value is checked when saved, and a refusal names the field; its schema stays out of the tool list, read with `schema`.
    using project_value =
        rfl::Description<"Project settings as project returns them: ignore, env, build_configurations, run_configurations, default_configurations and "
                         "analysis. Read schema for their fields.",
                         std::map<std::string, rfl::Generic>>;
    struct project_save {
        settings::project project;
        save<project_value> body;
    };
    struct entry {
        mcp::tool tool;
        std::string method;
        std::string path;
    };

    template <class Input, meta::fixed_string Name, meta::fixed_string Description> auto describe(std::string method, std::string path) -> entry {
        using definition = tools::tool<Name, Description>;
        return {
            {std::string{definition::name}, std::string{definition::description}, config::schema<Input>(), std::nullopt}, std::move(method), std::move(path)};
    }

    inline auto catalog() -> const std::vector<entry>& {
        static const std::vector<entry> entries{
            describe<location, "project", "Read project settings and revision. Member access.">("GET", "/api/v1/projects/{project}"),
            describe<project_save, "save_project",
                     "Replace project settings: build configurations, run configurations, analysis, ignore, environment and defaults. Read project first; "
                     "revision 0 creates a project. Member access.">("PUT", "/api/v1/projects/{project}"),
            describe<location, "effective", "Read resolved project configuration and limits. Member access.">("GET", "/api/v1/projects/{project}/effective"),
            describe<empty, "schema",
                     "Read the JSON Schema of project settings, with the options of every build adapter and language server, before writing "
                     "save_project.">("GET", "/api/v1/settings-schema"),
        };
        return entries;
    }

    // `Dispatch` answers a REST request as the actor: (actor, method, path, body) -> reply.
    template <class Dispatch> class bound {
    public:
        explicit bound(Dispatch dispatch) : dispatch_(std::move(dispatch)) {}
        auto list() const -> const std::vector<mcp::tool>& {
            static const auto all = [] {
                std::vector<mcp::tool> result;
                for (const auto& entry : catalog())
                    result.push_back(entry.tool);
                return result;
            }();
            return all;
        }
        auto call(std::string_view name, rfl::Generic args, mcp::notifier&, const worlds::actor& who) -> coroutine::task<mcp::tool_outcome> {
            auto found = std::ranges::find_if(catalog(), [name](const entry& e) { return e.tool.name == name; });
            if (found == catalog().end())
                co_return mcp::tool_outcome{.text = std::format("unknown tool: {}", name), .error = true};
            std::optional<web::reply> result;
            std::string error;
            try {
                auto fields = args.to_object().value();
                auto path = found->path;
                for (auto key : {"project", "name"}) {
                    auto marker = "{" + std::string{key} + "}";
                    auto at = path.find(marker);
                    if (at == std::string::npos)
                        continue;
                    auto value = fields.get(key).value().to_string().value();
                    config::check_name(key, value);
                    path.replace(at, marker.size(), value);
                }
                auto body = fields.get("body");
                result = co_await dispatch_(who, found->method, std::move(path), body ? rfl::json::write(*body) : "{}");
            } catch (const std::exception& e) {
                error = e.what();
            }
            if (!result)
                co_return mcp::tool_outcome{.text = std::move(error), .error = true};
            auto parsed = rfl::json::read<rfl::Generic>(result->body);
            if (!parsed)
                co_return mcp::tool_outcome{.text = std::format("the core answered status {} with a body that is not JSON: {}", result->head.status.code(),
                                                                 parsed.error().what()),
                                            .error = true};
            co_return mcp::tool_outcome{
                .text = result->body, .structured = std::move(*parsed), .error = result->head.status != 200};
        }

    private:
        Dispatch dispatch_;
    };
} // namespace settings
