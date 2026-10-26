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
    using project = rfl::Description<"Project name: 1-64 letters, digits, dots, underscores or hyphens; start with a letter or digit. Example: app", std::string>;
    using name = rfl::Description<"Existing build or analysis configuration name. Example: Debug", std::string>;
    struct location { settings::project project; };
    struct selection { settings::project project; settings::name name; };
    template <class T> struct save {
        rfl::Description<"Revision returned by the corresponding GET; 0 creates a new project. Example: 0", std::uint64_t> revision;
        T value;
    };
    template <class T> struct global { save<T> body; };
    struct project_save { settings::project project; save<config::settings> body; };
    struct preview { settings::project project; settings::name name; builds::request body; };

    struct entry {
        mcp::tool tool;
        std::string method;
        std::string path;
    };

    template <class Input, meta::fixed_string Name, meta::fixed_string Description>
    auto describe(std::string method, std::string path) -> entry {
        using definition = tools::tool<Name, Description>;
        return {{std::string{definition::name}, std::string{definition::description}, config::schema<Input>(), std::nullopt},
                std::move(method), std::move(path)};
    }

    inline auto catalog() -> const std::vector<entry>& {
        static const std::vector<entry> entries{
            describe<empty, "configuration", "Read the current configuration and revisions; members see no credentials.">("GET", "/api/v1/config"),
            describe<empty, "server", "Read gateway server settings and revision. Owner access.">("GET", "/api/v1/config/server"),
            describe<global<config::server>, "save_server", "Replace server settings at the read revision. Takes effect after gateway restart. Owner access.">("PUT", "/api/v1/config/server"),
            describe<empty, "limits", "Read global limits and revision.">("GET", "/api/v1/config/limits"),
            describe<global<config::limits>, "save_limits", "Replace global limits at the read revision. Owner access.">("PUT", "/api/v1/config/limits"),
            describe<location, "project", "Read project settings and revision. Member access.">("GET", "/api/v1/projects/{project}"),
            describe<empty, "projects", "List projects. Member access.">("GET", "/api/v1/projects"),
            describe<project_save, "save_project", "Replace project settings: build configurations, run configurations, analysis, ignore, environment and defaults. Read project first; revision 0 creates a project. Member access.">("PUT", "/api/v1/projects/{project}"),
            describe<location, "remove_project", "Remove a project from configuration; retained files remain. Member access.">("DELETE", "/api/v1/projects/{project}"),
            describe<location, "effective", "Read resolved project configuration and limits. Member access.">("GET", "/api/v1/projects/{project}/effective"),
            describe<empty, "adapters", "Read available build adapters and their option schemas before configuring builders.">("GET", "/api/v1/build-adapters"),
            describe<preview, "preview", "Preview a named build configuration without executing it. Member access.">("POST", "/api/v1/projects/{project}/build-configurations/{name}/plan"),
            describe<selection, "analysis", "Preview a named analysis configuration. Member access.">("POST", "/api/v1/projects/{project}/analysis/{name}/plan"),
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
                for (const auto& entry : catalog()) result.push_back(entry.tool);
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
                    if (at == std::string::npos) continue;
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
            co_return mcp::tool_outcome{.text = result->body, .structured = rfl::json::read<rfl::Generic>(result->body).value(),
                                       .error = result->head.status != 200};
        }
    private:
        Dispatch dispatch_;
    };
} // namespace settings
