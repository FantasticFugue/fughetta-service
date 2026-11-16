#include <sqlite3.h>
#if !defined(_WIN32)
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
import std;
import boost.ut;
import content;
import semantic;
import codec;
import kernel;
import archive;
import kernel;
import engine;
import storage;
import support;
import durability;

namespace {
    using namespace boost::ut;
    using nodes::value;
    auto fixture_root(std::filesystem::path root) -> std::filesystem::path {
        std::filesystem::create_directories(root);
        return root;
    }
    auto op(std::string_view name) -> std::string { return content::hash_of(name).str(); }
    auto attempt(nodes::local_node& node, nodes::node_request request) -> nodes::node_reply {
        if (request.owner.empty())
            request.owner = node.node_id();
        return node.call(request);
    }
    auto good(nodes::local_node& node, nodes::node_request request) -> nodes::node_reply {
        auto out = attempt(node, request);
        expect(out.code == "ok") << request.action << ": " << out.code << ": " << out.reason;
        if (out.code != "ok")
            throw std::runtime_error(request.action + ": " + out.reason);
        return out;
    }
    auto text(const nodes::node_reply& reply, std::string_view path) -> std::string {
        for (auto& f : reply.files)
            if (f.path == path)
                return f.text;
        throw std::runtime_error("file missing: " + std::string(path));
    }
    // One workspace with three callers. The store has one writer identity, so a test writes
    // concurrently the way two agents do: against a frontier observed before another publication.

    struct peers {
        test::scratch tmp{"node"};
        nodes::local_node a{fixture_root(tmp.dir / "a")};
        std::unique_ptr<nodes::local_node> b, c;
        peers() {
            good(a, {.action = "initialize", .genesis_nonce = op("node-fixture-genesis")});
            b = std::make_unique<nodes::local_node>(tmp.dir / "a");
            c = std::make_unique<nodes::local_node>(tmp.dir / "a");
        }
        auto seed(std::string path = "x", std::string source = "abcd") -> nodes::node_reply {
            return good(a, {.action = "apply", .operation = op("seed"), .edits = {{.kind = std::string{"create"}, .path = path, .text = source}}});
        }
    };

    auto change(nodes::local_node& node, std::string name, const std::vector<std::string>& base, nodes::edit edit) -> nodes::node_reply {
        return good(node, {.action = "apply", .operation = op(name), .expected = base, .edits = {std::move(edit)}});
    }
    auto inspect(nodes::local_node& node) -> nodes::node_reply { return good(node, {.action = "inspect"}); }

