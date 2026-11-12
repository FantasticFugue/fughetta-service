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
        auto run_operation = [&]() -> coroutine::task<> {
        auto sock = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, port});
        transport::channel<native::socket> chan{sock};
        transport::dispatcher<native::socket> dispatch;
        worker.install(dispatch, 0);
        co_await chan.post(jobs::hello{.name = "source-test-worker",
                                       .platform = {.os = "linux", .arch = "x86_64"},
                                       .max_inflight = 4,
                                       .protocol = jobs::protocol_version,
                                       .tools = host::detect_tools(test::toolset()), .toolchain_id = std::string(64, 'a')});
        auto run_handlers = [&](coroutine::scope& handlers) -> coroutine::task<> {
            static constexpr auto run_child = [](ctx& io, auto& channel) -> coroutine::task<> {
                for (;;) {
                    co_await execution::schedule_after(io, 2s);
                    co_await channel.post(jobs::heartbeat{});
                }
            };
            handlers.spawn(run_child(io, chan));
            for (;;)
                handlers.spawn(dispatch.handle(chan, co_await chan.recv()));
        };
        co_await coroutine::with_scope(std::move(run_handlers));
        };
        co_await worker.run(0, run_operation());
    }
    static constexpr auto cases = [] {
        "source range replies preserve Unicode positions and byte-limited output"_test = [] {
            nodes::file_view file{.path = "text", .kind = "text", .text = "a😀é\nZ", .encoding = "utf8"};
            std::string joined;
            std::uint64_t offset = 0;
            do {
                auto result = tools::worldline::render_read(file, offset, {}, 5);
                expect(result.total_scalars == 5u && result.text->size() <= 5u &&
                       result.data_base64 == tools::lines::base64(*result.text));
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
            for (unsigned i = 0; i < 300; ++i) file.text += "é";
            file.text += "Z";
            auto boundary = tools::worldline::render_read(file, 256, 3, 6);
            expect(boundary.text == "ééé" && boundary.count == 3u && boundary.total_scalars == 557u);
            auto tail = tools::worldline::render_read(file, 556, 10, 10);
            expect(tail.text == "Z" && tail.next_offset == 557u && tail.truncated == false);
            file.text.clear();
            expect(tools::worldline::render_read(file, 0, {}, 10).text == std::string{});
            file.kind = "binary"; file.text = "00ff"; file.encoding = "hex";
            auto binary = tools::worldline::render_read(file, {}, {}, 0);
            expect(binary.data_base64 == "AP8=" && binary.size == 2u && !binary.text);
            expect(throws<tools::tool_error>([&] { tools::worldline::render_read(file, 0, 1, 32); }));
        };
        "preparation errors retain a bounded diagnostic tail"_test = [] {
            jobs::build_report report;
            report.steps.push_back({.name = "configure", .exit_code = 2, .output = std::string(5000, 'x') + "missing compiler"});
            auto result = tools::prepared({}, {}, "v", report);
            expect(result.status == "failed" && result.error.contains("configure") && result.error.contains("exit code 2"));
            expect(result.error.contains("truncated=true") && result.error.ends_with("missing compiler") && result.error.size() < 4200u);
        };
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
            source_server::service source{rt, tmp.dir / "source", source_disk};
            routing::router router{rt};
            replica::service worker{rt, replica::options{.root = tmp.dir / "replica", .worker = "source-test-worker", .tools = test::toolset()}, worker_disk};
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto sources = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto workers = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                storage::registry registry{rt, tmp.dir / "state", config, gateway_disk,
                                           net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, sources.local_endpoint().port()}};
                tools::services services{config.slot(0), router, registry, 0};
                tools::backlog queue{rt, services};
                services.queue = &queue;
                mcp::notifier note;
                const worlds::actor tester{.issuer = "token", .user = "tester", .caller = "test-suite", .role = "owner"};
                auto run_children_2 = [&](coroutine::scope& children) -> coroutine::task<> {
                    static constexpr auto run_child_2 = [](auto& io, auto& listener, auto& source, auto& scope) -> coroutine::task<> {
                        for (;;) {
                            auto [connection, peer] = co_await net::async_accept(listener);
                            scope.spawn(source.serve(io, std::move(connection), 0));
                        }
                    };
                    children.spawn(run_child_2(io, sources, source, children));
                    children.spawn(worker_loop(io, workers.local_endpoint().port(), worker));
                    children.spawn(queue.serve(io));
                    auto [worker_connection, worker_peer] = co_await net::async_accept(workers);
                    children.spawn(router.workers(0).admit(io, std::move(worker_connection)));
                    while (!router.workers(0).holds({}))
                        co_await execution::schedule_after(io, 1ms);
                    auto line = [&](tools::worldline::input in) -> coroutine::task<tools::worldline::output> {
                        in.project = std::string{"app"};
                        if (!in.worldline.value()) in.worldline = std::string{"main"};
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
                    auto written = co_await fixture::write_file(services,
                                                                {.project = "app",
                                                                 .path = "src/message.txt",
                                                                 .content = "hello source"
                                                                            "\n",
                                                                 .operation_id = "write-2"},
                                                                note, tester);
                    auto read = co_await fixture::read_file(services, {.project = "app", .path = "src/message.txt"}, note, tester);
                    expect(read.content == "hello "
                                           "source\n" &&
                           read.version == written.version);
                    auto edit_input = fixture::edit_input{.project = "app",
                                                          .path = "src/message.txt",
                                                          .old_string = "hello",
                                                          .new_string = "hello again",
                                                          .operation_id = "text-edit"};
                    auto edited = co_await fixture::edit_text(services, edit_input, note, tester);
                    auto repeated = co_await fixture::edit_text(services, edit_input, note, tester);
                    expect(!edited.replayed && repeated.replayed && repeated.receipt == edited.receipt);
                    co_await fixture::write_file(services,
                                                 {.project = "app",
                                                  .path = "src/message.txt",
                                                  .content = "hello "
                                                             "source\n",
                                                  .operation_id = "write-3"},
                                                 note, tester);
                    auto first = co_await tools::build::run(services, {.project = "app", .worldline = std::string{"main"}}, note, tester);
                    expect(first.ok && first.results.at(0).steps.at(0).output == "hello source\n") << config::write(first);
                    expect(first.results.at(0).bytes_sent == 13u && first.worldline == "main");
                    auto cached = co_await tools::build::run(services, {.project = "app", .worldline = std::string{"main"}}, note, tester);
                    expect(cached.ok &&
                           cached.results.at(0).steps.at(0).output == "hello "
                                                                      "source\n" &&
                           cached.results.at(0).bytes_sent == 13u)
                        << "an equal build answers from the first one's result";
                    auto fresh = co_await tools::build::run(services, {.project = "app", .worldline = std::string{"main"}, .fresh = true}, note, tester);
                    expect(fresh.ok && fresh.results.at(0).bytes_sent == 0u) << "a fresh build runs again, on the worker that has the files";
                    auto pinned = co_await tools::build::run(services, {.project = "app", .worldline = std::string{"main"}, .version = first.revision}, note, tester);
                    co_await fixture::edit_text(
                        services, {.project = "app", .path = "src/message.txt", .old_string = "hello", .new_string = "goodbye", .operation_id = "edit-4"},
                        note, tester);
                    auto changed = co_await tools::build::run(services, {.project = "app", .worldline = std::string{"main"}}, note, tester);
                    expect(changed.ok && changed.results.at(0).steps.at(0).output == "goodbye "
                                                                                     "source\n");
                    expect(changed.results.at(0).bytes_sent == 15u);
                    auto older = co_await tools::build::run(services, {.project = "app", .worldline = std::string{"main"}, .version = pinned.revision}, note, tester);
                    expect(older.ok && older.results.at(0).steps.at(0).output == "hello source\n") << "a named version builds exactly that version";
                    auto hits = co_await fixture::search_files(services, {.project = "app", .query = "source"}, note, tester);
                    expect(hits.matches.size() == 1u && hits.matches.at(0).path == "src/message.txt");
                    auto listed = co_await fixture::list_files(services, {.project = "app"}, note, tester);
                    expect(listed.files.size() == 1u);
                    co_await fixture::move_file(services, {.project = "app", .from = "src/message.txt", .to = "src/moved.txt", .operation_id = "move-5"},
                                                note, tester);
                    auto moved = co_await fixture::read_file(services, {.project = "app", .path = "src/moved.txt"}, note, tester);
                    expect(moved.content == "goodbye source\n");
                    auto run_operation_24 = [&]() -> coroutine::task<> {
                        co_await fixture::write_file(services, {.project = "app", .path = "../escaped", .content = "x", .operation_id = "write-6"}, note,
                                                     tester);
                    };
                    auto escaped = co_await refused(std::move(run_operation_24));
                    expect(escaped == "invalid_record") << escaped;
                    auto run_operation_23 = [&]() -> coroutine::task<> {
                        co_await fixture::write_file(
                            services, {.project = "app", .path = "too-big", .content = std::string(16777217, 'x'), .operation_id = "write-7"}, note, tester);
                    };
                    auto oversized = co_await refused(std::move(run_operation_23));
                    expect(oversized == "resource_limit") << oversized;
                    co_await fixture::write_file(
                        services, {.project = "app", .path = "big.bin", .content = std::string(9u << 20, 'z'), .operation_id = "write-8"}, note, tester);
                    auto multipart = co_await tools::build::run(services, {.project = "app", .worldline = std::string{"main"}}, note, tester);
                    expect(!multipart.ok && multipart.results.at(0).bytes_sent == (9u << 20));
                    co_await fixture::remove_file(services, {.project = "app", .path = "big.bin", .operation_id = "remove-9"}, note, tester);
                    auto history = co_await registry.with_ledger(0, [](usage::ledger& ledger, const auto&) { return ledger.builds("app", 20); });
                    expect(history.size() == 4u && std::filesystem::exists(tmp.dir / "state/.source-usage/events.jsonl"))
                        << "builds equal to earlier ones ran once: " << history.size();
                    co_await fixture::move_file(services, {.project = "app", .from = "src/moved.txt", .to = "src/message.txt", .operation_id = "move-10"},
                                                note, tester);
                    auto wa = co_await line({.action = "fork", .name = std::string{"agent-a"}, .operation_id = std::string{"fork-a"}});
                    auto wb = co_await line({.action = "fork", .name = std::string{"agent-b"}, .operation_id = std::string{"fork-b"}});
                    expect(wa.version == wb.version);
                    auto other = tester;
                    other.user = "build-reader";
                    other.caller = "build-reader";
                    auto shared_build = co_await tools::build::run(services, {.project = "app", .worldline = "agent-a"}, note, other);
                    expect(shared_build.ok && shared_build.revision == wa.version && shared_build.worldline == "agent-a");
                    expect(shared_build.results.at(0).snapshot == wa.version);
                    auto shared_calls = co_await registry.with_ledger(0, [](usage::ledger& ledger, const auto&) { return ledger.events(0, 10000); });
                    expect(std::ranges::any_of(shared_calls.events, [&](const auto& event) {
                        return event.call && event.call->actor == worlds::identity_key(other) && event.call->worldline == "agent-a" &&
                               event.call->operation == "build" && event.call->reuse == "cache";
                    })) << "equal source manifests share builds across actors and worldlines, with independent call records";

                    auto listed_lines = co_await line({.action = "list"});
                    expect(listed_lines.worldlines->size() == 3u);
                    auto forked = co_await line({.action = "fork", .name = std::string{"agent-c"}, .operation_id = std::string{"fork-c"}});
                    co_await fixture::write_file(services,
                                                 {.project = "app",
                                                  .path = "src/later.txt",
                                                  .content = "main moved "
                                                             "on\n",
                                                  .operation_id = "write-11"},
                                                 note, tester);
                    auto reforked = co_await line({.action = "fork", .name = std::string{"agent-c"}, .operation_id = std::string{"fork-c"}});
                    expect(*reforked.replayed && reforked.version == forked.version) << "a fork retried after its source moved answers from its receipt";
                    auto run_operation_22 = [&]() -> coroutine::task<> {
                        co_await line({.action = "fork", .name = std::string{"agent-d"}, .operation_id = std::string{"fork-c"}});
                    };
                    auto renamed = co_await refused(std::move(run_operation_22));
                    expect(renamed == "identity_conflict") << renamed;
                    co_await fixture::remove_file(services, {.project = "app", .path = "src/later.txt", .operation_id = "remove-12"}, note,
                                                  tester); // Two identical requests
                    // in flight at once: one
                    // writes, the other
                    // answers from its
                    // receipt.
                    fixture::write_input twice{.project = "app", .path = "src/twice.txt", .content = "once\n", .operation_id = "twice"};
                    std::vector<tools::mutation> answers;
                    std::vector<std::string> failures;
                    auto run_both = [&](coroutine::scope& both) -> coroutine::task<> {
                        for (int i = 0; i < 2; ++i)
                            {
                            static constexpr auto run_child = [](auto& services, auto in, auto& note, auto& tester, auto& answers, auto& failures) -> coroutine::task<> {
                                try {
                                    answers.push_back(co_await fixture::write_file(services, std::move(in), note, tester));
                                } catch (const tools::tool_error& e) {
                                    failures.push_back(e.code + ": " + e.what());
                                } catch (const std::exception& e) {
                                    failures.push_back(e.what());
                                }
                            };
                            both.spawn(run_child(services, twice, note, tester, answers, failures));
                        }
                        co_await both.join();
                    };
                    co_await coroutine::with_scope(std::move(run_both));
                    expect(failures.empty()) << (failures.empty() ? std::string{} : failures.front());
                    expect((answers.size() == 2u) >> fatal);
                    expect(std::ranges::count(answers, true, &tools::mutation::replayed) == 1);
                    expect(answers.front().receipt == answers.back().receipt);
                    co_await fixture::remove_file(services, {.project = "app", .path = "src/twice.txt", .operation_id = "remove-13"}, note,
                                                  tester); // Small limits for
                    // project tiny; the
                    // workspace has one set
                    // of limits, so they are
                    // put back afterwards.
                    auto roomy = config.current(0);
                    auto with_limits = [&](config::limits limits) {
                        auto changed = *roomy;
                        changed.limits = limits;
                        config.set(0, std::make_shared<const config::document>(std::move(changed)));
                    };
                    with_limits({.max_file_bytes = 64, .max_project_bytes = 100});
                    auto tiny = [&](tools::worldline::input in) -> coroutine::task<tools::worldline::output> {
                        in.project = std::string{"tiny"};
                        if (!in.worldline.value()) in.worldline = std::string{"main"};
                        co_return co_await tools::worldline::run(services, std::move(in), note, tester);
                    };
                    co_await fixture::write_file(services, {.project = "tiny", .path = "full.txt", .content = std::string(64, 'x'), .operation_id = "write-14"},
                                                 note, tester);
                    auto full = co_await tiny({.action = "inspect"});
                    auto resize = [&](std::string id, std::uint64_t count, std::string text) {
                        return tiny({.action = "apply",
                                     .observed = full.version,
                                     .operation_id = std::move(id),
                                     .changes = std::vector<tools::change>{
                                         {.kind = "edit", .path = "full.txt", .offset = std::uint64_t{0}, .count = count, .text = std::move(text)}}});
                    };
                    auto run_operation_21 = [&]() -> coroutine::task<> { co_await resize("grow", 0, "x"); };
                    auto grown = co_await refused(std::move(run_operation_21));
                    expect(grown == "resource_limit") << "an edit cannot grow a file past the file limit: " << grown;
                    co_await resize("same-size", 1, "y"); // A package whose file
                    // is past the project's
                    // file limit is refused
                    // before anything is
                    // created.
                    auto tiny_packages = tmp.dir / "source/.packages";
                    auto big = std::string(100, 'b');
                    test::spit(tiny_packages / "big/blobs" / content::hash_of(big).view(), big);
                    test::spit(
                        tiny_packages / "big/manifest.json",
                        std::format(
                            R"({{"format":1,"project":"tiny","worldline":"main","version":"v","exported_at":0,"files":[{{"path":"big.txt","digest":"{}","kind":"text","size":100,"executable":false}}]}})",
                            content::hash_of(big)));
                    auto run_operation_20 = [&]() -> coroutine::task<> {
                        co_await tiny(
                            {.action = "restore", .name = std::string{"big"}, .operation_id = std::string{"restore-big"}, .package = std::string{"big"}});
                    };
                    auto too_big = co_await refused(std::move(run_operation_20));
                    expect(too_big == "resource_limit") << too_big;
                    auto tiny_lines = co_await tiny({.action = "list"});
                    expect(!std::ranges::contains(*tiny_lines.worldlines, std::string{"big"}, &tools::worldline::line_entry::name))
                        << "no worldline was created"; // An empty package
                    // restores as an empty
                    // worldline that answers
                    // to the caller's
                    // operation.
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
                    auto run_operation_19 = [&]() -> coroutine::task<> {
                        co_await tiny(
                            {.action = "restore", .name = std::string{"blank"}, .operation_id = std::string{"restore-empty"}, .package = std::string{"big"}});
                    };
                    auto other_package = co_await refused(std::move(run_operation_19));
                    expect(other_package == "identity_conflict") << "an operation ID answers only for its own restore: " << other_package;
                    co_await fixture::remove_file(services, {.project = "tiny", .path = "full.txt", .operation_id = "drop"}, note, tester);
                    co_await fixture::write_file(services, {.project = "tiny", .path = "other.txt", .content = std::string(64, 'o'), .operation_id = "other"},
                                                 note, tester);
                    auto past_tiny = co_await tiny({.action = "history"});
                    auto drop = std::ranges::find(*past_tiny.entries, std::string{"drop"}, &tools::worldline::history_entry::operation_id);
                    expect((drop != past_tiny.entries->end()) >> fatal);
                    auto now_tiny = co_await tiny({.action = "inspect"});
                    auto run_operation_18 = [&]() -> coroutine::task<> {
                        co_await tiny({.action = "reverse", .observed = now_tiny.version, .operation_id = std::string{"undrop"}, .target = drop->id});
                    };
                    auto restored = co_await refused(std::move(run_operation_18));
                    expect(restored == "resource_limit") << "a reversal cannot bring a project past its limit: " << restored;
                    std::filesystem::remove_all(tiny_packages / "big"); // A workspace whose
                    // repositories reach its
                    // storage limit takes no
                    // more history, but its
                    // locks still work.
                    with_limits({.max_workspace_bytes = 1});
                    auto run_operation_17 = [&]() -> coroutine::task<> {
                        co_await fixture::write_file(services, {.project = "full", .path = "a.txt", .content = "a", .operation_id = "full"}, note, tester);
                    };
                    auto stored_full = co_await refused(std::move(run_operation_17));
                    expect(stored_full == "resource_limit") << stored_full; // A write that reaches
                    // the limit commits; its
                    // replay still answers,
                    // and the next new write
                    // is refused.
                    with_limits({.max_workspace_bytes = jobs::stored(co_await registry.usage(0, tester)) + 1});
                    fixture::write_input filling{.project = "full", .path = "a.txt", .content = "a", .operation_id = "fills"};
                    auto filled = co_await fixture::write_file(services, filling, note, tester);
                    auto refilled = co_await fixture::write_file(services, filling, note, tester);
                    expect(!filled.replayed && refilled.replayed && refilled.receipt == filled.receipt)
                        << "a replay at the limit answers its receipt"; // Below what the
                    // workspace stores,
                    // whatever its
                    // repositories'
                    // write-ahead logs hold
                    // at the moment.
                    with_limits({.max_workspace_bytes = 1});
                    auto run_operation_16 = [&]() -> coroutine::task<> {
                        co_await fixture::write_file(services, {.project = "full", .path = "b.txt", .content = "b", .operation_id = "beyond"}, note, tester);
                    };
                    auto beyond = co_await refused(std::move(run_operation_16));
                    expect(beyond == "resource_limit") << beyond;
                    auto run_operation_15 = [&]() -> coroutine::task<> {
                        co_await tools::worldline::run(services, {.project = "full", .action = "export", .package = std::string{"copy"}, .worldline = std::string{"main"}}, note, tester);
                    };
                    auto exported_full = co_await refused(std::move(run_operation_15));
                    expect(exported_full == "resource_limit") << "an export counts toward the workspace's storage: " << exported_full;
                    if (std::filesystem::exists(tmp.dir / "source/.packages"))
                        for (auto& entry : std::filesystem::directory_iterator(tmp.dir / "source/.packages"))
                            expect(!entry.path().filename().string().starts_with(".staging-")) << "a refused export leaves nothing: " << entry.path();
                    config.set(0, roomy);
                    auto used = co_await registry.usage(0, tester);
                    auto tiny_used = std::ranges::find(used.projects, std::string{"tiny"}, &jobs::project_usage::project);
                    expect((tiny_used != used.projects.end()) >> fatal);
                    expect(tiny_used->source_bytes == 64u && tiny_used->storage_bytes > 64u); // A preparation a worker
                    // can take runs there,
                    // in a build tree beside
                    // the builds; the source
                    // keeps its receipt.
                    {
                        auto with_analysis = *roomy;
                        language::configuration analysis{.build_configuration = "Analysis"};
                        builds::configuration analysis_build;
                        builds::config<"command"> command;
                        command.operations["prepare"] = {.argv = {"python", "-c",
                                                                  "import json; from pathlib import Path; "
                                                                  "Path('commands.json').write_text(json.dumps([{'directory':str(Path.cwd()),'file':'main.c','"
                                                                  "arguments':['cc','-c','main.c']}]))"},
                                                         .outputs = {{"db", {"compilation_database", "commands.json"}}}};
                        analysis_build.builders["main"] = builds::with<"command">{.options = command};
                        analysis_build.tasks["db"] = {.builder = "main", .operation = "prepare"};
                        analysis_build.operations["prepare"] = {.tasks = {"db"}};
                        analysis_build.outputs["db"] = {.task = "db", .output = "db"};
                        with_analysis.projects.at("app").build_configurations["Analysis"] = analysis_build; // A server that
                        // exits at once: the
                        // survey fails, the
                        // preparation does
                        // not.
                        analysis.server = fixture::clice("python", {"-c", "raise SystemExit(1)"});
                        with_analysis.projects.at("app").analysis["Db"] = analysis;
                        config.set(0, std::make_shared<const config::document>(std::move(with_analysis)));
                        language::request ask{.build = analysis_build};
                        ask.config = analysis;
                        ask.worldline = "main";
                        auto first = co_await tools::prepare(services, "app", "Db", ask, tester);
                        expect(first.status == "prepared" && first.worker == "source-test-worker") << config::write(first);
                        expect(first.inputs.size() == 1u && first.artifacts.size() == 1u && first.tree.contains("replica"));
                        expect(first.analysis == "failed" && first.analysis_error.has_value()) << "the survey's failure is reported";
                        auto slow = analysis;
                        auto slow_build = analysis_build;
                        slow.build_configuration = "Slow";
                        fixture::options<"command">(slow_build.builders["main"]).operations["prepare"].argv = {"python", "-c", "import time; time.sleep(5)"};
                        auto with_slow = *config.current(0);
                        with_slow.projects.at("app").analysis["Slow"] = slow;
                        with_slow.projects.at("app").build_configurations["Slow"] = slow_build;
                        config.set(0, std::make_shared<const config::document>(std::move(with_slow)));
                        language::request cancelling{.build = slow_build};
                        cancelling.config = slow;
                        cancelling.worldline = "main";
                        auto cancelled = co_await coroutine::deadline(io, 1s, tools::prepare(services, "app", "Slow", cancelling, tester));
                        expect(!cancelled); // A worker
                        // preparation cannot
                        // rewrite tracked
                        // source files.
                        auto mutating = analysis;
                        auto mutating_build = analysis_build;
                        mutating.build_configuration = "Rewrite";
                        builds::config<"command"> rewrite;
                        rewrite.operations["prepare"] = {.argv = {"python", "-c",
                                                                  "import json; from pathlib import Path; "
                                                                  "Path('commands.json').write_text(json.dumps([{'directory':str(Path.cwd()),'file':'main.c','"
                                                                  "arguments':['cc','-c','main.c']}])); Path('src/message.txt').write_text('changed')"},
                                                         .outputs = {{"db", {"compilation_database", "commands.json"}}}};
                        mutating_build.builders["main"] = builds::with<"command">{.options = rewrite};
                        auto with_mutation = *config.current(0);
                        with_mutation.projects.at("app").analysis["Rewrite"] = mutating;
                        with_mutation.projects.at("app").build_configurations["Rewrite"] = mutating_build;
                        config.set(0, std::make_shared<const config::document>(std::move(with_mutation)));
                        language::request rewriting{.build = mutating_build};
                        rewriting.config = mutating;
                        rewriting.worldline = "main";
                        auto refused = co_await tools::prepare(services, "app", "Rewrite", rewriting, tester);
                        expect(refused.status == "worker_failed" && refused.error.contains("changed tracked file")) << config::write(refused);
                        co_await fixture::write_file(services,
                                                     {.project = "app",
                                                      .path = "analysis.c",
                                                      .content = "int probe;"
                                                                 "\n",
                                                      .operation_id = "analysis-source"},
                                                     note, tester);
                        auto query_config = analysis;
                        auto query_build = analysis_build;
                        query_config.build_configuration = "Query";
                        query_config.server = fixture::clice("python");
                        const std::string language_server = R"python(import json, sys

def send(value):
    body = json.dumps(value).encode()
    sys.stdout.buffer.write(b'Content-Length: %d\r\n\r\n' % len(body) + body)
    sys.stdout.buffer.flush()

while True:
    length = 0
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            sys.exit(0)
        if not line.strip():
            break
        if line.lower().startswith(b'content-length:'):
            length = int(line.split(b':')[1])
    message = json.loads(sys.stdin.buffer.read(length))
    method = message.get('method')
    if method == 'exit':
        break
    if 'id' not in message:
        continue
    result = None
    if method == 'initialize':
        result = {'capabilities': {'documentSymbolProvider': True, 'definitionProvider': True, 'referencesProvider': True, 'diagnosticProvider': {'interFileDependencies': False, 'workspaceDiagnostics': False}}}
    elif method == 'textDocument/documentSymbol':
        result = []
    elif method in ('textDocument/definition', 'textDocument/references'):
        result = [{'uri': message['params']['textDocument']['uri'], 'range': {'start': {'line': 0, 'character': 4}, 'end': {'line': 0, 'character': 9}}}]
    elif method == 'textDocument/diagnostic':
        result = {'kind': 'full', 'items': [{'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 3}}, 'severity': 1, 'message': 'fixture diagnostic'}]}
    send({'jsonrpc': '2.0', 'id': message['id'], 'result': result})
)python";
                        auto& argv = fixture::options<"command">(query_build.builders["main"]).operations["prepare"].argv;
                        argv.back() += "; import time; time.sleep(0.1); Path('serve').write_text(" + rfl::json::write(language_server) + ")";
                        fixture::options<"command">(query_build.builders["main"]).operations["build"] = {
                            .argv = {"python", "-c", "from pathlib import Path; assert Path('serve').is_file(); print('warm build tree')"}};
                        query_build.tasks["build"] = {.builder = "main", .operation = "build"};
                        query_build.operations["build"] = {.tasks = {"build"}};
                        auto with_query = *config.current(0);
                        with_query.projects.at("app").analysis["Query"] = query_config;
                        with_query.projects.at("app").build_configurations["Query"] = query_build;
                        config.set(0, std::make_shared<const config::document>(std::move(with_query)));
                        tools::language_query::output diagnostic, symbols;
                        auto query = [&](std::string action, tools::language_query::output& out) -> coroutine::task<> {
                            out =
                                co_await tools::language_query::run(services, {.project = "app", .worldline = std::string{"main"}, .configuration = "Query", .action = action}, note, tester);
                        };
                        auto run_calls = [&](coroutine::scope& calls) -> coroutine::task<> {
                            calls.spawn(query("diagnostics", diagnostic));
                            calls.spawn(query("symbols", symbols));
                            co_await calls.join();
                        };
                        co_await coroutine::with_scope(std::move(run_calls));
                        expect(diagnostic.analyzed && !diagnostic.stale);
                        expect(diagnostic.diagnostics && diagnostic.diagnostics->size() == 1u);
                        if (diagnostic.diagnostics && !diagnostic.diagnostics->empty())
                            expect(diagnostic.diagnostics->front().message == "fixture diagnostic");
                        expect(symbols.analyzed);
                        auto jobs = co_await queue.list(0);
                        expect(std::ranges::count(jobs, std::string{"Query"}, &tools::backlog::view::configuration) == 1);
                        auto cached = co_await tools::language_query::run(services, {.project = "app", .worldline = std::string{"main"}, .configuration = "Query"}, note, tester);
                        expect(cached.analyzed && cached.diagnostics && cached.diagnostics->size() == 1u);
                        auto branch = co_await line({.action="fork", .name=std::string{"cache-branch"}, .operation_id=std::string{"cache-fork"}});
                        auto reader = tester;
                        reader.user = "cache-reader";
                        reader.caller = "cache-reader";
                        auto shared = co_await tools::language_query::run(services,
                            {.project="app",.worldline="cache-branch",.configuration="Query"},note,reader);
                        expect(shared.analyzed && shared.version==branch.version);
                        auto records = co_await registry.with_ledger(0, [](usage::ledger& ledger, const auto&) { return ledger.events(0,10000); });
                        expect(std::ranges::any_of(records.events,[&](const auto& event) {
                            return event.call && event.call->actor==worlds::identity_key(reader) && event.call->worldline=="cache-branch" && event.call->reuse=="cache";
                        })) << "a cache hit keeps the new caller and worldline in its own record";

                        auto warm = co_await tools::build::run(services, {.project = "app", .configurations = std::vector<std::string>{"Query"}, .worldline = std::string{"main"}}, note, tester);
                        expect(warm.ok && warm.results.front().steps.front().output.contains("warm build tree"));
                        auto changed_environment = *config.current(0);
                        changed_environment.projects.at("app").build_configurations.at("Query").env["ANALYSIS_ENV"] = "changed";
                        config.set(0, std::make_shared<const config::document>(std::move(changed_environment)));
                        auto refreshed = co_await tools::language_query::run(services, {.project = "app", .worldline = std::string{"main"}, .configuration = "Query"}, note, tester);
                        expect(refreshed.analyzed && refreshed.environment != cached.environment);
                        auto activity = co_await registry.with_ledger(0, [](usage::ledger& ledger, const auto&) { return ledger.builds("app", 100); });
                        expect(std::ranges::count_if(activity,
                                                     [](const auto& event) {
                                                         return event.target == "Query" && event.operation == "prepare" && event.ok && !event.worker.empty();
                                                     }) == 2)
                            << "shared preparation is admitted and recorded once; changed inputs run again";
                        bool invalid_symbol = false;
                        try {
                            co_await tools::language_query::run(services,
                                {.project = "app", .worldline = std::string{"main"}, .configuration = "Query", .action = "definitions", .path = "analysis.c", .symbol = "absent"}, note, tester);
                        } catch (const tools::tool_error& error) { invalid_symbol = error.code == "invalid_record"; }
                        expect(invalid_symbol);
                        for (const auto& action : {"definitions", "references"}) {
                            auto locations = co_await tools::language_query::run(services,
                                {.project = "app", .worldline = std::string{"main"}, .configuration = "Query", .action = action, .path = "analysis.c", .symbol = "probe"}, note, tester);
                            const auto& found = std::string_view{action} == "definitions" ? locations.definitions : locations.references;
                            expect(locations.analyzed && found && found->size() == 1u);
                            if (found && !found->empty())
                                expect(found->front().path == "analysis.c" && found->front().line == 1u && found->front().column == 5u &&
                                       found->front().end_column == 10u);
                        }
                        auto allowed = config.current(0);
                        auto denied = *allowed;
                        denied.limits.builds_per_hour = 0;
                        denied.projects.at("app").build_configurations.at("Query").env["ANALYSIS_ENV"] = "quota probe";
                        config.set(0, std::make_shared<const config::document>(std::move(denied)));
                        bool limited = false;
                        try {
                            co_await tools::language_query::run(services, {.project = "app", .worldline = std::string{"main"}, .configuration = "Query"}, note, tester);
                        } catch (const tools::tool_error& error) {
                            limited = error.code == "resource_limit";
                        }
                        expect(limited) << "language preparation observes the build quota";
                        config.set(0, allowed);
                        for (auto action : {"plan", "prepare", "receipt"}) {
                            bool denied = false;
                            try {
                                co_await tools::language_query::run(services, {.project = "app", .worldline = std::string{"main"}, .configuration = "Query", .action = action}, note, tester);
                            } catch (const tools::tool_error& error) {
                                denied = error.code == "invalid_record";
                            }
                            expect(denied);
                        }
                        co_await fixture::remove_file(services, {.project = "app", .path = "analysis.c", .operation_id = "analysis-cleanup"}, note, tester);
                        config.set(0, roomy);
                    }
                    tools::standard::bound box{services};
                    auto malformed = co_await box.call(
                        "build", rfl::json::read<rfl::Generic>(R"({"project":"app","fresh":"not-a-boolean"})").value(),
                        note, tester);
                    expect(malformed.error && malformed.text.contains(R"("code":"invalid_record")")) << malformed.text;
                    auto run_operation_14 = [&]() -> coroutine::task<> {
                        co_await tools::generate_globally(services, "app",
                                                          templates::expand(templates::arguments{.template_id = "cpp-class", .name = "Anonymous"}), tester,
                                                          "");
                    };
                    auto anonymous = co_await refused(std::move(run_operation_14));
                    expect(anonymous == "invalid_record") << "a generation names its operation ID";
                    auto changed_a = co_await line({.action = "apply",
                                                    .worldline = std::string{"agent-a"},
                                                    .observed = wa.version,
                                                    .operation_id = std::string{"edit-a"},
                                                    .intent = std::string{"change greeting"},
                                                    .changes = std::vector<tools::change>{{.kind = "write",
                                                                                           .path = "src/message.txt",
                                                                                           .text = std::string{"worldline a"
                                                                                                               "\n"}}}});
                    auto again_a = co_await line({.action = "apply",
                                                  .worldline = std::string{"agent-a"},
                                                  .observed = wa.version,
                                                  .operation_id = std::string{"edit-a"},
                                                  .intent = std::string{"change greeting"},
                                                  .changes = std::vector<tools::change>{{.kind = "write",
                                                                                         .path = "src/message.txt",
                                                                                         .text = std::string{"worldline a"
                                                                                                             "\n"}}}});
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
                    expect(built_b.ok && built_b.results.at(0).steps.at(0).output == "goodbye "
                                                                                     "source\n");
                    auto warm_a = co_await tools::build::run(services, {.project = "app", .worldline = "agent-a", .fresh = true}, note, tester);
                    expect(warm_a.ok && warm_a.results.at(0).bytes_sent == 0u);
                    auto main_read = co_await fixture::read_file(services, {.project = "app", .path = "src/message.txt"}, note, tester);
                    expect(main_read.content == "goodbye source\n");
                    auto run_operation_13 = [&]() -> coroutine::task<> {
                        co_await tools::language_query::run(services, {.project = "app", .worldline = "agent-a", .action = "diagnostics"}, note, tester);
                    };
                    auto analysis = co_await refused(std::move(run_operation_13));
                    expect(analysis == "invalid_record") << "a project without an analysis configuration has nothing to query";
                    auto project_a =
                        co_await line({.action = "apply",
                                       .worldline = std::string{"agent-a"},
                                       .observed = changed_a.version,
                                       .operation_id = std::string{"create-c-project"},
                                       .changes = std::vector<tools::change>{{.kind = "create",
                                                                              .path = "CMakeLists.txt",
                                                                              .text = std::string{"cmake_minimum_required(VERSION 3.20)"
                                                                                                  "\npro"
                                                                                                  "ject(world C)"
                                                                                                  "\nadd"
                                                                                                  "_executable(hello main.c helper.c)"
                                                                                                  "\n"}},
                                                                             {.kind = "create",
                                                                              .path = "main.c",
                                                                              .text = std::string{"int helper(void); int main(void) { return helper(); }"
                                                                                                  "\n"}},
                                                                             {.kind = "create",
                                                                              .path = "helper.c",
                                                                              .text = std::string{"int helper(void) { return 0; }"
                                                                                                  "\n"}}}});
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
                                   .changes = std::vector<tools::change>{{.kind = "write",
                                                                          .path = "helper.c",
                                                                          .text = std::string{"int helper(void) { return 1; }"
                                                                                              "\n"}}}});
                    auto incremental = co_await tools::build::run(
                        services, {.project = "app", .configurations = std::vector<std::string>{"Dev"}, .worldline = "agent-a"}, note, tester);
                    expect(incremental.ok && incremental.snapshot != compiled.snapshot);
                    expect(incremental.results.at(0).bytes_sent == std::string_view{"int helper(void) { "
                                                                                    "return 1; }\n"}
                                                                       .size());
                    expect(std::filesystem::last_write_time(object) == stamp);
                    auto past = co_await line({.action = "history", .worldline = std::string{"agent-a"}});
                    expect(!past.entries->empty());
                    auto greeting = std::ranges::find(*past.entries, std::string{"edit-a"}, &tools::worldline::history_entry::operation_id);
                    expect((greeting != past.entries->end()) >> fatal);
                    auto described = co_await line({.action = "contribution", .worldline = std::string{"agent-a"}, .target = greeting->id});
                    expect((described.changes && described.changes->size() == 1u) >> fatal);
                    auto& greeted = described.changes->front();
                    expect(greeted.type == "modified" && greeted.before && greeted.after);
                    expect(greeted.before->text == std::optional<std::string>{"goodbye source"
                                                                              "\n"} &&
                           greeted.after->text == std::optional<std::string>{"worldline "
                                                                             "a\n"});
                    expect(described.entries->front().operation_id == "edit-a" && described.base_version == wa.version);
                    expect(described.contained_in && std::ranges::contains(*described.contained_in, std::string{"agent-a"}) &&
                           !std::ranges::contains(*described.contained_in, std::string{"agent-b"}))
                        << "a contribution names the worldlines that contain it";
                    auto run_operation_12 = [&]() -> coroutine::task<> {
                        co_await line({.action = "contribution", .worldline = std::string{"agent-a"}, .target = std::string(64, 'f')});
                    };
                    auto unknown = co_await refused(std::move(run_operation_12));
                    expect(unknown == "missing_objects") << unknown;
                    co_await fixture::write_file(services,
                                                 {.project = "app",
                                                  .path = "blob.dat",
                                                  .content = std::string("\0\1"
                                                                         "\2",
                                                                         3),
                                                  .operation_id = "binary"},
                                                 note, tester);
                    auto main_past = co_await line({.action = "history"});
                    auto binary = std::ranges::find(*main_past.entries, std::string{"binary"}, &tools::worldline::history_entry::operation_id);
                    expect((binary != main_past.entries->end()) >> fatal);
                    auto described_binary = co_await line({.action = "contribution", .target = binary->id});
                    expect((described_binary.changes && described_binary.changes->size() == 1u && described_binary.changes->front().after) >> fatal);
                    expect(described_binary.changes->front().after->kind == "binary" && !described_binary.changes->front().after->text)
                        << "a contribution never holds binary content";
                    co_await fixture::remove_file(services, {.project = "app", .path = "blob.dat", .operation_id = "binary-gone"}, note,
                                                  tester); // A version written out
                    // as a package comes
                    // back as a new
                    // worldline holding
                    // exactly its files.
                    auto exported = co_await line(
                        {.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"greeting"}});
                    expect(exported.version == changed_a.version && exported.files && !exported.files->empty());
                    auto run_operation_11 = [&]() -> coroutine::task<> {
                        co_await line({.action = "export", .worldline = std::string{"agent-a"}, .package = std::string{"greeting"}});
                    };
                    auto again = co_await refused(std::move(run_operation_11));
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
                    expect(restore_receipt.version == unpacked.version) << "a restore's receipt answers to the caller's operation ID"; // Replacing the package
                    // changes nothing a
                    // recorded restore
                    // answers.
                    auto replaced = co_await line({.action = "export", .worldline = std::string{"agent-b"}, .force = true, .package = std::string{"greeting"}});
                    expect(replaced.version != exported.version);
                    auto after_replace = co_await line(restoring);
                    expect(*after_replace.replayed && after_replace.version == unpacked.version && after_replace.configuration == unpacked.configuration);
                    auto blob = tmp.dir / "source/.packages/greeting/blobs" / replaced.files->front().digest;
                    test::spit(blob, "tampered");
                    auto run_operation_10 = [&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"damaged"},
                                       .operation_id = std::string{"restore-damaged"},
                                       .package = std::string{"greeting"}});
                    };
                    auto damaged = co_await refused(std::move(run_operation_10));
                    expect(damaged == "missing_objects") << "a damaged package restores nothing: " << damaged;
                    auto run_operation_9 = [&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"elsewhere"},
                                       .operation_id = std::string{"restore-greeting"},
                                       .package = std::string{"greeting"}});
                    };
                    auto elsewhere = co_await refused(std::move(run_operation_9));
                    expect(elsewhere == "identity_conflict")
                        << "an operation ID answers only for the worldline it restored: " << elsewhere; // A replacement a crash
                    // cut short between its
                    // renames: the new
                    // package was not whole,
                    // so the old one gets
                    // its name back.
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
                    expect(!std::filesystem::exists(acme_packages / ".staging-00000000000000c2"))
                        << "what a crashed export left is swept"; // A package whose name
                    // looks like scratch
                    // keeps its replacement
                    // journal, and the old
                    // package it names,
                    // through a sweep.
                    co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"staging-1"}});
                    std::filesystem::rename(acme_packages / "staging-1", acme_packages / ".retired-00000000000000c3");
                    test::spit(acme_packages / ".staging-1.replacing", R"({"retired":".retired-00000000000000c3","staging":".staging-00000000000000c3"})");
                    co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"bystander"}});
                    auto scratch_named = co_await line(
                        {.action = "restore", .name = std::string{"named"}, .operation_id = std::string{"restore-named"}, .package = std::string{"staging-1"}});
                    expect(scratch_named.files && scratch_named.files->size() == exported.files->size())
                        << "the sweep left the journal of a package named like scratch"; // Next to a journal that
                    // cannot be trusted
                    // nothing is swept, and
                    // the package it names
                    // is refused, not lost.
                    co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"corrupt"}});
                    std::filesystem::rename(acme_packages / "corrupt", acme_packages / ".retired-00000000000000c4");
                    test::spit(acme_packages / ".corrupt.replacing", "{not json");
                    std::filesystem::create_directories(acme_packages / ".staging-00000000000000c4");
                    auto run_operation_8 = [&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"corrupt"},
                                       .operation_id = std::string{"restore-corrupt"},
                                       .package = std::string{"corrupt"}});
                    };
                    auto untrusted = co_await refused(std::move(run_operation_8));
                    expect(untrusted == "storage_failure") << untrusted;
                    expect(std::filesystem::exists(acme_packages / ".retired-00000000000000c4") &&
                           std::filesystem::exists(acme_packages / ".staging-00000000000000c4") &&
                           std::filesystem::exists(acme_packages / ".corrupt.replacing"))
                        << "an unreadable journal keeps everything it might name";
                    test::spit(acme_packages / ".corrupt.replacing", R"({"retired":"../../escape","staging":".staging-00000000000000c4"})");
                    auto run_operation_7 = [&]() -> coroutine::task<> {
                        co_await line({.action = "restore",
                                       .name = std::string{"corrupt"},
                                       .operation_id = std::string{"restore-escape"},
                                       .package = std::string{"corrupt"}});
                    };
                    auto escaping = co_await refused(std::move(run_operation_7));
                    expect(escaping == "storage_failure") << "a journal naming anything but its scratch is not followed: " << escaping;
                    test::spit(acme_packages / ".corrupt.replacing", R"({"retired":".retired-00000000000000c4","staging":".staging-00000000000000c4"})");
                    auto mended = co_await line({.action = "restore",
                                                 .name = std::string{"corrupt"},
                                                 .operation_id = std::string{"restore-mended"},
                                                 .package = std::string{"corrupt"}});
                    expect(mended.files && mended.files->size() == exported.files->size())
                        << "once the journal is mended the old package returns"; // A staged package that
                    // does not load whole,
                    // or is a link, never
                    // takes the name from
                    // the old one.
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
                    } // A whole staged package
                    // takes the name even
                    // though the old one is
                    // whole too: it is the
                    // newer.
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
                    } // A name that holds no
                    // whole package while a
                    // replacement is pending
                    // keeps both copies and
                    // the journal.
                    {
                        co_await line({.action = "export", .worldline = std::string{"agent-a"}, .version = changed_a.version, .package = std::string{"held"}});
                        std::filesystem::rename(acme_packages / "held", acme_packages / ".retired-00000000000000c8");
                        std::filesystem::create_directory_symlink(acme_packages / "greeting", acme_packages / "held");
                        test::spit(acme_packages / ".held.replacing", R"({"retired":".retired-00000000000000c8","staging":".staging-00000000000000c8"})");
                        auto run_operation = [&]() -> coroutine::task<> {
                            co_await line({.action = "restore",
                                           .name = std::string{"held"},
                                           .operation_id = std::string{"restore-held"},
                                           .package = std::string{"held"}});
                        };
                        auto held = co_await refused(std::move(run_operation));
                        expect(held == "storage_failure") << held;
                        expect(std::filesystem::exists(acme_packages / ".retired-00000000000000c8/manifest.json") &&
                               std::filesystem::exists(acme_packages / ".held.replacing"));
                        std::filesystem::remove(acme_packages / "held");
                        std::filesystem::rename(acme_packages / ".retired-00000000000000c8", acme_packages / "held");
                        std::filesystem::remove(acme_packages / ".held.replacing");
                    } // Scratch-named
                    // leftovers that are not
                    // directories, and a
                    // half-written journal,
                    // are swept by their own
                    // names.
                    test::spit(acme_packages / ".staging-00000000000000c9", "x");
                    test::spit(acme_packages / ".gone.replacing.write-tmp", "{");
                    co_await line({.action = "restore",
                                   .name = std::string{"after-sweep"},
                                   .operation_id = std::string{"restore-after-sweep"},
                                   .package = std::string{"held"}});
                    expect(!std::filesystem::exists(acme_packages / ".staging-00000000000000c9") &&
                           !std::filesystem::exists(acme_packages / ".gone.replacing.write-tmp")); // When neither copy can
                    // take the name, both
                    // remains and the
                    // journal are kept.
                    std::filesystem::create_directories(acme_packages / ".staging-00000000000000ca/blobs");
                    test::spit(acme_packages / ".lost.replacing", R"({"retired":".retired-00000000000000ca","staging":".staging-00000000000000ca"})");
                    auto run_operation_6 = [&]() -> coroutine::task<> {
                        co_await line(
                            {.action = "restore", .name = std::string{"lost"}, .operation_id = std::string{"restore-lost"}, .package = std::string{"lost"}});
                    };
                    auto lost = co_await refused(std::move(run_operation_6));
                    expect(lost == "storage_failure") << lost;
                    expect(std::filesystem::exists(acme_packages / ".staging-00000000000000ca") && std::filesystem::exists(acme_packages / ".lost.replacing"));
                    std::filesystem::remove_all(acme_packages / ".staging-00000000000000ca");
                    std::filesystem::remove(acme_packages / ".lost.replacing"); // Journal names are
                    // exactly what scratch
                    // gives, so an embedded
                    // NUL or a journal's own
                    // name is refused.
                    for (auto bad : {R"({"retired":".retired-00000000000000c6\u0000x","staging":".staging-00000000000000c6"})",
                                     R"({"retired":".retired-00000000000000c6","staging":".staging-1.replacing"})"}) {
                        test::spit(acme_packages / ".odd.replacing", bad);
                        auto run_operation = [&]() -> coroutine::task<> {
                            co_await line(
                                {.action = "restore", .name = std::string{"odd"}, .operation_id = std::string{"restore-odd"}, .package = std::string{"odd"}});
                        };
                        auto odd = co_await refused(std::move(run_operation));
                        expect(odd == "storage_failure") << bad << ": " << odd;
                    }
                    std::filesystem::remove(acme_packages / ".odd.replacing"); // A journal that is not
                    // a plain file is not
                    // opened, so it cannot
                    // hold the workspace's
                    // packages.
                    if (host::os == "windows")
                        expect(std::filesystem::create_directory(acme_packages / ".fifo.replacing"));
                    else
                        expect(std::system(std::format("mkfifo '{}'", (acme_packages / ".fifo.replacing").string()).c_str()) == 0);
                    auto run_operation_5 = [&]() -> coroutine::task<> {
                        co_await line(
                            {.action = "restore", .name = std::string{"fifo"}, .operation_id = std::string{"restore-fifo"}, .package = std::string{"fifo"}});
                    };
                    auto piped = co_await refused(std::move(run_operation_5));
                    expect(piped == "storage_failure") << piped;
                    std::filesystem::remove(acme_packages / ".fifo.replacing"); // One digest declared
                    // with two sizes would
                    // let the limits
                    // undercount what is
                    // restored.
                    {
                        auto manifest_path = acme_packages / "greeting/manifest.json";
                        auto text = test::slurp(manifest_path);
                        auto doubled = rfl::json::read<package::manifest>(text).value();
                        auto copy = doubled.files.front();
                        copy.path = "src/copy.txt";
                        copy.size = 0;
                        doubled.files.push_back(copy);
                        test::spit(manifest_path, rfl::json::write(doubled));
                        auto run_operation = [&]() -> coroutine::task<> {
                            co_await line({.action = "restore",
                                           .name = std::string{"doubled"},
                                           .operation_id = std::string{"restore-doubled"},
                                           .package = std::string{"greeting"}});
                        };
                        auto sized = co_await refused(std::move(run_operation));
                        expect(sized == "invalid_record") << sized;
                        test::spit(manifest_path, text);
                    }
                    for (auto& entry : *past.entries)
                        expect(entry.actor == "token:tester") << "history names the signed-in user";
                    auto run_operation_4 = [&]() -> coroutine::task<> {
                        co_await line({.action = "apply",
                                       .worldline = std::string{"agent-b"},
                                       .observed = changed_a.version,
                                       .operation_id = std::string{"cross"},
                                       .changes = std::vector<tools::change>{{.kind = "remove", .path = "src/message.txt"}}});
                    };
                    auto stale = co_await refused(std::move(run_operation_4));
                    expect(stale == "worldline_boundary") << stale;
                    auto run_operation_3 = [&]() -> coroutine::task<> {
                        co_await line({.action = "apply",
                                       .worldline = std::string{"agent-a"},
                                       .observed = wa.version,
                                       .operation_id = std::string{"edit-a"},
                                       .intent = std::string{"change greeting"},
                                       .changes = std::vector<tools::change>{{.kind = "write",
                                                                              .path = "src/message.txt",
                                                                              .text = std::string{"another greeting"
                                                                                                  "\n"}}}});
                    };
                    auto changed_input = co_await refused(std::move(run_operation_3));
                    expect(changed_input == "identity_conflict") << "an operation ID keeps its first committed input: " << changed_input;
                    co_await fixture::write_file(services, {.project = "app", .path = "src/other.txt", .content = "other\n", .operation_id = "other"}, note,
                                                 tester);
                    auto run_operation_2 = [&]() -> coroutine::task<> {
                        co_await fixture::move_file(services, {.project = "app", .from = "src/other.txt", .to = "src/message.txt", .operation_id = "onto"},
                                                    note, tester);
                    };
                    auto onto = co_await refused(std::move(run_operation_2));
                    expect(onto == "source_conflict") << "a move never replaces a file: " << onto;
                    co_await fixture::remove_file(services, {.project = "app", .path = "src/other.txt", .operation_id = "other-gone"}, note,
                                                  tester); // Reusing an operation ID with another input remains invalid.
                    const worlds::actor rival{.issuer = "token", .user = "rival", .caller = "test-suite", .role = "member"};
                    auto member_write = co_await fixture::write_file(services,
                        {.project="app",.path="src/rival.txt",.content="first\n",.operation_id="rival"},note,rival);
                    expect(!member_write.replayed) << "members publish without scope locks";
                    auto run_operation = [&]() -> coroutine::task<> {
                        co_await fixture::write_file(services,
                                                     {.project = "app",
                                                      .path = "src/rival.txt",
                                                      .content = "second"
                                                                 "\n",
                                                      .operation_id = "rival"},
                                                     note, rival);
                    };
                    auto other_input = co_await refused(std::move(run_operation));
                    expect(other_input == "identity_conflict") << other_input;
                    auto second = co_await fixture::write_file(services,
                                                               {.project = "app",
                                                                .path = "src/rival.txt",
                                                                .content = "first"
                                                                           "\n",
                                                                .operation_id = "rival"},
                                                               note, rival);
                    expect(second.replayed) << "the same committed input returns its durable receipt";
                    co_await fixture::remove_file(services, {.project = "app", .path = "src/rival.txt", .operation_id = "rival-gone"}, note, rival);
                    auto peer = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, sources.local_endpoint().port()});
                    transport::channel<native::socket> raw{peer};
                    auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn(raw.pump([](const transport::frame&) {}));
                        co_await raw.post(jobs::hello{.name = "peer", .auth = jobs::source_token(), .protocol = jobs::protocol_version});
                        auto anonymous = co_await raw.call(jobs::node_call{.project = "app", .request = {.action = "tree"}});
                        const worlds::actor roleless{.issuer = "token", .user = "mallory", .caller = "stale-key"};
                        auto denied = co_await raw.call(jobs::node_call{.project = "app", .actor = roleless, .request = {.action = "tree"}});
                        auto* failure = anonymous ? nullptr : std::get_if<jobs::failure>(&anonymous.error());
                        expect(failure && failure->code == "permission_denied") << "no actor, no commit";
                        auto* refusal = denied ? nullptr : std::get_if<jobs::failure>(&denied.error());
                        expect(refusal && refusal->code == "permission_denied") << "no role, no commit";
                    };
                    co_await coroutine::with_scope(std::move(run_children));
                    auto stalled = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    storage::registry blocked{rt, tmp.dir / "cancel-state", config, gateway_disk,
                                              net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, stalled.local_endpoint().port()}};
                    unsigned requests = 0;
                    auto run_pending = [&](coroutine::scope& pending) -> coroutine::task<> {
                        static constexpr auto run_child = [](auto& listener, unsigned& requests) -> coroutine::task<> {
                            auto [peer, endpoint] = co_await net::async_accept(listener);
                            transport::channel<native::socket> chan{peer};
                            co_await chan.recv();
                            co_await chan.recv();
                            ++requests;
                            co_await chan.recv();
                        };
                        pending.spawn(run_child(stalled, requests));
                        auto begin = std::chrono::steady_clock::now();
                        auto result = co_await coroutine::deadline(io, 100ms, blocked.node(0, "app", {.action = "tree"}, tester, true));
                        expect(!result && requests == 1u);
                        expect(std::chrono::steady_clock::now() - begin < 2s);
                    };
                    co_await coroutine::with_scope(std::move(run_pending));
                };
                co_await coroutine::with_scope(std::move(run_children_2));
            };
            rt.run(std::move(run_core));
        };
    };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"tools">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
