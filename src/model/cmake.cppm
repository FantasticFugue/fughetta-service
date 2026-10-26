export module cmake;
import std;
import reflection;
export import provider;

namespace builds {
    template <> struct adapter<"cmake"> {
        struct options {
            std::string executable = "cmake";
            std::string source_dir = ".";
            std::string build_dir;
            std::string preset;
            std::string toolchain_file;
            std::string generator = "Ninja";
            std::string build_type = "Debug";
            std::string configuration;
            std::map<std::string, std::string> variables;
            std::vector<std::string> targets;
            std::string c_compiler;
            std::string cxx_compiler;
            std::string ctest = "ctest";
            std::vector<std::string> configure_options;
            std::vector<std::string> build_options;
            bool export_compile_commands = false;
            std::map<std::string, std::string> inputs;
            std::map<std::string, artifact> outputs;
        };
        static void validate(const options& c);
        static auto describe(const options& c) -> std::map<std::string, operation_spec>;
        static auto expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step>;
    };
    static_assert(system<adapter<"cmake">>);

    // A JSON compilation database, as CMake and other generators write it; its units are its entries.
    template <> struct format<"compilation_database"> {
        static constexpr std::string_view kind = "file";
        static constexpr std::uint64_t max_bytes = 64u << 20;
        static auto check(std::string_view text) -> std::uint64_t;
    };
    static_assert(layout<format<"compilation_database">>);
} // namespace builds
