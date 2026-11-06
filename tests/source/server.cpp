import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import configuration;
import registry;
import usage;
import server;
import client;
import tools;
import fleet;
import mcp;
import support;
import fixture;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = native::scheduler;

    static constexpr auto cases = [] {
        "business tools work on the main worldline and language preparation errors are explicit"_test = [] {
            test::scratch tmp{"source-tools"};
            config::document document;
            document.projects["app"].build_configurations["native"] = fixture::cmake("build/native");
            fixture::options<"cmake">(document.projects["app"].build_configurations["native"].builders.at("main")).export_compile_commands = true;
            document.projects["app"].analysis["Cpp"] = {.build_configuration = "native"};
            config::validate(document);
            config::slots config{1, std::make_shared<const config::document>(std::move(document))};
            cores::io_context<native::context> rt{1};
            routing::router router{rt};
            execution::thread_pool gateway_disk{2}, source_disk{2};
            source_server::service source{rt, tmp.dir / "source", source_disk};
            rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                storage::registry files{rt, tmp.dir / "state", config, gateway_disk,
                                        net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()}};
                tools::services services{config.slot(0), router, files, 0};
                tools::backlog queue{rt, services};
                services.queue = &queue;
                mcp::notifier note;
                const worlds::actor tester{.issuer = "token", .user = "tester", .caller = "suite", .role = "owner"};
                co_await coroutine::with_scope([&](coroutine::scope& tasks) -> coroutine::task<> {
                    tasks.spawn(queue.serve(io));
                    tasks.spawn([](ctx& io, auto& listener, auto& service, auto& tasks) -> coroutine::task<> {
                        for (;;)
                            tasks.spawn(service.serve(
                                io, co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); }), 0));
                    }(io, listener, source, tasks));
                    tools::standard::bound toolbox{services};
                    expect(std::ranges::none_of(toolbox.list(), [](const auto& tool) { return tool.name == "jobs"; }));
                    expect(toolbox.list().size() == 5u);
                    for (const auto name : {"build", "test", "run", "configurations", "language"})
                        expect(std::ranges::any_of(toolbox.list(), [name](const auto& tool) { return tool.name == name; }));
                    co_await fixture::write_file(services, {.project = "app", .path = "a.cpp", .content = "int a;\n", .operation_id = "write"}, note,
                                                 tester);
                    fixture::edit_input edit{.project = "app", .path = "a.cpp", .old_string = "a", .new_string = "alpha", .operation_id = "edit"};
                    auto first = co_await fixture::edit_text(services, edit, note, tester);
                    auto repeated = co_await fixture::edit_text(services, edit, note, tester);
                    expect(!first.replayed && repeated.replayed && repeated.replacements == 1u);
                    bool failed = false;
                    try {
                        co_await tools::language_query::run(services, {.project = "app", .configuration = "Cpp", .action = "diagnostics"}, note, tester);
                    } catch (const tools::tool_error& error) {
                        failed = error.code == "no_worker";
                    }
                    expect(failed) << "analysis without workers never falls back to source execution";
                    auto receipt =
                        co_await tools::worldline::run(services, {.project = "app", .action = "receipt", .operation_id = std::string{"edit"}}, note, tester);
                    expect(receipt.version == first.version && receipt.files->at(0).digest == content::hash_of("int alpha;\n"));
                    auto history = co_await tools::worldline::run(services, {.project = "app", .action = "history"}, note, tester);
                    auto authored = std::ranges::find(*history.entries, std::string{"edit"}, &tools::worldline::history_entry::operation_id);
                    expect(authored != history.entries->end() && authored->actor == "token:tester") << "history names the caller's own operation ID";
                    co_await fixture::move_file(services, {.project = "app", .from = "a.cpp", .to = "src/a.cpp", .operation_id = "move-26"}, note, tester);
                    auto read = co_await fixture::read_file(services, {.project = "app", .path = "src/a.cpp"}, note, tester);
                    expect(read.content == "int alpha;\n");
                    expect(!std::filesystem::exists(tmp.dir / "state/app"));
                });
            });
        };

        "node RPCs refuse callers without a role and retain capture results in source storage"_test = [] {
            test::scratch tmp{"source-node"};
            test::source_service server{tmp.dir / "source"};
            const worlds::actor tester{.issuer = "token", .user = "tester", .caller = "suite", .role = "owner"};
            auto endpoint = server.endpoint();
            // A capture takes the files of the project's directory into its node.
            test::spit(tmp.dir / "source/app/x", "captured");
            jobs::node_capture capture{
                .project = "app", .actor = tester, .operation = content::hash_of("source-node-capture").str(), .intent = "Capture files"};
            auto first = source::call(endpoint, capture);
            expect(first.code == "ok" && !first.replayed);
            auto replay = source::call(endpoint, capture);
            expect(replay.code == "ok" && replay.replayed && replay.version == first.version);
            auto read =
                source::call(endpoint, jobs::node_call{.project = "app", .actor = tester, .request = {.action = "read", .target = "x"}, .reading = true});
            expect((read.code == "ok" && read.files.size() == 1u) >> fatal);
            expect(read.files[0].text == "captured");
            expect(std::filesystem::exists(tmp.dir / "source/.nodes/app"));
            auto stranger = tester;
            stranger.role = "visitor";
            auto denied = [&](auto request) {
                request.actor = stranger;
                try {
                    source::call(endpoint, request);
                    expect(false) << "node request accepted from a caller that is neither owner nor member";
                } catch (const transport::remote_error& error) {
                    expect(error.code == "permission_denied");
                }
            };
            denied(jobs::node_call{.project = "app", .request = {.action = "inspect"}, .reading = true});
            denied(capture);
        };

        "node writers keep their own operation space and attribution"_test = [] {
            test::scratch tmp{"source-node-writers"};
            test::source_service server{tmp.dir / "source"};
            const worlds::actor alice{.issuer = "token", .user = "alice", .caller = "suite", .role = "owner"};
            const worlds::actor bob{.issuer = "token", .user = "bob", .caller = "suite", .role = "member"};
            auto endpoint = server.endpoint();
            auto call = [&](const worlds::actor& who, nodes::node_request request) {
                return source::call(endpoint, jobs::node_call{.project = "app", .actor = who, .request = std::move(request)});
            };
            expect(call(alice, {.action = "initialize"}).code == "ok");
            auto operation = content::hash_of("shared-operation-id").str();
            auto seed = call(alice, {.action = "apply", .operation = operation, .edits = {{.kind = "create", .path = "x", .text = "ab"}}});
            expect((seed.code == "ok") >> fatal) << seed.reason;
            expect(seed.operation == operation) << "the caller sees its own operation ID";
            auto other = call(
                bob,
                {.action = "apply", .operation = operation, .expected = seed.frontier, .edits = {{.kind = "edit", .path = "x", .text = "c", .offset = 2}}});
            expect((other.code == "ok") >> fatal) << other.reason;
            expect(!other.replayed && other.receipt != seed.receipt) << "the same ID from another caller is a new operation";
            auto again = call(alice, {.action = "apply", .operation = operation, .edits = {{.kind = "create", .path = "x", .text = "ab"}}});
            expect(again.replayed && again.receipt == seed.receipt);
            auto history = call(alice, {.action = "history"});
            std::set<std::string> actors;
            for (auto& entry : history.history) {
                actors.insert(entry.actor);
                expect(entry.operation == operation) << "history shows the caller's own operation ID";
            }
            expect(actors == std::set<std::string>{"token:alice", "token:bob"});
            auto token = alice;
            token.caller = "renamed-token";
            auto retried = call(token, {.action = "apply", .operation = operation, .edits = {{.kind = "create", .path = "x", .text = "ab"}}});
            expect(retried.replayed && retried.receipt == seed.receipt) << "another credential of the same user replays";
            auto by_receipt = call(bob, {.action = "receipt", .target = other.receipt});
            expect(by_receipt.operation == operation);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"source">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
