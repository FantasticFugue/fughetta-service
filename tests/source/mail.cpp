import std;
import boost.ut;
import content;
import engine;
import support;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "d-mail persists text with sender and worldline without changing source"_test = [] {
            test::scratch tmp{"mail"};
            auto operation = content::hash_of("mail-one").str();
            std::uint64_t id = 0;
            {
                nodes::local_node node{tmp.dir};
                auto initialized = node.initialize();
                expect(initialized.code == "ok");
                auto before = node.call({.action = "tree"});
                nodes::node_request request{.action = "mail_send",
                                            .mail = nodes::mail_input{"branch-b", "Keep the ABI stable.\nUse UTF-8: 世界."},
                                            .operation = operation,
                                            .actor = "alice"};
                auto first = node.call(request);
                expect((first.code == "ok" && first.messages.size() == 1u) >> fatal);
                id = first.messages.front().id;
                expect(first.messages.front().sender == "alice" && first.messages.front().worldline == "main");
                auto replay = node.call(request);
                expect(replay.code == "ok" && replay.replayed && replay.messages.front().id == id);
                request.mail->body = "different";
                expect(node.call(request).code == "source_conflict");
                request.actor = "bob";
                auto other = node.call(request);
                expect(other.code == "ok" && !other.replayed && other.messages.front().id > id);
                expect(node.call({.action = "tree"}).version == before.version);
                auto page = node.call({.action = "mail_list", .target = "branch-b", .limit = 1});
                expect(page.code == "ok" && page.truncated && page.messages.size() == 1u && page.messages.front().id == id);
                auto next = node.call({.action = "mail_list", .after = id, .target = "branch-b", .limit = 1});
                expect(next.code == "ok" && !next.truncated && next.messages.size() == 1u && next.messages.front().sender == "bob");
                expect(node.call({.action = "mail_list", .target = "someone-else"}).messages.empty());
            }
            nodes::local_node reopened{tmp.dir};
            auto page = reopened.call({.action = "mail_list", .target = "branch-b"});
            expect((page.code == "ok" && page.messages.size() == 2u) >> fatal);
            expect(page.messages.front().id == id && page.messages.front().body.contains("世界"));
            expect(reopened.call({.action = "mail_send", .mail = nodes::mail_input{"branch-b", "body"}, .operation = operation}).code == "invalid_record");
            expect(reopened
                       .call({.action = "mail_send",
                              .mail = nodes::mail_input{"branch-b", std::string{"x\0y", 3}},
                              .operation = content::hash_of("binary").str(),
                              .actor = "alice"})
                       .code == "invalid_record");
        };
    };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"mail">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
