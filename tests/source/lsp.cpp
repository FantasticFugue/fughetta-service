import std;
import boost.ut;
import coroutine;
import native;
import semantic;
import lsp;
import reflection;
import support;

namespace {
    using namespace boost::ut;

    static constexpr auto lsp_suite = [] {
        "a language server outline becomes semantic scopes and symbols"_test = [] {
            auto server = std::getenv("TEST_CLICE");
            if (!server) {
                std::println("skipped: TEST_CLICE names no clice executable");
                return;
            }
            test::scratch tmp{"lsp"};
            std::string text = "namespace demo {\r\n// é😀\r\nstruct shape {\n    int area() const { return 1; }\n};\n/*é😀*/ int twice(int x) { return x * 2; }\n}\n";
            test::spit(tmp.dir / "a b#1.cpp", text);
            test::spit(tmp.dir / "compile_commands.json",
                       std::format(R"([{{"directory":"{}","file":"a b#1.cpp","arguments":["clang++","-std=c++23","-c","a b#1.cpp"]}}])", tmp.dir.string()));
            native::context context;
            auto ran = cores::run(context, [&]() -> coroutine::task<std::pair<semantic::outline, int>> {
                lsp::session session{context.get_scheduler(), {.argv = {server, "serve", "--log-level", "off"}, .cwd = tmp.dir}};
                co_await session.start(tmp.dir, *rfl::json::read<rfl::Generic>(R"({"rules":[{"compile_commands":["compile_commands.json"]}],"project":{"cache_dir":"clice"}})"));
                auto outline = co_await session.describe({.path = "a b#1.cpp", .language = "cpp", .text = text});
                co_return std::pair{std::move(outline), co_await session.stop()};
            }());
            expect((ran.has_value()) >> fatal);
            auto& [outline, code] = *ran;
            expect(code == 0) << "the server exits after shutdown";
            auto find = [&](std::string_view name) {
                return std::ranges::find(outline.symbols, name, &semantic::symbol::name);
            };
            expect((find("demo") != outline.symbols.end() && find("shape") != outline.symbols.end() && find("twice") != outline.symbols.end()) >> fatal);
            auto area = find("area");
            expect((area != outline.symbols.end()) >> fatal);
            expect(area->scope == find("shape")->id) << "members nest under their class";
            auto& definition = *find("twice")->definitions.at(0).place;
            auto scalars = [](std::string_view s) { return std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }); };
            auto expected = scalars(std::string_view{text}.substr(0, text.find("twice")));
            expect(definition.path == "a b#1.cpp" && definition.begin == static_cast<std::uint64_t>(expected) && definition.end == definition.begin + 5)
                << definition.begin << " " << expected;
            auto scope = std::ranges::find(outline.scopes, find("demo")->id, &semantic::scope::id);
            expect(scope->range.place->begin == 0u && scope->kind == "namespace");
            expect(std::ranges::any_of(outline.scopes, [&](auto& s) { return s.id == area->id; })) << "a function is a scope";
            expect(find("area")->kind == "function" && find("shape")->kind == "type");
            expect(find("demo")->id == "a b#1.cpp#demo" && area->id == "a b#1.cpp#demo::shape::area") << "identities are the document and qualified name";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"lsp">{lsp_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
