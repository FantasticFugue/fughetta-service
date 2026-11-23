export module gradle;
import std;
export import provider;

namespace builds {
    template <> struct adapter<"gradle"> {
        struct options {
            std::string executable = "gradle", exporter = "fughetta-gradle-model";
            std::string source_dir = ".", model_dir = "build/fughetta/gradle";
            std::vector<std::string> arguments;
            std::vector<std::string> prepare_tasks, generate_tasks, compile_tasks{"assemble"}, test_tasks{"test"};
            std::map<std::string, artifact> generated, artifacts;
            std::optional<std::vector<std::string>> model_source_dirs;
            std::optional<std::string> android_variant;
        };
        static void validate(const options& c) {
            relative(c.source_dir, true);
            relative(c.model_dir);
            if (const auto& variant = c.android_variant;
                variant && (variant->empty() || variant->starts_with('-') || variant->contains("${") || variant->contains('\0') ||
                            variant->find_first_of(" \t\r\n\v\f") != std::string::npos))
                throw std::invalid_argument("Gradle Android variant must be a single variant name");
            for (const auto& name : {c.executable, c.exporter}) {
                if (name.empty())
                    throw std::invalid_argument("Gradle tools must be named");
                tools({name});
            }
            for (const auto* tasks : {&c.prepare_tasks, &c.generate_tasks, &c.compile_tasks, &c.test_tasks})
                for (const auto& task : *tasks)
                    if (task.empty() || task.starts_with('-'))
                        throw std::invalid_argument("Gradle task must be named");
            if (!c.generate_tasks.empty() && c.generated.empty())
                throw std::invalid_argument("Gradle generation requires generated outputs");
            if (c.model_source_dirs)
                for (const auto& dir : *c.model_source_dirs)
                    relative(dir, true);
            for (const auto& [name, output] : c.generated)
                if (output.type != "generated_sources")
                    throw std::invalid_argument("Gradle generated input must be generated_sources");
            for (const auto& outputs : {c.generated, c.artifacts})
                for (const auto& [name, output] : outputs)
                    relative(output.path);
        }
        static auto describe(const options& c) -> std::map<std::string, operation_spec> {
            auto normal = operation_spec{.needs = tools({c.executable}), .source_dirs = {c.source_dir}, .build_dirs = {c.model_dir}};
            std::map<std::string, operation_spec> out;
            out["model"] = normal;
            out["model"].needs = tools({c.executable, c.exporter});
            out["model"].outputs = {{"model", {"project_model", c.model_dir + "/model.json"}}};
            out["prepare"] = normal;
            out["prepare"].needs = tools({c.executable, c.exporter});
            out["prepare"].outputs = {{"dependencies", {"gradle_dependencies", c.model_dir + "/dependencies.json"}}};
            out["model"].inputs = {{"dependencies", "gradle_dependencies"}};
            for (const auto& [name, output] : c.generated)
                out["model"].inputs[name] = output.type;
            if (!c.generate_tasks.empty()) {
                out["generate"] = normal;
                out["generate"].outputs = c.generated;
                out["generate"].inputs = {{"dependencies", "gradle_dependencies"}};
            }
            if (!c.compile_tasks.empty()) {
                out["compile"] = normal;
                out["compile"].parameters = {"targets"};
                out["compile"].outputs = c.artifacts;
                out["compile"].inputs = out["model"].inputs;
            }
            if (!c.test_tasks.empty()) {
                out["test"] = normal;
                out["test"].parameters = {"targets"};
            }
            for (auto operation : {"compile", "test"})
                if (auto found = out.find(operation); found != out.end() &&
                    (operation == std::string_view{"test"} || c.android_variant))
                    found->second.needs = tools({c.executable, c.exporter});
            return out;
        }
        static auto expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
            if (!describe(c).contains(std::string{operation}))
                throw std::out_of_range("no such Gradle operation");
            const bool preparation = operation == "model" || operation == "prepare";
            const bool test = operation == "test";
            const bool variant_tasks = c.android_variant && (test || operation == "compile");
            const std::string_view aggregate = test ? "test" : "assemble";
            step out{.name = std::string{operation}, .cwd = c.source_dir, .gradle = true};
            out.argv = {tool(c.executable), "--max-workers=${jobs}"};
            if (test || variant_tasks) {
                out.argv.insert(out.argv.end(), {"--no-configuration-cache", "--init-script", tool(c.exporter)});
                if (test) {
                    out.argv.push_back("-Dfughetta.test.report=${reports}/gradle-results");
                    out.reports = {{"junit", "gradle-results", true}};
                }
                if (variant_tasks)
                    out.argv.push_back(test ? "-Dfughetta.variant.operation=test" : "-Dfughetta.variant.operation=assemble");
            }
            if (preparation) {
                out.argv.insert(out.argv.end(), {"--no-configuration-cache", "--init-script", tool(c.exporter), "-Dfughetta.root=${tree}",
                                                 "-Dfughetta.prepare.output=${tree}/" + c.model_dir + "/dependencies.json",
                                                 "-Dfughetta.model.output=${tree}/" + c.model_dir + "/model.json",
                                                 "-Dfughetta.dependencies=" + (operation == "model" ? std::string{"${input:dependencies}"}
                                                                                                    : "${tree}/" + c.model_dir + "/dependencies.json")});
                out.preparation = true;
                if (c.prepare_tasks.empty() && c.generated.empty())
                    out.model_source_dirs = c.model_source_dirs;
                if (operation == "model")
                    for (const auto& [name, _] : c.generated)
                        out.argv.push_back("-Dfughetta.generated." + name + "=${input:" + name + "}");
            }
            out.argv.insert(out.argv.end(), c.arguments.begin(), c.arguments.end());
            if (c.android_variant)
                out.argv.push_back("-Dfughetta.android.variant=" + *c.android_variant);
            if ((operation == "prepare" || operation == "model") && c.generated.empty())
                out.batch_prefix = out.argv.size();
            if (operation == "model")
                out.argv.push_back("fughettaModel");
            else {
                const auto& tasks = operation == "prepare"    ? c.prepare_tasks
                                    : operation == "generate" ? c.generate_tasks
                                    : operation == "test"     ? c.test_tasks
                                                              : c.compile_tasks;
                const auto& selected = p.targets.empty() ? tasks : p.targets;
                for (const auto& task : selected)
                    if (task.empty() || task.starts_with('-') || task.contains("${") || task.contains('\0'))
                        throw std::invalid_argument("Gradle task must be named");
                for (const auto& task : selected) {
                    const auto separator = task.rfind(':');
                    const auto name = separator == task.npos ? std::string_view{task} : std::string_view{task}.substr(separator + 1);
                    if (!variant_tasks || name != aggregate)
                        out.argv.push_back(task);
                    else if (separator == task.npos)
                        out.argv.push_back("fughettaAndroidVariant");
                    else
                        out.argv.push_back(task.substr(0, separator + 1) + "fughettaProjectAndroidVariant");
                }
                if (operation == "prepare")
                    out.argv.push_back("fughettaPrepare");
            }
            return {std::move(out)};
        }
    };
    static_assert(system<adapter<"gradle">>);
} // namespace builds
