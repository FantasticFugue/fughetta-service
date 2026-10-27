import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import configuration;
import registry;
import usage;
import server;
import fleet;
import tools;
import language;
import commands;
import executor;
import host;
import mcp;
import support;
import fixture;
import templates;
import content;
import package;
import reflection;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = native::scheduler;

    auto worker_loop(ctx& io, std::uint16_t port, replica::service& worker) -> coroutine::task<> {
        auto sock = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, port});
        transport::channel<native::socket> chan{sock};
        transport::dispatcher<native::socket> dispatch;
        worker.install(dispatch, 0);
        co_await chan.post(jobs::hello{.name = "source-test-worker",

                                       .platform = {.os = "linux", .arch = "x86_64"},
                                       .max_inflight = 4,
                                       .protocol = jobs::protocol_version,
                                       .tools = host::detect_tools(test::toolset())});
        co_await coroutine::with_scope([&](coroutine::scope& handlers) -> coroutine::task<> {
            handlers.spawn([](ctx& io, auto& channel) -> coroutine::task<> {
                for (;;) {
                    co_await execution::schedule_after(io, 2s);
                    co_await channel.post(jobs::heartbeat{});
                }
            }(io, chan));
            for (;;)
                handlers.spawn(dispatch.handle(chan, co_await chan.recv()));
        });
    }

    static constexpr auto cases = [] {
        "gateway file tools, worldlines and builds work on the source's node worldlines"_test = [] {
            test::scratch tmp{"source"};
            config::document configuration;
            configuration.limits.max_file_bytes = 16777216;
            auto& project = configuration.projects["app"];
            project.ignore = {".git"};
            project.build_configurations["Dev"] = fixture::cmake("build/dev", "cc");
            project.build_configurations["print"] =
                fixture::commands({"python", "-c", "import sys; from pathlib import Path; sys.stdout.buffer.write(Path('src/message.txt').read_bytes())"});
            project.default_configurations = {"print"};
            configuration.projects["tiny"] = {};
            configuration.projects["full"] = {};
            config::validate(configuration);
            config::slots config{1, std::make_shared<const config::document>(std::move(configuration))};
            cores::io_context<native::context> rt{1};
            execution::thread_pool gateway_disk{2}, source_disk{2}, worker_disk{2};
            source_server::service source{rt, tmp.dir / "source", source_disk, test::toolset()};
            routing::router router{rt};
            replica::service worker{rt, replica::options{.root = tmp.dir / "replica", .worker = "source-test-worker", .tools = test::toolset()}, worker_disk};
            rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                auto sources = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto workers = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                storage::registry registry{rt, tmp.dir / "state", config, gateway_disk,
                                           net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, sources.local_endpoint().port()}};
                tools::services services{config.slot(0), router, registry, 0};
                tools::backlog queue{rt, services};
                services.queue = &queue;
                mcp::notifier note;
                const worlds::actor tester{.issuer = "token", .user = "tester", .caller = "test-suite", .role = "owner"};
                co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn([](auto& io, auto& listener, auto& source, auto& scope) -> coroutine::task<> {
                        for (;;)
                            scope.spawn(source.serve(
                                io, co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); }), 0));
                    }(io, sources, source, children));
                    children.spawn(worker_loop(io, workers.local_endpoint().port(), worker));
                    children.spawn(queue.serve(io));
                    children.spawn(
                        router.workers(0).admit(io, co_await execution::then(net::async_accept(workers), [](auto socket, auto) { return std::move(socket); })));
                    while (!router.workers(0).holds({}))
                        co_await execution::schedule_after(io, 1ms);

                    auto line = [&](tools::worldline::input in) -> coroutine::task<tools::worldline::output> {
                        in.project = std::string{"app"};
                        co_return co_await tools::worldline::run(services, std::move(in), note, tester);
                    };
                    auto refused = [&](auto run) -> coroutine::task<std::string> {
                        try {
                            co_await run();
                        } catch (const tools::tool_error& e) {
                            co_return e.code;
                        }
                        co_return std::string{};
                    };
                    auto unlocked = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::write::run(services, {.project = "app", .path = "src/message.txt", .content = "x", .operation_id = "write-1"}, note,
                                                   tester);
                    });
                    expect(unlocked == "scope_locked") << "a file-level change needs the global namespace";
                    co_await line({.action = "lock", .scopes = std::vector<std::string>{"::"}});
                    auto written = co_await tools::write::run(
                        services, {.project = "app", .path = "src/message.txt", .content = "hello source\n", .operation_id = "write-2"}, note, tester);
                    auto read = co_await tools::read::run(services, {.project = "app", .path = "src/message.txt"}, note, tester);
                    expect(read.content == "hello source\n" && read.version == written.version);
                    auto edit_input = tools::edit::input{.project = "app",
                                                         .path = "src/message.txt",
                                                         .old_string = "hello",
                                                         .new_string = "hello again",
                                                         .operation_id = std::string{"text-edit"}};
                    auto edited = co_await tools::edit::run(services, edit_input, note, tester);
                    auto repeated = co_await tools::edit::run(services, edit_input, note, tester);
                    expect(!edited.replayed && repeated.replayed && repeated.receipt == edited.receipt);
                    co_await tools::write::run(services, {.project = "app", .path = "src/message.txt", .content = "hello source\n", .operation_id = "write-3"},
                                               note, tester);
                    auto first = co_await tools::build::run(services, {.project = "app"}, note, tester);
                    expect(first.ok && first.results.at(0).steps.at(0).output == "hello source\n") << config::write(first);
                    expect(first.results.at(0).bytes_sent == 13u && first.worldline == "main");
                    auto cached = co_await tools::build::run(services, {.project = "app"}, note, tester);
                    expect(cached.ok && cached.results.at(0).steps.at(0).output == "hello source\n" && cached.results.at(0).bytes_sent == 13u)
                        << "an equal build answers from the first one's result";
                    auto fresh = co_await tools::build::run(services, {.project = "app", .fresh = true}, note, tester);
                    expect(fresh.ok && fresh.results.at(0).bytes_sent == 0u) << "a fresh build runs again, on the worker that has the files";
                    auto pinned = co_await tools::build::run(services, {.project = "app", .version = first.revision}, note, tester);
                    co_await tools::edit::run(
                        services, {.project = "app", .path = "src/message.txt", .old_string = "hello", .new_string = "goodbye", .operation_id = "edit-4"}, note,
                        tester);
                    auto changed = co_await tools::build::run(services, {.project = "app"}, note, tester);
                    expect(changed.ok && changed.results.at(0).steps.at(0).output == "goodbye source\n");
                    expect(changed.results.at(0).bytes_sent == 15u);
                    auto older = co_await tools::build::run(services, {.project = "app", .version = pinned.revision}, note, tester);
                    expect(older.ok && older.results.at(0).steps.at(0).output == "hello source\n") << "a named version builds exactly that version";

                    auto hits = co_await tools::search::run(services, {.project = "app", .query = "source"}, note, tester);
                    expect(hits.matches.size() == 1u && hits.matches.at(0).path == "src/message.txt");
                    auto listed = co_await tools::list::run(services, {.project = "app"}, note, tester);
                    expect(listed.files.size() == 1u);
                    co_await tools::move::run(services, {.project = "app", .from = "src/message.txt", .to = "src/moved.txt", .operation_id = "move-5"}, note,
                                              tester);
                    auto moved = co_await tools::read::run(services, {.project = "app", .path = "src/moved.txt"}, note, tester);
                    expect(moved.content == "goodbye source\n");
                    auto escaped = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::write::run(services, {.project = "app", .path = "../escaped", .content = "x", .operation_id = "write-6"}, note, tester);
                    });
                    expect(escaped == "invalid_record") << escaped;
                    auto oversized = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::write::run(
                            services, {.project = "app", .path = "too-big", .content = std::string(16777217, 'x'), .operation_id = "write-7"}, note, tester);
                    });
                    expect(oversized == "resource_limit") << oversized;

                    co_await tools::write::run(
                        services, {.project = "app", .path = "big.bin", .content = std::string(9u << 20, 'z'), .operation_id = "write-8"}, note, tester);
                    auto multipart = co_await tools::build::run(services, {.project = "app"}, note, tester);
                    expect(!multipart.ok && multipart.results.at(0).bytes_sent == (9u << 20));
                    co_await tools::remove::run(services, {.project = "app", .path = "big.bin", .operation_id = "remove-9"}, note, tester);
                    auto history = co_await registry.with_ledger(0, [](usage::ledger& ledger, const auto&) { return ledger.builds("app", 20); });
                    expect(history.size() == 4u && std::filesystem::exists(tmp.dir / "state/.source-usage/events.jsonl"))
                        << "builds equal to earlier ones ran once: " << history.size();

                    co_await tools::move::run(services, {.project = "app", .from = "src/moved.txt", .to = "src/message.txt", .operation_id = "move-10"}, note,
                                              tester);
                    auto wa = co_await line({.action = "fork", .name = std::string{"agent-a"}, .operation_id = std::string{"fork-a"}});
                    auto wb = co_await line({.action = "fork", .name = std::string{"agent-b"}, .operation_id = std::string{"fork-b"}});
                    expect(wa.version == wb.version);
                    auto listed_lines = co_await line({.action = "list"});
                    expect(listed_lines.worldlines->size() == 3u);
                    auto forked = co_await line({.action = "fork", .name = std::string{"agent-c"}, .operation_id = std::string{"fork-c"}});
                    co_await tools::write::run(services, {.project = "app", .path = "src/later.txt", .content = "main moved on\n", .operation_id = "write-11"},
                                               note, tester);
                    auto reforked = co_await line({.action = "fork", .name = std::string{"agent-c"}, .operation_id = std::string{"fork-c"}});
                    expect(*reforked.replayed && reforked.version == forked.version) << "a fork retried after its source moved answers from its receipt";
                    auto renamed = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "fork", .name = std::string{"agent-d"}, .operation_id = std::string{"fork-c"}});
                    });
                    expect(renamed == "identity_conflict") << renamed;
                    co_await tools::remove::run(services, {.project = "app", .path = "src/later.txt", .operation_id = "remove-12"}, note, tester);

                    // Two identical requests in flight at once: one writes, the other answers from its receipt.
                    tools::write::input twice{.project = "app", .path = "src/twice.txt", .content = "once\n", .operation_id = std::string{"twice"}};
                    std::vector<tools::mutation> answers;
                    std::vector<std::string> failures;
                    co_await coroutine::with_scope([&](coroutine::scope& both) -> coroutine::task<> {
                        for (int i = 0; i < 2; ++i)
                            both.spawn([](auto& services, auto in, auto& note, auto& tester, auto& answers, auto& failures) -> coroutine::task<> {
                                try {
                                    answers.push_back(co_await tools::write::run(services, std::move(in), note, tester));
                                } catch (const tools::tool_error& e) {
                                    failures.push_back(e.code + ": " + e.what());
                                } catch (const std::exception& e) {
                                    failures.push_back(e.what());
                                }
                            }(services, twice, note, tester, answers, failures));
                        co_await both.join();
                    });
                    expect(failures.empty()) << (failures.empty() ? std::string{} : failures.front());
                    expect((answers.size() == 2u) >> fatal);
                    expect(std::ranges::count(answers, true, &tools::mutation::replayed) == 1);
                    expect(answers.front().receipt == answers.back().receipt);
                    co_await tools::remove::run(services, {.project = "app", .path = "src/twice.txt", .operation_id = "remove-13"}, note, tester);

                    // Small limits for project tiny; the workspace has one set of limits, so they are put back afterwards.
                    auto roomy = config.current(0);
                    auto with_limits = [&](config::limits limits) {
                        auto changed = *roomy;
                        changed.limits = limits;
                        config.set(0, std::make_shared<const config::document>(std::move(changed)));
                    };
                    with_limits({.max_file_bytes = 64, .max_project_bytes = 100});
                    auto tiny = [&](tools::worldline::input in) -> coroutine::task<tools::worldline::output> {
                        in.project = std::string{"tiny"};
                        co_return co_await tools::worldline::run(services, std::move(in), note, tester);
                    };
                    co_await tiny({.action = "lock", .scopes = std::vector<std::string>{"::"}});
                    co_await tools::write::run(services, {.project = "tiny", .path = "full.txt", .content = std::string(64, 'x'), .operation_id = "write-14"},
                                               note, tester);
                    auto full = co_await tiny({.action = "inspect"});
                    auto resize = [&](std::string id, std::uint64_t count, std::string text) {
                        return tiny({.action = "apply",
                                     .observed = full.version,
                                     .operation_id = std::move(id),
                                     .changes = std::vector<tools::change>{
                                         {.kind = "edit", .path = "full.txt", .offset = std::uint64_t{0}, .count = count, .text = std::move(text)}}});
                    };
                    auto grown = co_await refused([&]() -> coroutine::task<> { co_await resize("grow", 0, "x"); });
                    expect(grown == "resource_limit") << "an edit cannot grow a file past the file limit: " << grown;
                    co_await resize("same-size", 1, "y");
                    // A package whose file is past the project's file limit is refused before anything is created.
                    auto tiny_packages = tmp.dir / "source/.packages";
                    auto big = std::string(100, 'b');
                    test::spit(tiny_packages / "big/blobs" / content::hash_of(big).view(), big);
                    test::spit(
                        tiny_packages / "big/manifest.json",
                        std::format(
                            R"({{"format":1,"project":"tiny","worldline":"main","version":"v","exported_at":0,"files":[{{"path":"big.txt","digest":"{}","kind":"text","size":100,"executable":false}}]}})",
                            content::hash_of(big)));
                    auto too_big = co_await refused([&]() -> coroutine::task<> {
                        co_await tiny(
                            {.action = "restore", .name = std::string{"big"}, .operation_id = std::string{"restore-big"}, .package = std::string{"big"}});
                    });
                    expect(too_big == "resource_limit") << too_big;
                    auto tiny_lines = co_await tiny({.action = "list"});
                    expect(!std::ranges::contains(*tiny_lines.worldlines, std::string{"big"}, &tools::worldline::line_entry::name))
                        << "no worldline was created";
                    // An empty package restores as an empty worldline that answers to the caller's operation.
                    test::spit(tiny_packages / "empty/manifest.json",
                               R"({"format":1,"project":"tiny","worldline":"main","version":"v","exported_at":0,"files":[]})");
                    std::filesystem::create_directories(tiny_packages / "empty/blobs");
                    tools::worldline::input restore_empty{
                        .action = "restore", .name = std::string{"blank"}, .operation_id = std::string{"restore-empty"}, .package = std::string{"empty"}};
                    auto blank = co_await tiny(restore_empty);
                    auto blank_receipt = co_await tiny({.action = "receipt", .worldline = std::string{"blank"}, .operation_id = std::string{"restore-empty"}});
                    expect(blank_receipt.version == blank.version);
                    std::filesystem::remove_all(tiny_packages / "empty");
                    auto blank_again = co_await tiny(restore_empty);
                    expect(*blank_again.replayed && blank_again.version == blank.version) << "a replay does not read the package again";
                    auto other_package = co_await refused([&]() -> coroutine::task<> {
                        co_await tiny(
                            {.action = "restore", .name = std::string{"blank"}, .operation_id = std::string{"restore-empty"}, .package = std::string{"big"}});
                    });
                    expect(other_package == "identity_conflict") << "an operation ID answers only for its own restore: " << other_package;
                    co_await tools::remove::run(services, {.project = "tiny", .path = "full.txt", .operation_id = "drop"}, note, tester);
                    co_await tools::write::run(services, {.project = "tiny", .path = "other.txt", .content = std::string(64, 'o'), .operation_id = "other"},
                                               note, tester);
                    auto past_tiny = co_await tiny({.action = "history"});
                    auto drop = std::ranges::find(*past_tiny.entries, std::string{"drop"}, &tools::worldline::history_entry::operation_id);
                    expect((drop != past_tiny.entries->end()) >> fatal);
                    auto now_tiny = co_await tiny({.action = "inspect"});
                    auto restored = co_await refused([&]() -> coroutine::task<> {
                        co_await tiny({.action = "reverse", .observed = now_tiny.version, .operation_id = std::string{"undrop"}, .target = drop->id});
                    });
                    expect(restored == "resource_limit") << "a reversal cannot bring a project past its limit: " << restored;
                    std::filesystem::remove_all(tiny_packages / "big");

                    // A workspace whose repositories reach its storage limit takes no more history, but its locks still work.
                    with_limits({.max_workspace_bytes = 1});
                    co_await tools::worldline::run(services, {.project = "full", .action = "lock", .scopes = std::vector<std::string>{"::"}}, note, tester);
                    auto stored_full = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::write::run(services, {.project = "full", .path = "a.txt", .content = "a", .operation_id = "full"}, note, tester);
                    });
                    expect(stored_full == "resource_limit") << stored_full;
                    // A write that reaches the limit commits; its replay still answers, and the next new write is refused.
                    with_limits({.max_workspace_bytes = jobs::stored(co_await registry.usage(0, tester)) + 1});
                    tools::write::input filling{.project = "full", .path = "a.txt", .content = "a", .operation_id = "fills"};
                    auto filled = co_await tools::write::run(services, filling, note, tester);
                    auto refilled = co_await tools::write::run(services, filling, note, tester);
                    expect(!filled.replayed && refilled.replayed && refilled.receipt == filled.receipt) << "a replay at the limit answers its receipt";
                    // Below what the workspace stores, whatever its repositories' write-ahead logs hold at the moment.
                    with_limits({.max_workspace_bytes = 1});
                    auto beyond = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::write::run(services, {.project = "full", .path = "b.txt", .content = "b", .operation_id = "beyond"}, note, tester);
                    });
                    expect(beyond == "resource_limit") << beyond;
                    auto exported_full = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::worldline::run(services, {.project = "full", .action = "export", .package = std::string{"copy"}}, note, tester);
                    });
                    expect(exported_full == "resource_limit") << "an export counts toward the workspace's storage: " << exported_full;
                    if (std::filesystem::exists(tmp.dir / "source/.packages"))
                        for (auto& entry : std::filesystem::directory_iterator(tmp.dir / "source/.packages"))
                            expect(!entry.path().filename().string().starts_with(".staging-")) << "a refused export leaves nothing: " << entry.path();
                    co_await tools::worldline::run(services, {.project = "full", .action = "unlock", .scopes = std::vector<std::string>{"::"}}, note, tester);
                    config.set(0, roomy);
                    auto used = co_await registry.usage(0, tester);
                    auto tiny_used = std::ranges::find(used.projects, std::string{"tiny"}, &jobs::project_usage::project);
                    expect((tiny_used != used.projects.end()) >> fatal);
                    expect(tiny_used->source_bytes == 64u && tiny_used->storage_bytes > 64u);

                    // A preparation a worker can take runs there, in a build tree beside the builds; the source keeps its receipt.
                    {
                        auto with_analysis = *roomy;
                        language::configuration analysis;
                        builds::config<"command"> command;
                        command.operations["prepare"] = {.argv = {"python", "-c",
                                                                  "import json; from pathlib import Path; "
                                                                  "Path('commands.json').write_text(json.dumps([{'directory':str(Path.cwd()),'file':'main.c','"
                                                                  "arguments':['cc','-c','main.c']}]))"},
                                                         .outputs = {{"db", {"compilation_database", "commands.json"}}}};
                        analysis.build.builders["main"] = builds::with<"command">{.options = command};
                        analysis.build.tasks["db"] = {.builder = "main", .operation = "prepare"};
                        analysis.build.operations["prepare"] = {.tasks = {"db"}};
                        analysis.build.outputs["db"] = {.task = "db", .output = "db"};
                        analysis.inputs = {"db"};
                        // A server that exits at once: the survey fails, the preparation does not.
                        analysis.server = fixture::clice("python", {"-c", "raise SystemExit(1)"});
                        with_analysis.projects.at("app").analysis["Db"] = analysis;
                        config.set(0, std::make_shared<const config::document>(std::move(with_analysis)));
                        tools::language_query::input ask{
                            .project = "app", .configuration = std::string{"Db"}, .action = std::string{"prepare"}, .operation_id = std::string{"on-worker"}};
                        auto first = co_await tools::language_query::run(services, ask, note, tester);
                        expect((first.preparation.has_value()) >> fatal);
                        expect(first.preparation->status == "prepared" && first.preparation->worker == "source-test-worker" && !first.preparation->replayed)
                            << config::write(*first.preparation);
                        expect(first.preparation->inputs.size() == 1u && first.preparation->tree.contains("replica"));
                        expect(first.preparation->analysis == "failed" && first.preparation->analysis_error.has_value()) << "the survey's failure is reported";
                        auto again = co_await tools::language_query::run(services, ask, note, tester);
                        expect(again.preparation->replayed && again.preparation->status == "prepared") << "a repeated preparation answers its receipt";
                        ask.action = "receipt";
                        auto receipt = co_await tools::language_query::run(services, ask, note, tester);
                        expect(receipt.preparation->status == "prepared" && receipt.preparation->worker == "source-test-worker");

                        auto slow = analysis;
                        fixture::options<"command">(slow.build.builders["main"]).operations["prepare"].argv = {"python", "-c", "import time; time.sleep(5)"};
                        auto with_slow = *config.current(0);
                        with_slow.projects.at("app").analysis["Slow"] = slow;
                        config.set(0, std::make_shared<const config::document>(std::move(with_slow)));
                        tools::language_query::input cancelling{.project = "app",
                                                                .configuration = std::string{"Slow"},
                                                                .action = std::string{"prepare"},
                                                                .operation_id = std::string{"cancel-worker-preparation"}};
                        auto cancelled = co_await coroutine::deadline(io, 1s, tools::language_query::run(services, cancelling, note, tester));
                        expect(!cancelled);
                        cancelling.action = "receipt";
                        auto cancellation_receipt = co_await tools::language_query::run(services, cancelling, note, tester);
                        for (int retry = 0; retry < 100 && cancellation_receipt.preparation->status == "preparing"; ++retry) {
                            co_await execution::schedule_after(io, 10ms);
                            cancellation_receipt = co_await tools::language_query::run(services, cancelling, note, tester);
                        }
                        expect(cancellation_receipt.preparation->status == "cancelled") << config::write(*cancellation_receipt.preparation);
                        expect(cancellation_receipt.preparation->worker == "source-test-worker");

                        // A preparation that rewrites a tracked file fails, and its receipt says so.
                        auto mutating = analysis;
                        builds::config<"command"> rewrite;
                        rewrite.operations["prepare"] = {.argv = {"python", "-c",
                                                                  "import json; from pathlib import Path; "
                                                                  "Path('commands.json').write_text(json.dumps([{'directory':str(Path.cwd()),'file':'main.c','"
                                                                  "arguments':['cc','-c','main.c']}])); Path('src/message.txt').write_text('changed')"},
                                                         .outputs = {{"db", {"compilation_database", "commands.json"}}}};
                        mutating.build.builders["main"] = builds::with<"command">{.options = rewrite};
                        auto with_mutation = *config.current(0);
                        with_mutation.projects.at("app").analysis["Rewrite"] = mutating;
                        config.set(0, std::make_shared<const config::document>(std::move(with_mutation)));
                        tools::language_query::input rewriting{.project = "app",
                                                               .configuration = std::string{"Rewrite"},
                                                               .action = std::string{"prepare"},
                                                               .operation_id = std::string{"rewrites"}};
                        auto refused = co_await tools::language_query::run(services, rewriting, note, tester);
                        expect(refused.preparation->status == "failed" && refused.preparation->error.contains("changed tracked file"))
                            << config::write(*refused.preparation);
                        rewriting.action = "receipt";
                        expect((co_await tools::language_query::run(services, rewriting, note, tester)).preparation->status == "failed")
                            << "the receipt is final, not left preparing";
                        config.set(0, roomy);
                    }

                    tools::standard::bound box{services};
                    auto malformed = co_await box.call(
                        "worldline", rfl::json::read<rfl::Generic>(R"({"project":"app","action":"apply","changes":[{"kind":"edit","offset":"x"}]})").value(),
                        note, tester);
                    expect(malformed.error && malformed.text.contains(R"("code":"invalid_record")")) << malformed.text;
                    auto anonymous = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::generate::run(services,
                                                      {.project = "app",
                                                       .action = std::string{"apply"},
                                                       .parameters = templates::arguments{.template_id = "cpp-class", .name = "Anonymous"}},
                                                      note, tester);
                    });
                    expect(anonymous == "invalid_record") << "a generation names its operation ID";
                    co_await line({.action = "lock", .worldline = std::string{"agent-a"}, .scopes = std::vector<std::string>{"::"}});
                    auto changed_a = co_await line(
                        {.action = "apply",
                         .worldline = std::string{"agent-a"},
                         .observed = wa.version,
                         .operation_id = std::string{"edit-a"},
                         .intent = std::string{"change greeting"},
                         .changes = std::vector<tools::change>{{.kind = "write", .path = "src/message.txt", .text = std::string{"worldline a\n"}}}});
                    auto again_a = co_await line(
                        {.action = "apply",
                         .worldline = std::string{"agent-a"},
                         .observed = wa.version,
                         .operation_id = std::string{"edit-a"},
                         .intent = std::string{"change greeting"},
                         .changes = std::vector<tools::change>{{.kind = "write", .path = "src/message.txt", .text = std::string{"worldline a\n"}}}});
                    expect(*again_a.replayed && again_a.version == changed_a.version);
                    auto receipt = co_await line({.action = "receipt", .worldline = std::string{"agent-a"}, .operation_id = std::string{"edit-a"}});
                    expect(receipt.version == changed_a.version && receipt.files->size() == 1u);
                    auto content =
                        co_await line({.action = "read", .worldline = std::string{"agent-a"}, .version = wa.version, .path = std::string{"src/message.txt"}});
                    expect(content.data_base64 == std::optional<std::string>{"Z29vZGJ5ZSBzb3VyY2UK"} && content.size == std::optional<std::uint64_t>{15u})
                        << "a pinned version stays readable after the worldline moves";
                    auto built_a = co_await tools::build::run(services, {.project = "app", .worldline = "agent-a"}, note, tester);
                    auto built_b = co_await tools::build::run(services, {.project = "app", .worldline = "agent-b"}, note, tester);
                    expect(built_a.ok && built_a.results.at(0).steps.at(0).output == "worldline a\n");
                    expect(built_b.ok && built_b.results.at(0).steps.at(0).output == "goodbye source\n");
                    auto warm_a = co_await tools::build::run(services, {.project = "app", .worldline = "agent-a", .fresh = true}, note, tester);
                    expect(warm_a.ok && warm_a.results.at(0).bytes_sent == 0u);
                    auto main_read = co_await tools::read::run(services, {.project = "app", .path = "src/message.txt"}, note, tester);
                    expect(main_read.content == "goodbye source\n");
                    auto analysis = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::language_query::run(services, {.project = "app", .worldline = "agent-a", .action = "scopes"}, note, tester);
                    });
                    expect(analysis == "invalid_record") << "a project without an analysis configuration has nothing to query";

                    auto project_a = co_await line(
                        {.action = "apply",
                         .worldline = std::string{"agent-a"},
                         .observed = changed_a.version,
                         .operation_id = std::string{"create-c-project"},
                         .changes = std::vector<tools::change>{
                             {.kind = "create",
                              .path = "CMakeLists.txt",
                              .text = std::string{"cmake_minimum_required(VERSION 3.20)\nproject(world C)\nadd_executable(hello main.c helper.c)\n"}},
                             {.kind = "create", .path = "main.c", .text = std::string{"int helper(void); int main(void) { return helper(); }\n"}},
                             {.kind = "create", .path = "helper.c", .text = std::string{"int helper(void) { return 0; }\n"}}}});
                    auto compiled = co_await tools::build::run(
                        services, {.project = "app", .configurations = std::vector<std::string>{"Dev"}, .worldline = "agent-a"}, note, tester);
                    expect(compiled.ok) << config::write(compiled);
                    expect(compiled.ok >> fatal);
                    std::filesystem::path object;
                    for (auto& entry : std::filesystem::recursive_directory_iterator(tmp.dir / "replica"))
                        if ((entry.path().filename() == "main.c.o" || entry.path().filename() == "main.c.obj"))
                            object = entry.path();
                    expect(!object.empty() >> fatal);
                    auto stamp = std::filesystem::last_write_time(object);
                    co_await line({.action = "apply",
                                   .worldline = std::string{"agent-a"},
                                   .observed = project_a.version,
                                   .operation_id = std::string{"change-helper"},
                                   .changes = std::vector<tools::change>{
                                       {.kind = "write", .path = "helper.c", .text = std::string{"int helper(void) { return 1; }\n"}}}});
                    auto incremental = co_await tools::build::run(
                        services, {.project = "app", .configurations = std::vector<std::string>{"Dev"}, .worldline = "agent-a"}, note, tester);
                    expect(incremental.ok && incremental.snapshot != compiled.snapshot);
                    expect(incremental.results.at(0).bytes_sent == std::string_view{"int helper(void) { return 1; }\n"}.size());
                    expect(std::filesystem::last_write_time(object) == stamp);

                    auto past = co_await line({.action = "history", .worldline = std::string{"agent-a"}});
                    expect(!past.entries->empty());
                    auto greeting = std::ranges::find(*past.entries, std::string{"edit-a"}, &tools::worldline::history_entry::operation_id);
                    expect((greeting != past.entries->end()) >> fatal);
                    auto described = co_await line({.action = "contribution", .worldline = std::string{"agent-a"}, .target = greeting->id});
                    expect((described.changes && described.changes->size() == 1u) >> fatal);
                    auto& greeted = described.changes->front();
                    expect(greeted.type == "modified" && greeted.before && greeted.after);
                    expect(greeted.before->text == std::optional<std::string>{"goodbye source\n"} &&
                           greeted.after->text == std::optional<std::string>{"worldline a\n"});
                    expect(described.entries->front().operation_id == "edit-a" && described.base_version == wa.version);
                    expect(described.contained_in && std::ranges::contains(*described.contained_in, std::string{"agent-a"}) &&
                           !std::ranges::contains(*described.contained_in, std::string{"agent-b"}))
                        << "a contribution names the worldlines that contain it";
                    auto unknown = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "contribution", .worldline = std::string{"agent-a"}, .target = std::string(64, 'f')});
                    });
                    expect(unknown == "missing_objects") << unknown;
                    co_await tools::write::run(services, {.project = "app", .path = "blob.dat", .content = std::string("\0\1\2", 3), .operation_id = "binary"},
                                               note, tester);
                    auto main_past = co_await line({.action = "history"});
                    auto binary = std::ranges::find(*main_past.entries, std::string{"binary"}, &tools::worldline::history_entry::operation_id);
                    expect((binary != main_past.entries->end()) >> fatal);
                    auto described_binary = co_await line({.action = "contribution", .target = binary->id});
                    expect((described_binary.changes && described_binary.changes->size() == 1u && described_binary.changes->front().after) >> fatal);
                    expect(described_binary.changes->front().after->kind == "binary" && !described_binary.changes->front().after->text)
                        << "a contribution never holds binary content";
                    co_await tools::remove::run(services, {.project = "app", .path = "blob.dat", .operation_id = "binary-gone"}, note, tester);

                    // A version written out as a package comes back as a new worldline holding exactly its files.
                    auto exported = co_await line(
                        {.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"greeting"}});
                    expect(exported.version == changed_a.version && exported.files && !exported.files->empty());
                    auto again = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "export", .worldline = std::string{"agent-a"}, .package = std::string{"greeting"}});
                    });
                    expect(again == "source_conflict") << "an export does not replace a package unless asked: " << again;
                    tools::worldline::input restoring{.action = "restore",
                                                      .name = std::string{"restored"},
                                                      .operation_id = std::string{"restore-greeting"},
                                                      .package = std::string{"greeting"}};
                    auto unpacked = co_await line(restoring);
                    auto restored_text = co_await line({.action = "read", .worldline = std::string{"restored"}, .path = std::string{"src/message.txt"}});
                    expect(restored_text.data_base64 == std::optional<std::string>{"d29ybGRsaW5lIGEK"}) << "worldline a, restored";
                    expect(unpacked.files->size() == exported.files->size());
                    expect(unpacked.configuration && unpacked.configuration->contains("print")) << "the package returns the project configuration it kept";
                    auto replayed_restore = co_await line(restoring);
                    expect(*replayed_restore.replayed && replayed_restore.version == unpacked.version);
                    auto restore_receipt =
                        co_await line({.action = "receipt", .worldline = std::string{"restored"}, .operation_id = std::string{"restore-greeting"}});
                    expect(restore_receipt.version == unpacked.version) << "a restore's receipt answers to the caller's operation ID";
                    // Replacing the package changes nothing a recorded restore answers.
                    auto replaced = co_await line({.action = "export", .worldline = std::string{"agent-b"}, .force = true, .package = std::string{"greeting"}});
                    expect(replaced.version != exported.version);
                    auto after_replace = co_await line(restoring);
                    expect(*after_replace.replayed && after_replace.version == unpacked.version && after_replace.configuration == unpacked.configuration);
                    auto blob = tmp.dir / "source/.packages/greeting/blobs" / replaced.files->front().digest;
                    test::spit(blob, "tampered");
                    auto damaged = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"damaged"},
                                       .operation_id = std::string{"restore-damaged"},
                                       .package = std::string{"greeting"}});
                    });
                    expect(damaged == "missing_objects") << "a damaged package restores nothing: " << damaged;
                    auto elsewhere = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"elsewhere"},
                                       .operation_id = std::string{"restore-greeting"},
                                       .package = std::string{"greeting"}});
                    });
                    expect(elsewhere == "identity_conflict") << "an operation ID answers only for the worldline it restored: " << elsewhere;

                    // A replacement a crash cut short between its renames: the new package was not whole, so the old one gets its name back.
                    co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"journaled"}});
                    auto acme_packages = tmp.dir / "source/.packages";
                    std::filesystem::rename(acme_packages / "journaled", acme_packages / ".retired-00000000000000c1");
                    std::filesystem::create_directories(acme_packages / ".staging-00000000000000c1/blobs");
                    test::spit(acme_packages / ".journaled.replacing", R"({"retired":".retired-00000000000000c1","staging":".staging-00000000000000c1"})");
                    std::filesystem::create_directories(acme_packages / ".staging-00000000000000c2");
                    auto recovered = co_await line({.action = "restore",
                                                    .name = std::string{"recovered"},
                                                    .operation_id = std::string{"restore-journaled"},
                                                    .package = std::string{"journaled"}});
                    expect(recovered.files && recovered.files->size() == exported.files->size()) << "the old package is whole again";
                    expect(!std::filesystem::exists(acme_packages / ".journaled.replacing") &&
                           !std::filesystem::exists(acme_packages / ".retired-00000000000000c1") &&
                           !std::filesystem::exists(acme_packages / ".staging-00000000000000c1"));
                    expect(!std::filesystem::exists(acme_packages / ".staging-00000000000000c2")) << "what a crashed export left is swept";
                    // A package whose name looks like scratch keeps its replacement journal, and the old package it names, through a sweep.
                    co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"staging-1"}});
                    std::filesystem::rename(acme_packages / "staging-1", acme_packages / ".retired-00000000000000c3");
                    test::spit(acme_packages / ".staging-1.replacing", R"({"retired":".retired-00000000000000c3","staging":".staging-00000000000000c3"})");
                    co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"bystander"}});
                    auto scratch_named = co_await line(
                        {.action = "restore", .name = std::string{"named"}, .operation_id = std::string{"restore-named"}, .package = std::string{"staging-1"}});
                    expect(scratch_named.files && scratch_named.files->size() == exported.files->size())
                        << "the sweep left the journal of a package named like scratch";
                    // Next to a journal that cannot be trusted nothing is swept, and the package it names is refused, not lost.
                    co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"corrupt"}});
                    std::filesystem::rename(acme_packages / "corrupt", acme_packages / ".retired-00000000000000c4");
                    test::spit(acme_packages / ".corrupt.replacing", "{not json");
                    std::filesystem::create_directories(acme_packages / ".staging-00000000000000c4");
                    auto untrusted = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"corrupt"},
                                       .operation_id = std::string{"restore-corrupt"},
                                       .package = std::string{"corrupt"}});
                    });
                    expect(untrusted == "storage_failure") << untrusted;
                    expect(std::filesystem::exists(acme_packages / ".retired-00000000000000c4") &&
                           std::filesystem::exists(acme_packages / ".staging-00000000000000c4") &&
                           std::filesystem::exists(acme_packages / ".corrupt.replacing"))
                        << "an unreadable journal keeps everything it might name";
                    test::spit(acme_packages / ".corrupt.replacing", R"({"retired":"../../escape","staging":".staging-00000000000000c4"})");
                    auto escaping = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"corrupt"},
                                       .operation_id = std::string{"restore-escape"},
                                       .package = std::string{"corrupt"}});
                    });
                    expect(escaping == "storage_failure") << "a journal naming anything but its scratch is not followed: " << escaping;
                    test::spit(acme_packages / ".corrupt.replacing", R"({"retired":".retired-00000000000000c4","staging":".staging-00000000000000c4"})");
                    auto mended = co_await line({.action = "restore",
                                                 .name = std::string{"corrupt"},
                                                 .operation_id = std::string{"restore-mended"},
                                                 .package = std::string{"corrupt"}});
                    expect(mended.files && mended.files->size() == exported.files->size()) << "once the journal is mended the old package returns";
                    // A staged package that does not load whole, or is a link, never takes the name from the old one.
                    for (auto damage : {"manifest", "link"}) {
                        co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"half"}});
                        std::filesystem::rename(acme_packages / "half", acme_packages / ".retired-00000000000000c5");
                        if (damage == std::string_view{"manifest"}) {
                            std::filesystem::create_directories(acme_packages / ".staging-00000000000000c5/blobs");
                            test::spit(acme_packages / ".staging-00000000000000c5/manifest.json", "{truncated");
                        } else
                            std::filesystem::create_directory_symlink(acme_packages / "greeting", acme_packages / ".staging-00000000000000c5");
                        test::spit(acme_packages / ".half.replacing", R"({"retired":".retired-00000000000000c5","staging":".staging-00000000000000c5"})");
                        auto kept_old = co_await line({.action = "restore",
                                                       .name = std::string{"half-"} + damage,
                                                       .operation_id = std::string{"restore-half-"} + damage,
                                                       .package = std::string{"half"}});
                        expect(kept_old.files && kept_old.files->size() == exported.files->size()) << damage << ": the old package keeps its name";
                        expect(!std::filesystem::is_symlink(acme_packages / "half") && std::filesystem::exists(acme_packages / "greeting/manifest.json"))
                            << damage << ": nothing a link leads to is moved or removed";
                        std::filesystem::remove_all(acme_packages / "half");
                    }
                    // A whole staged package takes the name even though the old one is whole too: it is the newer.
                    {
                        co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"swap"}});
                        std::filesystem::copy(acme_packages / "swap", tmp.dir / "staged-swap", std::filesystem::copy_options::recursive);
                        co_await line({.action = "export", .worldline = std::string{"agent-b"}, .force = true, .package = std::string{"swap"}});
                        std::filesystem::rename(acme_packages / "swap", acme_packages / ".retired-00000000000000c7");
                        std::filesystem::rename(tmp.dir / "staged-swap", acme_packages / ".staging-00000000000000c7");
                        test::spit(acme_packages / ".swap.replacing", R"({"retired":".retired-00000000000000c7","staging":".staging-00000000000000c7"})");
                        co_await line({.action = "restore",
                                       .name = std::string{"swapped"},
                                       .operation_id = std::string{"restore-swapped"},
                                       .package = std::string{"swap"}});
                        auto swapped = co_await line({.action = "read", .worldline = std::string{"swapped"}, .path = std::string{"src/message.txt"}});
                        expect(swapped.data_base64 == std::optional<std::string>{"d29ybGRsaW5lIGEK"}) << "the staged package, worldline a, took the name";
                        expect(!std::filesystem::exists(acme_packages / ".retired-00000000000000c7") &&
                               !std::filesystem::exists(acme_packages / ".swap.replacing"));
                    }
                    // A name that holds no whole package while a replacement is pending keeps both copies and the journal.
                    {
                        co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"held"}});
                        std::filesystem::rename(acme_packages / "held", acme_packages / ".retired-00000000000000c8");
                        std::filesystem::create_directory_symlink(acme_packages / "greeting", acme_packages / "held");
                        test::spit(acme_packages / ".held.replacing", R"({"retired":".retired-00000000000000c8","staging":".staging-00000000000000c8"})");
                        auto held = co_await refused([&]() -> coroutine::task<> {
                            co_await line({.action = "restore",
                                           .name = std::string{"held"},
                                           .operation_id = std::string{"restore-held"},
                                           .package = std::string{"held"}});
                        });
                        expect(held == "storage_failure") << held;
                        expect(std::filesystem::exists(acme_packages / ".retired-00000000000000c8/manifest.json") &&
                               std::filesystem::exists(acme_packages / ".held.replacing"));
                        std::filesystem::remove(acme_packages / "held");
                        std::filesystem::rename(acme_packages / ".retired-00000000000000c8", acme_packages / "held");
                        std::filesystem::remove(acme_packages / ".held.replacing");
                    }
                    // Scratch-named leftovers that are not directories, and a half-written journal, are swept by their own names.
                    test::spit(acme_packages / ".staging-00000000000000c9", "x");
                    test::spit(acme_packages / ".gone.replacing.write-tmp", "{");
                    co_await line({.action = "restore",
                                   .name = std::string{"after-sweep"},
                                   .operation_id = std::string{"restore-after-sweep"},
                                   .package = std::string{"held"}});
                    expect(!std::filesystem::exists(acme_packages / ".staging-00000000000000c9") &&
                           !std::filesystem::exists(acme_packages / ".gone.replacing.write-tmp"));
                    // When neither copy can take the name, both remains and the journal are kept.
                    std::filesystem::create_directories(acme_packages / ".staging-00000000000000ca/blobs");
                    test::spit(acme_packages / ".lost.replacing", R"({"retired":".retired-00000000000000ca","staging":".staging-00000000000000ca"})");
                    auto lost = co_await refused([&]() -> coroutine::task<> {
                        co_await line(
                            {.action = "restore", .name = std::string{"lost"}, .operation_id = std::string{"restore-lost"}, .package = std::string{"lost"}});
                    });
                    expect(lost == "storage_failure") << lost;
                    expect(std::filesystem::exists(acme_packages / ".staging-00000000000000ca") && std::filesystem::exists(acme_packages / ".lost.replacing"));
                    std::filesystem::remove_all(acme_packages / ".staging-00000000000000ca");
                    std::filesystem::remove(acme_packages / ".lost.replacing");
                    // Journal names are exactly what scratch gives, so an embedded NUL or a journal's own name is refused.
                    for (auto bad : {R"({"retired":".retired-00000000000000c6\u0000x","staging":".staging-00000000000000c6"})",
                                     R"({"retired":".retired-00000000000000c6","staging":".staging-1.replacing"})"}) {
                        test::spit(acme_packages / ".odd.replacing", bad);
                        auto odd = co_await refused([&]() -> coroutine::task<> {
                            co_await line(
                                {.action = "restore", .name = std::string{"odd"}, .operation_id = std::string{"restore-odd"}, .package = std::string{"odd"}});
                        });
                        expect(odd == "storage_failure") << bad << ": " << odd;
                    }
                    std::filesystem::remove(acme_packages / ".odd.replacing");
                    // A journal that is not a plain file is not opened, so it cannot hold the workspace's packages.
                    expect(std::system(std::format("mkfifo '{}'", (acme_packages / ".fifo.replacing").string()).c_str()) == 0);
                    auto piped = co_await refused([&]() -> coroutine::task<> {
                        co_await line(
                            {.action = "restore", .name = std::string{"fifo"}, .operation_id = std::string{"restore-fifo"}, .package = std::string{"fifo"}});
                    });
                    expect(piped == "storage_failure") << piped;
                    std::filesystem::remove(acme_packages / ".fifo.replacing");
                    // One digest declared with two sizes would let the limits undercount what is restored.
                    {
                        auto manifest_path = acme_packages / "greeting/manifest.json";
                        auto text = test::slurp(manifest_path);
                        auto doubled = rfl::json::read<package::manifest>(text).value();
                        auto copy = doubled.files.front();
                        copy.path = "src/copy.txt";
                        copy.size = 0;
                        doubled.files.push_back(copy);
                        test::spit(manifest_path, rfl::json::write(doubled));
                        auto sized = co_await refused([&]() -> coroutine::task<> {
                            co_await line({.action = "restore",
                                           .name = std::string{"doubled"},
                                           .operation_id = std::string{"restore-doubled"},
                                           .package = std::string{"greeting"}});
                        });
                        expect(sized == "invalid_record") << sized;
                        test::spit(manifest_path, text);
                    }
                    for (auto& entry : *past.entries)
                        expect(entry.actor == "token:tester") << "history names the signed-in user";
                    auto stale = co_await refused([&]() -> coroutine::task<> {
                        co_await line({.action = "apply",
                                       .worldline = std::string{"agent-b"},
                                       .observed = changed_a.version,
                                       .operation_id = std::string{"cross"},
                                       .changes = std::vector<tools::change>{{.kind = "remove", .path = "src/message.txt"}}});
                    });
                    expect(stale == "worldline_boundary") << stale;
                    auto changed_input = co_await refused([&]() -> coroutine::task<> {
                        co_await line(
                            {.action = "apply",
                             .worldline = std::string{"agent-a"},
                             .observed = wa.version,
                             .operation_id = std::string{"edit-a"},
                             .intent = std::string{"change greeting"},
                             .changes = std::vector<tools::change>{{.kind = "write", .path = "src/message.txt", .text = std::string{"another greeting\n"}}}});
                    });
                    expect(changed_input == "identity_conflict") << "an operation ID keeps its first committed input: " << changed_input;

                    co_await tools::write::run(services, {.project = "app", .path = "src/other.txt", .content = "other\n", .operation_id = "other"}, note,
                                               tester);
                    auto onto = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::move::run(services, {.project = "app", .from = "src/other.txt", .to = "src/message.txt", .operation_id = "onto"}, note,
                                                  tester);
                    });
                    expect(onto == "source_conflict") << "a move never replaces a file: " << onto;
                    co_await tools::remove::run(services, {.project = "app", .path = "src/other.txt", .operation_id = "other-gone"}, note, tester);

                    // Another member meets the tester's lock and cannot force it; its refused attempt keeps the operation ID's input.
                    const worlds::actor rival{.issuer = "token", .user = "rival", .caller = "test-suite", .role = "member"};
                    std::string named;
                    try {
                        co_await tools::write::run(services, {.project = "app", .path = "src/rival.txt", .content = "first\n", .operation_id = "rival"}, note,
                                                   rival);
                    } catch (const tools::tool_error& e) {
                        named = e.code + ": " + e.what();
                    }
                    expect(named.starts_with("scope_locked") && named.contains("tester")) << "a held scope names its holder: " << named;
                    auto held = co_await line({.action = "locks"});
                    expect((held.locks && held.locks->size() == 1u) >> fatal);
                    auto forced = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::worldline::run(services, {.project = "app", .action = "unlock", .lock_id = held.locks->front().id, .force = true}, note,
                                                       rival);
                    });
                    expect(forced == "permission_denied") << "only an owner releases another writer's lock: " << forced;
                    co_await line({.action = "unlock", .scopes = std::vector<std::string>{"::"}});
                    co_await tools::worldline::run(services, {.project = "app", .action = "lock", .scopes = std::vector<std::string>{"::"}}, note, rival);
                    auto other_input = co_await refused([&]() -> coroutine::task<> {
                        co_await tools::write::run(services, {.project = "app", .path = "src/rival.txt", .content = "second\n", .operation_id = "rival"}, note,
                                                   rival);
                    });
                    expect(other_input == "identity_conflict") << other_input;
                    auto second = co_await tools::write::run(
                        services, {.project = "app", .path = "src/rival.txt", .content = "first\n", .operation_id = "rival"}, note, rival);
                    expect(!second.replayed) << "the same input runs again once the scope is free";
                    co_await tools::remove::run(services, {.project = "app", .path = "src/rival.txt", .operation_id = "rival-gone"}, note, rival);
                    co_await tools::worldline::run(services, {.project = "app", .action = "unlock", .scopes = std::vector<std::string>{"::"}}, note, rival);
                    co_await line({.action = "lock", .scopes = std::vector<std::string>{"::"}});

                    auto peer = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, sources.local_endpoint().port()});
                    transport::channel<native::socket> raw{peer};
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn(raw.pump([](const transport::frame&) {}));
                        co_await raw.post(jobs::hello{.name = "peer", .auth = jobs::source_token(), .protocol = jobs::protocol_version});
                        auto anonymous = co_await raw.call(jobs::node_call{.project = "app", .request = {.action = "lock", .scopes = {"::"}}});
                        const worlds::actor roleless{.issuer = "token", .user = "mallory", .caller = "stale-key"};
                        auto denied = co_await raw.call(jobs::node_call{.project = "app", .actor = roleless, .request = {.action = "lock", .scopes = {"::"}}});
                        auto* failure = anonymous ? nullptr : std::get_if<jobs::failure>(&anonymous.error());
                        expect(failure && failure->code == "permission_denied") << "no actor, no commit";
                        auto* refusal = denied ? nullptr : std::get_if<jobs::failure>(&denied.error());
                        expect(refusal && refusal->code == "permission_denied") << "no role, no commit";
                    });

                    auto stalled = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    storage::registry blocked{rt, tmp.dir / "cancel-state", config, gateway_disk,
                                              net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, stalled.local_endpoint().port()}};
                    unsigned requests = 0;
                    co_await coroutine::with_scope([&](coroutine::scope& pending) -> coroutine::task<> {
                        pending.spawn([](auto& listener, unsigned& requests) -> coroutine::task<> {
                            auto peer = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
                            transport::channel<native::socket> chan{peer};
                            co_await chan.recv();
                            co_await chan.recv();
                            ++requests;
                            co_await chan.recv();
                        }(stalled, requests));
                        auto begin = std::chrono::steady_clock::now();
                        auto result = co_await coroutine::deadline(io, 100ms, blocked.node(0, "app", {.action = "tree"}, tester, true));
                        expect(!result && requests == 1u);
                        expect(std::chrono::steady_clock::now() - begin < 2s);
                    });
                });
            });
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"tools">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
