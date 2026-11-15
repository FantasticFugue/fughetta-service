export module msbuild;
import std;
export import provider;

namespace builds {
    template <> struct adapter<"msbuild"> {
        struct options {
            std::string executable = "dotnet", exporter = "fughetta-msbuild-model";
            std::string source_dir = ".", model_dir = "build/fughetta/msbuild";
            std::vector<std::string> projects;
            std::map<std::string, std::string> properties;
            std::vector<std::string> generate_targets;
            std::map<std::string, artifact> generated, artifacts;
        };
        static void validate(const options& c) {
            relative(c.source_dir, true);
            relative(c.model_dir);
            if (c.projects.empty())
                throw std::invalid_argument("MSBuild requires projects");
            for (const auto& project : c.projects)
                relative(project);
            for (const auto& name : {c.executable, c.exporter}) {
                if (name.empty())
                    throw std::invalid_argument("MSBuild tools must be named");
                tools({name});
            }
            for (const auto& [key, value] : c.properties)
                if (key.empty() || key.find_first_of("=:;,\n\r") != std::string::npos || value.find_first_of("\n\r") != std::string::npos)
                    throw std::invalid_argument("invalid MSBuild property");
            if (!c.generate_targets.empty() && c.generated.empty())
                throw std::invalid_argument("MSBuild generation requires generated outputs");
            for (const auto& [name, output] : c.generated)
                if (output.type != "generated_sources")
                    throw std::invalid_argument("MSBuild generated input must be generated_sources");
            for (const auto& outputs : {c.generated, c.artifacts})
                for (const auto& [name, output] : outputs)
                    relative(output.path);
        }
        static auto describe(const options& c) -> std::map<std::string, operation_spec> {
            operation_spec normal{.needs = tools({c.executable}), .source_dirs = {c.source_dir}, .build_dirs = {c.model_dir}};
            std::map<std::string, operation_spec> out{{"restore", normal}, {"model", normal}, {"compile", normal}};
            out["model"].needs = tools({c.executable, c.exporter});
            out["model"].outputs = {{"model", {"project_model", c.model_dir + "/model.json"}}};
            out["restore"].needs = tools({c.executable, c.exporter});
            out["restore"].outputs = {{"assets", {"nuget_assets", c.model_dir + "/assets.json"}}};
            out["model"].inputs = {{"assets", "nuget_assets"}};
            for (const auto& [name, output] : c.generated)
                out["model"].inputs[name] = output.type;
            out["compile"].inputs = out["model"].inputs;
            out["compile"].outputs = c.artifacts;
            if (!c.generate_targets.empty()) {
                out["generate"] = normal;
                out["generate"].outputs = c.generated;
                out["generate"].inputs = {{"assets", "nuget_assets"}};
            }
            return out;
        }
        static auto expand(const options& c, std::string_view operation, const parameters&) -> std::vector<step> {
            if (!describe(c).contains(std::string{operation}))
                throw std::out_of_range("no such MSBuild operation");
            std::vector<std::string> properties;
            for (const auto& [name, value] : c.properties) {
                // MSBuild splits property values at semicolons before unescaping them.
                std::string escaped;
                for (char ch : value) {
                    if (ch == '%')
                        escaped += "%25";
                    else if (ch == ';')
                        escaped += "%3B";
                    else if (ch == ',')
                        escaped += "%2C";
                    else
                        escaped += ch;
                }
                properties.push_back("-property:" + name + "=" + escaped);
            }
            if (operation == "model" || operation == "restore") {
                std::vector<std::string> argv{tool(c.executable),
                                              "fsi",
                                              "--exec",
                                              tool(c.exporter),
                                              "--",
                                              "--tool",
                                              tool(c.executable),
                                              "--root",
                                              "${tree}",
                                              "--project-dir",
                                              "${tree}/" + c.source_dir,
                                              "--restore-output",
                                              "${tree}/" + c.model_dir + "/assets.json",
                                              "--model-output",
                                              "${tree}/" + c.model_dir + "/model.json",
                                              "--assets",
                                              operation == "model" ? "${input:assets}" : "${tree}/" + c.model_dir + "/assets.json",
                                              "--jobs",
                                              "${jobs}"};
                for (const auto& project : c.projects) {
                    argv.push_back("--project");
                    argv.push_back(project);
                }
                if (operation == "model")
                    for (const auto& [name, _] : c.generated)
                        argv.insert(argv.end(), {"--generated", "${input:" + name + "}"});
                for (const auto& property : properties)
                    argv.insert(argv.end(), {"--argument", property});
                argv.push_back("--operations");
                step out{.name = std::string{operation}, .argv = std::move(argv), .cwd = c.source_dir, .preparation = true};
                if (c.generated.empty())
                    out.batch_prefix = out.argv.size();
                out.argv.push_back(std::string{operation});
                return {std::move(out)};
            }
            std::string target = "Build";
            if (operation == "generate") {
                target.clear();
                for (const auto& name : c.generate_targets) {
                    if (!target.empty())
                        target += ';';
                    target += name;
                }
            }
            std::vector<step> out;
            for (const auto& project : c.projects) {
                std::vector<std::string> argv{tool(c.executable), "msbuild", project, "-nologo", "-maxCpuCount:${jobs}", "-target:" + target};
                argv.insert(argv.end(), properties.begin(), properties.end());
                out.push_back({.name = std::string{operation}, .argv = std::move(argv), .cwd = c.source_dir});
            }
            return out;
        }
    };
    static_assert(system<adapter<"msbuild">>);
} // namespace builds
