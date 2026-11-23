import std;
import boost.ut;
import json;
import nodes;
import protocol;
import semantic;
import tools;
namespace {
    using namespace boost::ut;
    static constexpr auto cases =
        [] {
            "resolution uses the queried symbol among projected declarations"_test = [] {
                semantic::result selected;
                selected.items.resize(1);
                selected.symbols = {{.id = "other", .name = "Other", .kind = std::string{"type"}},
                                    {.id = "target", .name = "call", .qualified_name = "api::call", .kind = std::string{"function"}}};
                selected.occurrences = {{.symbol = "target", .location = {.source = semantic::place{"main.cpp", 4, 8}}}};
                const auto target = tools::language_query::resolve_target(selected, {.symbol_id = "target"}, true);
                expect(target.has_value() >> fatal);
                expect(target->name.text == "api::call" && target->name.kind == "function" && target->occurrence->begin == 4_u);
                selected.symbols.pop_back();
                expect(!tools::language_query::resolve_target(selected, {.symbol_id = "target"}, true));
            };

            "source range replies preserve Unicode positions and byte-limited output"_test = [] {
                nodes::file_view file{.path = "text", .kind = std::string{"text"}, .text = "a😀é\nZ", .encoding = "utf8"};
                std::string joined;
                std::uint64_t offset = 0;
                do {
                    auto result = tools::worldline::render_read(file, offset, {}, 5);
                    expect(result.total_scalars == 5u && result.text->size() <= 5u && result.data_base64 == tools::lines::base64(*result.text));
                    expect(*result.next_offset > offset);
                    joined += *result.text;
                    offset = *result.next_offset;
                } while (offset < 5);
                expect(joined == file.text);
                auto limited = tools::worldline::render_read(file, 1, std::numeric_limits<std::uint64_t>::max(), 3);
                expect(limited.text == std::string{} && limited.count == 0u && limited.next_offset == 1u && limited.truncated == true);
                auto empty = tools::worldline::render_read(file, 2, 0, 100);
                expect(empty.text == std::string{} && empty.count == 0u && empty.next_offset == 2u && empty.truncated == false);
                auto end = tools::worldline::render_read(file, 5, 1, 100);
                expect(end.text == std::string{} && end.total_scalars == 5u && end.truncated == false);
                expect(throws<tools::tool_error>([&] { tools::worldline::render_read(file, 6, 1, 32); }));
                auto whole = tools::worldline::render_read(file, {}, {}, 0);
                expect(whole.text == file.text && !whole.offset && !whole.truncated);
                file.text = std::string(255, 'a') + "😀";
                for (unsigned i = 0; i < 300; ++i)
                    file.text += "é";
                file.text += "Z";
                auto boundary = tools::worldline::render_read(file, 256, 3, 6);
                expect(boundary.text == "ééé" && boundary.count == 3u && boundary.total_scalars == 557u);
                auto tail = tools::worldline::render_read(file, 556, 10, 10);
                expect(tail.text == "Z" && tail.next_offset == 557u && tail.truncated == false);
                file.text.clear();
                expect(tools::worldline::render_read(file, 0, {}, 10).text == std::string{});
                file.kind = std::string{"binary"};
                file.text = "00ff";
                file.encoding = "hex";
                auto binary = tools::worldline::render_read(file, {}, {}, 0);
                expect(binary.data_base64 == "AP8=" && binary.size == 2u && !binary.text);
                expect(throws<tools::tool_error>([&] { tools::worldline::render_read(file, 0, 1, 32); }));
            };
            "replace finds its old text once within the window and edits by Unicode scalar"_test = [] {
                nodes::node_reply observed;
                observed.files.push_back({.path = "f", .kind = std::string{"text"}, .text = "é{ret 1}\né{ret 1}\n"});
                const std::map<std::string, semantic::place> located{{"second", {"f", 9, 18}}, {"elsewhere", {"g", 0, 1}}};
                auto edits = [&](std::string_view changes) {
                    return tools::edits_of(json::read<std::vector<tools::change>>(changes).value(), observed, {}, located);
                };
                auto scoped = edits(R"([{"kind":"replace","path":"f","old_text":"ret 1","new_text":"ret 2","scope":"second"}])");
                expect(fatal(scoped.size() == 1u));
                expect(scoped[0].kind == nodes::edit_kind::edit && scoped[0].offset == 11u && scoped[0].count == 5u && scoped[0].text == "ret 2");
                auto windowed = edits(R"([{"kind":"replace","path":"f","old_text":"ret 1","new_text":"","offset":0,"count":9}])");
                expect(windowed[0].offset == 2u && windowed[0].count == 5u);
                expect(edits(R"([{"kind":"replace","path":"f","old_text":"é","new_text":"e","offset":9}])")[0].offset == 9u);
                auto refused = [&](std::string_view changes, std::string_view reason) {
                    try {
                        edits(changes);
                    } catch (const tools::tool_error& e) {
                        return std::string_view{e.what()}.contains(reason);
                    }
                    return false;
                };
                expect(refused(R"([{"kind":"replace","path":"f","old_text":"ret 1","new_text":"x"}])", "more than once"));
                expect(refused(R"([{"kind":"replace","path":"f","old_text":"ret 9","new_text":"x"}])", "does not occur"));
                expect(refused(R"([{"kind":"replace","path":"f","old_text":"ret 1","new_text":"x","offset":0,"count":4}])", "does not occur"));
                expect(refused(R"([{"kind":"replace","path":"f","old_text":"","new_text":"x"}])", "non-empty"));
                expect(refused(R"([{"kind":"replace","path":"f","old_text":"ret","new_text":"x","scope":"elsewhere"}])", "is in g"));
                expect(refused(R"([{"kind":"replace","path":"f","old_text":"ret","new_text":"x","scope":"unknown"}])", "resolved"));
                expect(refused(R"([{"kind":"replace","path":"f","old_text":"ret","new_text":"x","scope":"second","offset":0}])", "not both"));
                expect(refused(R"([{"kind":"replace","path":"absent","old_text":"ret","new_text":"x"}])", "not a text file"));
            };
            "preparation errors retain a bounded diagnostic tail"_test = [] {
                jobs::build_report report;
                report.steps.push_back({.name = "configure", .exit_code = 2, .output = std::string(5000, 'x') + "missing compiler"});
                auto result = tools::prepared({}, {}, "v", report);
                expect(result.status == "failed" && result.error.contains("configure") && result.error.contains("exit code 2"));
                expect(result.error.contains("truncated=true") && result.error.ends_with("missing compiler") && result.error.size() < 4200u);
            };
        };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"tools-units">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
