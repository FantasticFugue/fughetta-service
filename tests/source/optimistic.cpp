import std;
import boost.ut;
import content;
import engine;
import support;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "directory moves reject concurrent destination overlap and preserve unrelated files"_test = [] {
            for (const std::string occupied : {"out", "out/a", "out/a/child", "out/b"}) {
                test::scratch tmp{"directory-collision"};
                nodes::local_node node{tmp.dir};
                expect(node.initialize().code == "ok");
                auto call = [&](nodes::node_request request) {
                    request.owner = node.node_id();
                    return node.call(request);
                };
                auto op = [](std::string_view value) { return content::hash_of(value).str(); };
                auto seed = call({.action = "apply", .operation = op("seed"), .edits = {{.kind = "create", .path = "src/a", .text = "source"}}});
                expect((seed.code == "ok") >> fatal);
                auto placed = call({.action = "apply",
                                    .operation = op("placed"),
                                    .expected = seed.frontier,
                                    .edits = {{.kind = "create", .path = occupied, .text = "concurrent"}}});
                expect((placed.code == "ok") >> fatal);
                auto moved = call({.action = "apply",
                                   .operation = op("move"),
                                   .expected = seed.frontier,
                                   .edits = {{.kind = "directory_move", .path = "src", .target = "out"}}});
                expect(moved.code == (occupied == "out/b" ? "ok" : "source_conflict")) << moved.reason;
                auto result = call({.action = "inspect"});
                expect(result.conflicts.empty());
                expect(result.files.size() == 2_u);
                expect(std::ranges::any_of(result.files, [&](const auto& file) { return file.path == occupied && file.text == "concurrent"; }));
                const auto destination = occupied == "out/b" ? "out/a" : "src/a";
                expect(std::ranges::any_of(result.files, [&](const auto& file) { return file.path == destination && file.text == "source"; }));
            }
        };

        "file and directory placements cannot overlap across observed versions"_test = [] {
            for (bool nested_first : {false, true}) {
                test::scratch tmp{"path-collision"};
                nodes::local_node node{tmp.dir};
                expect(node.initialize().code == "ok");
                auto call = [&](nodes::node_request request) {
                    request.owner = node.node_id();
                    return node.call(request);
                };
                auto op = [](std::string_view value) { return content::hash_of(value).str(); };
                auto seed = call({.action = "apply", .operation = op("seed"), .edits = {{.kind = "create", .path = "seed", .text = "base"}}});
                const std::string first = nested_first ? "out/log.txt" : "out", second = nested_first ? "out" : "out/log.txt";
                auto placed = call(
                    {.action = "apply", .operation = op("first"), .expected = seed.frontier, .edits = {{.kind = "create", .path = first, .text = "first"}}});
                expect((placed.code == "ok") >> fatal);
                for (const auto& observed : {seed.frontier, placed.frontier}) {
                    auto refused = call({.action = "apply",
                                         .operation = op("second" + std::to_string(observed == seed.frontier)),
                                         .expected = observed,
                                         .edits = {{.kind = "create", .path = second, .text = "second"}}});
                    expect(refused.code == "source_conflict") << refused.reason;
                }
                auto moved = call(
                    {.action = "apply", .operation = op("move"), .expected = seed.frontier, .edits = {{.kind = "move", .path = "seed", .target = second}}});
                expect(moved.code == "source_conflict");
                auto replaced = call({.action = "apply",
                                      .operation = op("replace"),
                                      .expected = placed.frontier,
                                      .edits = {{.kind = "remove", .path = first}, {.kind = "create", .path = second, .text = "replacement"}}});
                expect(replaced.code == "ok") << replaced.reason;
                expect(call({.action = "inspect"}).conflicts.empty());
            }
        };

        "observed edits merge without locks and retain the writer's submitted baseline"_test = [] {
            test::scratch tmp{"optimistic"};
            nodes::local_node node{tmp.dir};
            expect(node.initialize().code == "ok");
            auto call = [&](nodes::node_request request) {
                request.owner = node.node_id();
                return node.call(request);
            };
            auto op = [](std::string_view value) { return content::hash_of(value).str(); };
            auto seed = call({.action = "apply", .operation = op("seed"), .edits = {{.kind = "create", .path = "x", .text = "abcd"}}});
            expect((seed.code == "ok") >> fatal) << seed.reason;
            auto first = call({.action = "apply",
                               .operation = op("a"),
                               .actor = "alice",
                               .expected = seed.frontier,
                               .edits = {{.kind = "edit", .path = "x", .text = "A", .offset = 0, .count = 1}}});
            expect((first.code == "ok") >> fatal) << first.reason;
            nodes::node_request request{.action = "apply",
                                        .operation = op("b"),
                                        .actor = "bob",
                                        .expected = seed.frontier,
                                        .edits = {{.kind = "edit", .path = "x", .text = "Z", .offset = 3, .count = 1}}};
            auto second = call(request);
            expect((second.code == "ok" && !second.written_version.empty()) >> fatal) << second.reason;
            auto merged = call({.action = "inspect", .revision = second.version});
            expect(merged.files.front().text == "AbcZ");
            auto own = call({.action = "inspect", .revision = second.written_version});
            expect(own.code == "ok" && own.files.front().text == "abcZ");
            auto replay = call(request);
            expect(replay.replayed && replay.written_version == second.written_version);
            // Bob continued editing before the merged reply could be written locally.
            auto continued = call({.action = "apply",
                                   .operation = op("continued"),
                                   .actor = "bob",
                                   .expected = own.frontier,
                                   .edits = {{.kind = "edit", .path = "x", .text = "B", .offset = 1, .count = 1}}});
            expect(continued.code == "ok");
            expect(call({.action = "inspect", .revision = continued.version}).files.front().text == "ABcZ");
            for (const auto action : {"lock", "unlock", "locks"})
                expect(call({.action = action}).code == "invalid_record");
        };
    };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"optimistic">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
