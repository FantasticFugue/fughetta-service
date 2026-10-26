import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import mcp;
import reflection;
import http;
import configuration;
import usage;
import world;
import registry;
import fleet;
import tools;
import executor;
import host;
import support;
import fixture;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = io::scheduler_of_t<native::context>;
    using socket = native::socket;

    // The notifications an event stream holds and nobody has read, in order.
    auto notifications(web::event_stream& stream) -> std::vector<mcp::notification> {
        std::vector<mcp::notification> out;
        auto text = stream.take();
        constexpr std::string_view lead = "event: message\ndata: ";
        for (std::size_t at = 0; (at = text.find(lead, at)) != std::string::npos;) {
            at += lead.size();
            auto end = text.find("\n\n", at);
            out.push_back(rfl::json::read<mcp::notification>(text.substr(at, end - at)).value());
            at = end;
        }
        return out;
    }

    struct outcome {
        std::vector<tools::config_result> builds;
        std::string quota;
        std::string unknown_project;
        std::vector<usage::build_event> recorded;
        tools::results both, broken, tested, filtered, ran;
        double both_seconds = 0;
        tools::results command_tested, command_failed, command_ran;
        bool command_options_rejected = false;
        tools::config_result cached;
        bool slow_stopped = false;
        bool slow_finished_anyway = false;
        std::vector<usage::build_event> slow_recorded;
    };

    constexpr std::string_view cmake_lists = R"(cmake_minimum_required(VERSION 3.20)
project(hello C)
enable_testing()
add_executable(hello hello.c)
add_test(NAME greets COMMAND hello)
add_test(NAME fails COMMAND hello --fail)
)";

    constexpr std::string_view hello_c = R"(#include <stdio.h>
