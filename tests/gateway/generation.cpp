import std;
import boost.ut;
import templates;

namespace {
    using namespace boost::ut;
    // Templates render the files a generation creates; creating them in a worldline is the generate tool's (tests/gateway/tools.cpp).
    static const auto cases = [] {
        // Rendering again over the same parameters proves nothing: render() reads no clock, no environment
        // and no I/O. What a template must produce is its own tree, and these check that.
        // The catalog holds file templates only: projects are made through their settings, and no template renders a project tree.
        "file templates are the whole catalog"_test = [] {
            auto items = templates::catalog();
            expect(!items.empty());
            for (auto& item : items)
                expect(item.kind == "file") << item.id;
            for (auto id : {"cpp-class", "python-class", "java-class", "typescript-class", "typescript-interface"}) {
                expect(std::ranges::contains(items, std::string{id}, &templates::descriptor::id)) << id;
                auto plan = templates::render({.template_id = id, .name = "Example"});
                expect(!plan.files.empty());
                for (auto& file : plan.files)
                    expect(!file.path.empty() && !file.text.empty()) << std::format("{}: {}", id, file.path);
            }
            for (auto id : {"cpp-console", "python-console", "java-console", "typescript-console"})
                expect(throws([&] { templates::render({.template_id = id, .name = "example"}); })) << id;
        };
        "class templates render under their directory"_test = [] {
            auto plan = templates::render({.template_id = "cpp-class", .name = "Widget", .directory = "src"});
            expect(plan.files.size() == 2u);
            for (auto& file : plan.files)
                expect(file.path.starts_with("src/Widget.")) << file.path;
        };
        "invalid parameters render nothing"_test = [] {
            for (auto name : {"../bad", "class", "bad-name", "a;touch sentinel", ""})
                expect(throws([&] { templates::render({.template_id = "java-class", .name = name}); }));
            expect(throws([] { templates::render({.template_id = "java-class", .name = "Example", .package = "com..example"}); }));
            expect(throws([] { templates::render({.template_id = "python-class", .name = "Example", .directory = "../outside"}); }));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"generation">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
