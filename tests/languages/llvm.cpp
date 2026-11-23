// Read ELF, Mach-O, COFF/PDB and Rust artifacts with the LLVM reader.
import std;
import launch;
import host;

namespace {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

    int sequence = 0;

    auto source() -> std::string_view {
        return R"(static int core(int x) {
  int y = x * 3;
  y += 7;
  return y;
}
API int api(int x) {
  int a = core(x);
  a *= 2;
  return a + 1;
}
)";
    }

    // Runs one command. Standard output goes to a file so the reply stays clean;
    // the child object's captured text is then only standard error.
    auto run(std::vector<std::string> argv, const fs::path& cwd) -> std::string {
        auto out = cwd / std::format("stdout-{}.txt", sequence++);
        auto command = test::command(argv.front(), cwd, std::vector<std::string>{argv.begin() + 1, argv.end()});
        command.standard_output = out;
        test::child child{std::move(command)};
        auto code = child.wait(180s);
        auto errors = child.output();
        auto text = test::slurp(out);
        if (code)
            throw std::runtime_error(text + errors);
        return text;
    }

    auto parse(std::string_view value) -> json::value {
        auto result = json::read<json::value>(std::string{value});
        test::require(bool(result), value);
        return std::move(*result);
    }

    auto field(const json::value& value, std::string_view key) -> json::value {
        auto result = value.to_object().value().get(key);
        test::require(bool(result), std::format("missing {} in {}", key, json::write(value)));
        return std::move(*result);
    }

    auto integer(const json::value& value, std::string_view key) -> std::int64_t { return field(value, key).to_int64().value(); }
    auto string(const json::value& value, std::string_view key) -> std::string { return field(value, key).to_string().value(); }
    auto flag(const json::value& value, std::string_view key) -> bool { return field(value, key).to_bool().value(); }

    auto symbols(const json::value& report) -> std::map<std::string, json::value> {
        std::map<std::string, json::value> found;
        for (const auto& symbol : field(report, "symbols").to_array().value())
            found[string(symbol, "name")] = symbol;
        return found;
    }

    auto lines(const json::value& report, std::string_view suffix) -> std::set<std::int64_t> {
        std::set<std::int64_t> result;
        for (const auto& location : field(report, "locations").to_array().value())
            for (const auto& frame : field(location, "frames").to_array().value())
                if (string(frame, "file").ends_with(suffix))
                    result.insert(integer(frame, "line"));
        return result;
    }

    auto contains(const std::set<std::int64_t>& set, std::initializer_list<std::int64_t> wanted) -> bool {
        return std::ranges::all_of(wanted, [&](std::int64_t line) { return set.contains(line); });
    }

    void verify(const fs::path& root, const std::string& host, const std::string& compiler, const std::string& lld_link,
                const std::string& rustc) {
        auto read = [&](const std::string& artifact, std::vector<std::string> flags = {}) {
            std::vector<std::string> argv{host, artifact};
            argv.append_range(flags);
            return parse(run(std::move(argv), root));
        };
        auto compile = [&](std::vector<std::string> flags) {
            std::vector<std::string> argv{compiler, "--driver-mode=gcc", "-x", "c"};
            argv.append_range(flags);
            run(std::move(argv), root);
        };
        test::spit(root / "m.c", source());

        // The ELF library links without a C runtime, so any host can build it.
        compile({"--target=x86_64-unknown-linux-gnu", "-fuse-ld=lld", "-nostdlib", "-g", "-O0", "-shared", "-fPIC",
                 "-DAPI=__attribute__((visibility(\"default\")))", "m.c", "-o", "libm.so"});
        auto report = read("libm.so", {"--lines"});
        auto found = symbols(report);
        test::require(string(report, "format").starts_with("elf64"), string(report, "format"));
        test::require(flag(found.at("api"), "exported") && string(found.at("api"), "binding") == "Global", "api export");
        test::require(string(found.at("core"), "binding") == "Local" && !flag(found.at("core"), "exported") &&
                          integer(found.at("core"), "size") > 0,
                      "core is local");
        test::require(contains(lines(report, "m.c"), {2, 3, 4, 7, 8, 9}), "elf lines");

        // Mach-O names lose their leading underscore and assembler temporaries stay out.
        compile({"--target=arm64-apple-macos", "-g", "-O0", "-c", "-DAPI=", "m.c", "-o", "m.macho.o"});
        report = read("m.macho.o", {"--lines"});
        found = symbols(report);
        test::require(string(report, "format") == "Mach-O arm64", string(report, "format"));
        test::require(std::ranges::equal(found | std::views::keys, std::array<std::string, 2>{"api", "core"}), "macho symbols");
        test::require(std::ranges::all_of(field(report, "symbols").to_array().value(),
                                          [](const json::value& symbol) { return integer(symbol, "size") > 0; }),
                      "macho sizes");
        test::require(contains(lines(report, "m.c"), {2, 3, 4, 7, 8, 9}), "macho lines");

        // A COFF object keeps its symbol sizes; a DLL takes its functions, statics and lines from the PDB.
        compile({"--target=x86_64-pc-windows-msvc", "-gcodeview", "-O0", "-c", "-DAPI=__declspec(dllexport)", "m.c", "-o", "m.obj"});
        found = symbols(read("m.obj"));
        test::require(found.size() == 2 && found.contains("api") && found.contains("core") && integer(found.at("api"), "size") > 0 &&
                          integer(found.at("core"), "size") > 0,
                      "coff symbols");
        if (!lld_link.empty()) {
            run({lld_link, "/dll", "/debug", "/noentry", "/nodefaultlib", "m.obj", "/out:m.dll"}, root);
            report = read("m.dll", {"--lines"});
            found = symbols(report);
            test::require(flag(found.at("api"), "exported") && integer(found.at("api"), "size") > 0, "dll api");
            test::require(string(found.at("core"), "binding") == "Local" && string(found.at("core"), "object_file").ends_with("m.obj"),
                          "dll core");
            test::require(contains(lines(report, "m.c"), {2, 3, 4, 7, 8, 9}), "dll lines");
        }

        if (!rustc.empty()) {
            test::spit(root / "r.rs", R"(#[no_mangle]
pub extern "C" fn rust_api(x: i32) -> i32 {
    helper(x) + 1
}
#[inline(never)]
fn helper(x: i32) -> i32 {
    x * 2
}
)");
            auto system = std::string{host::os};
            auto artifact = system == "windows" ? std::string{"r.dll"}
                            : system == "darwin" || system == "macos" ? std::string{"libr.dylib"}
                                                                      : std::string{"libr.so"};
            run({rustc, "--crate-type", "cdylib", "-g", "-O", "r.rs", "-o", artifact}, root);
            report = read(artifact, {"--lines"});
            found = symbols(report);
            test::require(flag(found.at("rust_api"), "exported"), "rust api");
            test::require(std::ranges::any_of(found, [](const auto& entry) {
                              return entry.first.ends_with("6helper") || entry.first.ends_with("::helper");
                          }) &&
                              !flag(found.at(std::ranges::find_if(found, [](const auto& entry) {
                                                 return entry.first.ends_with("6helper") || entry.first.ends_with("::helper");
                                             })->first),
                                    "exported"),
                          "rust helper");
            test::require(contains(lines(report, "r.rs"), {3, 7}), "rust lines");
        }
    }
} // namespace

int main(int argc, const char** argv) {
    try {
        test::require(argc >= 4, "usage: llvm-test <host> <compiler> <work> [--lld-link X] [--rustc X]");
        std::string host = argv[1], compiler = argv[2];
        fs::path work = argv[3];
        std::string lld_link, rustc;
        for (int index = 4; index < argc; ++index) {
            std::string_view option = argv[index];
            test::require(index + 1 < argc, "missing value for " + std::string{option});
            if (option == "--lld-link")
                lld_link = argv[++index];
            else if (option == "--rustc")
                rustc = argv[++index];
            else
                test::require(false, "unknown option " + std::string{option});
        }
        fs::create_directories(work);
        test::scratch temporary{"native"};
        verify(temporary.dir, host, compiler, lld_link, rustc);
        std::cout << "ELF, Mach-O, COFF and Rust artifacts read\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