#include <string.h>
int main(int argc, char** argv) {
#ifndef PRIMARY_PROFILE
  puts("hello from alternate");
#else
  puts("hello from primary");
#endif
  return argc > 1 && strcmp(argv[1], "--fail") == 0;
}
)";

    // The one build tree a worker keeps for `project`, named for its worldline and configuration.
    auto tree_of(const std::filesystem::path& work, std::string_view project) -> std::filesystem::path {
        std::vector<std::filesystem::path> trees;
        for (auto& entry : std::filesystem::directory_iterator(work / "trees" / project))
            if (entry.is_directory())
                trees.push_back(entry.path());
        if (trees.size() != 1)
            throw std::runtime_error(std::format("{} build trees of {}", trees.size(), project));
        return trees.front();
    }

    struct harness {
        bool check_commands = false;
        test::scratch tmp{"build"};
        config::slots config{2, [] {
            config::document value;
            value.limits.max_file_bytes = 33554432;
            if (!test::toolset().contains("cc"))
                throw std::runtime_error("TEST_CC is required");
            value.limits.builds_per_hour = 4;
            auto& projects = value.projects;
            auto& cm = projects["cm"];
            for (auto name : {"Debug-primary", "Debug-alternate"}) {
                auto& build = cm.build_configurations[name];
                build = fixture::cmake(std::string{"cmake-build-"} + (std::string_view{name} == "Debug-primary" ? "debug-primary" : "debug-alternate"), "cc");
                fixture::launcher(build, std::string{"hello"} + TEST_EXECUTABLE_SUFFIX);
            }
            auto& primary = cm.build_configurations.at("Debug-primary");
            auto& options = fixture::options<"cmake">(primary.builders.at("main"));
            options.variables["CMAKE_C_FLAGS"] = "-DPRIMARY_PROFILE";
            auto python = [](std::string code) {
                if (!test::toolset().contains("python"))
                    throw std::runtime_error("TEST_PYTHON is required");
                return std::vector<std::string>{"python", "-c", std::move(code)};
            };
            projects["app"].env["GREETING"] = "hello from the config";
            projects["app"].build_configurations["script"] =
                fixture::commands(python("import os,sys; from pathlib import Path; sys.stdout.reconfigure(newline='\\n'); "
                                         "print(Path('src/msg.txt').read_text(), end=''); print(os.environ['GREETING']); "
                                         "print(Path('big.bin').stat().st_size if Path('big.bin').exists() else ''); "
                                         "print(' '.join(p.name for p in Path('out').glob('*'))); "
                                         "Path('out').mkdir(exist_ok=True); Path('out/stamp').write_text('built\\n')"));
            projects["app"].build_configurations.at("script").limits.timeout_s = 30;
            projects["app"].default_configurations = {"script"};
            auto& commands = projects["commands"];
            commands.build_configurations["make"] = fixture::commands(
                python("from pathlib import Path; Path('built').write_text('built\\n')"), python("from pathlib import Path; Path('prepared').touch()"),
                python("from pathlib import Path; assert Path('prepared').exists() and Path('built').exists(); print('tests passed')"), "pkg",
                python("import sys; from pathlib import Path; sys.stdout.reconfigure(newline='\\n'); print(Path('built').read_text(), end='')"));
            commands.build_configurations["bad"] =
                fixture::commands(python("from pathlib import Path; Path('built').touch()"), {}, python("raise SystemExit(1)"), "pkg");
            commands.default_configurations = {"make"};
            commands.run_configurations["app"] = {.configurations = {"make"}};
            projects["slow"].build_configurations["wait"] =
                fixture::commands(python("import time; from pathlib import Path; time.sleep(3); Path('out').mkdir(exist_ok=True); Path('out/late').touch()"));
            projects["slow"].default_configurations = {"wait"};
            cm.default_configurations = {"Debug-primary", "Debug-alternate"};
            cm.run_configurations["hello"] = {};
            config::validate(value);
            return std::make_shared<const config::document>(std::move(value));
        }()};
        cores::io_context<native::context> rt{2};
        routing::router router{rt};
        execution::thread_pool pool{2};
        test::source_service source{tmp.dir / "source"};
        storage::registry files{rt, tmp.dir / "storage", config, pool, source.endpoint()};
        replica::service worker{rt, replica::options{.root = tmp.dir / "work", .worker = "w1", .tools = test::toolset()}, pool, 1};
        std::uint16_t port = 0;
        test::event listening;
        web::event_stream progress;
        outcome out;

        void run() {
            rt.run([this](ctx& io, std::size_t core) { return core == 0 ? gate(io) : work(io); });
        }

        auto work(ctx& io) -> coroutine::task<> {
            co_await listening.wait();
            auto sock = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, port});
            transport::channel<socket> chan{sock};
            transport::dispatcher<socket> serve;
            worker.install(serve, 1);
            co_await chan.post(jobs::hello{.name = "w1",
                                           .platform = {.os = "linux", .arch = "x86_64"},
                                           .max_inflight = 4,
                                           .protocol = jobs::protocol_version,
                                           .tools = host::detect_tools(test::toolset())});
            try {
                co_await coroutine::with_scope([&](coroutine::scope& requests) -> coroutine::task<> {
                    requests.spawn([](ctx& io, auto& channel) -> coroutine::task<> {
                        for (;;) {
                            co_await execution::schedule_after(io, 2s);
                            co_await channel.post(jobs::heartbeat{});
                        }
                    }(io, chan));
                    for (;;)
                        requests.spawn(serve.handle(chan, co_await chan.recv()));
                });
            } catch (const std::exception&) {
            }
        }

        auto gate(ctx& io) -> coroutine::task<> {
            auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
            port = l.local_endpoint().port();
            rt.post(1, [this] { listening.fire(); });
            auto peer = co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); });
            tools::services s{config.slot(0), router, files, 0};
            tools::backlog queue{rt, s};
            s.queue = &queue;
            mcp::notifier note{progress};
            const worlds::actor tester{.issuer = "token", .user = "tester", .caller = "test-suite", .role = "owner"};
            auto build = [&](bool fresh = false) -> coroutine::task<tools::config_result> {
                auto out = co_await tools::build::run(s, {.project = "app", .configurations = std::nullopt, .targets = std::nullopt, .fresh = fresh}, note,
                                                      tester);
                co_return out.results.at(0);
            };

            co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                children.spawn(queue.serve(io));
                children.spawn(router.workers(0).admit(io, std::move(peer)));
                while (!router.workers(0).holds({}))
                    co_await execution::schedule_after(io, 1ms);

                for (auto project : {"app", "cm", "commands", "slow"})
                    co_await tools::worldline::run(s, {.project = project, .action = "lock", .scopes = std::vector<std::string>{"::"}},
                                                   note, tester);
                co_await tools::write::run(s, {.project = "app", .path = "src/msg.txt", .content = "hello v1\n", .operation_id = "write-15"}, note, tester);
                out.builds.push_back(co_await build());
                co_await tools::edit::run(
                    s, {.project = "app", .path = "src/msg.txt", .old_string = "v1", .new_string = "v2", .replace_all = std::nullopt, .operation_id = "edit-16"},
                    note, tester);
                co_await tools::write::run(s, {.project = "app", .path = "big.bin", .content = std::string(9u << 20, 'z'), .operation_id = "write-17"}, note, tester);
                out.builds.push_back(co_await build());
                out.cached = co_await build();
                out.builds.push_back(co_await build(true));
                out.builds.push_back(co_await build(true));
                out.quota = (co_await build(true)).error;
                try {
                    co_await tools::build::run(s, {.project = "nope", .configurations = std::nullopt, .targets = std::nullopt}, note,
                                               tester);
                } catch (const config::not_found& e) {
                    out.unknown_project = e.what();
                }
                out.recorded = co_await files.with_ledger(0, [](usage::ledger& ledger, const config::document&) { return ledger.builds("app", 100); });
                auto unlimited = *config.current(0);
                unlimited.limits.builds_per_hour.reset();
                auto snapshot = std::make_shared<const config::document>(std::move(unlimited));
                for (std::size_t c = 0; c < rt.size(); ++c)
                    config.set(c, snapshot);

                co_await tools::write::run(s, {.project = "cm", .path = "CMakeLists.txt", .content = std::string{cmake_lists}, .operation_id = "write-18"}, note,
                                           tester);
                co_await tools::write::run(s, {.project = "cm", .path = "hello.c", .content = std::string{hello_c}, .operation_id = "write-19"}, note, tester);
                auto started = std::chrono::steady_clock::now();
                out.both = co_await tools::build::run(s, {.project = "cm", .configurations = std::nullopt, .targets = std::nullopt}, note,
                                                      tester);
                out.both_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                co_await tools::edit::run(s,
                                          {
                                           .project = "cm",
                                           .path = "hello.c",
                                           .old_string = R"(puts("hello from primary");)",
                                           .new_string = R"(puts("hello from primary"))",
                                           .replace_all = std::nullopt, .operation_id = "edit-20"},
                                          note, tester);
                out.broken = co_await tools::build::run(s, {.project = "cm", .configurations = std::nullopt, .targets = std::nullopt},
                                                        note, tester);
                co_await tools::edit::run(s,
                                          {
                                           .project = "cm",
                                           .path = "hello.c",
                                           .old_string = R"(puts("hello from primary"))",
                                           .new_string = R"(puts("hello from primary");)",
                                           .replace_all = std::nullopt, .operation_id = "edit-21"},
                                          note, tester);
                out.tested = co_await tools::test::run(
                    s, {.project = "cm", .configurations = std::nullopt, .filter = std::nullopt, .labels = std::nullopt}, note, tester);
                out.filtered = co_await tools::test::run(s,
                                                         {
                                                          .project = "cm",
                                                          .configurations = std::vector<std::string>{"Debug-alternate"},
                                                          .filter = std::string{"greets"},
                                                          .labels = std::nullopt},
                                                         note, tester);
                out.ran = co_await tools::launch::run(s, {.project = "cm", .configuration = "hello", .configurations = std::nullopt}, note,
                                                      tester);

                if (check_commands) {
                    co_await tools::write::run(
                        s,
                        {
                         .project = "commands",
                         .path = "pkg/Makefile",
                         .content = "all:\n\t@echo built > built\ntest:\n\t@test -f prepared && test -f built\n\t@echo tests passed\nfail:\n\t@exit 1\n", .operation_id = "write-22"},
                        note, tester);
                    out.command_tested = co_await tools::test::run(
                        s, {.project = "commands", .configurations = std::nullopt, .filter = std::nullopt, .labels = std::nullopt}, note,
                        tester);
                    out.command_failed = co_await tools::test::run(s,
                                                                   {
                                                                    .project = "commands",
                                                                    .configurations = std::vector<std::string>{"bad"},
                                                                    .filter = std::nullopt,
                                                                    .labels = std::nullopt},
                                                                   note, tester);
                    out.command_ran = co_await tools::launch::run(
                        s, {.project = "commands", .configuration = "app", .configurations = std::nullopt}, note, tester);
                    try {
                        co_await tools::test::run(
                            s,
                            {.project = "commands", .configurations = std::nullopt, .filter = std::string{"unit"}, .labels = std::nullopt},
                            note, tester);
                    } catch (const std::invalid_argument&) {
                        out.command_options_rejected = true;
                    }
                }

                co_await tools::write::run(s, {.project = "slow", .path = "x", .content = "x", .operation_id = "write-23"}, note, tester);
                auto stopped = co_await coroutine::deadline(
                    io, 800ms,
                    tools::build::run(s, {.project = "slow", .configurations = std::nullopt, .targets = std::nullopt}, note, tester));
                out.slow_stopped = !stopped;
                co_await execution::schedule_after(io, std::chrono::milliseconds{3500});
                out.slow_finished_anyway = std::filesystem::exists(tree_of(tmp.dir / "work", "slow") / "out/late");
                out.slow_recorded = co_await files.with_ledger(0, [](usage::ledger& ledger, const config::document&) { return ledger.builds("slow", 10); });
            });
        }
    };

    auto scenario() -> harness& {
        static auto instance = [] {
            auto value = std::make_unique<harness>();
            value->check_commands = true;
            value->run();
            return value;
        }();
        return *instance;
    }

    void native_graph() {
        harness h;
        auto document = *h.config.current(0);
        document.projects.clear();
        document.projects["app"] = config::parse_as<config::settings>(
            R"fixture({"build_configurations":{"worker":{"builders":{"cmake":{"adapter":"cmake","requires":{"tools":["bazel"]},"options":{"build_dir":"build/worker-cmake","c_compiler":"cc","export_compile_commands":true,"targets":["app","native_bazel"],"inputs":{"header":"cxx_header"},"variables":{"GENERATED_HEADER":"${input:header}","BAZEL":"${tool:bazel}","BAZEL_BASE":"${tree}/build/worker-bazel","NATIVE_PROOF":"${tree}/build/worker-cmake/native-proof"},"outputs":{"binary":{"type":"executable","path":"build/worker-cmake/app"},"native_proof":{"type":"marker","path":"build/worker-cmake/native-proof"}}}},"bazel":{"adapter":"bazel","requires":{"tools":["python3","cmake"]},"options":{"executable":"bazel","build_dir":"build/worker-bazel","startup_options":["--host_jvm_args=-Xmx512m"],"operations":{"generate":{"command":"run","targets":["//:bridge"],"options":["--jobs=2","--host_platform=//:local","--platforms=//:local"],"args":["generate","${tree}/build/worker-generated/generated.h"],"outputs":{"header":{"type":"cxx_header","path":"build/worker-generated/generated.h"}}},"package":{"command":"run","targets":["//:bridge"],"options":["--jobs=2","--host_platform=//:local","--platforms=//:local"],"inputs":{"binary":"executable"},"args":["package","${input:binary}","${tree}/build/worker-package.bin"],"outputs":{"package":{"type":"archive","path":"build/worker-package.bin"}}}}}}},"tasks":{"generate":{"builder":"bazel","operation":"generate"},"configure":{"builder":"cmake","operation":"configure","inputs":{"header":{"task":"generate","output":"header"}}},"compile":{"builder":"cmake","operation":"build","after":["configure"]},"package":{"builder":"bazel","operation":"package","inputs":{"binary":{"task":"compile","output":"binary"}}}},"operations":{"build":{"tasks":["package"]},"prepare":{"tasks":["configure"]}},"outputs":{"database":{"task":"configure","output":"compilation_database"},"package":{"task":"package","output":"package"}},"env":{"BUILD_OWNER":"worker"},"limits":{"timeout_s":180}}},"default_configurations":["worker"]})fixture", "native build configuration");
        config::validate(document);
        auto snapshot = std::make_shared<const config::document>(std::move(document));
        for (std::size_t c = 0; c < h.rt.size(); ++c)
            h.config.set(c, snapshot);
        const auto sources = config::parse_as<std::map<std::string, std::string>>(
            R"fixture({"MODULE.bazel":"module(name = \"fixture\")\n","BUILD.bazel":"load(\":bridge.bzl\", \"bridge\")\nplatform(name = \"local\")\nbridge(name = \"bridge\")\n","bridge.bzl":"def _bridge_impl(ctx):\n    output = ctx.actions.declare_file(ctx.label.name + \".py\")\n    ctx.actions.write(output, \"\"\"#!/usr/bin/env python3\nimport pathlib, subprocess, sys\nmode, *args = sys.argv[1:]\nif mode == 'generate':\n    path = pathlib.Path(args[0])\n    path.parent.mkdir(parents=True, exist_ok=True)\n    path.write_text('#define GREETING \"from bazel\"\\\\n')\nelif mode == 'package':\n    subprocess.run(['cmake', '-E', 'copy', args[0], args[1]], check=True)\n    subprocess.run([args[0]], check=True)\nelif mode == 'native-cmake':\n    subprocess.run(['cmake', '-E', 'touch', args[0]], check=True)\nelse:\n    raise SystemExit('unexpected bridge operation')\n\"\"\", is_executable=True)\n    return [DefaultInfo(executable=output)]\nbridge = rule(implementation=_bridge_impl, executable=True)\n","CMakeLists.txt":"cmake_minimum_required(VERSION 3.20)\nproject(mixed C)\nget_filename_component(HEADER_DIR \"${GENERATED_HEADER}\" DIRECTORY)\nadd_executable(app main.c \"${GENERATED_HEADER}\")\ntarget_include_directories(app PRIVATE \"${HEADER_DIR}\")\nadd_custom_target(native_bazel\n  COMMAND \"${BAZEL}\" --batch \"--output_base=${BAZEL_BASE}\" \"--output_user_root=${BAZEL_BASE}/user\"\n    --host_jvm_args=-Xmx512m run --repo_contents_cache= --jobs=2 --host_platform=//:local --platforms=//:local //:bridge -- native-cmake \"${NATIVE_PROOF}\"\n  WORKING_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}\" VERBATIM)\n","main.c":"#include <stdio.h>\n#include \"generated.h\"\nint main(void) { puts(GREETING); return 0; }\n"})fixture", "native build sources");
        auto gate = [&](ctx& io) -> coroutine::task<> {
            auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
            h.port = listener.local_endpoint().port();
            h.rt.post(1, [&h] { h.listening.fire(); });
            auto peer = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
            tools::services services{h.config.slot(0), h.router, h.files, 0};
            tools::backlog queue{h.rt, services};
            services.queue = &queue;
            mcp::notifier note;
            const worlds::actor actor{.issuer = "token", .user = "tester", .caller = "native-graph", .role = "owner"};
            co_await coroutine::with_scope([&](coroutine::scope& tasks) -> coroutine::task<> {
                tasks.spawn(queue.serve(io));
                tasks.spawn(h.router.workers(0).admit(io, std::move(peer)));
                while (!h.router.workers(0).holds({}))
                    co_await execution::schedule_after(io, 1ms);
                co_await tools::worldline::run(services, {.project = "app", .action = "lock", .scopes = std::vector<std::string>{"::"}}, note, actor);
                for (const auto& [path, text] : sources)
                    co_await tools::write::run(services, {.project = "app", .path = path, .content = text, .operation_id = "write " + path}, note, actor);
                auto result = co_await tools::build::run(services, {.project = "app"}, note, actor);
                expect(result.ok >> fatal) << config::write(result);
                std::vector<std::string> names;
                for (const auto& step : result.results.at(0).steps)
                    names.push_back(step.name);
                expect(names == std::vector<std::string>{"generate", "configure", "compile", "package"});
                expect(result.results.at(0).worker == "w1" && result.results.at(0).steps.back().output.contains("from bazel"));
                auto tree = tree_of(h.tmp.dir / "work", "app");
                expect(std::filesystem::exists(tree / "build/worker-cmake/native-proof"));
                expect(test::slurp(tree / "build/worker-package.bin") == test::slurp(tree / "build/worker-cmake/app"));
                expect(!std::filesystem::exists(h.tmp.dir / "storage/app"));
            });
        };
        h.rt.run([&](ctx& io, std::size_t core) { return core == 0 ? gate(io) : h.work(io); });
    }

    static constexpr auto build_suite = [] {
        "a Worker cannot substitute the offered source snapshot"_test = [] {
            harness h;
            bool rejected=false;
            h.rt.run([&](ctx& io,std::size_t core) -> coroutine::task<> {
                if (core!=0) co_return;
                auto listener=native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto client=co_await io::connect(io,{net::ip::address_v4{{127,0,0,1}},listener.local_endpoint().port()});
                auto server=co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
                transport::channel<socket> incoming{server},outgoing{client};
                co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(outgoing.pump([](const transport::frame&){}));
                    children.spawn([](transport::channel<socket>& incoming) -> coroutine::task<> {
                        auto offer=co_await incoming.recv();
                        expect(offer.kind==jobs::code<jobs::sync_offer>);
                        co_await incoming.reply(offer.corr,jobs::sync_plan{.snapshot=content::hash_of("substituted")});
                    }(incoming));
                    auto run=[&]() -> coroutine::task<> {
                        try {
                            co_await tools::sync_and_run(io,h.files,0,outgoing,"app",{},jobs::build_request{},nullptr,"main","selected-version",{},
                                std::make_shared<tools::execution_identity>());
                        } catch (const tools::tool_error& error) {rejected=error.code=="invalid_worker_reply";}
                    };
                    auto finished=co_await coroutine::deadline(io,200ms,run());
                    expect(finished.has_value());
                    expect(rejected);
                });
            });
        };

        if (test::toolset().contains("bazel"))
            "native Bazel and CMake graph runs through source and worker RPCs"_test = native_graph;
        else
            skip / "native Bazel and CMake graph requires TEST_BAZEL"_test = [] {};

        "command workflows reject unsupported options before dispatch"_test = [] {
            config::effective_config c{.name = "npm", .build = fixture::commands({"npm", "run", "build"})};
            expect(throws<std::invalid_argument>([&] { tools::detail::operation(c, {.operation = "test"}); }));
            expect(throws<std::invalid_argument>([&] { tools::detail::operation(c, {.operation = "build", .parameters = {.targets = {"all"}}}); }));
            expect(nothrow([&] { tools::detail::operation(c, {.operation = "build"}); }));
            c.build.enabled = false;
            expect(throws<std::invalid_argument>([&] { tools::detail::operation(c, {.operation = "build"}); }));
        };

        "an edit is synced to a worker and built there"_test = [] {
            auto& h = scenario();
            auto& b = h.out.builds;
            expect((b.size() == 4u) >> fatal);
            auto output = [](const tools::config_result& r) { return r.steps.empty() ? std::string{} : r.steps.back().output; };

            expect(b[0].ok) << output(b[0]) << b[0].error;
            expect(b[0].worker == "w1" && b[0].configuration == "script");
            expect(output(b[0]).starts_with("hello v1\nhello from the config\n")) << output(b[0]);
            expect(!output(b[0]).contains("stamp"));
            expect(b[0].blobs_sent == 1u);

            expect(b[1].ok) << output(b[1]);
            expect(output(b[1]).starts_with("hello v2\n")) << output(b[1]).substr(0, 300) << b[1].error;
            expect(output(b[1]).contains(std::format("{}\n", 9u << 20))) << "the big file arrived whole";
            expect(output(b[1]).contains("stamp"));
            expect(b[1].blobs_sent == 2u);
            expect(b[1].bytes_sent == (9u << 20) + 9u);

            expect(h.out.cached.ok && h.out.cached.bytes_sent == b[1].bytes_sent && output(h.out.cached) == output(b[1]))
                << "an equal build answers from the last one's result";
            expect(b[2].blobs_sent == 0u) << "a fresh build runs again, on the worker that has the files";
            expect(h.out.quota.contains("4 builds")) << h.out.quota;
            expect(h.out.unknown_project == "unknown project 'nope'");
            std::vector<std::string> stages;
            for (auto& n : notifications(h.progress))
                stages.push_back(n.params.to_object().value().get("message").value().to_string().value());
            expect(std::ranges::contains(stages, std::string{"script: build"})) << "progress, named by configuration";
            expect(std::ranges::contains(stages, std::string{"script: sync"}));

            auto& rec = h.out.recorded;
            expect((rec.size() == 4u) >> fatal) << "every build that was admitted is in the ledger";
            expect(rec[0].ok && rec[0].worker == "w1" && rec[0].target == "script");
            expect(rec[0].worldline=="main" && !rec[0].version.empty() && !rec[0].snapshot.empty());
            expect(rec[0].actor==worlds::identity_key({.issuer="token",.user="tester",.caller="test-suite"}));
            expect(rec[0].operation=="build");
            expect(rec[2].bytes_sent == (9u << 20) + 9u);
        };

        "a cancelled build is stopped on the worker and recorded"_test = [] {
            auto& h = scenario();
            expect(h.out.slow_stopped);
            expect(!h.out.slow_finished_anyway) << "the worker killed its process";
            expect((h.out.slow_recorded.size() == 1u) >> fatal);
            auto& e = h.out.slow_recorded[0];
            expect(e.error_code=="cancelled" && e.worldline=="main" && !e.version.empty() && e.operation=="build");
            expect(e.error == "cancelled" && e.target == "wait" && e.seconds > 0.5 && e.seconds < 3.0) << e.error << " " << e.seconds;
        };

        "a CMake project builds, tests and runs on two configurations in one call each"_test = [] {
            auto& h = scenario();
            auto names = [](const tools::config_result& r) {
                std::vector<std::string> out;
                for (auto& st : r.steps)
                    out.push_back(st.name);
                return out;
            };

            expect(h.out.command_tested.ok >> fatal) << config::write(h.out.command_tested);
            auto& command = h.out.command_tested.results.at(0);
            expect((names(command) == std::vector<std::string>{"prepare", "build", "test"}) >> fatal);
            expect(command.steps.back().output.contains("tests passed") && !command.tests);
            expect(!h.out.command_failed.results.empty() >> fatal) << config::write(h.out.command_failed);
            expect(!h.out.command_failed.results.at(0).steps.empty() >> fatal) << config::write(h.out.command_failed);
            expect(!h.out.command_failed.ok && h.out.command_failed.results.at(0).steps.back().name == "test");
            expect(h.out.command_ran.ok && h.out.command_ran.results.at(0).steps.back().output == "built\n") << config::write(h.out.command_ran);
            expect(h.out.command_options_rejected);

            auto& both = h.out.both;
            expect((both.results.size() == 2u) >> fatal);
            expect(both.ok) << config::write(both);
            expect(both.results[0].configuration == "Debug-primary" && both.results[1].configuration == "Debug-alternate");
            expect(!both.snapshot.empty() && both.results[0].snapshot == both.snapshot && both.results[1].snapshot == both.snapshot)
                << "one call, one snapshot";
            expect(names(both.results[0]) == std::vector<std::string>{"configure", "build"});
            auto built = [&](std::string_view directory) {
                for (auto& tree : std::filesystem::directory_iterator(h.tmp.dir / "work/trees/cm"))
                    if (std::filesystem::exists(tree.path() / directory / (std::string{"hello"} + TEST_EXECUTABLE_SUFFIX)))
                        return true;
                return false;
            };
            expect(built("cmake-build-debug-primary") && built("cmake-build-debug-alternate")) << "a tree and build directory per profile";

            auto& broken = h.out.broken;
            expect((broken.results.size() == 2u) >> fatal);
            expect(!broken.ok && broken.failed());
            auto& primary = broken.results[0];
            expect(!primary.ok && primary.steps.back().name == "build");
            expect(!primary.diagnostics.empty()) << primary.steps.back().output;
            expect(!primary.diagnostics.empty() >> fatal);
            expect(primary.diagnostics[0].file == "hello.c" && primary.diagnostics[0].line == 7u && primary.diagnostics[0].severity == "error")
                << primary.diagnostics[0].file << ":" << primary.diagnostics[0].line << " " << primary.diagnostics[0].message;
            expect(broken.results[1].ok) << "alternate compiles the other branch";

            auto& tested = h.out.tested;
            expect((tested.results.size() == 2u) >> fatal);
            expect(!tested.ok);
            for (auto& r : tested.results) {
                expect(names(r) == std::vector<std::string>{"configure", "build", "test"});
                expect(r.tests.has_value() >> fatal) << r.steps.back().output;
                expect(r.tests->total == 2u && r.tests->failed == 1u && r.tests->failed_tests == std::vector<std::string>{"fails"});
            }
            auto& filtered = h.out.filtered;
            expect((filtered.results.size() == 1u) >> fatal);
            expect(filtered.ok && filtered.results[0].tests && filtered.results[0].tests->total == 1u)
                << filtered.results[0].error << filtered.results[0].steps.back().output;

            auto& ran = h.out.ran;
            expect((ran.results.size() == 2u) >> fatal);
            expect(ran.ok);
            expect(ran.results[0].steps.back().name == "run" &&
                   (ran.results[0].steps.back().output == "hello from primary\n" || ran.results[0].steps.back().output == "hello from primary\r\n"));
            expect((ran.results[1].steps.back().output == "hello from alternate\n" || ran.results[1].steps.back().output == "hello from alternate\r\n"));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"dispatch">{build_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
