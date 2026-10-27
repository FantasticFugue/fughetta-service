#include <sqlite3.h>
import std;
import boost.ut;
import content;
import codec;
import kernel;
import archive;
import index;
import kernel;
import engine;
import storage;
import support;

namespace {
    using namespace boost::ut;
    using nodes::value;
    auto fixture_root(std::filesystem::path root, std::string role) -> std::filesystem::path {
        std::filesystem::create_directories(root);
        for (auto kind : {"author", "node"}) {
            auto bytes = nodes::unhex(content::hash_of("node-fixture-" + std::string(kind) + "-" + role));
            std::ofstream out(root / (std::string(kind) + ".key"), std::ios::binary);
            out.write(bytes.data(), bytes.size());
        }
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
    void exchange(nodes::local_node& from, nodes::local_node& to) {
        auto offered = good(from, {.action = "offer"});
        auto have = good(to, {.action = "offer"});
        std::set<std::string> present(have.objects.begin(), have.objects.end());
        std::vector<std::string> needed;
        for (auto& id : offered.objects)
            if (!present.contains(id))
                needed.push_back(id);
        for (auto& part : from.objects(needed))
            good(to, {.action = "receive", .parts = {part}});
        auto result = good(to, {.action = "offer"});
        expect(result.complete) << "complete object closure";
    }
    struct peers {
        test::scratch tmp{"node"};
        nodes::local_node a{fixture_root(tmp.dir / "a", "a")};
        std::unique_ptr<nodes::local_node> b, c;
        peers() {
            auto genesis = good(a, {.action = "initialize", .genesis_nonce = op("node-fixture-genesis")});
            b = std::make_unique<nodes::local_node>(fixture_root(tmp.dir / "b", "b"), genesis.project);
            c = std::make_unique<nodes::local_node>(fixture_root(tmp.dir / "c", "c"), genesis.project);
            good(a, {.action = "policy", .authors = {a.author_id(), b->author_id(), c->author_id()}, .nodes = {a.node_id(), b->node_id(), c->node_id()}});
            exchange(a, *b);
            exchange(a, *c);
        }
        auto seed(std::string path = "x", std::string source = "abcd") -> nodes::node_reply {
            auto out = good(a, {.action = "apply", .operation = op("seed"), .edits = {{.kind = "create", .path = path, .text = source}}});
            exchange(a, *b);
            exchange(a, *c);
            good(*b, {.action = "integrate", .operation = op("join-b"), .source = out.frontier});
            good(*c, {.action = "integrate", .operation = op("join-c"), .source = out.frontier});
            return out;
        }
    };
    auto change(nodes::local_node& node, std::string name, const std::vector<std::string>& base, nodes::edit edit) -> nodes::node_reply {
        return good(node, {.action = "apply", .operation = op(name), .expected = base, .edits = {std::move(edit)}});
    }
    auto inspect(nodes::local_node& node) -> nodes::node_reply { return good(node, {.action = "inspect"}); }

    const auto cases = [] {
        "DCN1 rejects ambiguous encodings and preserves typed values"_test = [] {
            value input =
                value::map{{"array", value::array{value{}, true, false, std::uint64_t{42}, nodes::bytes{std::string{"a\0b", 3}}}}, {"text", "hello 世界"}};
            auto bytes = nodes::codec::encode(input);
            expect(nodes::codec::decode(bytes) == input);
            expect(nodes::digest_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
            expect(nodes::digest_of("abc") == content::hash_of("abc"));
            expect(throws<nodes::error>([&] { nodes::codec::decode(bytes + "x"); }));
            expect(throws<nodes::error>([&] { nodes::codec::decode(std::string_view(bytes).substr(0, bytes.size() - 1)); }));
            for (auto invalid : {std::string{"\xc0\xaf", 2}, std::string{"\xed\xa0\x80", 3}, std::string{"\xf4\x90\x80\x80", 4}})
                expect(throws<nodes::error>([&] { nodes::scalars(invalid); }));
            auto unicode = nodes::scalars("a😀é");
            expect(unicode.size() == 3u);
            std::string out;
            for (auto cp : unicode)
                nodes::append_scalar(out, cp);
            expect(out == "a😀é");
        };
        "compressed archive bounds and asynchronous exchange files preserve bytes"_test = [] {
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
        "persistent Merkle indexes and sparse causal sets preserve exact states"_test = [] {
            nodes::persistent_index<std::string> forward, backward;
            for (int i = 0; i < 128; ++i) {
                auto key = std::format("key/{:03}", i);
                forward.set(key, key, nodes::digest_of(key));
            }
            for (int i = 127; i >= 0; --i) {
                auto key = std::format("key/{:03}", i);
                backward.set(key, key, nodes::digest_of(key));
            }
            expect(forward.digest() == backward.digest());
            auto original = forward;
            auto root = original.digest();
            forward.set("key/050", "changed", nodes::digest_of("changed"));
            forward.erase("key/090");
            forward.set("key/new", "added", nodes::digest_of("added"));
            std::vector<std::string> changed;
            original.difference(forward, [&](const std::string& key, const auto*, const auto*) { changed.push_back(key); });
            expect(changed == std::vector<std::string>{"key/050", "key/090", "key/new"});
            expect(original.digest() == root);
            expect(original.find("key/050")->second == "key/050");
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
                                      .edits = {{.kind = "edit", .path = "x", .text = inserted, .offset = offset, .count = count}}});
                auto work = cold.statistics();
                expect(work.projected_files - stats.projected_files <= 1u);
                expect(work.file_histories_read == stats.file_histories_read);
                expect(work.insertion_payloads_read == stats.insertion_payloads_read);
                expect(work.causal_headers_read - stats.causal_headers_read < 16u);
                expect(work.sequence_pages_read - stats.sequence_pages_read < 256u);
                nodes::local_node complete{p.tmp.dir / "a", {}, true};
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
                std::vector<nodes::edit> files{{.kind = "create", .path = "target", .text = "a"}};
                for (int i = 0; i < 64; ++i)
                    files.push_back({.kind = "create", .path = "unrelated/" + std::to_string(i), .text = std::string(1024, 'u')});
                frontier = good(node, {.action = "apply", .operation = op("append-tail-seed"), .edits = std::move(files)}).frontier;
            }
            nodes::local_node cold{tmp.dir / "a"};
            auto before = cold.statistics();
            auto edited = change(cold, "append-tail-edit", frontier, {.kind = "edit", .path = "target", .text = "z"});
            auto after = cold.statistics();
            expect(after.causal_blocks_read == before.causal_blocks_read);
            expect(after.projected_files - before.projected_files == 1u);
            expect(after.file_payloads_read - before.file_payloads_read == 1u);
            expect(after.file_histories_read == before.file_histories_read);
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
                head = change(node, "metadata-seed", {}, {.kind = "create", .path = "x", .text = std::string(8192, 'a')}).frontier;
            }
            nodes::local_node cold{tmp.dir / "a"};
            auto before = cold.statistics();
            auto mode = change(cold, "metadata-mode", head, {.kind = "mode", .path = "x", .executable = true});
            auto moved = change(cold, "metadata-move", mode.frontier, {.kind = "move", .path = "x", .target = "y"});
            auto removed = change(cold, "metadata-remove", moved.frontier, {.kind = "remove", .path = "y"});
            auto after = cold.statistics();
            expect(after.file_payloads_read == before.file_payloads_read);
            expect(after.file_histories_read == before.file_histories_read);
            expect(after.sequence_pages_read == before.sequence_pages_read);
            expect(good(cold, {.action = "tree"}).files.empty());
            nodes::local_node complete{tmp.dir / "a", {}, true};
            expect(inspect(complete).source_digest == removed.source_digest);
        };
        "historical queries restore activation effects from an authenticated current base"_test = [] {
            test::scratch tmp{"demand-history-control"};
            std::string version;
            std::vector<std::string> restored;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                auto seed = change(node, "history-control-seed", {}, {.kind = "create", .path = "x", .text = "ab"});
                auto edited = change(node, "history-control-edit", seed.frontier, {.kind = "edit", .path = "x", .text = "X", .offset = 1});
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
                files.push_back({.kind = "create", .path = std::to_string(i), .text = "v"});
            auto seed = good(p.a, {.action = "apply", .operation = op("delta-seed"), .edits = std::move(files)});
            exchange(p.a, *p.b);
            good(*p.b, {.action = "integrate", .operation = op("delta-fork"), .source = seed.frontier});
            auto local = change(p.a, "delta-local", seed.frontier, {.kind = "edit", .path = "0", .text = "A", .offset = 1});
            auto remote = change(*p.b, "delta-remote", seed.frontier, {.kind = "edit", .path = "1", .text = "B", .offset = 1});
            nodes::local_node cold{p.tmp.dir / "a"};
            auto before = cold.statistics();
            exchange(*p.b, cold);
            auto after = cold.statistics();
            expect(after.projected_files - before.projected_files <= 4u);
            expect(inspect(cold).frontier == local.frontier);
            auto merged = good(cold, {.action = "integrate", .operation = op("delta-integrate"), .expected = local.frontier, .source = remote.frontier});
            expect(text(inspect(cold), "0") == "vA");
            expect(text(inspect(cold), "1") == "vB");
            nodes::local_node complete{p.tmp.dir / "a", {}, true};
            expect(inspect(complete).source_digest == merged.source_digest);
        };
        "cold branch integration and contribution reversal retain the complete renderer result"_test = [] {
            peers p;
            auto seed = p.seed("x", "a😀bcé");
            auto a = change(p.a, "demand-branch-a", seed.frontier, {.kind = "edit", .path = "x", .text = "审", .offset = 1});
            auto b = change(*p.b, "demand-branch-b", seed.frontier, {.kind = "edit", .path = "x", .text = "尾", .offset = 5});
            nodes::local_node cold{p.tmp.dir / "a"};
            exchange(*p.b, cold);
            auto merged = good(cold, {.action = "integrate", .operation = op("demand-integrate"), .expected = a.frontier, .source = b.frontier});
            auto reversed = good(cold, {.action = "reverse", .operation = op("demand-reverse"), .expected = merged.frontier, .target = a.frontier.front()});
            expect(text(inspect(cold), "x") == "a😀bcé尾");
            nodes::local_node reopened{p.tmp.dir / "a"};
            auto redone = good(reopened, {.action = "redo", .operation = op("demand-redo"), .expected = reversed.frontier, .target = a.frontier.front()});
            nodes::local_node complete{p.tmp.dir / "a", {}, true};
            expect(inspect(reopened).source_digest == inspect(complete).source_digest);
            expect(text(inspect(reopened), "x") == "a审😀bcé尾");
            expect(good(reopened, {.action = "receipt", .target = merged.receipt}).receipt == merged.receipt);
            expect(redone.receipt != reversed.receipt);
        };
        "equal text on different worldlines retains distinct authenticated sequence roots"_test = [] {
            test::scratch tmp{"demand-equal-text"};
            std::vector<std::string> branch, main;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                auto seed = change(node, "equal-seed", {}, {.kind = "create", .path = "x", .text = "a"});
                auto fork = good(node, {.action = "fork", .worldline = "branch", .operation = op("equal-fork"), .source = seed.frontier});
                branch = fork.frontier;
                auto changed = change(node, "equal-change", seed.frontier, {.kind = "write", .path = "x", .text = "b"});
                auto restored = change(node, "equal-restore", changed.frontier, {.kind = "write", .path = "x", .text = "a"});
                main = restored.frontier;
            }
            nodes::local_node cold{tmp.dir / "a"};
            auto b = good(cold, {.action = "apply",
                                 .worldline = "branch",
                                 .operation = op("equal-branch-edit"),
                                 .expected = branch,
                                 .edits = {{.kind = "edit", .path = "x", .text = "Q", .offset = 1}}});
            auto m = change(cold, "equal-main-edit", main, {.kind = "edit", .path = "x", .text = "W", .offset = 1});
            expect(text(good(cold, {.action = "read", .worldline = "branch", .target = "x"}), "x") == "aQ");
            expect(text(inspect(cold), "x") == "aW");
            nodes::local_node complete{tmp.dir / "a", {}, true};
            expect(inspect(complete).source_digest == m.source_digest);
            expect(good(complete, {.action = "tree", .worldline = "branch"}).source_digest == b.source_digest);
        };
        "a batch places each file on a path that is free once it is done"_test = [] {
            test::scratch tmp{"placing"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = good(node, {.action = "apply",
                                    .operation = op("placing-seed"),
                                    .edits = {{.kind = "create", .path = "a", .text = "A"},
                                              {.kind = "create", .path = "b", .text = "B"},
                                              {.kind = "create", .path = "c", .text = "C"},
                                              {.kind = "create", .path = "d/e", .text = "E"}}});
            auto onto =
                attempt(node, {.action = "apply", .operation = op("onto"), .expected = seed.frontier, .edits = {{.kind = "move", .path = "a", .target = "b"}}});
            expect(onto.code == "source_conflict") << onto.reason;
            auto both = attempt(node, {.action = "apply",
                                       .operation = op("both"),
                                       .expected = seed.frontier,
                                       .edits = {{.kind = "move", .path = "a", .target = "z"}, {.kind = "move", .path = "c", .target = "z"}}});
            expect(both.code == "source_conflict") << "two files of one batch never land on one path: " << both.reason;
            auto created = attempt(node, {.action = "apply",
                                          .operation = op("created"),
                                          .expected = seed.frontier,
                                          .edits = {{.kind = "create", .path = "z", .text = "Z"}, {.kind = "move", .path = "a", .target = "z"}}});
            expect(created.code == "source_conflict") << created.reason;
            auto inward = attempt(node, {.action = "apply",
                                         .operation = op("inward"),
                                         .expected = seed.frontier,
                                         .edits = {{.kind = "directory_move", .path = "d", .target = "x"}, {.kind = "move", .path = "a", .target = "x/e"}}});
            expect(inward.code == "source_conflict") << inward.reason;
            auto swapped = good(node, {.action = "apply",
                                       .operation = op("swapped"),
                                       .expected = seed.frontier,
                                       .edits = {{.kind = "remove", .path = "b"}, {.kind = "move", .path = "a", .target = "b"}}});
            expect(text(good(node, {.action = "inspect"}), "b") == "A") << "a path the batch empties takes another file";
            expect(inspect(node).frontier == swapped.frontier);
        };
        "an apply may take the global scope for its own batch only"_test = [] {
            test::scratch tmp{"batch-scope"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "scope-seed", {}, {.kind = "create", .path = "x", .text = "abcd"});
            auto unlocked = attempt(node, {.action = "apply",
                                           .operation = op("unlocked"),
                                           .holder = "console",
                                           .expected = seed.frontier,
                                           .edits = {{.kind = "create", .path = "y", .text = "y"}}});
            expect(unlocked.code == "scope_locked");
            auto own = good(node, {.action = "apply",
                                   .operation = op("own"),
                                   .holder = "console",
                                   .expected = seed.frontier,
                                   .edits = {{.kind = "create", .path = "y", .text = "y"}},
                                   .scopes = {"::"}});
            expect(good(node, {.action = "locks", .holder = "console"}).locks.empty()) << "nothing stays held after the batch";
            good(node, {.action = "lock", .holder = "agent", .scopes = {"::"}});
            auto blocked = attempt(node, {.action = "apply",
                                          .operation = op("blocked"),
                                          .holder = "console",
                                          .expected = own.frontier,
                                          .edits = {{.kind = "create", .path = "z", .text = "z"}},
                                          .scopes = {"::"}});
            expect(blocked.code == "scope_locked" && blocked.reason.contains("agent")) << blocked.reason;
            auto narrow = attempt(node, {.action = "apply",
                                         .operation = op("narrow"),
                                         .holder = "agent",
                                         .expected = own.frontier,
                                         .edits = {{.kind = "create", .path = "z", .text = "z"}},
                                         .scopes = {"some-function"}});
            expect(narrow.code == "invalid_record");
        };
        "size limits hold for the state a publication leaves, reversals included"_test = [] {
            test::scratch tmp{"limits"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = good(node, {.action = "apply",
                                    .operation = op("limit-seed"),
                                    .edits = {{.kind = "create", .path = "a", .text = std::string(60, 'a')}},
                                    .file_limit = 64,
                                    .project_limit = 100});
            auto grown = attempt(node, {.action = "apply",
                                        .operation = op("grown"),
                                        .expected = seed.frontier,
                                        .edits = {{.kind = "edit", .path = "a", .text = std::string(5, 'g'), .offset = 0}},
                                        .file_limit = 64,
                                        .project_limit = 100});
            expect(grown.code == "resource_limit") << grown.reason;
            auto dropped = good(node, {.action = "apply",
                                       .operation = op("dropped"),
                                       .expected = seed.frontier,
                                       .edits = {{.kind = "remove", .path = "a"}},
                                       .file_limit = 64,
                                       .project_limit = 100});
            auto other = good(node, {.action = "apply",
                                     .operation = op("other"),
                                     .expected = dropped.frontier,
                                     .edits = {{.kind = "create", .path = "b", .text = std::string(60, 'b')}},
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
                                      .edits = {{.kind = "edit", .path = "b", .text = "", .offset = 0, .count = 30}},
                                      .file_limit = 10,
                                      .project_limit = 10});
            expect(text(good(node, {.action = "inspect"}), "b").size() == 30u) << "a file or project already past its limit may still shrink";
            expect(inspect(node).frontier == shrunk.frontier);
        };
        "rewriting a text longer than one collection erases it as ranges"_test = [] {
            peers p;
            auto seed = p.seed();
            auto length = nodes::codec::collection_limit + 1024;
            auto long_text = change(*p.b, "long-create", seed.frontier, {.kind = "create", .path = "long", .text = std::string(length, 'a')});
            auto middle = change(*p.b, "long-middle", long_text.frontier, {.kind = "edit", .path = "long", .text = "m", .offset = 10, .count = 10});
            auto rewritten = change(*p.b, "long-rewrite", middle.frontier, {.kind = "write", .path = "long", .text = std::string(length, 'b')});
            expect(text(good(*p.b, {.action = "inspect"}), "long") == std::string(length, 'b'));
            auto reversed =
                good(*p.b, {.action = "reverse", .operation = op("long-reverse"), .expected = rewritten.frontier, .target = rewritten.frontier.front()});
            auto restored = std::string(length - 9, 'a');
            restored[10] = 'm';
            expect(text(good(*p.b, {.action = "inspect"}), "long") == restored);
            exchange(*p.b, p.a);
            good(p.a, {.action = "integrate", .operation = op("long-join"), .expected = seed.frontier, .source = reversed.frontier});
            expect(text(good(p.a, {.action = "inspect"}), "long") == restored) << "a peer replays the range erasures";
            nodes::local_node reopened{p.tmp.dir / "a"};
            expect(text(good(reopened, {.action = "inspect"}), "long") == restored);
        };
        "an integration is held to the size limits of the worldline it joins"_test = [] {
            peers p;
            auto seed = p.seed();
            auto big = change(*p.b, "big", seed.frontier, {.kind = "create", .path = "big", .text = std::string(60, 'b')});
            exchange(*p.b, p.a);
            auto refused =
                attempt(p.a, {.action = "integrate", .operation = op("join-big"), .expected = seed.frontier, .source = big.frontier, .file_limit = 10});
            expect(refused.code == "resource_limit") << refused.reason;
            good(p.a, {.action = "integrate", .operation = op("join-big-unlimited"), .expected = seed.frontier, .source = big.frontier});
        };
        "writers in one worldline publish from the same observed state and converge"_test = [] {
            test::scratch tmp{"shared-line"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "shared-seed", {}, {.kind = "create", .path = "x", .text = "abcd"});
            auto halves = good(node, {.action = "anchor", .spans = {{.place = semantic::place{"x", 0, 2}}, {.place = semantic::place{"x", 2, 4}}}});
            good(node, {.action = "semantic.publish",
                        .generation = semantic::generation{.version = seed.version,
                                                           .environment = "test",
                                                           .service = "test",
                                                           .service_version = "1",
                                                           .scopes = {{.id = "left", .kind = "function", .range = halves.spans[0]},
                                                                      {.id = "right", .kind = "function", .range = halves.spans[1]}}}});
            good(node, {.action = "lock", .holder = "alice", .environment = "test", .scopes = {"left"}});
            auto taken = attempt(node, {.action = "lock", .holder = "bob", .environment = "test", .scopes = {"left"}});
            expect(taken.code == "scope_locked" && taken.reason.contains("alice")) << taken.reason;
            auto everything = attempt(node, {.action = "lock", .holder = "bob", .scopes = {"::"}});
            expect(everything.code == "scope_locked") << "the global namespace contains every scope";
            good(node, {.action = "lock", .holder = "bob", .environment = "test", .scopes = {"right"}});
            auto trespass = attempt(node, {.action = "apply",
                                           .operation = op("shared-trespass"),
                                           .holder = "bob",
                                           .expected = seed.frontier,
                                           .edits = {{.kind = "edit", .path = "x", .text = "!", .offset = 0, .count = 1}}});
            expect(trespass.code == "scope_locked" && trespass.reason.contains("alice")) << trespass.reason;
            auto whole = attempt(node, {.action = "apply",
                                        .operation = op("shared-whole"),
                                        .holder = "bob",
                                        .expected = seed.frontier,
                                        .edits = {{.kind = "write", .path = "x", .text = "gone"}}});
            expect(whole.code == "scope_locked") << "replacing a file needs the global namespace";
            auto first = good(node, {.action = "apply",
                                     .operation = op("shared-first"),
                                     .actor = "alice",
                                     .holder = "alice",
                                     .expected = seed.frontier,
                                     .edits = {{.kind = "edit", .path = "x", .text = "1", .offset = 1}}});
            auto second = good(node, {.action = "apply",
                                      .operation = op("shared-second"),
                                      .actor = "bob",
                                      .holder = "bob",
                                      .expected = seed.frontier,
                                      .edits = {{.kind = "edit", .path = "x", .text = "2", .offset = 3}}});
            expect(second.frontier.size() == 2u) << "both writers remain concurrent heads";
            expect(std::ranges::find(second.frontier, first.frontier.front()) != second.frontier.end());
            expect(text(inspect(node), "x") == "a1bc2d");
            auto history = good(node, {.action = "history"});
            std::set<std::string> actors;
            for (auto& entry : history.history)
                actors.insert(entry.actor);
            expect(actors.contains("alice") && actors.contains("bob"));
            expect(std::ranges::all_of(history.history, [&](auto& entry) { return entry.line == node.project_id(); }));
            auto listed = good(node, {.action = "locks"});
            expect(listed.locks.size() == 2u && listed.locks[0].place && listed.locks[0].place->path == "x" && listed.locks[0].scope == "left");
            expect(attempt(node, {.action = "unlock", .holder = "bob"}).code == "invalid_record") << "an unlock names what it releases";
            good(node, {.action = "unlock", .holder = "bob", .environment = "test", .scopes = {"right"}});
            expect(good(node, {.action = "locks"}).locks.size() == 1u);
            auto foreign = attempt(node, {.action = "unlock", .holder = "bob", .target = std::to_string(listed.locks[0].id)});
            expect(foreign.code == "permission_denied") << "a writer cannot release another writer's lock";
            good(node, {.action = "unlock", .holder = "bob", .target = std::to_string(listed.locks[0].id), .force = true});
            expect(good(node, {.action = "locks"}).locks.empty());
            auto unrelated = attempt(node, {.action = "apply",
                                            .operation = op("shared-unrelated"),
                                            .expected = {op("missing-event")},
                                            .edits = {{.kind = "edit", .path = "x", .text = "3", .offset = 0}}});
            expect(unrelated.code == "missing_objects") << unrelated.code;
            auto blind = attempt(node, {.action = "apply", .operation = op("shared-blind"), .edits = {{.kind = "create", .path = "x", .text = "again"}}});
            expect(blind.code == "stale_frontier") << "an edit cannot observe the empty state of a nonempty worldline";
        };
        "a scope lock authorizes edits only while its scope is whole"_test = [] {
            test::scratch tmp{"lock-lost"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "lost-seed", {}, {.kind = "create", .path = "x", .text = "abcd"});
            auto parts = good(node, {.action = "anchor", .spans = {{.place = semantic::place{"x", 0, 2}}, {.place = semantic::place{"x", 2, 4}}}});
            good(node, {.action = "semantic.publish",
                        .generation = semantic::generation{.version = seed.version,
                                                           .environment = "test",
                                                           .service = "test",
                                                           .service_version = "1",
                                                           .scopes = {{.id = "left", .kind = "function", .range = parts.spans[0]},
                                                                      {.id = "data", .kind = "variable", .range = parts.spans[1]}}}});
            auto variable = attempt(node, {.action = "lock", .holder = "alice", .environment = "test", .scopes = {"data"}});
            expect(variable.code == "invalid_record") << "only enclosing scopes are locked";
            good(node, {.action = "lock", .holder = "alice", .environment = "test", .scopes = {"left"}});
            auto emptied = good(node, {.action = "apply",
                                       .operation = op("lost-empty"),
                                       .holder = "alice",
                                       .expected = seed.frontier,
                                       .edits = {{.kind = "edit", .path = "x", .offset = 0, .count = 2}}});
            auto stray = attempt(node, {.action = "apply",
                                        .operation = op("lost-stray"),
                                        .holder = "alice",
                                        .expected = emptied.frontier,
                                        .edits = {{.kind = "edit", .path = "x", .text = "z", .offset = 1, .count = 1}}});
            expect(stray.code == "scope_locked") << "a scope that lost its characters no longer covers the rest of the file";
            auto again = attempt(node, {.action = "lock", .holder = "alice", .environment = "test", .scopes = {"left"}});
            expect(again.code == "invalid_record") << again.reason;
            auto joined = attempt(
                node, {.action = "integrate", .operation = op("lost-integrate"), .holder = "alice", .expected = emptied.frontier, .source = emptied.frontier});
            expect(joined.code == "scope_locked") << "a selection needs the global namespace";
            good(node, {.action = "unlock", .holder = "alice", .environment = "test", .scopes = {"left"}});
            good(node, {.action = "lock", .holder = "alice", .scopes = {"::"}});
            good(node, {.action = "apply",
                        .operation = op("lost-global"),
                        .holder = "alice",
                        .expected = emptied.frontier,
                        .edits = {{.kind = "edit", .path = "x", .text = "z", .offset = 1, .count = 1}}});
        };
        "worldlines never take each other's source"_test = [] {
            test::scratch tmp{"line-boundary"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "boundary-seed", {}, {.kind = "create", .path = "x", .text = "a"});
            auto fork = good(node, {.action = "fork", .worldline = "branch", .operation = op("boundary-fork"), .source = seed.frontier});
            auto branch = good(node, {.action = "apply",
                                      .worldline = "branch",
                                      .operation = op("boundary-branch"),
                                      .expected = fork.frontier,
                                      .edits = {{.kind = "write", .path = "x", .text = "b"}}});
            auto main = change(node, "boundary-main", seed.frontier, {.kind = "write", .path = "x", .text = "m"});
            auto integrated =
                attempt(node, {.action = "integrate", .operation = op("boundary-integrate"), .expected = main.frontier, .source = branch.frontier});
            expect(integrated.code == "worldline_boundary") << integrated.code;
            auto read = attempt(node, {.action = "read", .source = branch.frontier, .target = "x"});
            expect(read.code == "worldline_boundary") << read.code;
            auto mixed = attempt(
                node, {.action = "fork", .worldline = "mixed", .operation = op("boundary-mixed"), .source = {branch.frontier.front(), main.frontier.front()}});
            expect(mixed.code == "worldline_boundary") << mixed.code;
            expect(text(good(node, {.action = "read", .worldline = "branch", .source = seed.frontier, .target = "x"}), "x") == "a")
                << "the fork base stays readable from the branch";
            auto missing = attempt(
                node, {.action = "apply", .worldline = "absent", .operation = op("boundary-absent"), .edits = {{.kind = "create", .path = "y", .text = "y"}}});
            expect(missing.code == "invalid_record") << missing.code;
        };
        "another node joins a branch only with its complete state and then writes in it"_test = [] {
            peers p;
            auto seed = p.seed();
            auto fork = good(p.a, {.action = "fork", .worldline = "branch", .operation = op("join-fork"), .source = seed.frontier});
            auto first = good(p.a, {.action = "apply",
                                    .worldline = "branch",
                                    .operation = op("join-first"),
                                    .expected = fork.frontier,
                                    .edits = {{.kind = "edit", .path = "x", .text = "A", .offset = 0}}});
            exchange(p.a, *p.b);
            auto empty = attempt(*p.b, {.action = "integrate", .worldline = "branch", .operation = op("join-empty")});
            expect(empty.code == "stale_frontier") << empty.code;
            auto joined = good(*p.b, {.action = "integrate", .worldline = "branch", .operation = op("join-branch"), .source = first.frontier});
            auto second = good(*p.b, {.action = "apply",
                                      .worldline = "branch",
                                      .operation = op("join-second"),
                                      .expected = joined.frontier,
                                      .edits = {{.kind = "edit", .path = "x", .text = "B", .offset = 5}}});
            auto other = good(p.a, {.action = "apply",
                                    .worldline = "branch",
                                    .operation = op("join-other"),
                                    .expected = first.frontier,
                                    .edits = {{.kind = "edit", .path = "x", .text = "C", .offset = 1}}});
            exchange(*p.b, p.a);
            auto merged =
                good(p.a, {.action = "integrate", .worldline = "branch", .operation = op("join-back"), .expected = other.frontier, .source = second.frontier});
            expect(text(good(p.a, {.action = "read", .worldline = "branch", .target = "x"}), "x") == "ACabcdB");
            expect(text(inspect(p.a), "x") == "abcd") << "main stays apart";
            auto duplicate = attempt(*p.b, {.action = "fork", .worldline = "branch", .operation = op("join-duplicate"), .source = seed.frontier});
            expect(duplicate.code == "source_conflict") << duplicate.code;
        };
        "a node edits main only after it integrates the accepted main"_test = [] {
            peers p;
            auto seed = good(p.a, {.action = "apply", .operation = op("late-seed"), .edits = {{.kind = "create", .path = "x", .text = "abcd"}}});
            exchange(p.a, *p.c);
            auto late = attempt(*p.c, {.action = "apply", .operation = op("late-edit"), .edits = {{.kind = "create", .path = "late", .text = "z"}}});
            expect(late.code == "stale_frontier") << late.code;
            auto empty = attempt(*p.c, {.action = "integrate", .operation = op("late-empty")});
            expect(empty.code == "stale_frontier") << empty.code;
            auto joined = good(*p.c, {.action = "integrate", .operation = op("late-join"), .source = seed.frontier});
            good(*p.c,
                 {.action = "apply", .operation = op("late-after"), .expected = joined.frontier, .edits = {{.kind = "create", .path = "late", .text = "z"}}});
        };
        "forks made apart under one name leave the name to one worldline on every node"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = good(p.a, {.action = "fork", .worldline = "twin", .operation = op("twin-a"), .source = seed.frontier});
            auto b = good(*p.b, {.action = "fork", .worldline = "twin", .operation = op("twin-b"), .source = seed.frontier});
            exchange(p.a, *p.b);
            exchange(*p.b, p.a);
            auto edit = [&](nodes::local_node& node, const nodes::node_reply& fork, std::string name) {
                return attempt(node, {.action = "apply",
                                      .worldline = "twin",
                                      .operation = op(name),
                                      .expected = fork.frontier,
                                      .edits = {{.kind = "edit", .path = "x", .text = "!", .offset = 0}}});
            };
            auto left = edit(p.a, a, "twin-edit-a"), right = edit(*p.b, b, "twin-edit-b");
            expect((left.code == "ok") != (right.code == "ok")) << left.code << " " << right.code;
            expect(left.code == "worldline_boundary" || right.code == "worldline_boundary");
            auto& loser = left.code == "ok" ? *p.b : p.a;
            auto hidden = attempt(loser, {.action = "inspect", .worldline = "twin"});
            expect(hidden.code == "worldline_boundary" && !hidden.frontier.empty()) << "the losing state stays reachable for a new fork";
            auto renamed = good(loser, {.action = "fork", .worldline = "twin-2", .operation = op("twin-rename"), .source = hidden.frontier});
            expect(good(loser, {.action = "read", .worldline = "twin-2", .target = "x"}).files.size() == 1u);
        };
        "language data follows edits through character identities and reports staleness"_test = [] {
            test::scratch tmp{"semantic"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            auto seed = change(node, "semantic-seed", {}, {.kind = "create", .path = "x.cpp", .text = "int f() { return 1; }\nint g();\n"});
            auto located = good(node, {.action = "anchor",
                                       .revision = seed.version,
                                       .spans = {{.place = semantic::place{"x.cpp", 0, 21}}, {.place = semantic::place{"x.cpp", 4, 5}}}});
            expect((located.spans.size() == 2u) >> fatal);
            semantic::generation generation{.version = seed.version,
                                            .environment = "debug",
                                            .service = "test",
                                            .service_version = "1",
                                            .scopes = {{.id = "f", .kind = "function", .symbol = "f", .range = located.spans[0]}},
                                            .symbols = {{.id = "f", .name = "f", .kind = "function", .scope = "", .definitions = {located.spans[1]}}}};
            good(node, {.action = "semantic.publish", .generation = generation});
            auto fresh = good(node, {.action = "semantic", .environment = "debug"});
            expect((fresh.semantic.has_value() && !fresh.stale) >> fatal);
            expect(fresh.semantic->scopes[0].range.place && fresh.semantic->scopes[0].range.place->begin == 0u &&
                   fresh.semantic->scopes[0].range.place->end == 21u);
            auto edited = change(node, "semantic-edit", seed.frontier, {.kind = "edit", .path = "x.cpp", .text = "// head\n", .offset = 0});
            auto moved = good(node, {.action = "semantic", .environment = "debug"});
            expect((moved.semantic.has_value() && moved.stale) >> fatal) << "an older generation still answers, marked stale";
            auto& moved_place = *moved.semantic->scopes[0].range.place;
            expect(moved_place.begin == 8u && moved_place.end == 29u && moved_place.path == "x.cpp");
            auto removed = change(node, "semantic-remove", edited.frontier, {.kind = "edit", .path = "x.cpp", .offset = 12, .count = 1});
            auto gone = good(node, {.action = "semantic", .environment = "debug"});
            expect(!gone.semantic->symbols[0].definitions[0].place) << "a removed character leaves its span without a place";
            expect(!good(node, {.action = "semantic", .environment = "release"}).semantic.has_value());
            auto exact = generation;
            exact.version = removed.version;
            exact.scopes.clear();
            exact.symbols.clear();
            good(node, {.action = "semantic.publish", .generation = exact});
            good(node, {.action = "semantic.publish", .generation = generation});
            auto preferred = good(node, {.action = "semantic", .environment = "debug"});
            expect(!preferred.stale && preferred.semantic->scopes.empty()) << "the selected version's own generation wins over a later older one";
            auto fork = good(node, {.action = "fork", .worldline = "branch", .operation = op("semantic-fork"), .source = removed.frontier});
            expect(good(node, {.action = "semantic", .worldline = "branch", .environment = "debug"}).semantic.has_value()) << "a fork sees its base";
            auto gone_file = change(node, "semantic-delete", removed.frontier, {.kind = "remove", .path = "x.cpp"});
            auto older = good(node, {.action = "semantic", .environment = "debug", .revision = seed.version});
            expect(older.semantic->scopes[0].range.place.has_value());
            auto deleted = good(node, {.action = "semantic", .environment = "debug"});
            expect(deleted.stale);
            auto bad = generation;
            bad.scopes[0].parent = "missing";
            expect(attempt(node, {.action = "semantic.publish", .generation = bad}).code == "invalid_record");
            bad = generation;
            bad.version = removed.version;
            expect(attempt(node, {.action = "semantic.publish", .generation = bad}).code == "invalid_record")
                << "spans must exist in the version they describe";
        };
        "missing causal and corrupt sequence pages refuse edits and explicit archive audit reconstructs them"_test = [] {
            for (bool missing : {false, true}) {
                test::scratch tmp{"demand-damaged"};
                std::vector<std::string> head;
                std::string receipt;
                {
                    nodes::local_node seed{tmp.dir / "a"};
                    good(seed, {.action = "initialize"});
                    auto result = change(seed, "demand-damaged-seed", {}, {.kind = "create", .path = "x", .text = "a😀bc"});
                    head = result.frontier;
                    receipt = result.receipt;
                }
                sqlite3* raw = nullptr;
                expect((sqlite3_open((tmp.dir / "a/repository.sqlite").string().c_str(), &raw) == SQLITE_OK) >> fatal);
                std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{raw, sqlite3_close};
                sqlite3_stmt* q = nullptr;
                expect((sqlite3_prepare_v2(raw, "SELECT id,data FROM state_values", -1, &q, nullptr) == SQLITE_OK) >> fatal);
                std::vector<std::string> victims;
                while (sqlite3_step(q) == SQLITE_ROW) {
                    auto ptr = static_cast<const char*>(sqlite3_column_blob(q, 1));
                    auto raw_value = nodes::archive::unpack(std::string_view{ptr, static_cast<std::size_t>(sqlite3_column_bytes(q, 1))});
                    if (!raw_value.starts_with("DCN1"))
                        continue;
                    auto v = nodes::codec::decode(raw_value);
                    auto& map = v.object();
                    bool selected = missing ? map.find("ancestors") != map.end()
                                            : map.find("item") != map.end() && std::holds_alternative<value::map>(v.at("item").data) &&
                                                  v.at("item").object().find("run") != v.at("item").object().end();
                    if (selected)
                        victims.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(q, 0)));
                }
                sqlite3_finalize(q);
                expect(!victims.empty() >> fatal);
                for (auto& id : victims) {
                    auto sql = missing ? "DELETE FROM state_values WHERE id='" + id + "'" : "UPDATE state_values SET data=x'00' WHERE id='" + id + "'";
                    expect(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
                }
                db.reset();
                {
                    nodes::local_node damaged{tmp.dir / "a"};
                    auto rejected = attempt(damaged, {.action = "apply",
                                                      .operation = op("demand-damaged-edit"),
                                                      .expected = head,
                                                      .edits = {{.kind = "edit", .path = "x", .text = "z", .offset = 1}}});
                    expect(rejected.code == "missing_objects" || rejected.code == "storage_failure" || rejected.code == "outcome_unknown") << rejected.code;
                    expect(good(damaged, {.action = "tree"}).frontier == head);
                }
                nodes::local_node audit{tmp.dir / "a", {}, true};
                good(audit, {.action = "audit"});
                expect(text(inspect(audit), "x") == "a😀bc");
                expect(good(audit, {.action = "receipt", .target = receipt}).receipt == receipt);
                nodes::local_node repaired{tmp.dir / "a"};
                auto edited = change(repaired, "demand-repaired-edit", head, {.kind = "edit", .path = "x", .text = "z", .offset = 1});
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
                files.push_back({.kind = "create", .path = std::to_string(i), .text = "a😀bc"});
            auto current = good(node, {.action = "apply", .operation = op("demand-cache-seed"), .edits = std::move(files)});
            auto before = node.statistics();
            for (int i = 0; i < 24; ++i)
                current =
                    change(node, "demand-cache-" + std::to_string(i), current.frontier, {.kind = "edit", .path = std::to_string(i), .text = "z", .offset = 1});
            expect(node.statistics().cache_evictions > before.cache_evictions);
            nodes::local_node complete{tmp.dir / "a", {}, true};
            expect(inspect(node).source_digest == inspect(complete).source_digest);
            expect(good(node, {.action = "receipt", .target = current.receipt}).receipt == current.receipt);
        };
        "sequential text projections match complete causal rendering"_test = [] {
            for (bool replace : {false, true}) {
                peers p;
                auto seed = p.seed();
                auto head = seed.frontier;
                nodes::kernel incremental, complete;
                std::string expected = "abcd";
                auto add = [&](const std::string& id) {
                    auto body = nodes::codec::decode(nodes::unhex(p.a.objects({id}).front().data)).at("body");
                    incremental.add(id, body);
                    complete.add(id, body);
                };
                add(head.front());
                (void)incremental.project(head);
                for (int i = 0; i < 64; ++i) {
                    auto offset = replace ? std::size_t(i * 3) % expected.size() : std::size_t(i * 7) % (expected.size() + 1);
                    auto text = std::string(1, static_cast<char>('a' + i % 26));
                    auto next = change(p.a, "sequential-oracle-" + std::to_string(i), head,
                                       {.kind = "edit", .path = "x", .text = text, .offset = offset, .count = replace ? 1u : 0u});
                    expected.replace(offset, replace ? 1 : 0, text);
                    head = next.frontier;
                    add(head.front());
                    auto state = incremental.project(head);
                    expect(state.path("x").content() == expected);
                    if (i % 8 == 7) {
                        auto reference = complete.project(head);
                        expect(state.digest == reference.digest);
                        expect(reference.path("x").content() == expected);
                        for (std::uint64_t point = 0; point < state.path("x").size(); ++point)
                            expect(state.path("x").at(point) == reference.path("x").at(point));
                    }
                }
                auto concurrent = change(*p.b, "sequential-oracle-concurrent", seed.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 2});
                auto body = nodes::codec::decode(nodes::unhex(p.b->objects(concurrent.frontier).front().data)).at("body");
                incremental.add(concurrent.frontier.front(), body);
                complete.add(concurrent.frontier.front(), body);
                head.insert(head.end(), concurrent.frontier.begin(), concurrent.frontier.end());
                expect(incremental.project(head).digest == complete.project(head).digest);
            }
        };
        "incremental edits and cold point reads avoid unrelated file work"_test = [] {
            test::scratch tmp{"node-incremental"};
            nodes::local_node node{tmp.dir / "a"};
            good(node, {.action = "initialize"});
            std::vector<nodes::edit> files;
            for (int i = 0; i < 512; ++i)
                files.push_back({.kind = "create", .path = std::format("files/{:03}.txt", i), .text = std::string(1024, 'x')});
            auto seeded = good(node, {.action = "apply", .operation = op("incremental-seed"), .edits = std::move(files)});
            auto before = node.statistics();
            auto changed = change(node, "incremental-one", seeded.frontier, {.kind = "edit", .path = "files/001.txt", .text = "z"});
            auto after = node.statistics();
            expect(after.projected_files - before.projected_files == 1u);
            expect(after.state_nodes_written - before.state_nodes_written < 1024u);
            expect(after.state_bytes_written - before.state_bytes_written < 512u * 1024u);
            std::vector<nodes::edit> batch;
            for (int i = 3; i < 19; ++i)
                batch.push_back({.kind = "edit", .path = std::format("files/{:03}.txt", i), .text = "batch"});
            batch.push_back({.kind = "remove", .path = "files/000.txt"});
            batch.push_back({.kind = "remove", .path = "files/002.txt"});
            for (int i = 0; i < 8; ++i)
                batch.push_back({.kind = "create", .path = std::format("new/{:03}.txt", i), .text = "new"});
            changed = good(node, {.action = "apply", .operation = op("incremental-batch"), .expected = changed.frontier, .edits = std::move(batch)});
            nodes::local_node cold{tmp.dir / "a"};
            auto opened = cold.statistics();
            auto selected = good(cold, {.action = "read", .target = "files/001.txt"});
            auto read = cold.statistics();
            expect(!read.causal_loaded);
            expect(read.projected_files == 0u);
            expect(read.state_nodes_read - opened.state_nodes_read < 128u);
            expect(read.state_values_read - opened.state_values_read <= 8u);
            expect(text(selected, "files/001.txt") == "z" + std::string(1024, 'x'));
            expect(selected.source_digest == changed.source_digest);
            nodes::local_node audited{tmp.dir / "a", {}, true};
            expect(inspect(audited).source_digest == changed.source_digest);
            expect(inspect(audited).files.size() == 518u);
            sqlite3* raw = nullptr;
            if (sqlite3_open_v2((tmp.dir / "a/repository.sqlite").c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
                throw std::runtime_error("open state evidence");
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{raw, sqlite3_close};
            sqlite3_stmt* query = nullptr;
            if (sqlite3_prepare_v2(raw,
                                   "SELECT (SELECT COUNT(*) FROM state_nodes WHERE refs<=0)+(SELECT COUNT(*) FROM state_values WHERE refs<=0)+(SELECT COUNT(*) "
                                   "FROM state_payloads WHERE refs<=0)",
                                   -1, &query, nullptr) != SQLITE_OK)
                throw std::runtime_error("prepare state evidence");
            expect(sqlite3_step(query) == SQLITE_ROW);
            expect(sqlite3_column_int64(query, 0) == 0);
            sqlite3_finalize(query);
        };
        "corrupted Merkle cache is refused and explicit audit repairs it"_test = [] {
            test::scratch tmp{"node-state-integrity"};
            std::string digest, receipt;
            {
                nodes::local_node node{tmp.dir / "a"};
                good(node, {.action = "initialize"});
                auto seeded = change(node, "integrity-seed", {}, {.kind = "create", .path = "x", .text = "verified"});
                digest = seeded.source_digest;
                receipt = seeded.receipt;
            }
            {
                sqlite3* raw = nullptr;
                if (sqlite3_open_v2((tmp.dir / "a/repository.sqlite").c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
                    throw std::runtime_error("open state fixture");
                std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{raw, sqlite3_close};
                expect(sqlite3_exec(raw, "UPDATE state_values SET data=x'00'", nullptr, nullptr, nullptr) == SQLITE_OK);
            }
            expect(throws<nodes::error>([&] { nodes::local_node corrupted{tmp.dir / "a"}; }));
            {
                nodes::local_node audited{tmp.dir / "a", {}, true};
                auto repaired = good(audited, {.action = "audit"});
                expect(repaired.complete);
            }
            nodes::local_node reopened{tmp.dir / "a"};
            auto restored = inspect(reopened);
            expect(restored.source_digest == digest);
            expect(text(restored, "x") == "verified");
            expect(good(reopened, {.action = "receipt", .target = receipt}).receipt == receipt);
        };
        "incremental state pages retain offline CRDT anchors"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "snapshot-a", seed.frontier, {.kind = "edit", .path = "x", .text = "A", .offset = 1});
            auto checkpoint = good(p.a, {.action = "checkpoint"});
            auto saved = good(p.a, {.action = "identity"});
            expect(saved.snapshot_events == 2u);
            expect(!saved.state_snapshot.empty());
            auto tail = change(p.a, "snapshot-tail", a.frontier, {.kind = "edit", .path = "x", .text = "T", .offset = 4});
            nodes::local_node reopened{p.tmp.dir / "a"};
            auto restored = good(reopened, {.action = "identity"});
            expect(restored.snapshot_events == 3u);
            expect(restored.replayed_events == 0u);
            expect(restored.loaded_records < 30u);
            expect(text(inspect(reopened), "x") == "aAbcTd");
            expect(inspect(reopened).frontier == tail.frontier);
            expect(text(good(reopened, {.action = "read", .revision = a.version, .snapshot = checkpoint.checkpoint, .target = "x"}), "x") == "aAbcd");
            expect(attempt(reopened, {.action = "tree", .revision = tail.version, .snapshot = checkpoint.checkpoint}).code == "invalid_record");
            auto tree = good(reopened, {.action = "tree", .revision = a.version});
            expect(tree.files.size() == 1u);
            expect(tree.files.front().text.empty());

            auto b = change(*p.b, "snapshot-offline", seed.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 2});
            exchange(*p.b, reopened);
            auto merged = good(reopened, {.action = "integrate", .operation = op("snapshot-merge"), .expected = tail.frontier, .source = b.frontier});
            expect(merged.conflicts.empty());
            auto reversed =
                good(reopened, {.action = "reverse", .operation = op("snapshot-reverse"), .expected = merged.frontier, .target = a.frontier.front()});
            auto expected = text(inspect(reopened), "x");
            expect(expected.find('A') == std::string::npos);
            expect(expected.find('B') != std::string::npos);
            expect(expected.find('T') != std::string::npos);
            good(reopened, {.action = "checkpoint"});
            nodes::local_node resumed{p.tmp.dir / "a"};
            auto identity = good(resumed, {.action = "identity"});
            expect(identity.replayed_events == 0u);
            expect(text(inspect(resumed), "x") == expected);
            auto receipt = good(resumed, {.action = "receipt", .target = reversed.receipt});
            expect(receipt.receipt == reversed.receipt);
            nodes::local_node audited{p.tmp.dir / "a", {}, true};
            expect(text(inspect(audited), "x") == expected);
        };
        "libsodium matches the RFC 8032 empty-message signing vector"_test = [] {
            auto key = nodes::key::from_secret(nodes::unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
            expect(nodes::hex(key.public_key) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
            auto signature = key.sign("");
            expect(nodes::hex(signature) ==
                   "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
            expect(nodes::verify_signature(key.public_key, "", signature));
            expect(!nodes::verify_signature(key.public_key, "different", signature));
            auto signed_object = nodes::seal(value::map{{"kind", "test"}, {"schema", std::uint64_t{1}}}, key);
            expect(nodes::open(signed_object).at("kind").text() == "test");
            std::get<value::map>(signed_object.data)["signature"] = nodes::bytes{std::string(64, 'x')};
            expect(throws<nodes::error>([&] { nodes::open(signed_object); }));
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
                                          .edits = {{.kind = "edit", .path = "x", .text = "never accepted"}},
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
                    auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = inserted, .offset = 1, .count = count});
                    auto b = change(*p.b, "b", seed.frontier, {.kind = "edit", .path = "x", .text = "BB", .offset = 3});
                    exchange(*p.b, p.a);
                    auto integrated = good(p.a, {.action = "integrate", .operation = op("integrate"), .expected = a.frontier, .source = b.frontier});
                    auto expected = "a" + inserted + (count ? "cBBd" : "bcBBd");
                    expect(text(inspect(p.a), "x") == expected);
                    expect(integrated.conflicts.empty());
                    auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = integrated.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == "abcBBd");
                    good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == expected);
                }
        };
        "Unicode scalar deletion and complete run replacement preserve independent effects"_test = [] {
            {
                peers p;
                auto seed = p.seed("x", "a😀éb");
                auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .offset = 1, .count = 1});
                auto b = change(*p.b, "b", seed.frontier, {.kind = "edit", .path = "x", .text = "Ω", .offset = 3});
                exchange(*p.b, p.a);
                auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
                expect(text(inspect(p.a), "x") == "aéΩb");
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "a😀éΩb");
                good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "aéΩb");
            }
            {
                peers p;
                auto seed = p.seed("x", "😀é");
                auto a = change(p.a, "a", seed.frontier, {.kind = "write", .path = "x", .text = "替换"});
                auto b = change(*p.b, "b", seed.frontier, {.kind = "mode", .path = "x", .executable = true});
                exchange(*p.b, p.a);
                auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
                expect(text(inspect(p.a), "x") == "替换");
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "😀é");
                expect(inspect(p.a).files.front().executable);
                good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                expect(text(inspect(p.a), "x") == "替换");
                expect(inspect(p.a).files.front().executable);
            }
        };
        "K01-K02 all arrival permutations converge and receipt never integrates branches"_test = [] {
            peers p;
            auto seed = p.seed("x", "abcdef");
            auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = "A", .offset = 1});
            auto b = change(*p.b, "b", seed.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 3});
            auto c = change(*p.c, "c", seed.frontier, {.kind = "edit", .path = "x", .text = "C", .offset = 5});
            std::vector<nodes::local_node*> senders{&p.a, p.b.get(), p.c.get()};
            std::array<int, 3> order{0, 1, 2};
            std::string digest;
            unsigned trial = 0;
            do {
                nodes::local_node receiver{p.tmp.dir / ("receiver" + std::to_string(trial++)), p.a.project_id()};
                for (auto n : order) {
                    exchange(*senders[n], receiver);
                    exchange(*senders[n], receiver);
                }
                expect(inspect(receiver).frontier.empty());
                std::vector<std::string> heads{a.frontier.front(), b.frontier.front(), c.frontier.front()};
                auto comparison = good(receiver, {.action = "compare", .source = a.frontier});
                expect(!comparison.changes.empty());
                expect(comparison.version == a.version);
                expect(inspect(receiver).frontier.empty());
                auto result = good(receiver, {.action = "inspect", .source = heads});
                expect(text(result, "x") == "aAbcBdeCf");
                expect(result.conflicts.empty());
                if (digest.empty())
                    digest = result.source_digest;
                else
                    expect(result.source_digest == digest);
            } while (std::next_permutation(order.begin(), order.end()));
            expect(trial == 6u);
        };
        "K03 evidence-only batches leave source digests unchanged"_test = [] {
            peers p;
            auto seed = p.seed();
            auto before = inspect(p.a);
            expect(before.files.size() == 1u);
            auto& file = before.files.front();
            auto leaf = nodes::digest_of(nodes::codec::encode(
                value::map{{"content", file.digest}, {"executable", file.executable}, {"file", file.id}, {"kind", file.kind}, {"path", file.path}}));
            auto root = nodes::digest_of(nodes::codec::encode(value::map{{"key", file.id}, {"left", ""}, {"right", ""}, {"value", leaf}}));
            expect(before.source_digest ==
                   nodes::digest_of(nodes::codec::encode(value::map{{"conflicts", nodes::strings(before.conflicts)}, {"files", root}})));
            auto evidence =
                good(p.a, {.action = "apply", .operation = op("evidence"), .expected = seed.frontier, .intent = "Review only", .result = "Actual observation"});
            auto after = inspect(p.a);
            expect(before.source_digest == after.source_digest);
            expect(before.version != after.version);
            expect(evidence.result == "Actual observation");
            change(*p.b, "unrelated", seed.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 3});
            exchange(*p.b, p.a);
            auto fixed = good(p.a, {.action = "inspect", .source = seed.frontier});
            expect(fixed.version == before.version);
            expect(fixed.source_digest == before.source_digest);
            expect(text(fixed, "x") == "abcd");
            expect(inspect(p.a).frontier == after.frontier);
        };
        "fixed-version comparison and history retain exact source and tool evidence"_test = [] {
            peers p;
            auto seed = p.seed();
            auto changed = good(p.a, {.action = "apply",
                                      .operation = op("observed"),
                                      .expected = seed.frontier,
                                      .edits = {{.kind = "edit", .path = "x", .text = "AA", .offset = 1}},
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
            expect(found->executor == p.a.node_id());
            expect(found->intent == "Add the selected contribution");
            expect(found->result == "Retained actual result");
            expect(found->receipts == std::vector<std::string>{changed.receipt});
            expect(found->tool == "editor.replace/7");
            auto checkpoint = good(p.a, {.action = "checkpoint"});
            exchange(p.a, *p.b);
            auto batch = nodes::codec::decode(nodes::unhex(p.a.objects(changed.frontier).front().data));
            expect(batch.at("body").at("effect_digest").text() ==
                   nodes::digest_of(nodes::codec::encode(value::map{{"effects", batch.at("body").at("effects")}})));
            auto object = nodes::codec::decode(nodes::unhex(p.a.objects({batch.at("body").at("attempt").text()}).front().data));
            auto body = object.at("body").object();
            body["tool"] = "other.tool/1";
            auto signer = nodes::key::from_secret(nodes::unhex(content::hash_of("node-fixture-author-a")));
            auto raw = nodes::codec::encode(nodes::seal(body, signer));
            expect(attempt(*p.b, {.action = "receive", .parts = {{nodes::digest_of(raw), 0, raw.size(), nodes::hex(raw)}}}).code == "invalid_record");
            std::stringstream stream;
            p.b->export_stream(stream);
            nodes::local_node receiver{p.tmp.dir / "refused-stream", p.a.project_id()};
            auto imported = receiver.import_stream(stream);
            expect(!imported.complete);
            expect(!imported.refused.empty());
            expect(!imported.reason.empty());
            expect(inspect(receiver).frontier.empty());
            expect(good(receiver, {.action = "checkpoint", .target = checkpoint.checkpoint}).complete);
        };
        "K05 accepted retries return the original receipt after later head changes"_test = [] {
            peers p;
            auto seed = p.seed();
            nodes::node_request request{.action = "apply",
                                        .operation = op("first"),
                                        .expected = seed.frontier,
                                        .edits = {{.kind = "edit", .path = "x", .text = "A", .offset = 1}},
                                        .result = "original result"};
            auto first = good(p.a, request);
            change(p.a, "later", first.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 4});
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
                .action = "apply", .operation = op("invalid"), .expected = seed.frontier, .edits = {{.kind = "edit", .path = "x", .offset = 50}}};
            expect(attempt(p.a, in).code == "invalid_record");
            in.edits.front().offset = 1;
            in.edits.front().text = "changed";
            expect(attempt(p.a, in).code == "identity_conflict");
            expect(inspect(p.a).frontier == seed.frontier);
        };
        "an abandoned operation never runs, and one that ran answers abandon with its receipt"_test = [] {
            peers p;
            auto seed = p.seed();
            nodes::node_request lost{
                .action = "apply", .operation = op("lost"), .expected = seed.frontier, .edits = {{.kind = "edit", .path = "x", .text = "A", .offset = 1}}};
            expect(good(p.a, {.action = "abandon", .operation = lost.operation}).receipt.empty());
            expect(attempt(p.a, lost).code == "identity_conflict");
            expect(good(p.a, {.action = "abandon", .operation = lost.operation}).receipt.empty());
            expect(attempt(p.a, {.action = "receipt", .operation = lost.operation}).code == "identity_conflict");
            expect(inspect(p.a).frontier == seed.frontier);
            auto ran = change(p.a, "ran", seed.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 1});
            expect(good(p.a, {.action = "abandon", .operation = op("ran")}).receipt == ran.receipt);
            expect(
                good(p.a,
                     {.action = "apply", .operation = op("ran"), .expected = seed.frontier, .edits = {{.kind = "edit", .path = "x", .text = "B", .offset = 1}}})
                    .replayed);
        };
        "R04-R05 file and directory moves preserve another author's text"_test = [] {
            for (bool directory : {false, true}) {
                peers p;
                auto seed = good(p.a, {.action = "apply",
                                       .operation = op("seed"),
                                       .edits = {{.kind = "create", .path = "src/a", .text = "abcd"}, {.kind = "create", .path = "src/b", .text = "second"}}});
                exchange(p.a, *p.b);
                good(*p.b, {.action = "integrate", .operation = op("fork"), .source = seed.frontier});
                auto a = change(p.a, "move", seed.frontier,
                                {.kind = directory ? "directory_move" : "move", .path = directory ? "src" : "src/a", .target = directory ? "lib" : "lib/a"});
                auto b = change(*p.b, "edit", seed.frontier, {.kind = "edit", .path = "src/a", .text = "BB", .offset = 3});
                exchange(*p.b, p.a);
                auto integrated = good(p.a, {.action = "integrate", .operation = op("integrate"), .expected = a.frontier, .source = b.frontier});
                expect(text(inspect(p.a), "lib/a") == "abcBBd");
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = integrated.frontier, .target = a.frontier.front()});
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
                    auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = inserted, .offset = 1, .count = count});
                    exchange(p.a, *p.b);
                    auto adopted = good(*p.b, {.action = "integrate", .operation = op("adopt"), .expected = seed.frontier, .source = a.frontier});
                    auto b = change(*p.b, "b", adopted.frontier, {.kind = "edit", .path = "x", .text = "BB", .offset = 3 + inserted.size() - count});
                    exchange(*p.b, p.a);
                    auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
                    auto before = text(inspect(p.a), "x");
                    auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == "abcBBd");
                    good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                    expect(text(inspect(p.a), "x") == before);
                }
        };
        "R07 another deletion claim remains when only one deletion is reversed"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .offset = 1, .count = 1});
            auto b = change(*p.b, "b", seed.frontier, {.kind = "edit", .path = "x", .offset = 1, .count = 1});
            exchange(*p.b, p.a);
            auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
            expect(!merged.conflicts.empty());
            expect(attempt(p.a, {.action = "checkpoint"}).code == "source_conflict");
            auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse-a"), .expected = merged.frontier, .target = a.frontier.front()});
            expect(text(inspect(p.a), "x") == "acd");
            good(p.a, {.action = "reverse", .operation = op("reverse-b"), .expected = reversed.frontier, .target = b.frontier.front()});
            expect(text(inspect(p.a), "x") == "abcd");
        };
        "R08 hidden insertion anchors still retain another author's descendants"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = "AA", .offset = 1});
            exchange(p.a, *p.b);
            good(*p.b, {.action = "integrate", .operation = op("adopt"), .expected = seed.frontier, .source = a.frontier});
            auto b = change(*p.b, "b", a.frontier, {.kind = "edit", .path = "x", .text = "BB", .offset = 2});
            exchange(*p.b, p.a);
            auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
            good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
            expect(text(inspect(p.a), "x") == "aBBbcd");
        };
        "R09 recorded source dependencies block an unreviewed reversal"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = "AA", .offset = 1});
            exchange(p.a, *p.b);
            good(*p.b, {.action = "integrate", .operation = op("adopt"), .expected = seed.frontier, .source = a.frontier});
            auto b = good(*p.b, {.action = "apply",
                                 .operation = op("b"),
                                 .expected = a.frontier,
                                 .edits = {{.kind = "edit", .path = "x", .text = "dependent", .offset = 2, .count = 1}}});
            exchange(*p.b, p.a);
            auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
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
            auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = "AA", .offset = 1});
            exchange(p.a, *p.b);
            good(*p.b, {.action = "integrate", .operation = op("adopt"), .expected = seed.frontier, .source = a.frontier});
            auto x = good(p.a, {.action = "reverse", .operation = op("x-reverse"), .expected = a.frontier, .target = a.frontier.front()});
            auto y = good(*p.b, {.action = "reverse", .operation = op("y-reverse"), .expected = a.frontier, .target = a.frontier.front()});
            auto redo = good(*p.b, {.action = "redo", .operation = op("y-redo"), .expected = y.frontier, .target = a.frontier.front()});
            exchange(*p.b, p.a);
            auto integrated = good(p.a, {.action = "integrate", .operation = op("activation-merge"), .expected = x.frontier, .source = redo.frontier});
            expect(!integrated.conflicts.empty());
            auto resolved = good(p.a, {.action = "resolve",
                                       .operation = op("choose-active"),
                                       .expected = integrated.frontier,
                                       .target = a.frontier.front(),
                                       .activation = true,
                                       .decisions = integrated.conflicts});
            expect(resolved.conflicts.empty());
            expect(text(inspect(p.a), "x") == "aAAbcd");
        };
        "K09 structural and overlapping conflicts are visible and block checkpoints"_test = [] {
            for (auto scenario : {"same_gap", "replace_replace", "replace_delete"}) {
                peers p;
                auto seed = p.seed();
                bool insertion = std::string_view{scenario} == "same_gap";
                auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = "A", .offset = 1, .count = insertion ? 0u : 1u});
                auto b = change(*p.b, "b", seed.frontier,
                                {.kind = "edit",
                                 .path = "x",
                                 .text = std::string_view{scenario} == "replace_delete" ? "" : "B",
                                 .offset = 1,
                                 .count = insertion ? 0u : 1u});
                exchange(*p.b, p.a);
                auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
                expect(!merged.conflicts.empty());
                expect(attempt(p.a, {.action = "checkpoint"}).code == "source_conflict");
                auto resolved = good(p.a, {.action = "resolve", .operation = op("resolve"), .expected = merged.frontier, .decisions = merged.conflicts});
                expect(resolved.conflicts.empty());
                good(p.a, {.action = "checkpoint"});
            }
        };
        "file modes and removal reverse while retaining another author's text"_test = [] {
            for (auto kind : {"mode", "remove"}) {
                peers p;
                auto seed = p.seed();
                auto a = change(p.a, "a", seed.frontier, {.kind = kind, .path = "x", .executable = true});
                auto b = change(*p.b, "b", seed.frontier, {.kind = "edit", .path = "x", .text = "BB", .offset = 3});
                exchange(*p.b, p.a);
                auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
                if (std::string_view{kind} == "mode") {
                    expect(inspect(p.a).files.front().executable);
                    good(p.a, {.action = "materialize"});
                    expect((std::filesystem::status(p.tmp.dir / "a/working/main/x").permissions() & std::filesystem::perms::owner_exec) !=
                           std::filesystem::perms::none);
                } else
                    expect(!merged.conflicts.empty());
                auto reversed = good(p.a, {.action = "reverse", .operation = op("reverse"), .expected = merged.frontier, .target = a.frontier.front()});
                expect(reversed.conflicts.empty());
                expect(text(inspect(p.a), "x") == "abcBBd");
                expect(!inspect(p.a).files.front().executable);
                good(p.a, {.action = "materialize"});
                expect((std::filesystem::status(p.tmp.dir / "a/working/main/x").permissions() & std::filesystem::perms::owner_exec) ==
                       std::filesystem::perms::none);
                auto redone = good(p.a, {.action = "redo", .operation = op("redo"), .expected = reversed.frontier, .target = a.frontier.front()});
                if (std::string_view{kind} == "mode") {
                    expect(inspect(p.a).files.front().executable);
                    expect(text(inspect(p.a), "x") == "abcBBd");
                    good(p.a, {.action = "materialize"});
                    expect((std::filesystem::status(p.tmp.dir / "a/working/main/x").permissions() & std::filesystem::perms::owner_exec) !=
                           std::filesystem::perms::none);
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
                if (std::string_view{scenario} == "binary") {
                    seed = good(p.a, {.action = "apply", .operation = op("seed"), .edits = {{.kind = "binary_create", .path = "x", .text = "0102"}}});
                    exchange(p.a, *p.b);
                    good(*p.b, {.action = "integrate", .operation = op("fork"), .source = seed.frontier});
                } else
                    seed = p.seed();
                nodes::node_reply a, b;
                if (std::string_view{scenario} == "rename_delete") {
                    a = change(p.a, "a", seed.frontier, {.kind = "move", .path = "x", .target = "moved"});
                    b = change(*p.b, "b", seed.frontier, {.kind = "remove", .path = "x"});
                } else if (std::string_view{scenario} == "binary") {
                    a = change(p.a, "a", seed.frontier, {.kind = "binary", .path = "x", .text = "0001"});
                    b = change(*p.b, "b", seed.frontier, {.kind = "binary", .path = "x", .text = "ff00"});
                } else {
                    a = change(p.a, "a", seed.frontier, {.kind = "create", .path = "new", .text = "A"});
                    b = change(*p.b, "b", seed.frontier,
                               {.kind = "create", .path = std::string_view{scenario} == "duplicate" ? "new" : "new/child", .text = "B"});
                }
                exchange(*p.b, p.a);
                auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
                expect(!merged.conflicts.empty()) << scenario;
                expect(attempt(p.a, {.action = "checkpoint"}).code == "source_conflict");
            }
        };
        "K10 a new overlapping candidate is outside a prior conflict decision"_test = [] {
            peers p;
            auto seed = p.seed();
            auto a = change(p.a, "a", seed.frontier, {.kind = "edit", .path = "x", .text = "A", .offset = 1});
            auto b = change(*p.b, "b", seed.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 1});
            auto c = change(*p.c, "c", seed.frontier, {.kind = "edit", .path = "x", .text = "C", .offset = 1});
            exchange(*p.b, p.a);
            auto merged = good(p.a, {.action = "integrate", .operation = op("merge"), .expected = a.frontier, .source = b.frontier});
            auto resolved = good(p.a, {.action = "resolve", .operation = op("resolve"), .expected = merged.frontier, .decisions = merged.conflicts});
            expect(resolved.conflicts.empty());
            exchange(*p.c, p.a);
            auto later = good(p.a, {.action = "integrate", .operation = op("later"), .expected = resolved.frontier, .source = c.frontier});
            expect(!later.conflicts.empty());
        };
        "K11 altered signatures and cross-project objects are refused"_test = [] {
            peers p;
            auto seed = p.seed();
            auto parts = p.a.objects({seed.frontier.front()});
            auto data = nodes::unhex(parts.front().data);
            data.back() ^= 1;
            parts.front().id = content::hash_of(data).str();
            parts.front().data = nodes::hex(data);
            expect(attempt(*p.b, {.action = "receive", .parts = parts}).code == "invalid_signature");
            nodes::local_node other{p.tmp.dir / "other"};
            auto genesis = good(other, {.action = "initialize"});
            auto wrong = other.objects({genesis.project});
            expect(attempt(*p.b, {.action = "receive", .parts = wrong}).code == "wrong_project");
            expect(inspect(*p.b).frontier == seed.frontier);
        };
        "K11 signed operation identity reuse is inspectable and needs a fixed-version admission"_test = [] {
            peers p;
            auto seed = p.seed();
            auto secret = [&] {
                std::ifstream input(p.tmp.dir / "a/author.key", std::ios::binary);
                return std::string{std::istreambuf_iterator<char>{input}, {}};
            }();
            p.b->use_author(nodes::key::from_secret(secret));
            auto a = change(p.a, "reused", seed.frontier, {.kind = "edit", .path = "x", .text = "X", .offset = 1});
            auto b = change(*p.b, "reused", seed.frontier, {.kind = "edit", .path = "x", .text = "Y", .offset = 3});
            exchange(*p.b, p.a);
            exchange(*p.b, *p.c);
            exchange(p.a, *p.c);
            auto a_status = good(p.a, {.action = "identity"}), c_status = good(*p.c, {.action = "identity"});
            expect(!a_status.identity_conflicts.empty());
            expect(a_status.identity_conflicts == c_status.identity_conflicts);
            expect(text(inspect(p.a), "x") == "aXbcd");
            expect(text(good(p.a, {.action = "inspect", .source = b.frontier}), "x") == "abcYd");
            auto combined = std::vector<std::string>{a.frontier.front(), b.frontier.front()};
            auto av = good(p.a, {.action = "inspect", .source = combined}), cv = good(*p.c, {.action = "inspect", .source = combined});
            expect(av.source_digest == cv.source_digest);
            expect(av.conflicts == cv.conflicts);
            expect(!av.conflicts.empty());
            expect(attempt(p.a, {.action = "integrate", .operation = op("refuse-reuse"), .expected = a.frontier, .source = b.frontier}).code ==
                   "identity_conflict");
            expect(inspect(p.a).frontier == a.frontier);
            auto selected =
                good(p.a, {.action = "admit", .operation = op("choose-original"), .expected = a.frontier, .decisions = a_status.identity_conflicts});
            auto next = change(p.a, "after-choice", selected.frontier, {.kind = "edit", .path = "x", .text = "Z"});
            expect(!next.receipt.empty());
            expect(text(inspect(p.a), "x") == "ZaXbcd");
            auto original = good(p.a, {.action = "receipt", .operation = op("reused")});
            expect(original.receipt == a.receipt);
            expect(original.issuer == p.a.node_id());
        };
        "identical source effects with another sealed policy are applied once"_test = [] {
            peers p;
            auto seed = p.seed();
            auto changed = change(p.a, "one-logical-change", seed.frontier, {.kind = "edit", .path = "x", .text = "AA", .offset = 1});
            auto before = inspect(p.a);
            auto policy = good(p.a, {.action = "policy",
                                     .authors = {p.a.author_id(), p.b->author_id(), p.c->author_id()},
                                     .nodes = {p.a.node_id(), p.b->node_id(), p.c->node_id()}});
            auto old = nodes::codec::decode(nodes::unhex(p.a.objects({changed.frontier.front()}).front().data)).at("body").object();
            old["policy"] = policy.policies.front();
            std::ifstream input(p.tmp.dir / "a/author.key", std::ios::binary);
            auto signer = nodes::key::from_secret({std::istreambuf_iterator<char>{input}, {}});
            auto sealed = nodes::seal(old, signer);
            auto raw = nodes::codec::encode(sealed);
            auto alias = content::hash_of(raw);
            good(p.a, {.action = "receive", .parts = {{alias.str(), 0, raw.size(), nodes::hex(raw)}}});
            auto merged = good(p.a, {.action = "integrate", .operation = op("observe-alias"), .expected = changed.frontier, .source = {alias.str()}});
            expect(merged.conflicts.empty());
            expect(inspect(p.a).source_digest == before.source_digest);
            expect(text(inspect(p.a), "x") == "aAAbcd");
        };
        "K13 revocation settles known work but leaves offline work for explicit admission"_test = [] {
            peers p;
            auto seed = p.seed();
            auto settled = change(*p.b, "settled-before-revocation", seed.frontier, {.kind = "edit", .path = "x", .text = "K", .offset = 1});
            exchange(*p.b, p.a);
            auto prior = good(p.a, {.action = "integrate", .operation = op("settled-admission"), .expected = seed.frontier, .source = settled.frontier});
            auto b = change(*p.b, "offline", settled.frontier, {.kind = "edit", .path = "x", .text = "BB", .offset = 4});
            auto accepted = nodes::codec::decode(nodes::unhex(p.a.objects({prior.receipt}).front().data)).at("body").at("admission").text();
            good(p.a, {.action = "policy",
                       .authors = {p.a.author_id(), p.c->author_id()},
                       .nodes = {p.a.node_id(), p.b->node_id(), p.c->node_id()},
                       .settled = {accepted}});
            good(p.a, {.action = "checkpoint"});
            expect(text(inspect(p.a), "x") == "aKbcd");
            auto next = change(p.a, "after-revocation", prior.frontier, {.kind = "edit", .path = "x", .text = "Z", .offset = 0});
            exchange(p.a, *p.b);
            expect(attempt(*p.b, {.action = "apply",
                                  .operation = op("newly-denied"),
                                  .expected = b.frontier,
                                  .edits = {{.kind = "edit", .path = "x", .text = "not accepted"}}})
                       .code == "policy_denied");
            expect(inspect(*p.b).frontier == b.frontier);
            exchange(*p.b, p.a);
            auto denied = attempt(p.a, {.action = "integrate", .operation = op("denied"), .expected = next.frontier, .source = b.frontier});
            expect(denied.code == "policy_denied");
            expect(inspect(p.a).frontier == next.frontier);
            auto admitted = good(p.a, {.action = "admit", .operation = op("reviewed"), .expected = next.frontier, .source = b.frontier});
            expect(text(inspect(p.a), "x") == "ZaKbcBBd");
            expect(!admitted.receipt.empty());
            change(p.a, "continue-reviewed", admitted.frontier, {.kind = "edit", .path = "x", .text = "Q", .offset = 0});
            good(p.a, {.action = "checkpoint"});
            expect(text(inspect(p.a), "x") == "QZaKbcBBd");
        };
        "K12 later batches cannot indirectly introduce a refused author closure"_test = [] {
            peers p;
            auto seed = p.seed();
            auto b = change(*p.b, "refused", seed.frontier, {.kind = "edit", .path = "x", .text = "B", .offset = 1});
            exchange(*p.b, *p.c);
            auto adopted = good(*p.c, {.action = "integrate", .operation = op("adopt"), .expected = seed.frontier, .source = b.frontier});
            auto c = change(*p.c, "dependent", adopted.frontier, {.kind = "edit", .path = "x", .text = "C", .offset = 4});
            auto admission = nodes::codec::decode(nodes::unhex(p.a.objects({seed.receipt}).front().data)).at("body").at("admission").text();
            good(p.a, {.action = "policy",
                       .authors = {p.a.author_id(), p.c->author_id()},
                       .nodes = {p.a.node_id(), p.b->node_id(), p.c->node_id()},
                       .settled = {admission}});
            exchange(*p.c, p.a);
            expect(attempt(p.a, {.action = "integrate", .operation = op("indirect"), .expected = seed.frontier, .source = c.frontier}).code == "policy_denied");
            expect(inspect(p.a).frontier == seed.frontier);
        };
        "K14 concurrent policy heads require a joined-parent maintainer resolution"_test = [] {
            peers p;
            auto genesis = p.a.project_id();
            nodes::local_node branch{p.tmp.dir / "branch", genesis};
            exchange(p.a, branch);
            auto seed = nodes::key::from_secret([&] {
                std::ifstream in(p.tmp.dir / "a" / "author.key", std::ios::binary);
                return std::string{std::istreambuf_iterator<char>{in}, {}};
            }());
            auto base = good(p.a, {.action = "identity"}).policies;
            auto initial = nodes::codec::decode(nodes::unhex(p.a.objects({base.front()}).front().data)).at("body");
            auto make_policy = [&](std::string who) {
                auto map = initial.object();
                map["previous"] = nodes::strings(base);
                map["epoch"] = initial.at("epoch").number() + 1;
                auto authors = std::vector<std::string>{p.a.author_id(), std::move(who)};
                std::ranges::sort(authors);
                map["authors"] = nodes::strings(authors);
                auto object = nodes::seal(map, seed);
                auto bytes = nodes::codec::encode(object);
                return nodes::object_part{content::hash_of(bytes).str(), 0, bytes.size(), nodes::hex(bytes)};
            };
            auto one = make_policy(p.b->author_id()), two = make_policy(p.c->author_id());
            good(p.a, {.action = "receive", .parts = {one, two}});
            good(branch, {.action = "receive", .parts = {two, one}});
            auto a = good(p.a, {.action = "identity"}), b = good(branch, {.action = "identity"});
            expect(a.policies == b.policies);
            expect(a.policies.size() == 2u);
            expect(attempt(p.a, {.action = "apply", .operation = op("suspended")}).code == "policy_conflict");
            auto resolved = good(p.a, {.action = "policy",
                                       .authors = {p.a.author_id(), p.b->author_id(), p.c->author_id()},
                                       .nodes = {p.a.node_id(), p.b->node_id(), p.c->node_id()},
                                       .policies = a.policies});
            expect(resolved.policies.size() == 1u);
        };
        "K06 incomplete source and checkpoint closures never become complete views"_test = [] {
            peers p;
            auto seed = p.seed();
            auto checkpoint = good(p.a, {.action = "checkpoint"});
            for (auto selected : {seed.frontier.front(), checkpoint.checkpoint}) {
                nodes::local_node partial{p.tmp.dir / ("partial-" + selected), p.a.project_id()};
                for (auto& part : p.a.objects({selected}))
                    good(partial, {.action = "receive", .parts = {part}});
                auto offered = good(partial, {.action = "offer"});
                expect(!offered.complete);
                expect(!offered.missing.empty());
                expect(attempt(partial, {.action = "inspect", .source = seed.frontier}).code == "missing_objects");
            }
        };
        "K06 each required object class prevents a complete retained checkpoint when omitted"_test = [] {
            peers p;
            auto seed = p.seed();
            auto binary = good(p.a, {.action = "apply",
                                     .operation = op("binary-content"),
                                     .expected = seed.frontier,
                                     .edits = {{.kind = "binary_create", .path = "binary", .text = "00ff"}}});
            auto evidence = good(p.a, {.action = "apply", .operation = op("configuration"), .expected = binary.frontier, .result = "retained evidence"});
            auto receipt = nodes::codec::decode(nodes::unhex(p.a.objects({evidence.receipt}).front().data)).at("body");
            auto signer = nodes::key::from_secret(nodes::unhex(content::hash_of("node-fixture-author-a")));
            auto raw_config = nodes::codec::encode(nodes::seal(
                value::map{{"data", nodes::bytes{"fixture configuration"}}, {"kind", "blob"}, {"project", p.a.project_id()}, {"schema", std::uint64_t{1}}},
                signer));
            auto config = nodes::digest_of(raw_config);
            good(p.a, {.action = "receive", .parts = {{config, 0, raw_config.size(), nodes::hex(raw_config)}}});
            auto checkpoint = good(p.a, {.action = "checkpoint", .configuration = config});
            auto checkpoint_body = nodes::codec::decode(nodes::unhex(p.a.objects({checkpoint.checkpoint}).front().data)).at("body");
            expect(nodes::string_list(checkpoint_body.at("admissions")).size() == 1u);
            auto retained = nodes::string_list(checkpoint_body.at("retained"));
            expect(std::ranges::find(retained, seed.receipt) != retained.end());
            expect(std::ranges::find(retained, evidence.receipt) != retained.end());
            auto historical = good(p.a, {.action = "checkpoint", .source = seed.frontier});
            expect(historical.frontier == seed.frontier);
            expect(good(p.a, {.action = "checkpoint", .target = historical.checkpoint}).complete);
            auto source = nodes::codec::decode(nodes::unhex(p.a.objects({binary.frontier.front()}).front().data)).at("body");
            std::string blob;
            for (auto& id : nodes::string_list(source.at("required"))) {
                auto b = nodes::codec::decode(nodes::unhex(p.a.objects({id}).front().data)).at("body");
                if (b.at("kind").text() == "blob" && b.at("data").raw() == std::string{"\0\xff", 2})
                    blob = id;
            }
            expect(!blob.empty());
            auto offered = good(p.a, {.action = "offer"});
            for (auto& omitted : std::set<std::string>{blob, receipt.at("result").text(), seed.frontier.front(), receipt.at("admission").text(),
                                                       source.at("intent").text(), config}) {
                nodes::local_node receiver{p.tmp.dir / ("omitted-" + omitted), p.a.project_id()};
                std::vector<std::string> included;
                for (auto& id : offered.objects)
                    if (id != omitted)
                        included.push_back(id);
                for (auto& part : p.a.objects(included))
                    good(receiver, {.action = "receive", .parts = {part}});
                auto status = good(receiver, {.action = "checkpoint", .target = checkpoint.checkpoint});
                expect(!status.complete);
                expect(std::ranges::find(status.missing, omitted) != status.missing.end());
                expect(inspect(receiver).frontier.empty());
                if (omitted == config)
                    expect(good(receiver, {.action = "receipt", .target = evidence.receipt}).issuer == p.a.node_id());
                else
                    expect(attempt(receiver, {.action = "receipt", .target = evidence.receipt}).code == "outcome_unknown");
            }
        };
        "K16 a third node continues after the original node is unavailable"_test = [] {
            peers p;
            auto seed = p.seed();
            auto b = change(*p.b, "offline", seed.frontier, {.kind = "edit", .path = "x", .text = "BB", .offset = 3});
            exchange(*p.b, *p.c);
            auto merged = good(*p.c, {.action = "integrate", .operation = op("third-integrate"), .expected = seed.frontier, .source = b.frontier});
            expect(text(inspect(*p.c), "x") == "abcBBd");
            auto proof = good(*p.c, {.action = "receipt", .target = b.receipt});
            expect(proof.issuer == p.b->node_id());
            expect(proof.version == b.version);
            auto next = change(*p.c, "third-edit", merged.frontier, {.kind = "edit", .path = "x", .text = "C", .offset = 0});
            expect(!next.receipt.empty());
        };
        "native source reads remain available while the writer entry is occupied"_test = [] {
            test::scratch tmp{"native-reader"};
            storage::native_nodes registry;
            auto seed = registry.with(tmp.dir / "node", [](nodes::local_node& node) {
                node.initialize();
                return node.call({.action = "apply",
                                  .operation = content::hash_of("reader-seed").str(),
                                  .owner = node.node_id(),
                                  .edits = {{.kind = "create", .path = "x", .text = "retained"}, {.kind = "create", .path = "y", .text = "retained"}}});
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
                                              .edits = {{.kind = "edit", .path = "x", .text = std::to_string(i) + ":"},
                                                        {.kind = "edit", .path = "y", .text = std::to_string(i) + ":"}}});
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
