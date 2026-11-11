export module commands;
import std;
export import provider;

namespace builds {
    // Any tool chain: each operation is one command line.
    template <> struct adapter<"command"> {
        struct operation {
            std::vector<std::string> argv;
            std::string working_dir;
            environment env;
            std::map<std::string, std::string> inputs;
            std::map<std::string, artifact> outputs;
            std::vector<std::string> source_dirs;
            std::vector<std::string> build_dirs;
            bool append_targets = false;
            bool append_args = false;
        };
        struct options {
            std::map<std::string, operation, std::less<>> operations;
        };
        static void validate(const options& c) {
            if (c.operations.empty())
                throw std::invalid_argument("command adapter has no operations");
            for (auto& [id, op] : c.operations) {
                if (op.argv.empty() || op.argv.front().empty())
                    throw std::invalid_argument("command operation has no executable: " + id);
                relative(op.working_dir, true);
                validate_environment(op.env);
            }
        }
        static auto describe(const options& c) -> std::map<std::string, operation_spec> {
            std::map<std::string, operation_spec> out;
            for (auto& [id, op] : c.operations) {
                std::set<std::string> parameters;
                if (op.append_targets)
                    parameters.insert("targets");
                if (op.append_args)
                    parameters.insert("args");
                out[id] = {.parameters = std::move(parameters),
                           .inputs = op.inputs,
                           .outputs = op.outputs,
                           .needs = tools({op.argv.front()}),
                           .source_dirs = op.source_dirs,
                           .build_dirs = op.build_dirs};
            }
            return out;
        }
        static auto expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
            auto found = c.operations.find(operation);
            if (found == c.operations.end())
                throw std::out_of_range("no such operation");
            auto& op = found->second;
            auto argv = op.argv;
            argv.front() = tool(argv.front());
            argv.insert(argv.end(), p.targets.begin(), p.targets.end());
            argv.insert(argv.end(), p.args.begin(), p.args.end());
            return {{std::string{operation}, std::move(argv), op.working_dir, op.env}};
        }
    };
    static_assert(system<adapter<"command">>);
} // namespace builds