    const auto cases = [] {
        "named tags and channels preserve source selection and operation recovery"_test = [] {
            test::scratch tmp{"named-checkpoints"};
            std::string first, second, reference_receipt;
            {
                nodes::local_node node{tmp.dir};
                good(node, {.action = "initialize"});
                auto a = good(node, {.action = "apply",
                                     .operation = op("release-source"),
                                     .edits = {{.kind = std::string{"create"}, .path = "app", .text = "one", .executable = true}}});
                first = a.version;
                nodes::node_request tag{.action = "reference",
                                        .reference = nodes::reference_input{.name = "v1.0.0", .attestations = {{"review", "", "Approved"}}},
                                        .revision = first,
                                        .operation = op("release-tag"),
                                        .owner = node.node_id(),
                                        .actor = "alice",
                                        .intent = "First release"};
                auto named = good(node, tag);
                reference_receipt = named.receipt;
                auto b = good(node, {.action = "apply",
                                     .operation = op("next-source"),
                                     .expected = a.frontier,
                                     .edits = {{.kind = std::string{"write"}, .path = "app", .text = "two"}}});
                second = b.version;
                expect(good(node, tag).replayed);
                expect(text(good(node, {.action = "read", .revision = "@v1.0.0", .target = "app"}), "app") == "one");
                expect(good(node, {.action = "history", .revision = "v1.0.0"}).history.size() == 1_u);
                tag.operation = op("overwrite-tag");
                tag.revision = second;
                expect(attempt(node, tag).code == nodes::code::source_conflict);
                nodes::node_request channel{.action = "reference",
                                            .reference = nodes::reference_input{.name = "stable", .kind = std::string{"channel"}},
                                            .revision = first,
                                            .operation = op("stable-start"),
                                            .owner = node.node_id(),
                                            .actor = "alice"};
                auto start = good(node, channel);
                channel.reference->expected = start.receipt;
                channel.revision = second;
                channel.operation = op("stable-move");
                auto moved = good(node, channel);
                expect(moved.references.front().previous == start.receipt);
                expect(good(node, channel).replayed);
                channel.operation = op("stale-move");
                expect(attempt(node, channel).code == "stale_reference");
                expect(good(node, {.action = "head", .revision = "@stable"}).version == second);
                auto replay = good(node, {.action = "receipt", .operation = op("release-tag")});
                expect(replay.version == first && replay.receipt == reference_receipt);
                expect(good(node, {.action = "head"}).version == second);
                auto refs = good(node, {.action = "reference_history", .limit = 2});
                expect(refs.references.size() == 2_u && refs.truncated);
                expect(good(node, {.action = "reference_history", .after = refs.references.back().sequence}).references.size() == 1_u);
                auto fork = good(node, {.action = "fork", .worldline = "release-fix", .operation = op("release-fork"), .source = a.frontier});
                expect(text(good(node, {.action = "read", .worldline = "release-fix", .revision = fork.version, .target = "app"}), "app") == "one");
                expect(attempt(node, {.action = "head", .worldline = "release-fix", .revision = "@stable"}).code == nodes::code::invalid_record);
            }
            nodes::local_node reopened{tmp.dir};
            expect(good(reopened, {.action = "references"}).references.size() == 2_u);
            expect(good(reopened, {.action = "head", .revision = "@v1.0.0"}).version == first);
            expect(good(reopened, {.action = "head", .revision = "@stable"}).version == second);
            expect(good(reopened, {.action = "receipt", .operation = op("release-tag")}).receipt == reference_receipt);
        };
        "DCN1 rejects ambiguous encodings and preserves typed values"_test = [] {
            value input =
                value::map{{"array", value::array{value{}, true, false, std::uint64_t{42}, nodes::bytes{std::string{"a\0b", 3}}}}, {"text", "hello 世界"}};
            auto bytes = nodes::codec::encode(input);
            expect(nodes::codec::decode(bytes) == input);
            expect(nodes::digest_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
            expect(nodes::digest_of("abc") == content::hash_of("abc"));
            expect(throws<nodes::error>([&] { nodes::codec::decode(bytes + "x"); }));
            expect(throws<nodes::error>([&] { nodes::codec::decode(std::string_view(bytes).substr(0, bytes.size() - 1)); }));
            for (auto invalid : {std::string{"\xc0\xaf", 2}, std::string{"\xed\xa0\x80", 3}, std::string{"\xf4\x90\x80\x80", 4}}) {
                expect(throws<nodes::error>([&] { nodes::scalars(invalid); }));
                expect(throws<nodes::error>([&] { nodes::scalar_count(invalid); }));
            }
            auto unicode = nodes::scalars("a😀é");
            expect(unicode.size() == 3u);
            expect(nodes::scalar_count("a😀é") == unicode.size());
            std::string out;
            for (auto cp : unicode)
                nodes::append_scalar(out, cp);
            expect(out == "a😀é");
        };
        "compressed archive bounds and streaming archive files preserve bytes"_test = [] {
            test::scratch tmp{"node-archive"};
            std::string raw(300000, 'x');
            raw.replace(120, 5, "hello");
            auto packed = nodes::archive::pack(raw);
            expect(packed.size() < raw.size() / 10);
            expect(nodes::archive::unpack(packed) == raw);
            expect(throws<nodes::error>([&] { nodes::archive::unpack(packed.substr(0, packed.size() - 2)); }));
            expect(throws<nodes::error>([&] { nodes::archive::unpack("wrong"); }));
            auto path = tmp.dir / "exchange";
            nodes::archive::output_file output(path);
            output.stream.write(raw.data(), raw.size());
            output.finish();
            nodes::archive::input_file input(path);
            std::string restored{std::istreambuf_iterator<char>{input.stream}, {}};
            expect(restored == raw);
        };
        "sparse causal sets preserve exact states"_test = [] {
            nodes::causal_set a, b;
            std::set<std::size_t> expected_a, expected_b;
            for (std::size_t i = 0; i < 512; ++i) {
                if (i % 3 != 0) {
                    a.add(i);
                    expected_a.insert(i);
                }
                if (i % 5 < 2) {
                    b.add(i);
                    expected_b.insert(i);
                }
            }
            auto merged = a;
            merged.unite(b);
            auto difference = a.without(b);
            for (std::size_t i = 0; i < 512; ++i) {
                expect(merged.contains(i) == (expected_a.contains(i) || expected_b.contains(i)));
                expect(difference.contains(i) == (expected_a.contains(i) && !expected_b.contains(i)));
            }
            nodes::causal_set serial;
            for (std::size_t i = 0; i < 10000; ++i)
                serial.add(i);
            expect(serial.ranges.size() == 1u);
            expect(serial.contains(9999u));
            expect(!serial.contains(10000u));
        };
        "cold counted sequence edits match an independent complete archive renderer"_test = [] {
            peers p;
            std::string initial;
            for (int i = 0; i < 800; ++i)
                initial += "a😀é\r\n";
            auto current = p.seed("x", initial);
            for (int i = 0; i < 32; ++i) {
                nodes::local_node cold{p.tmp.dir / "a"};
                auto before = good(cold, {.action = "read", .target = "x"});
                auto old = nodes::scalars(text(before, "x"));
                auto offset = std::uint64_t(i * 137) % (old.size() + 1);
                auto count = i % 4 && offset < old.size() ? std::min<std::uint64_t>(3, old.size() - offset) : 0;
                auto inserted = i % 3 ? std::string{"界😀é\n"} : std::string{};
                auto stats = cold.statistics();
                current = good(cold, {.action = "apply",
                                      .operation = op("demand-unicode-" + std::to_string(i)),
                                      .expected = before.frontier,
                                      .edits = {{.kind = std::string{"edit"}, .path = "x", .text = inserted, .offset = offset, .count = count}}});
                auto work = cold.statistics();
                expect(work.projected_files - stats.projected_files <= 1u);
                expect(work.file_histories_read - stats.file_histories_read <= 1u);
                expect(work.causal_headers_read - stats.causal_headers_read < 16u);
                nodes::local_node complete{p.tmp.dir / "a", true};
                auto reference = good(complete, {.action = "inspect"});
                auto actual = good(cold, {.action = "inspect"});
                expect(actual.source_digest == reference.source_digest);
                expect(text(actual, "x") == text(reference, "x"));
                auto replay = good(cold, {.action = "receipt", .target = current.receipt});
                expect(replay.receipt == current.receipt);
            }
        };
        "a cold edit never reads the large causal append tail"_test = [] {
            test::scratch tmp{"demand-append-tail"};
            std::vector<std::string> frontier;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                std::vector<nodes::edit> files{{.kind = std::string{"create"}, .path = "target", .text = "a"}};
                for (int i = 0; i < 64; ++i)
                    files.push_back({.kind = std::string{"create"}, .path = "unrelated/" + std::to_string(i), .text = std::string(1024, 'u')});
                frontier = good(node, {.action = "apply", .operation = op("append-tail-seed"), .edits = std::move(files)}).frontier;
            }
            nodes::local_node cold{tmp.dir / "a"};
            auto before = cold.statistics();
            auto edited = change(cold, "append-tail-edit", frontier, {.kind = std::string{"edit"}, .path = "target", .text = "z"});
            auto after = cold.statistics();
            expect(after.projected_files - before.projected_files == 1u) << "projected " << before.projected_files << " -> " << after.projected_files;

            expect(after.file_payloads_read == before.file_payloads_read);
            expect(after.file_histories_read - before.file_histories_read == 1u);
            nodes::local_node reopened{tmp.dir / "a"};
            expect(text(inspect(reopened), "target") == "za");
            expect(good(reopened, {.action = "receipt", .target = edited.receipt}).receipt == edited.receipt);
        };
        "cold metadata changes reuse authenticated file content and sequence roots"_test = [] {
            test::scratch tmp{"demand-metadata"};
            std::vector<std::string> head;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                head = change(node, "metadata-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = std::string(8192, 'a')}).frontier;
            }
            nodes::local_node cold{tmp.dir / "a"};
            auto mode = change(cold, "metadata-mode", head, {.kind = std::string{"mode"}, .path = "x", .executable = true});
            auto before = cold.statistics();
            auto moved = change(cold, "metadata-move", mode.frontier, {.kind = std::string{"move"}, .path = "x", .target = "y"});
            auto removed = change(cold, "metadata-remove", moved.frontier, {.kind = std::string{"remove"}, .path = "y"});
            auto after = cold.statistics();
            expect(after.file_payloads_read == before.file_payloads_read);
            expect(after.file_histories_read == before.file_histories_read);
            expect(good(cold, {.action = "tree"}).files.empty());
            nodes::local_node complete{tmp.dir / "a", true};
            expect(inspect(complete).source_digest == removed.source_digest);
        };
        "historical queries restore activation effects from an authenticated current base"_test = [] {
            test::scratch tmp{"demand-history-control"};
            std::string version;
            std::vector<std::string> restored;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                auto seed = change(node, "history-control-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "ab"});
                auto edited = change(node, "history-control-edit", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "X", .offset = 1});
                version = edited.version;
                restored =
                    good(node,
                         {.action = "reverse", .operation = op("history-control-reverse"), .expected = edited.frontier, .target = edited.frontier.front()})
                        .frontier;
            }
            nodes::local_node cold{tmp.dir / "a"};
            expect(text(good(cold, {.action = "read", .revision = version, .target = "x"}), "x") == "aXb");
            expect(text(inspect(cold), "x") == "ab");
            expect(inspect(cold).frontier == restored);
        };
        "cold remote delta validation does not project unrelated files"_test = [] {
            peers p;
            std::vector<nodes::edit> files;
            for (int i = 0; i < 64; ++i)
                files.push_back({.kind = std::string{"create"}, .path = std::to_string(i), .text = "v"});
            auto seed = good(p.a, {.action = "apply", .operation = op("delta-seed"), .edits = std::move(files)});
            nodes::local_node cold{p.tmp.dir / "a"};
            expect(text(inspect(cold), "0") == "v");
            auto local = change(p.a, "delta-local", seed.frontier, {.kind = std::string{"edit"}, .path = "0", .text = "A", .offset = 1});
            auto remote = change(*p.b, "delta-remote", seed.frontier, {.kind = std::string{"edit"}, .path = "1", .text = "B", .offset = 1});
            auto before = cold.statistics();
            auto merged = inspect(cold);
            auto after = cold.statistics();
            expect(after.projected_files - before.projected_files <= 4u) << "only the changed files are projected again";
            expect(merged.frontier.size() == 2u);
            expect(text(merged, "0") == "vA");
            expect(text(merged, "1") == "vB");
            expect(inspect(cold).frontier == merged.frontier);
            nodes::local_node complete{p.tmp.dir / "a", true};
            expect(inspect(complete).source_digest == merged.source_digest);
        };
        "cold concurrent branches and contribution reversal retain the complete renderer result"_test = [] {
            peers p;
            auto seed = p.seed("x", "a😀bcé");
            auto a = change(p.a, "demand-branch-a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "审", .offset = 1});
            auto b = change(*p.b, "demand-branch-b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "尾", .offset = 5});
            nodes::local_node cold{p.tmp.dir / "a"};
            auto merged = inspect(cold);
            expect(merged.conflicts.empty());
            expect(text(merged, "x") == "a审😀bcé尾");
            auto reversed = good(cold, {.action = "reverse", .operation = op("demand-reverse"), .expected = merged.frontier, .target = a.frontier.front()});
            expect(text(inspect(cold), "x") == "a😀bcé尾");
            nodes::local_node reopened{p.tmp.dir / "a"};
            auto redone = good(reopened, {.action = "redo", .operation = op("demand-redo"), .expected = reversed.frontier, .target = a.frontier.front()});
            nodes::local_node complete{p.tmp.dir / "a", true};
            expect(inspect(reopened).source_digest == inspect(complete).source_digest);
            expect(text(inspect(reopened), "x") == "a审😀bcé尾");
            expect(good(reopened, {.action = "receipt", .target = b.receipt}).receipt == b.receipt);
            expect(redone.receipt != reversed.receipt);
        };
        "equal text on different worldlines retains distinct authenticated sequence roots"_test = [] {
            test::scratch tmp{"demand-equal-text"};
            std::vector<std::string> branch, main;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                auto seed = change(node, "equal-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "a"});
                auto fork = good(node, {.action = "fork", .worldline = "branch", .operation = op("equal-fork"), .source = seed.frontier});
                branch = fork.frontier;
                auto changed = change(node, "equal-change", seed.frontier, {.kind = std::string{"write"}, .path = "x", .text = "b"});
                auto restored = change(node, "equal-restore", changed.frontier, {.kind = std::string{"write"}, .path = "x", .text = "a"});
                main = restored.frontier;
            }
            nodes::local_node cold{tmp.dir / "a"};
            auto b = good(cold, {.action = "apply",
                                 .worldline = "branch",
                                 .operation = op("equal-branch-edit"),
                                 .expected = branch,
                                 .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "Q", .offset = 1}}});
            auto m = change(cold, "equal-main-edit", main, {.kind = std::string{"edit"}, .path = "x", .text = "W", .offset = 1});
            expect(text(good(cold, {.action = "read", .worldline = "branch", .target = "x"}), "x") == "aQ");
            expect(text(inspect(cold), "x") == "aW");
            nodes::local_node complete{tmp.dir / "a", true};
            expect(inspect(complete).source_digest == m.source_digest);
            expect(good(complete, {.action = "tree", .worldline = "branch"}).source_digest == b.source_digest);
        };
        "a batch places each file on a path that is free once it is done"_test = [] {
            test::scratch tmp{"placing"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = good(node, {.action = "apply",
                                    .operation = op("placing-seed"),
                                    .edits = {{.kind = std::string{"create"}, .path = "a", .text = "A"},
                                              {.kind = std::string{"create"}, .path = "b", .text = "B"},
                                              {.kind = std::string{"create"}, .path = "c", .text = "C"},
                                              {.kind = std::string{"create"}, .path = "d/e", .text = "E"}}});
            auto onto = attempt(
                node,
                {.action = "apply", .operation = op("onto"), .expected = seed.frontier, .edits = {{.kind = std::string{"move"}, .path = "a", .target = "b"}}});
            expect(onto.code == "source_conflict") << onto.reason;
            auto both = attempt(
                node, {.action = "apply",
                       .operation = op("both"),
                       .expected = seed.frontier,
                       .edits = {{.kind = std::string{"move"}, .path = "a", .target = "z"}, {.kind = std::string{"move"}, .path = "c", .target = "z"}}});
            expect(both.code == "source_conflict") << "two files of one batch never land on one path: " << both.reason;
            auto created = attempt(
                node, {.action = "apply",
                       .operation = op("created"),
                       .expected = seed.frontier,
                       .edits = {{.kind = std::string{"create"}, .path = "z", .text = "Z"}, {.kind = std::string{"move"}, .path = "a", .target = "z"}}});
            expect(created.code == "source_conflict") << created.reason;
            auto inward = attempt(node, {.action = "apply",
                                         .operation = op("inward"),
                                         .expected = seed.frontier,
                                         .edits = {{.kind = std::string{"directory_move"}, .path = "d", .target = "x"},
                                                   {.kind = std::string{"move"}, .path = "a", .target = "x/e"}}});
            expect(inward.code == "source_conflict") << inward.reason;
            auto parent = attempt(node, {.action = "apply",
                                         .operation = op("parent"),
                                         .expected = seed.frontier,
                                         .edits = {{.kind = std::string{"create"}, .path = "z/child", .text = "child"},
                                                   {.kind = std::string{"create"}, .path = "z", .text = "parent"}}});
            expect(parent.code == "source_conflict") << parent.reason;
            auto swapped = good(node, {.action = "apply",
                                       .operation = op("swapped"),
                                       .expected = seed.frontier,
                                       .edits = {{.kind = std::string{"remove"}, .path = "b"},
                                                 {.kind = std::string{"move"}, .path = "a", .target = "b"},
                                                 {.kind = std::string{"create"}, .path = "z-one", .text = "sibling"},
                                                 {.kind = std::string{"create"}, .path = "z/two", .text = "child"}}});
            expect(text(good(node, {.action = "inspect"}), "b") == "A") << "a path the batch empties takes another file";
            expect(inspect(node).frontier == swapped.frontier);
        };
        "only the workspace's own actions are accepted"_test = [] {
            test::scratch tmp{"no-locks"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            for (const auto action : {"lock", "unlock", "locks", "semantic", "semantic.publish", "anchor"})
                expect(attempt(node, {.action = action}).code == "invalid_record") << action;
            for (const auto action : {"policy", "offer", "receive", "send", "rejected", "integrate", "admit", "recover", "audit", "checkpoint", "materialize"})
                expect(attempt(node, {.action = action}).code == "invalid_record") << action;
        };
        "size limits hold for the state a publication leaves, reversals included"_test = [] {
            test::scratch tmp{"limits"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = good(node, {.action = "apply",
                                    .operation = op("limit-seed"),
                                    .edits = {{.kind = std::string{"create"}, .path = "a", .text = std::string(60, 'a')}},
                                    .file_limit = 64,
                                    .project_limit = 100});
            auto grown = attempt(node, {.action = "apply",
                                        .operation = op("grown"),
                                        .expected = seed.frontier,
                                        .edits = {{.kind = std::string{"edit"}, .path = "a", .text = std::string(5, 'g'), .offset = 0}},
                                        .file_limit = 64,
                                        .project_limit = 100});
            expect(grown.code == "resource_limit") << grown.reason;
            auto dropped = good(node, {.action = "apply",
                                       .operation = op("dropped"),
                                       .expected = seed.frontier,
                                       .edits = {{.kind = std::string{"remove"}, .path = "a"}},
                                       .file_limit = 64,
                                       .project_limit = 100});
            auto other = good(node, {.action = "apply",
                                     .operation = op("other"),
                                     .expected = dropped.frontier,
                                     .edits = {{.kind = std::string{"create"}, .path = "b", .text = std::string(60, 'b')}},
                                     .file_limit = 64,
                                     .project_limit = 100});
            auto restored = attempt(node, {.action = "reverse",
                                           .operation = op("restored"),
                                           .expected = other.frontier,
                                           .target = dropped.frontier.front(),
                                           .file_limit = 64,
                                           .project_limit = 100});
            expect(restored.code == "resource_limit") << "a reversal cannot bring the project past its limit: " << restored.reason;
            auto shrunk = good(node, {.action = "apply",
                                      .operation = op("shrunk"),
                                      .expected = other.frontier,
                                      .edits = {{.kind = std::string{"edit"}, .path = "b", .text = "", .offset = 0, .count = 30}},
                                      .file_limit = 10,
                                      .project_limit = 10});
            expect(text(good(node, {.action = "inspect"}), "b").size() == 30u) << "a file or project already past its limit may still shrink";
            expect(inspect(node).frontier == shrunk.frontier);
        };
        "rewriting a text longer than one collection erases it as ranges"_test = [] {
            peers p;
            auto seed = p.seed();
            auto length = nodes::codec::collection_limit + 1024;
            auto long_text = change(*p.b, "long-create", seed.frontier, {.kind = std::string{"create"}, .path = "long", .text = std::string(length, 'a')});
            auto middle =
                change(*p.b, "long-middle", long_text.frontier, {.kind = std::string{"edit"}, .path = "long", .text = "m", .offset = 10, .count = 10});
            auto rewritten = change(*p.b, "long-rewrite", middle.frontier, {.kind = std::string{"write"}, .path = "long", .text = std::string(length, 'b')});
            expect(text(good(*p.b, {.action = "inspect"}), "long") == std::string(length, 'b'));
            auto reversed =
                good(*p.b, {.action = "reverse", .operation = op("long-reverse"), .expected = rewritten.frontier, .target = rewritten.frontier.front()});
            auto restored = std::string(length - 9, 'a');
            restored[10] = 'm';
            expect(text(good(*p.b, {.action = "inspect"}), "long") == restored);
            expect(text(good(p.a, {.action = "inspect"}), "long") == restored) << "a second connection replays the range erasures";
            nodes::local_node reopened{p.tmp.dir / "a"};
            expect(text(good(reopened, {.action = "inspect"}), "long") == restored);
        };
        "writers in one worldline publish from the same observed state and converge"_test = [] {
            test::scratch tmp{"shared-line"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "shared-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "abcd"});
            auto first = good(node, {.action = "apply",
                                     .operation = op("shared-first"),
                                     .actor = "alice",
                                     .expected = seed.frontier,
                                     .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "1", .offset = 1}}});
            auto second = good(node, {.action = "apply",
                                      .operation = op("shared-second"),
                                      .actor = "bob",
                                      .expected = seed.frontier,
                                      .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "2", .offset = 3}}});
            expect(second.frontier.size() == 2u);
            expect(std::ranges::contains(second.frontier, first.frontier.front()));
            expect(text(inspect(node), "x") == "a1bc2d");
            auto history = good(node, {.action = "history"});
            std::set<std::string> actors;
            for (const auto& entry : history.history)
                actors.insert(entry.actor);
            expect(actors.contains("alice") && actors.contains("bob"));
        };
        "worldlines never take each other's source"_test = [] {
            test::scratch tmp{"line-boundary"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "boundary-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "a"});
            auto fork = good(node, {.action = "fork", .worldline = "branch", .operation = op("boundary-fork"), .source = seed.frontier});
            auto branch = good(node, {.action = "apply",
                                      .worldline = "branch",
                                      .operation = op("boundary-branch"),
                                      .expected = fork.frontier,
                                      .edits = {{.kind = std::string{"write"}, .path = "x", .text = "b"}}});
            auto main = change(node, "boundary-main", seed.frontier, {.kind = std::string{"write"}, .path = "x", .text = "m"});
            auto read = attempt(node, {.action = "read", .source = branch.frontier, .target = "x"});
            expect(read.code == "worldline_boundary") << read.code;
            auto mixed = attempt(
                node, {.action = "fork", .worldline = "mixed", .operation = op("boundary-mixed"), .source = {branch.frontier.front(), main.frontier.front()}});
            expect(mixed.code == "worldline_boundary") << mixed.code;
            expect(text(good(node, {.action = "read", .worldline = "branch", .source = seed.frontier, .target = "x"}), "x") == "a")
                << "the fork base stays readable from the branch";
            auto missing = attempt(node, {.action = "apply",
                                          .worldline = "absent",
                                          .operation = op("boundary-absent"),
                                          .edits = {{.kind = std::string{"create"}, .path = "y", .text = "y"}}});
            expect(missing.code == "invalid_record") << missing.code;
            expect(attempt(node, {.action = "fork", .worldline = "branch", .operation = op("boundary-again"), .source = seed.frontier}).code ==
                   nodes::code::source_conflict);
        };
        "semantic declaration anchors follow source identity through moves and insertions"_test = [] {
            test::scratch tmp{"semantic-anchors"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto base = good(
                node,
                {.action = "apply", .operation = op("declaration-seed"), .edits = {{.kind = std::string{"create"}, .path = "a.cpp", .text = "int run();\n"}}});
            auto original = good(node, {.action = "scope_register", .revision = base.version, .declaration_ranges = {{"run", {"a.cpp", 4, 7}}}});
            auto renamed = good(node, {.action = "apply",
                                       .operation = op("declaration-move"),
                                       .expected = base.frontier,
                                       .edits = {{.kind = std::string{"move"}, .path = "a.cpp", .target = "src/a.cpp"}}});
            auto changed = good(node, {.action = "apply",
                                       .operation = op("declaration-insert"),
                                       .expected = renamed.frontier,
                                       .edits = {{.kind = std::string{"edit"}, .path = "src/a.cpp", .text = "// 🌍\n", .offset = 0}}});
            auto moved = good(node, {.action = "scope_register", .revision = changed.version, .declaration_ranges = {{"run", {"src/a.cpp", 9, 12}}}});
            expect(original.declaration_anchors.at("run") == moved.declaration_anchors.at("run"));
            auto pinned = good(node, {.action = "scope_register", .revision = base.version, .declaration_ranges = {{"run", {"a.cpp", 4, 7}}}});
            expect(pinned.declaration_anchors.at("run") == original.declaration_anchors.at("run"));
        };
        "one caller edits independently leased regions without exposing either to another caller"_test = [] {
            test::scratch tmp{"scope-batch"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            std::vector<nodes::edit> seed{{.kind = std::string{"create"}, .path = "x", .text = "abcdefghij"}};
            for (unsigned i = 0; i < 9; ++i)
                seed.push_back({.kind = std::string{"create"}, .path = "spare/" + std::to_string(i), .text = "spare"});
            auto base = good(node, {.action = "apply", .operation = op("scope-seed"), .edits = std::move(seed)});
            auto references = good(node, {.action = "scope_register",
                                          .revision = base.version,
                                          .analyzed_scopes = {{.id = "first", .location = {.source = semantic::place{"x", 1, 3}}},
                                                              {.id = "second", .location = {.source = semantic::place{"x", 6, 8}}}}});
            auto refs = std::vector<std::string>{*references.scopes[0].reference, *references.scopes[1].reference};
            auto occupied = good(node, {.action = "scope_acquire", .lease_owner = "run-b", .operation = op("occupied"), .scope_refs = {refs[1]}});
            expect(attempt(node, {.action = "scope_acquire", .lease_owner = "run-a", .operation = op("conflicting-set"), .scope_refs = refs}).code ==
                   "scope_conflict");
            auto unchanged = good(node, {.action = "scope_list"});
            expect(unchanged.leases.size() == 1u && unchanged.leases[0].owner == "run-b");
            good(node, {.action = "scope_release", .lease_owner = "run-b", .lease_token = occupied.leases[0].token});
            auto leased = good(node, {.action = "scope_acquire", .lease_owner = "run-a", .operation = op("both-scopes"), .scope_refs = refs});
            auto first = leased.leases[0].token, second = leased.leases[1].token;
            good(node, {.action = "apply",
                        .lease_owner = "run-a",
                        .leases = {first, second},
                        .operation = op("unrelated"),
                        .expected = base.frontier,
                        .edits = {{.kind = std::string{"edit"}, .path = "spare/0", .text = "!", .offset = 1}}});
            auto refused = attempt(node, {.action = "apply",
                                          .lease_owner = "run-b",
                                          .operation = op("intruder"),
                                          .expected = base.frontier,
                                          .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "!", .offset = 1, .count = 1}}});
            expect(refused.code == "scope_conflict");
            auto edited = good(node, {.action = "apply",
                                      .lease_owner = "run-a",
                                      .leases = {first, second},
                                      .operation = op("edit-first"),
                                      .expected = base.frontier,
                                      .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 1, .count = 1}}});
            good(node, {.action = "apply",
                        .lease_owner = "run-a",
                        .leases = {first, second},
                        .operation = op("edit-second"),
                        .expected = edited.frontier,
                        .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "G", .offset = 6, .count = 1}}});
            expect(text(good(node, {.action = "read", .target = "x"}), "x") == "aBcdefGhij");
            good(node, {.action = "scope_release", .lease_owner = "run-a", .lease_token = first});
            expect(attempt(node, {.action = "apply",
                                  .lease_owner = "run-a",
                                  .leases = {first},
                                  .operation = op("unrelated-expired"),
                                  .expected = edited.frontier,
                                  .edits = {{.kind = std::string{"edit"}, .path = "spare/0", .text = "!", .offset = 1}}})
                       .code == "scope_expired");
            expect(attempt(node, {.action = "scope_acquire", .lease_owner = "run-a", .operation = op("both-scopes"), .scope_refs = refs}).code ==
                   "scope_expired");
            expect(good(node, {.action = "scope_list"}).leases.size() == 1u);
        };
        "lease grouping protects directory ancestors and move destinations"_test = [] {
            test::scratch tmp{"scope-paths"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto state =
                good(node, {.action = "apply", .operation = op("path-seed"), .edits = {{.kind = std::string{"create"}, .path = "dir/x", .text = "text"}}});
            auto held = good(node, {.action = "scope_acquire",
                                    .lease_owner = "owner",
                                    .operation = op("path-lease"),
                                    .revision = state.version,
                                    .scope_files = {"dir/x"},
                                    .scope_entries = {"target"}});
            auto apply = [&](std::string id, nodes::edit edit, bool owns = false) {
                std::vector<std::string> tokens;
                if (owns)
                    for (const auto& lease : held.leases)
                        tokens.push_back(lease.token);
                return attempt(node, {.action = "apply",
                                      .lease_owner = owns ? "owner" : "other",
                                      .leases = std::move(tokens),
                                      .operation = op(id),
                                      .expected = state.frontier,
                                      .edits = {std::move(edit)}});
            };
            auto sibling = apply("sibling", {.kind = std::string{"create"}, .path = "target-other/x", .text = "free"});
            expect(sibling.code == "ok");
            state = sibling;
            expect(apply("ancestor", {.kind = std::string{"directory_move"}, .path = "dir", .target = "elsewhere"}).code == "scope_conflict");
            expect(apply("destination", {.kind = std::string{"move"}, .path = "target-other/x", .target = "target/x"}).code == "scope_conflict");
            auto moved = apply("owner-move", {.kind = std::string{"directory_move"}, .path = "dir", .target = "target"}, true);
            expect(moved.code == "ok");
            state = moved;
            expect(apply("moved-file", {.kind = std::string{"edit"}, .path = "target/x", .text = "!", .offset = 1}).code == "scope_conflict");
            auto edited = apply("owner-edit", {.kind = std::string{"edit"}, .path = "target/x", .text = "!", .offset = 1}, true);
            expect(edited.code == "ok");
            expect(text(good(node, {.action = "read", .target = "target/x"}), "target/x") == "t!ext");
        };
        "task revocation fences delayed source writes and fork binding is atomic"_test = [] {
            test::scratch tmp{"task-source"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto base = change(node, "task-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "text"});
            good(node, {.action = "task_create", .operation = op("task-create"), .task_id = "task", .execution_owner = "owner"});
            auto started = good(
                node, {.action = "task_start", .operation = op("task-start"), .task_id = "task", .execution_identity = "identity", .execution_owner = "owner"});
            const auto run = *started.execution;
            auto lease = good(node, {.action = "scope_acquire",
                                     .revision = base.version,
                                     .lease_owner = run.run,
                                     .operation = op("task-lease"),
                                     .run_id = run.run,
                                     .execution_token = run.token,
                                     .scope_files = {"x"}});
            nodes::node_request delayed{.action = "apply",
                                        .lease_owner = run.run,
                                        .leases = {lease.leases[0].token},
                                        .operation = op("delayed"),
                                        .expected = base.frontier,
                                        .edits = {{.kind = std::string{"write"}, .path = "x", .text = "late"}},
                                        .run_id = run.run,
                                        .execution_token = run.token};
            good(node, {.action = "task_stop", .task_id = "task", .run_id = run.run, .execution_owner = "owner"});
            expect(attempt(node, delayed).code == "execution_expired");
            expect(good(node, {.action = "scope_list"}).leases.empty());
            expect(text(good(node, {.action = "read", .target = "x"}), "x") == "text");
            auto failed = attempt(node, {.action = "fork",
                                         .worldline = "unbound",
                                         .operation = op("fork-bound-task"),
                                         .source = base.frontier,
                                         .task_id = "task",
                                         .execution_owner = "owner"});
            expect(failed.code == "identity_conflict");
            expect(!good(node, {.action = "worldlines"}).worldlines.contains("unbound"));
        };
        "source rejects removed semantic and syntax-scope operations"_test = [] {
            test::scratch tmp{"no-semantics"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            for (auto action : {"semantic", "semantic.publish", "anchor"})
                expect(attempt(node, {.action = action}).code != "ok");
            expect(attempt(node, {.action = "lock"}).code == "invalid_record");
        };
        "missing causal log and corrupt effect pages refuse edits and a rebuild reconstructs them"_test = [] {
            for (bool missing : {false, true}) {
                test::scratch tmp{"demand-damaged"};
                std::vector<std::string> head;
                std::string receipt;
                {
                    nodes::local_node seed{tmp.dir / "a"};
                    good(seed, {.action = "initialize"});
                    auto result = change(seed, "demand-damaged-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "a😀bc"});
                    head = result.frontier;
                    receipt = result.receipt;
                }
                sqlite3* raw = nullptr;
                expect((sqlite3_open((tmp.dir / "a/repository.sqlite").string().c_str(), &raw) == SQLITE_OK) >> fatal);
                std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{raw, sqlite3_close};
                auto sql = missing ? "DELETE FROM events" : "UPDATE file_effects SET payload=x'00'";
                expect((sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK) >> fatal);
                db.reset();
                if (missing) {
                    // The snapshot's event coverage is the gate: a missing causal log is refused at open.
                    expect(throws<nodes::error>([&] { nodes::local_node damaged{tmp.dir / "a"}; }));
                } else {
                    nodes::local_node damaged{tmp.dir / "a"};
                    auto rejected = attempt(damaged, {.action = "apply",
                                                      .operation = op("demand-damaged-edit"),
                                                      .expected = head,
                                                      .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "z", .offset = 1}}});
                    expect(rejected.code == "invalid_record" || rejected.code == "missing_objects" || rejected.code == "storage_failure" ||
                           rejected.code == "outcome_unknown")
                        << rejected.code;
                    expect(good(damaged, {.action = "tree"}).frontier == head);
                }
                nodes::local_node audit{tmp.dir / "a", true};
                expect(text(inspect(audit), "x") == "a😀bc");
                expect(good(audit, {.action = "receipt", .target = receipt}).receipt == receipt);
                nodes::local_node repaired{tmp.dir / "a"};
                auto edited = change(repaired, "demand-repaired-edit", head, {.kind = std::string{"edit"}, .path = "x", .text = "z", .offset = 1});
                expect(!edited.receipt.empty());
                expect(text(inspect(repaired), "x") == "az😀bc");
            }
        };
        "demand caches release reconstruction state between unrelated file edits"_test = [] {
            test::scratch tmp{"demand-cache"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            std::vector<nodes::edit> files;
            for (int i = 0; i < 24; ++i)
                files.push_back({.kind = std::string{"create"}, .path = std::to_string(i), .text = "a😀bc"});
            auto current = good(node, {.action = "apply", .operation = op("demand-cache-seed"), .edits = std::move(files)});
            auto before = node.statistics();
            for (int i = 0; i < 24; ++i)
                current = change(node, "demand-cache-" + std::to_string(i), current.frontier,
                                 {.kind = std::string{"edit"}, .path = std::to_string(i), .text = "z", .offset = 1});
            expect(node.statistics().cache_evictions > before.cache_evictions);
            nodes::local_node complete{tmp.dir / "a", true};
            expect(inspect(node).source_digest == inspect(complete).source_digest);
            expect(good(node, {.action = "receipt", .target = current.receipt}).receipt == current.receipt);
        };
        "incremental edits and cold point reads avoid unrelated file work"_test = [] {
            test::scratch tmp{"node-incremental"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            std::vector<nodes::edit> files;
            for (int i = 0; i < 512; ++i)
                files.push_back({.kind = std::string{"create"}, .path = std::format("files/{:03}.txt", i), .text = std::string(1024, 'x')});
            auto seeded = good(node, {.action = "apply", .operation = op("incremental-seed"), .edits = std::move(files)});
            auto before = node.statistics();
            auto changed = change(node, "incremental-one", seeded.frontier, {.kind = std::string{"edit"}, .path = "files/001.txt", .text = "z"});
            auto after = node.statistics();
            expect(after.projected_files - before.projected_files == 1u);
            // One changed file writes one manifest row and one path row, never the whole manifest.
            expect(after.manifest_rows_written - before.manifest_rows_written < 32u);

            expect(after.state_bytes_written - before.state_bytes_written < 512u * 1024u);
            std::vector<nodes::edit> batch;
            for (int i = 3; i < 19; ++i)
                batch.push_back({.kind = std::string{"edit"}, .path = std::format("files/{:03}.txt", i), .text = "batch"});
            batch.push_back({.kind = std::string{"remove"}, .path = "files/000.txt"});
            batch.push_back({.kind = std::string{"remove"}, .path = "files/002.txt"});
            for (int i = 0; i < 8; ++i)
                batch.push_back({.kind = std::string{"create"}, .path = std::format("new/{:03}.txt", i), .text = "new"});
            changed = good(node, {.action = "apply", .operation = op("incremental-batch"), .expected = changed.frontier, .edits = std::move(batch)});
            nodes::local_node cold{tmp.dir / "a"};
            auto opened = cold.statistics();
            auto selected = good(cold, {.action = "read", .target = "files/001.txt"});
            auto read = cold.statistics();
            expect(!read.causal_loaded);
            expect(read.projected_files == 0u);
            expect(read.manifest_rows_read - opened.manifest_rows_read < 16u);
            expect(read.blob_reads - opened.blob_reads <= 8u);
            expect(text(selected, "files/001.txt") == "z" + std::string(1024, 'x'));
            expect(selected.source_digest == changed.source_digest);
            nodes::local_node audited{tmp.dir / "a", true};
            expect(inspect(audited).source_digest == changed.source_digest);
            expect(inspect(audited).files.size() == 518u);
        };
        "manifest deltas keep a publication proportional to changed files"_test = [] {
            test::scratch tmp{"manifest-chain"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            std::vector<nodes::edit> files;
            for (int i = 0; i < 128; ++i)
                files.push_back({.kind = std::string{"create"}, .path = std::format("files/{:03}.txt", i), .text = std::format("v0-{}", i)});
            auto current = good(node, {.action = "apply", .operation = op("chain-seed"), .edits = std::move(files)});
            std::string old_version;
            std::uint64_t written_total = 0;
            for (int i = 0; i < 40; ++i) {
                auto before = node.statistics();
                auto path = std::format("files/{:03}.txt", i % 128);
                current =
                    change(node, "chain-" + std::to_string(i), current.frontier, {.kind = std::string{"write"}, .path = path, .text = std::format("v1-{}", i)});
                auto after = node.statistics();
                auto written = after.manifest_rows_written - before.manifest_rows_written;
                written_total += written;
                // Every 32 revisions a complete snapshot anchors the chain; all other writes are deltas.
                expect(written < 32u || written >= 128u) << "revision " << i << " wrote " << written << " manifest rows";
                // The retained projection answers the next publication without refetching the manifest.
                expect(after.manifest_rows_read - before.manifest_rows_read < 32u) << "revision " << i << " read the manifest";
                if (i == 4)
                    old_version = current.version;
            }
            expect(written_total < 128u + 40u * 8u) << written_total;
            // An old revision resolves through its delta chain and answers a point read without the whole manifest.
            auto before = node.statistics();
            auto old = good(node, {.action = "read", .revision = old_version, .target = "files/004.txt"});
            expect(text(old, "files/004.txt") == "v1-4");
            expect(node.statistics().manifest_rows_read - before.manifest_rows_read < 16u);
            expect(good(node, {.action = "tree"}).files.size() == 128u);
            nodes::local_node audit{tmp.dir / "a", true};
            expect(inspect(audit).source_digest == current.source_digest);
        };
        "point reads follow a move and later content edits"_test = [] {
            test::scratch tmp{"manifest-move"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "move-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "a"});
            auto moved = change(node, "move-move", seed.frontier, {.kind = std::string{"move"}, .path = "x", .target = "y"});
            auto edited = change(node, "move-edit", moved.frontier, {.kind = std::string{"write"}, .path = "y", .text = "bb"});
            expect(text(good(node, {.action = "read", .revision = edited.version, .target = "y"}), "y") == "bb");
            expect(text(good(node, {.action = "read", .revision = moved.version, .target = "y"}), "y") == "a");
            expect(good(node, {.action = "read", .revision = edited.version, .target = "x"}).files.empty());
            nodes::local_node audit{tmp.dir / "a", true};
            expect(inspect(audit).source_digest == edited.source_digest);
        };
        "conflicted versions keep the manifest fast path"_test = [] {
            peers p;
            auto seed = p.seed();
            change(p.a, "conflict-a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "A", .offset = 1, .count = 0});
            change(*p.b, "conflict-b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 1, .count = 0});
            auto merged = inspect(p.a);
            expect(!merged.conflicts.empty());
            auto before = p.a.statistics();
            auto again = inspect(p.a);
            auto after = p.a.statistics();
            expect(again.source_digest == merged.source_digest);
            expect(again.conflicts == merged.conflicts);
            expect(after.projected_files == before.projected_files);
            expect(after.manifest_rows_read - before.manifest_rows_read < 16u);
        };
#if !defined(_WIN32)
        "a killed publication leaves no manifest or event rows"_test = [] {
            test::scratch tmp{"kill-mid-commit"};
            std::vector<std::string> head;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                head = change(node, "kill-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "before"}).frontier;
            }
            for (const std::string boundary : {"before_commit", "after_commit"}) {
                auto child = fork();
                if (child == 0) {
                    setenv("NODE_FAULT", boundary.c_str(), 1);
                    nodes::local_node node{tmp.dir / "a"};
                    (void)node.call({.action = "apply",
                                     .operation = op("kill-" + boundary),
                                     .owner = node.node_id(),
                                     .expected = head,
                                     .edits = {{.kind = std::string{"write"}, .path = "x", .text = "after"}}});
                    _Exit(2);
                }
                int status = 0;
                expect(waitpid(child, &status, 0) == child);
                expect(WIFEXITED(status) && WEXITSTATUS(status) == 86);
                nodes::local_node reopened{tmp.dir / "a"};
                auto state = good(reopened, {.action = "inspect"});
                sqlite3* raw = nullptr;
                expect((sqlite3_open_v2((tmp.dir / "a/repository.sqlite").string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) >> fatal);
                std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{raw, sqlite3_close};
                auto count = [&](const char* sql) {
                    sqlite3_stmt* query = nullptr;
                    expect((sqlite3_prepare_v2(raw, sql, -1, &query, nullptr) == SQLITE_OK) >> fatal);
                    expect(sqlite3_step(query) == SQLITE_ROW);
                    auto value = sqlite3_column_int64(query, 0);
                    sqlite3_finalize(query);
                    return static_cast<long long>(value);
                };
                if (boundary == "before_commit") {
                    expect(state.frontier == head);
                    expect(text(state, "x") == "before");
                    expect(count("SELECT COUNT(*) FROM events") == 1);
                    expect(count("SELECT COUNT(*) FROM revisions") == 2);
                    expect(count("SELECT COUNT(*) FROM manifest_files") == 1);
                } else {
                    expect(text(state, "x") == "after");
                    expect(count("SELECT COUNT(*) FROM events") == 2);
                    expect(count("SELECT COUNT(*) FROM revisions") == 3);
                    expect(count("SELECT COUNT(*) FROM manifest_files") == 2);
                }
            }
        };
#endif
        "a corrupted manifest size is refused and a rebuild repairs it"_test = [] {
            test::scratch tmp{"manifest-size"};
            std::string digest;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                digest = change(node, "size-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "abc"}).source_digest;
            }
            {
                sqlite3* raw = nullptr;
                expect((sqlite3_open_v2((tmp.dir / "a/repository.sqlite").string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) >> fatal);
                std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{raw, sqlite3_close};
                expect(sqlite3_exec(raw, "UPDATE manifest_files SET size=size+1", nullptr, nullptr, nullptr) == SQLITE_OK);
            }
            {
                nodes::local_node damaged{tmp.dir / "a"};
                expect(attempt(damaged, {.action = "stat", .target = "x"}).code == "missing_objects");
                expect(attempt(damaged, {.action = "tree"}).code == "missing_objects");
            }
            {
                nodes::local_node audited{tmp.dir / "a", true};
                expect(text(inspect(audited), "x") == "abc");
            }
            nodes::local_node reopened{tmp.dir / "a"};
            expect(inspect(reopened).source_digest == digest);
        };
        "corrupted content cache is refused and a rebuild repairs it"_test = [] {
            test::scratch tmp{"node-state-integrity"};
            std::string digest, receipt;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                auto seeded = change(node, "integrity-seed", {}, {.kind = std::string{"create"}, .path = "x", .text = "verified"});
                digest = seeded.source_digest;
                receipt = seeded.receipt;
            }
            {
                sqlite3* raw = nullptr;
                if (sqlite3_open_v2((tmp.dir / "a/repository.sqlite").string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
                    throw std::runtime_error("open state fixture");
                std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{raw, sqlite3_close};
                expect(sqlite3_exec(raw, "UPDATE blobs SET data=x'00'", nullptr, nullptr, nullptr) == SQLITE_OK);
            }
            {
                nodes::local_node corrupted{tmp.dir / "a"};
                auto rejected = attempt(corrupted, {.action = "read", .target = "x"});
                expect(rejected.code == "storage_failure" || rejected.code == "invalid_record") << rejected.code;
            }
            {
                nodes::local_node audited{tmp.dir / "a", true};
                expect(text(inspect(audited), "x") == "verified");
            }
            nodes::local_node reopened{tmp.dir / "a"};
            auto restored = inspect(reopened);
            expect(restored.source_digest == digest);
            expect(text(restored, "x") == "verified");
            expect(good(reopened, {.action = "receipt", .target = receipt}).receipt == receipt);
        };
        "incremental state retains offline CRDT anchors"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "snapshot-a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "A", .offset = 1});
            auto saved = good(p.a, {.action = "identity"});
            expect(saved.snapshot_events == 2u);
            expect(!saved.state_snapshot.empty());
            auto tail = change(p.a, "snapshot-tail", a.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "T", .offset = 4});
            nodes::local_node reopened{p.tmp.dir / "a"};
            auto restored = good(reopened, {.action = "identity"});
            expect(restored.snapshot_events == 3u);
            expect(restored.replayed_events == 0u);
            expect(restored.loaded_records < 30u);
            expect(text(inspect(reopened), "x") == "aAbcTd");
            expect(inspect(reopened).frontier == tail.frontier);
            expect(text(good(reopened, {.action = "read", .revision = a.version, .target = "x"}), "x") == "aAbcd");
            expect(attempt(reopened, {.action = "tree", .revision = "not-a-version"}).code == "invalid_record");
            auto tree = good(reopened, {.action = "tree", .revision = a.version});
            expect(tree.files.size() == 1u);
            expect(tree.files.front().text.empty());

            change(*p.b, "snapshot-offline", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 2});
            auto merged = inspect(reopened);
            expect(merged.conflicts.empty());
            auto reversed =
                good(reopened, {.action = "reverse", .operation = op("snapshot-reverse"), .expected = merged.frontier, .target = a.frontier.front()});
            auto expected = text(inspect(reopened), "x");
            expect(expected.find('A') == std::string::npos);
            expect(expected.find('B') != std::string::npos);
            expect(expected.find('T') != std::string::npos);
            nodes::local_node resumed{p.tmp.dir / "a"};
            auto identity = good(resumed, {.action = "identity"});
            expect(identity.replayed_events == 0u);
            expect(text(inspect(resumed), "x") == expected);
            auto receipt = good(resumed, {.action = "receipt", .target = reversed.receipt});
            expect(receipt.receipt == reversed.receipt);
            nodes::local_node audited{p.tmp.dir / "a", true};
            expect(text(inspect(audited), "x") == expected);
        };
        "local record bounds preserve the prior durable source"_test = [] {
            {
                value::array items(nodes::codec::collection_limit + 1);
                value::map body;
                body.emplace("items", std::move(items));
                try {
                    (void)nodes::codec::encode(std::move(body));
                    expect(false);
                } catch (const nodes::error& e) {
                    expect(e.code == "resource_limit");
                }
            }
            peers p;
            auto seed = p.seed();
            auto rejected = attempt(p.a, {.action = "apply",
                                          .operation = op("oversized-result"),
                                          .expected = seed.frontier,
                                          .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "never accepted"}},
                                          .result = std::string(nodes::codec::record_limit, 'x')});
            expect(rejected.code == "resource_limit");
            expect(inspect(p.a).frontier == seed.frontier);
            nodes::local_node reopened{p.tmp.dir / "a"};
            expect(inspect(reopened).frontier == seed.frontier);
            expect(text(inspect(reopened), "x") == "abcd");
        };
        "R01-R03 independent insertion deletion and replacement reverse selectively"_test = [] {
            for (auto count : {0u, 1u})
                for (auto inserted : {std::string{}, std::string{"AA"}}) {
                    if (count == 0 && inserted.empty())
                        continue;
                    peers p;
                    auto seed = p.seed();
                    auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = inserted, .offset = 1, .count = count});
                    change(*p.b, "b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "BB", .offset = 3});
                    auto merged = inspect(p.a);
                    auto expected = "a" + inserted + (count ? "cBBd" : "bcBBd");
                    expect(text(merged, "x") == expected);
                    expect(merged.conflicts.empty());
                    auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == "abcBBd");
                    good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == expected);
                }
        };
        "Unicode scalar deletion and complete run replacement preserve independent effects"_test = [] {
            {
                peers p;
                auto seed = p.seed("x", "a😀éb");
                auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .offset = 1, .count = 1});
                change(*p.b, "b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "Ω", .offset = 3});
                auto merged = inspect(p.a);
                expect(text(merged, "x") == "aéΩb");
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "a😀éΩb");
                good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "aéΩb");
            }
            {
                peers p;
                auto seed = p.seed("x", "😀é");
                auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"write"}, .path = "x", .text = "替换"});
                change(*p.b, "b", seed.frontier, {.kind = std::string{"mode"}, .path = "x", .executable = true});
                auto merged = inspect(p.a);
                expect(text(merged, "x") == "替换");
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "😀é");
                expect(inspect(p.a).files.front().executable);
                good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "替换");
                expect(inspect(p.a).files.front().executable);
            }
        };
        "K03 evidence-only batches leave source digests unchanged"_test = [] {
            peers p;
            auto seed = p.seed();
            auto before = inspect(p.a);
            expect(before.files.size() == 1u);
            auto& file = before.files.front();
            // The version's state root is SHA-256 over its canonical sorted manifest tuples.
            content::sha256 manifest;
            manifest.update(nodes::codec::encode(value::map{
                {"digest", file.digest}, {"executable", file.executable}, {"file", file.id}, {"kind", file.kind}, {"path", file.path}, {"size", file.size}}));
            auto root = content::to_hex(manifest.finish()).str();
            expect(before.source_digest ==
                   nodes::digest_of(nodes::codec::encode(value::map{{"conflicts", nodes::strings(before.conflicts)}, {"files", root}})));
            auto evidence =
                good(p.a, {.action = "apply", .operation = op("evidence"), .expected = seed.frontier, .intent = "Review only", .result = "Actual observation"});
            auto after = inspect(p.a);
            expect(before.source_digest == after.source_digest);
            expect(before.version != after.version);
            expect(evidence.result == "Actual observation");
            change(*p.b, "unrelated", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 3});
            auto fixed = good(p.a, {.action = "inspect", .source = seed.frontier});
            expect(fixed.version == before.version);
            expect(fixed.source_digest == before.source_digest);
            expect(text(fixed, "x") == "abcd");
        };
        "cross-worldline promotion publishes one independent atomic contribution"_test = [] {
            peers p;
            auto seed = p.seed();
            auto fork = good(p.a, {.action = "fork", .worldline = "experiment", .operation = op("promotion-fork"), .source = seed.frontier});
            auto edited = good(p.a, {.action = "apply",
                                     .worldline = "experiment",
                                     .operation = op("promotion-edit"),
                                     .expected = fork.frontier,
                                     .edits = {{.kind = std::string{"write"}, .path = "x", .text = "experiment"},
                                               {.kind = std::string{"binary_create"}, .path = "asset", .text = "00ff", .executable = true}}});
            auto independent = change(p.a, "promotion-independent", seed.frontier, {.kind = std::string{"create"}, .path = "local", .text = "retained"});
            auto diff = good(p.a, {.action = "diff", .worldline = "main", .expected = edited.frontier, .target = "experiment"});
            expect(diff.changes.size() == 3u);
            nodes::node_request request{.action = "promote",
                                        .operation = op("promotion-accept"),
                                        .expected = independent.frontier,
                                        .source = edited.frontier,
                                        .target = "experiment",
                                        .intent = "Adopt the experiment"};
            auto promoted = good(p.a, request);
            auto current = inspect(p.a);
            expect(text(current, "x") == "experiment");
            expect(text(current, "local") == "retained");
            expect(text(current, "asset") == "00ff");
            expect(current.files[0].executable || current.files[1].executable || current.files[2].executable);
            auto history = good(p.a, {.action = "history"});
            expect(history.history.size() == 3u);
            expect(history.history.back().parents == independent.frontier);
            auto replay = good(p.a, request);
            expect(replay.replayed && replay.version == promoted.version);
            expect(text(inspect(*p.b), "asset") == "00ff") << "every connection sees the promoted contribution";
            auto source = good(p.a, {.action = "inspect", .worldline = "experiment"});
            expect(source.version == edited.version);
            auto remaining = good(p.a, {.action = "diff", .worldline = "main", .expected = edited.frontier, .target = "experiment"});
            expect(remaining.changes.size() == 1u && remaining.changes.front().after->path == "local");
            auto divergent = good(p.a, {.action = "apply",
                                        .worldline = "experiment",
                                        .operation = op("promotion-diverge"),
                                        .expected = edited.frontier,
                                        .edits = {{.kind = std::string{"write"}, .path = "x", .text = "different"}}});
            request.operation = op("promotion-conflict");
            request.expected = promoted.frontier;
            request.source = divergent.frontier;
            expect(attempt(p.a, request).code == nodes::code::source_conflict);
            expect(inspect(p.a).version == promoted.version);
        };
        "fixed-version comparison and history retain exact source and tool evidence"_test = [] {
            peers p;
            auto seed = p.seed();
            auto changed = good(p.a, {.action = "apply",
                                      .operation = op("observed"),
                                      .expected = seed.frontier,
                                      .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "AA", .offset = 1}},
                                      .intent = "Add the selected contribution",
                                      .result = "Retained actual result",
                                      .tool = "editor.replace/7"});
            auto difference = good(p.a, {.action = "compare", .expected = seed.frontier, .source = changed.frontier});
            expect(difference.changes.size() == 1u);
            expect(difference.base_version == seed.version);
            expect(difference.version == changed.version);
            expect(difference.changes[0].before->text == "abcd");
            expect(difference.changes[0].after->text == "aAAbcd");
            expect(difference.changes[0].before->id == difference.changes[0].after->id);
            auto history = good(p.a, {.action = "history", .source = changed.frontier});
            expect(history.history.size() == 2u);
            auto found = std::ranges::find(history.history, op("observed"), &nodes::history_entry::operation);
            expect(found != history.history.end());
            expect(found->author == p.a.author_id());
            expect(found->actor == "");
            expect(found->intent == "Add the selected contribution");
            expect(found->result == "Retained actual result");
            expect(found->tool == "editor.replace/7");
            expect(found->event == changed.receipt) << "a history entry is named by the batch its receipt answers for";
            auto contribution = good(p.a, {.action = "contribution", .target = changed.frontier.front()});
            expect(contribution.changes.size() == 1u);
            expect(contribution.changes.front().after->text == "aAAbcd");
            expect(good(p.a, {.action = "receipt", .target = changed.receipt}).result == "Retained actual result");
        };
        "K05 accepted retries return the original receipt after later head changes"_test = [] {
            peers p;
            auto seed = p.seed();
            nodes::node_request request{.action = "apply",
                                        .operation = op("first"),
                                        .expected = seed.frontier,
                                        .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "A", .offset = 1}},
                                        .result = "original result"};
            auto first = good(p.a, request);
            change(p.a, "later", first.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 4});
            auto retry = good(p.a, request);
            expect(retry.replayed);
            expect(retry.receipt == first.receipt);
            expect(retry.version == first.version);
            expect(retry.result == "original result");
            request.tool = "another.tool/1";
            expect(attempt(p.a, request).code == "identity_conflict");
            request.tool.clear();
            request.result = "different";
            expect(attempt(p.a, request).code == "identity_conflict");
        };
        "failed request identities survive changes to the attempted input"_test = [] {
            peers p;
            auto seed = p.seed();
            nodes::node_request in{
                .action = "apply", .operation = op("invalid"), .expected = seed.frontier, .edits = {{.kind = std::string{"edit"}, .path = "x", .offset = 50}}};
            expect(attempt(p.a, in).code == "invalid_record");
            in.edits.front().offset = 1;
            in.edits.front().text = "changed";
            expect(attempt(p.a, in).code == "identity_conflict");
            expect(inspect(p.a).frontier == seed.frontier);
        };
        "an abandoned operation never runs, and one that ran answers abandon with its receipt"_test = [] {
            peers p;
            auto seed = p.seed();
            nodes::node_request lost{.action = "apply",
                                     .operation = op("lost"),
                                     .expected = seed.frontier,
                                     .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "A", .offset = 1}}};
            expect(good(p.a, {.action = "abandon", .operation = lost.operation}).receipt.empty());
            expect(attempt(p.a, lost).code == "identity_conflict");
            expect(good(p.a, {.action = "abandon", .operation = lost.operation}).receipt.empty());
            expect(attempt(p.a, {.action = "receipt", .operation = lost.operation}).code == "identity_conflict");
            expect(inspect(p.a).frontier == seed.frontier);
            auto ran = change(p.a, "ran", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 1});
            expect(good(p.a, {.action = "abandon", .operation = op("ran")}).receipt == ran.receipt);
            expect(good(p.a, {.action = "apply",
                              .operation = op("ran"),
                              .expected = seed.frontier,
                              .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 1}}})
                       .replayed);
        };
        "R04-R05 file and directory moves preserve another author's text"_test = [] {
            for (bool directory : {false, true}) {
                peers p;
                auto seed = good(p.a, {.action = "apply",
                                       .operation = op("seed"),
                                       .edits = {{.kind = std::string{"create"}, .path = "src/a", .text = "abcd"},
                                                 {.kind = std::string{"create"}, .path = "src/b", .text = "second"}}});
                auto a = change(p.a, "move", seed.frontier,
                                {.kind = directory ? "directory_move" : "move", .path = directory ? "src" : "src/a", .target = directory ? "lib" : "lib/a"});
                change(*p.b, "edit", seed.frontier, {.kind = std::string{"edit"}, .path = "src/a", .text = "BB", .offset = 3});
                auto merged = inspect(p.a);
                expect(text(merged, "lib/a") == "abcBBd");
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "src/a") == "abcBBd");
                expect(text(inspect(p.a), "src/b") == "second");
                good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "lib/a") == "abcBBd");
            }
        };
        "R06 later independent text changes survive reversal and redo"_test = [] {
            for (auto count : {0u, 1u})
                for (auto inserted : {std::string{}, std::string{"AA"}}) {
                    if (count == 0 && inserted.empty())
                        continue;
                    peers p;
                    auto seed = p.seed();
                    auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = inserted, .offset = 1, .count = count});
                    change(*p.b, "b", a.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "BB", .offset = 3 + inserted.size() - count});
                    auto merged = inspect(p.a);
                    auto before = text(merged, "x");
                    auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == "abcBBd");
                    good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == before);
                }
        };
        "R07 sibling removals reverse independently while one shared claim stays a conflict"_test = [] {
            peers p;
            auto seed = p.seed();
            // One writer identity stands in for three callers: each reads before the other publishes, so each removal is a
            // contribution of its own over the seed. Two removals of *different* characters reverse independently.
            auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .offset = 1, .count = 1});
            auto b = change(*p.b, "b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .offset = 2, .count = 1});
            expect(a.receipt != b.receipt) << "each removal is its own contribution";
            auto merged = good(p.a, {.action = "inspect"});
            expect(merged.frontier.size() == 2u) << "both removals are the current frontier";
            expect(text(merged, "x") == "ad") << "sibling removals leave the ends";
            expect(merged.conflicts.empty()) << "removals of different characters do not conflict";
            auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse-a"), .expected = merged.frontier, .target = a.receipt});
            expect(text(inspect(p.a), "x") == "abd") << "the other removal remains";
            good(p.a, {.action = "reverse", .operation = op("reverse-b"), .expected = reversed.frontier, .target = b.receipt});
            expect(text(inspect(p.a), "x") == "abcd");
        };
        "R07b one character removed twice stays a recorded conflict"_test = [] {
            peers p;
            auto seed = p.seed();
            change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .offset = 1, .count = 1});
            change(*p.b, "b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .offset = 1, .count = 1});
            auto merged = inspect(p.a);
            expect(text(merged, "x") == "acd") << "both claims select the same character";
            expect(merged.frontier.size() == 2u);
            expect(!merged.conflicts.empty()) << "one character removed twice is a recorded conflict";
        };
        "R08 hidden insertion anchors still retain another author's descendants"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "AA", .offset = 1});
            change(*p.b, "b", a.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "BB", .offset = 2});
            auto merged = inspect(p.a);
            good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
            expect(text(inspect(p.a), "x") == "aBBbcd");
        };
        "R09 recorded source dependencies block an unreviewed reversal"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "AA", .offset = 1});
            good(*p.b, {.action = "apply",
                        .operation = op("b"),
                        .expected = a.frontier,
                        .edits = {{.kind = std::string{"edit"}, .path = "x", .text = "dependent", .offset = 2, .count = 1}}});
            auto merged = inspect(p.a);
            auto reversal = attempt(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
            expect(reversal.code == "dependency_conflict");
            expect(inspect(p.a).frontier == merged.frontier);
            expect(!reversal.conflicts.empty());
            auto reviewed = good(p.a, {.action = "reverse",
                                       .operation = op("reviewed-reverse"),
                                       .expected = merged.frontier,
                                       .target = a.frontier.front(),
                                       .decisions = reversal.conflicts});
            expect(reviewed.conflicts.empty());
            expect(text(inspect(p.a), "x") == "adependentbcd");
        };
        "R10 concurrent activation choices require an explicit fixed-frontier choice"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "AA", .offset = 1});
            auto x = good(p.a, {.action = "reverse", .operation = op("x-reverse"), .expected = a.frontier, .target = a.frontier.front()});
            auto y = good(*p.b, {.action = "reverse", .operation = op("y-reverse"), .expected = a.frontier, .target = a.frontier.front()});
            // The second caller redoes from the state it published, before it saw the other caller's reversal.
            good(*p.b, {.action = "redo", .operation = op("y-redo"), .expected = {y.receipt}, .target = a.frontier.front()});
            auto merged = inspect(p.a);
            expect(!merged.conflicts.empty());
            auto resolved = good(p.a, {.action = "resolve",
                                       .operation = op("choose-active"),
                                       .expected = merged.frontier,
                                       .target = a.frontier.front(),
                                       .activation = true,
                                       .decisions = merged.conflicts});
            expect(resolved.conflicts.empty());
            expect(text(inspect(p.a), "x") == "aAAbcd");
        };
        "K09 structural and overlapping conflicts are visible and block publication decisions"_test = [] {
            for (auto scenario : {"same_gap", "replace_replace", "replace_delete"}) {
                peers p;
                auto seed = p.seed();
                bool insertion = std::string_view{scenario} == "same_gap";
                change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "A", .offset = 1, .count = insertion ? 0u : 1u});
                change(*p.b, "b", seed.frontier,
                       {.kind = std::string{"edit"},
                        .path = "x",
                        .text = std::string_view{scenario} == "replace_delete" ? "" : "B",
                        .offset = 1,
                        .count = insertion ? 0u : 1u});
                auto merged = inspect(p.a);
                expect(!merged.conflicts.empty()) << scenario;
                auto resolved = good(p.a, {.action = "resolve", .operation = op("resolve"), .expected = merged.frontier, .decisions = merged.conflicts});
                expect(resolved.conflicts.empty());
            }
        };
        "file modes and removal reverse while retaining another author's text"_test = [] {
            for (auto kind : {"mode", "remove"}) {
                peers p;
                auto seed = p.seed();
                auto a = change(p.a, "a", seed.frontier, {.kind = kind, .path = "x", .executable = true});
                change(*p.b, "b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "BB", .offset = 3});
                auto merged = inspect(p.a);
                if (std::string_view{kind} == "mode")
                    expect(merged.files.front().executable);
                else
                    expect(!merged.conflicts.empty());
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                expect(reversed.conflicts.empty());
                expect(text(inspect(p.a), "x") == "abcBBd");
                expect(!inspect(p.a).files.front().executable);
                auto redone = good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                if (std::string_view{kind} == "mode") {
                    expect(inspect(p.a).files.front().executable);
                    expect(text(inspect(p.a), "x") == "abcBBd");
                } else {
                    expect(!redone.conflicts.empty());
                    good(p.a, {.action = "reverse", .operation = op("restore-again"), .expected = redone.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == "abcBBd");
                }
            }
        };
        "K09 rename-delete duplicate paths directory collisions and binary alternatives"_test = [] {
            for (auto scenario : {"rename_delete", "duplicate", "directory", "binary"}) {
                peers p;
                nodes::node_reply seed;
                if (std::string_view{scenario} == "binary")
                    seed =
                        good(p.a, {.action = "apply", .operation = op("seed"), .edits = {{.kind = std::string{"binary_create"}, .path = "x", .text = "0102"}}});
                else
                    seed = p.seed();
                auto second = [&](nodes::edit edit) {
                    return attempt(*p.b, {.action = "apply", .operation = op("b"), .expected = seed.frontier, .edits = {edit}});
                };
                if (std::string_view{scenario} == "rename_delete") {
                    change(p.a, "a", seed.frontier, {.kind = std::string{"move"}, .path = "x", .target = "moved"});
                    expect(second({.kind = std::string{"remove"}, .path = "x"}).code == "ok");
                    expect(!inspect(p.a).conflicts.empty()) << "a move and a removal of one file stay a recorded conflict";
                } else if (std::string_view{scenario} == "binary") {
                    change(p.a, "a", seed.frontier, {.kind = std::string{"binary"}, .path = "x", .text = "0001"});
                    expect(second({.kind = std::string{"binary"}, .path = "x", .text = "ff00"}).code == "ok");
                    expect(!inspect(p.a).conflicts.empty()) << "two binary alternatives stay a recorded conflict";
                } else {
                    change(p.a, "a", seed.frontier, {.kind = std::string{"create"}, .path = "new", .text = "A"});
                    auto path = std::string_view{scenario} == "duplicate" ? "new" : "new/child";
                    auto refused = second({.kind = std::string{"create"}, .path = std::string{path}, .text = "B"});
                    expect(refused.code == nodes::code::source_conflict) << scenario << ": " << refused.code;
                }
            }
        };
        "K10 a new overlapping candidate is outside a prior conflict decision"_test = [] {
            peers p;
            auto seed = p.seed();
            change(p.a, "a", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "A", .offset = 1});
            change(*p.b, "b", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "B", .offset = 1});
            auto merged = inspect(p.a);
            auto resolved = good(p.a, {.action = "resolve", .operation = op("resolve"), .expected = merged.frontier, .decisions = merged.conflicts});
            expect(resolved.conflicts.empty());
            change(*p.c, "c", seed.frontier, {.kind = std::string{"edit"}, .path = "x", .text = "C", .offset = 1});
            expect(!inspect(p.a).conflicts.empty()) << "a later overlapping contribution is outside the prior decision";
        };
        "native source reads remain available while the writer entry is occupied"_test = [] {
            test::scratch tmp{"native-reader"};
            storage::native_nodes registry;
            auto seed = registry.with(tmp.dir / "node", [](nodes::local_node& node) {
                node.initialize();
                return node.call({.action = "apply",
                                  .operation = content::hash_of("reader-seed").str(),
                                  .owner = node.node_id(),
                                  .edits = {{.kind = std::string{"create"}, .path = "x", .text = "retained"},
                                            {.kind = std::string{"create"}, .path = "y", .text = "retained"}}});
            });
            expect((seed.code == "ok") >> fatal);
            std::promise<void> entered, finish;
            auto release = finish.get_future();
            auto occupied = entered.get_future();
            std::jthread writer([&] {
                registry.with(tmp.dir / "node", [&](nodes::local_node&) {
                    entered.set_value();
                    release.wait();
                });
            });
            occupied.wait();
            auto read = std::async(std::launch::async, [&] {
                return registry.with(tmp.dir / "node", [](nodes::local_node& node) { return node.call({.action = "read", .target = "x"}); }, true);
            });
            auto available = read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            finish.set_value();
            auto result = read.get();
            expect(available);
            expect(result.version == seed.version);
            expect((result.files.size() == 1u) >> fatal);
            expect(result.files.front().text == "retained");
            writer.join();
            std::atomic<bool> done = false, consistent = true;
            std::barrier start{2};
            std::jthread publications([&] {
                try {
                    auto frontier = seed.frontier;
                    start.arrive_and_wait();
                    for (int i = 0; i < 32; ++i) {
                        auto accepted = registry.with(tmp.dir / "node", [&](nodes::local_node& node) {
                            return node.call({.action = "apply",
                                              .operation = content::hash_of("reader-publication-" + std::to_string(i)).str(),
                                              .owner = node.node_id(),
                                              .expected = frontier,
                                              .edits = {{.kind = std::string{"edit"}, .path = "x", .text = std::to_string(i) + ":"},
                                                        {.kind = std::string{"edit"}, .path = "y", .text = std::to_string(i) + ":"}}});
                        });
                        if (accepted.code != "ok")
                            consistent = false;
                        frontier = accepted.frontier;
                    }
                } catch (...) {
                    consistent = false;
                }
                done = true;
            });
            start.arrive_and_wait();
            int observations = 0;
            do {
                auto observed = registry.with(tmp.dir / "node", [](nodes::local_node& node) { return node.call({.action = "inspect"}); }, true);
                if (observed.code != "ok" || observed.files.size() != 2 || observed.files[0].text != observed.files[1].text)
                    consistent = false;
                ++observations;
            } while (!done || observations < 8);
            publications.join();
            expect(consistent.load());
            expect(observations >= 8);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"node">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
