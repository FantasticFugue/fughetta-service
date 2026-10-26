export module bazel;
import std;
export import provider;

namespace builds {
    template <> struct adapter<"bazel"> {
        struct operation {
            std::string command = "build";
            std::vector<std::string> targets;
            std::vector<std::string> options;
            std::vector<std::string> args;
            std::map<std::string, std::string> inputs;
            std::map<std::string, artifact> outputs;
        };
        struct options {
            std::string executable = "bazel";
            std::string source_dir = ".";
            std::string build_dir;
            std::vector<std::string> startup_options;
            std::map<std::string, operation, std::less<>> operations;
        };
        static void validate(const options& c) {
            relative(c.source_dir, true);
            relative(c.build_dir);
            if (c.source_dir == c.build_dir || c.source_dir.starts_with(c.build_dir + "/"))
                throw std::invalid_argument("Bazel output base must be separate from its source directory");
            if (c.executable.empty() || c.operations.empty())
                throw std::invalid_argument("Bazel executable and operations are required");
            for (auto& arg : c.startup_options)
                if (arg.starts_with("--output_base") || arg.starts_with("--output_user_root") || arg == "--nobatch")
                    throw std::invalid_argument("Bazel output and process lifetime are owned by the adapter");
            for (auto& [id, op] : c.operations)
                if (op.command != "build" && op.command != "test" && op.command != "run")
                    throw std::invalid_argument("Bazel operation command must be build, test or run: " + id);
        }
        static auto describe(const options& c) -> std::map<std::string, operation_spec> {
            std::map<std::string, operation_spec> out;
            for (auto& [id, op] : c.operations) {
                std::set<std::string> parameters{"targets"};
                if (op.command == "run")
                    parameters.insert("args");
                if (op.command == "test")
                    parameters.insert("filter");
                out[id] = {.parameters = std::move(parameters),
                           .inputs = op.inputs,
                           .outputs = op.outputs,
                           .needs = tools({c.executable}),
                           .source_dirs = {c.source_dir},
                           .build_dirs = {c.build_dir}};
            }
            return out;
        }
        static auto expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
            auto found = c.operations.find(operation);
            if (found == c.operations.end())
                throw std::out_of_range("no such operation");
            auto& op = found->second;
            std::vector<std::string> argv{tool(c.executable), "--batch", "--output_base=${tree}/" + c.build_dir,
                                          "--output_user_root=${tree}/" + c.build_dir + "/user"};
            argv.insert(argv.end(), c.startup_options.begin(), c.startup_options.end());
            argv.push_back(op.command);
            argv.push_back("--repo_contents_cache=");
            argv.push_back("--jobs=${jobs}");
            argv.insert(argv.end(), op.options.begin(), op.options.end());
            if (p.filter)
                argv.push_back("--test_filter=" + *p.filter);
            auto& targets = p.targets.empty() ? op.targets : p.targets;
            if (op.command == "run" && targets.size() != 1)
                throw std::invalid_argument("Bazel run requires exactly one target");
            argv.insert(argv.end(), targets.begin(), targets.end());
            if (!op.args.empty() || !p.args.empty()) {
                if (op.command != "run")
                    throw std::invalid_argument("program arguments require Bazel run");
                argv.push_back("--");
                argv.insert(argv.end(), op.args.begin(), op.args.end());
                argv.insert(argv.end(), p.args.begin(), p.args.end());
            }
            return {{std::string{operation}, std::move(argv), c.source_dir, {}}};
        }
    };
    static_assert(system<adapter<"bazel">>);
} // namespace builds
