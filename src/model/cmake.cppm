export module cmake;
import std;
import reflection;
export import provider;

namespace builds {
    // CMake configures and builds; CTest runs the tests beside it. Each command has one layer of typed fields, and the
    // adapter composes its argv from them. An option that would set a field again is refused, so the field is the one
    // place its argv comes from.
    template <> struct adapter<"cmake"> {
        // What `ctest` runs, and what a run that found no test means.
        struct ctest {
            std::string executable = "ctest";
            // CTest reads CMakePresets.json from the source directory it is run in.
            std::string preset;
            // CTest regular expressions over test names and labels: -R, -E, -L and -LE.
            std::string include;
            std::string exclude;
            std::string labels;
            std::string exclude_labels;
            // Absent leaves each test's timeout to CMakeLists.txt, and the jobs to the runner's ${jobs}.
            std::optional<std::uint32_t> timeout;
            std::optional<std::uint32_t> parallel;
            bool output_on_failure = true;
            bool stop_on_failure = false;
            // `error` fails a run that ran no test; a suite is never reported passing because nothing ran.
            std::string no_tests_action = "error";
            // Arguments CTest itself has no typed field for; the flags a field composes are refused.
            std::vector<std::string> args;
        };
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
            std::vector<std::string> configure_options;
            std::vector<std::string> build_options;
            bool export_compile_commands = false;
            std::map<std::string, std::string> inputs;
            std::map<std::string, artifact> outputs;
            ctest test;
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
