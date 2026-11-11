import std;
import boost.ut;
import configuration;
import usage;
import registry;
import support;
import fixture;
import coroutine;
import native;
import front;

namespace {
    using namespace boost::ut;
    using scheduler = io::scheduler_of_t<native::context>;
    auto document() -> config::document {
        config::document d;
        d.limits = {.max_file_bytes = 1000, .max_projects = 5, .read_max_lines = 50};
        d.projects["app"].build_configurations["Debug"] = fixture::cmake("build/debug");
        fixture::options<"cmake">(d.projects["app"].build_configurations["Debug"].builders.at("main")).export_compile_commands = true;
        d.projects["app"].analysis["Cpp"] = {.server = fixture::clice("clice", {"--log-level", "off"}), .build_configuration = "Debug"};
        return d;
    }
    static constexpr auto cases = [] {
        "worker dependency publisher requires an absolute exclusive destination"_test = [] {
            for (const auto* text : {R"({"dependency_publish":""})", R"({"dependency_publish":"relative"})",
                    R"({"dependency_publish":"/published","dependency_upstream":"/upstream"})",
                    R"({"dependency_publish":"/published"})",
                    R"({"dependency_recipe":{"executable":"/usr/bin/vcpkg","root":"/opt/vcpkg","triplets":["x64-linux"]}})"})
                expect(throws<std::invalid_argument>([&] { config::parse_worker(text); }));
            const auto worker = config::parse_worker(R"({"dependency_publish":"/published","dependency_recipe":{"executable":"/usr/bin/vcpkg","root":"/opt/vcpkg","triplets":["x64-linux"]}})");
            expect(worker.dependency_publish == "/published" && !worker.dependency_upstream);
        };
        "source limits and language profiles resolve independently"_test = [] {
            auto d = document();
            config::validate(d);
            auto e = config::resolve(d, "app");
            expect(e.limits.max_file_bytes == 1000u && e.limits.read_max_lines == 50u);
            expect(language::provider_of(e.analysis.at("Cpp").server) == "clice");
        };
        "source configuration edits use revision preconditions and preserve conflicting drafts"_test = [] {
            test::scratch tmp{"source-config"};
            config::store s{document(), tmp.dir / "config.json"};
            auto settings = *s.current()->find("app");
            auto revision = config::revision_of(*s.current(), "app");
            settings.ignore = {"generated"};
            s.put_project("app", settings, revision);
            expect(throws<config::revision_conflict>([&] { s.put_project("app", settings, revision); }));
            auto restored = config::load(tmp.dir / "config.json");
            expect(restored.find("app")->ignore == std::vector<std::string>{"generated"});
            auto& server = rfl::get<language::with<"clice">>(restored.find("app")->analysis.at("Cpp").server.variant());
            expect(server.options.args == std::vector<std::string>{"--log-level", "off"});
        };
        "language servers are known by name and keep their files inside the source"_test = [] {
            expect(throws<std::invalid_argument>([] { language::validate({.server = language::with<"clice">{.options = {.cache_dir = "../cache"}}}); }));
            expect(throws<std::invalid_argument>([] { language::validate({.server = fixture::clice("")}); }));
            expect(throws<std::invalid_argument>([] { config::parse_as<language::configuration>(R"({"server":{"provider":"clangd"}})", "analysis"); }));
            expect(throws<std::invalid_argument>(
                [] { config::parse_as<language::configuration>(R"({"server":{"provider":"clice","command":["sh","-c","touch sentinel"]}})", "analysis"); }));
        };
    };
} // namespace
namespace {
    using namespace boost::ut;

    auto sample() -> config::document {
        config::document d;
        d.limits = {.max_file_bytes = 1000, .builds_per_hour = 7, .concurrent_builds = 2};
        auto& app = d.projects["app"];
        app.ignore = {".git", "*.tmp"};
        app.env = {{"LANG", "C"}, {"CC", "clang"}};
        app.build_configurations["Release-gcc"] = fixture::cmake("build/release", "gcc");
        app.build_configurations["Debug-clang"] = fixture::cmake("build/clang", "clang");
        app.build_configurations["Debug-clang"].env = {{"P", "1"}};
        app.default_configurations = {"Debug-gcc", "Debug-clang"};
        app.run_configurations["tests"] = {.operation = "test", .parameters = {.filter = "unit"}};
        app.build_configurations["Debug-gcc"] = fixture::cmake("build/gcc14", "gcc-14");
        fixture::needs(app.build_configurations["Debug-gcc"].builders.at("main")).tools = {"gcc-14"};
        app.build_configurations["lint"] = fixture::commands({"make", "lint"});
        app.build_configurations["lint"].needs.get() = {.tools = {"make"}, .platform = {.os = "linux"}};
        app.build_configurations["lint"].limits.timeout_s = 60;
        app.run_configurations["lint"] = {.operation = "build", .configurations = {"lint"}};
        d.projects["lib"] = {};
        return d;
    }

    static constexpr auto config_suite = [] {
        "selections say what they cannot do when the settings are read"_test = [] {
            auto d = sample();
            auto& app = d.projects.at("app");
            auto e = config::resolve(d, "app");
            auto names = [](auto picked) {
                std::vector<std::string> out;
                for (auto* c : picked)
                    out.push_back(c->name);
                return out;
            };
            expect(names(e.pick({"*"}, "test")) == std::vector<std::string>{"Debug-clang", "Debug-gcc", "Release-gcc"}) << "lint has no test";
            expect(names(e.pick({"*"}, "build")).size() == 4u);
            auto off = d;
            off.projects.at("app").build_configurations.at("Debug-gcc").enabled = false;
            expect(throws<std::invalid_argument>([&] { config::resolve(off, "app"); })) << "a default that is disabled";
            app.run_configurations["lint"] = {.operation = "test", .configurations = {"lint"}};
            expect(throws<std::invalid_argument>([&] { config::resolve(d, "app"); })) << "a run its configuration does not offer";
        };

        "independent named build configurations persist and resolve"_test = [] {
            test::scratch tmp{"command-config"};
            auto file = tmp.dir / "gateway.json";
            auto d = sample();
            auto& cargo = d.projects.at("app").build_configurations["cargo"];
            cargo = fixture::commands({"cargo", "build"}, {"cargo", "fetch"}, {"cargo", "test"}, "crate");
            cargo.enabled = false;
            config::store store{config::document{}, file};
            store.put_project("app", d.projects.at("app"), 0);
            auto recovered = config::resolve(config::load(file), "app");
            auto& c = recovered.config("cargo");
            expect(!c.enabled);
            auto plan = builds::prepare(c.build, {.operation = "test"});
            expect(plan.steps.size() == 3u && plan.steps[0].argv == std::vector<std::string>{"${tool:cargo}", "fetch"});
            expect(plan.steps.back().cwd == "crate");
            for (auto dir : {"../escape", "/outside", "C:\\outside", "a\\..\\b"}) {
                auto bad = d;
                auto& build = bad.projects.at("app").build_configurations.at("cargo");
                auto& options = fixture::options<"command">(build.builders.at("main"));
                options.operations.at("build").working_dir = dir;
                expect(throws<std::invalid_argument>([&] { config::validate(bad); }));
            }
        };

        "a project's settings resolve on their own, its limits from the workspace capped by the host"_test = [] {
            auto d = sample();
            config::validate(d);
            auto app = config::resolve(d, "app");
            expect(app.ignore == std::vector<std::string>{".git", "*.tmp"});
            expect(app.env.at("CC") == "clang" && app.env.at("LANG") == "C");
            expect(app.limits.max_file_bytes == 1000u && app.limits.builds_per_hour == 7u && app.limits.concurrent_builds == 2u);
            auto& gcc = app.config("Debug-gcc");
            auto& options = fixture::options<"cmake">(gcc.build.builders.at("main"));
            expect(options.c_compiler == "gcc-14" && options.build_dir == "build/gcc14");
            auto plan = builds::prepare(gcc.build, {.operation = "build"});
            expect(std::ranges::contains(plan.needs.tools, "gcc-14") && std::ranges::contains(plan.needs.tools, "cmake"));
            expect(app.config("Debug-clang").env.at("P") == "1");
            expect(app.config("lint").limits.timeout_s == 60u);
            expect(app.default_configurations == std::vector<std::string>{"Debug-gcc", "Debug-clang"});
            expect(app.pick({}, "build").size() == 2u && app.pick({"*"}, "build").size() == 4u);
            expect(app.run("tests").configurations == app.default_configurations);
            expect(app.run("lint").configurations == std::vector<std::string>{"lint"});
            expect(throws<config::not_found>([&] { app.config("arm"); }));
            expect(throws<config::not_found>([&] { app.run("missing"); }));
            auto lib = config::resolve(d, "lib");
            expect(lib.configs.empty() && lib.env.empty() && lib.limits.builds_per_hour == 7u) << "nothing of app's is shared";
            expect(throws<config::not_found>([&] { config::resolve(d, "missing"); }));
            d.granted = config::limits{.builds_per_hour = 3, .read_max_lines = 10000};
            auto capped = config::resolve(d, "app").limits;
            expect(capped.builds_per_hour == 3u && capped.read_max_lines == 2000u) << "a grant lowers a limit and never raises one";
        };

        "analysis references its worker build environment"_test = [] {
            auto d = sample();
            auto source_build = fixture::cmake("build/source", "cc");
            auto& options = fixture::options<"cmake">(source_build.builders.at("main"));
            options.export_compile_commands = true;
            source_build.outputs["database"] = {"configure", "compilation_database"};
            source_build.env["OWNER"] = "source";
            d.projects.at("app").build_configurations["Analysis"] = source_build;
            d.projects.at("app").analysis["cpp"] = {.build_configuration = "Analysis"};
            config::validate(d);
            auto e = config::resolve(d, "app");
            expect(e.config("Debug-gcc").build.env.contains("CC"));
            auto& analysis = e.config(e.analysis.at("cpp").build_configuration);
            expect(analysis.build.env.at("CC") == "clang" && analysis.build.env.at("OWNER") == "source");
            expect(fixture::options<"cmake">(analysis.build.builders.at("main")).build_dir == "build/source");
        };

        "project names and operation references are validated"_test = [] {
            expect(config::valid_name("app-1.2_x") && !config::valid_name("") && !config::valid_name(".hidden") && !config::valid_name("a/b"));
            expect(throws<std::invalid_argument>([] { config::parse(R"({"projects":{"../x":{}}})"); }));
            expect(throws<std::invalid_argument>([] { config::parse("{not json"); }));
            auto d = sample();
            d.projects.at("app").default_configurations = {"missing"};
            expect(throws<std::invalid_argument>([&] { config::validate(d); }));
            d = sample();
            d.projects.at("app").run_configurations.at("tests").configurations = {"missing"};
            expect(throws<std::invalid_argument>([&] { config::validate(d); }));
        };

        "bearer tokens are validated before publication"_test = [] {
            test::scratch tmp{"config-tokens"};
            auto file = tmp.dir / "gateway.json";
            config::store store{config::document{}, file};
            auto good = std::string(64, 'a');
            auto server = store.current()->server;
            server.auth.tokens = {{.name = "agent", .hash = good, .role = "member"}};
            store.put_server(server, 0);
            auto before = store.current();
            auto saved = test::slurp(file);
            for (auto token : std::vector<config::token>{{.name = "", .hash = good},
                                                         {.name = "../x", .hash = good},
                                                         {.name = "short", .hash = "abc"},
                                                         {.name = "upper", .hash = std::string(64, 'A')},
                                                         {.name = "operator", .hash = good, .role = "operator"}}) {
                auto changed = before->server;
                changed.auth.tokens.push_back(token);
                expect(throws<std::invalid_argument>([&] { store.put_server(changed, before->revisions.server); })) << token.name;
            }
            auto twice = before->server;
            twice.auth.tokens.push_back({.name = "agent", .hash = std::string(64, 'b')});
            expect(throws<std::invalid_argument>([&] { store.put_server(twice, before->revisions.server); }));
            auto mode = before->server;
            mode.auth.mode = "supabase";
            expect(throws<std::invalid_argument>([&] { store.put_server(mode, before->revisions.server); }));
            expect(store.current() == before) << "a refused save keeps the published snapshot";
            expect(test::slurp(file) == saved) << "a refused save keeps the file";
        };

        "listener addresses and every run limit are validated before publication"_test = [] {
            test::scratch tmp{"config-validation"};
            auto file = tmp.dir / "gateway.json";
            config::store store{config::document{}, file};
            auto before = store.current();
            auto saved = test::slurp(file);
            for (auto address : {"", "localhost", "127.0.0.256", "127.0.0.1:8080", "1.2.3", "1.2.3.4.5"}) {
                auto server = before->server;
                server.http_bind = address;
                expect(throws<std::invalid_argument>([&] { store.put_server(server, store.current()->revisions.server); }));
                server = before->server;
                server.worker_bind = address;
                expect(throws<std::invalid_argument>([&] { store.put_server(server, store.current()->revisions.server); }));
                server = before->server;
                server.source = std::string{address} + ":9091";
                expect(throws<std::invalid_argument>([&] { store.put_server(server, store.current()->revisions.server); }));
            }
            for (auto json :
                 {R"({"build_configurations":{"p":{"limits":{"timeout_s":0}}}})", R"({"build_configurations":{"p":{"limits":{"max_memory_bytes":0}}}})",
                  R"({"run_configurations":{"r":{"limits":{"timeout_s":0}}}})", R"({"run_configurations":{"r":{"limits":{"max_memory_bytes":0}}}})",
                  R"({"run_configurations":{"r":{"limits":{"max_cpu_seconds":0}}}})", R"({"run_configurations":{"r":{"parameters":{"parallel":0}}}})"}) {
                auto settings = config::parse_as<config::settings>(json, "test");
                expect(throws<std::invalid_argument>([&] { store.put_project("app", settings, 0); }));
            }
            expect(store.current() == before && test::slurp(file) == saved) << "invalid edits never publish or overwrite the file";
            expect(
                config::parse(
                    R"({"server":{"http_bind":"0.0.0.0","worker_bind":"192.168.1.2"},"projects":{"app":{"run_configurations":{"r":{"limits":{"output_limit":0}}}}}})")
                    .server.http_bind == "0.0.0.0");

            for (auto invalid : {config::source{.bind = "localhost"}, config::source{.storage = ""}})
                expect(throws<std::invalid_argument>([&] { config::validate(invalid); }));
            config::validate(config::source{.bind = "127.0.0.1", .port = 9091, .storage = "source"});

            auto worker_file = tmp.dir / "worker.json";
            for (auto json : {R"({"gateway":"localhost:9090"})", R"({"gateway":"127.0.0.1:65536"})", R"({"work_root":""})", R"({"max_inflight":0})"}) {
                test::spit(worker_file, json);
                expect(throws<std::invalid_argument>([&] { config::load_worker(worker_file); }));
            }
            test::spit(worker_file, R"({"token":"s3cret"})");
            auto enrolled = config::load_worker(worker_file);
            expect(enrolled.token == std::optional<std::string>{"s3cret"});
        };

        "configuration readers reject misspelled fields"_test = [] {
            expect(throws<std::invalid_argument>([] { config::parse_as<config::worker>(R"({"max_infligth":4})", "worker"); }));
            expect(throws<std::invalid_argument>([] { config::parse(R"({"limits":{"max_projcts":1}})"); }));
            expect(throws<std::invalid_argument>([] { config::parse(R"({"projects":{"app":{"build_configurations":{"x":{"operatons":{}}}}}})"); }));
            expect(throws<std::invalid_argument>(
                [] { config::parse_as<builds::builder>(R"({"adapter":"cmake","options":{"build_dir":"build/debug","generatr":"Ninja"}})", "builder"); }));
        };

        "saves name the revision they read; a stale one is refused with the current value"_test = [] {
            test::scratch tmp{"config-revision"};
            auto file = tmp.dir / "gateway.json";
            config::store s{config::document{}, file};
            auto saved = test::slurp(file);
            expect(throws<config::revision_conflict>([&] { s.put_project("app", {}, 1); })) << "a new project names revision 0";
            expect(test::slurp(file) == saved) << "a refused save never touches the file";
            s.put_project("app", {}, 0);
            auto app = config::revision_of(*s.current(), "app");
            expect(app == 1u);
            s.put_limits({.max_file_bytes = 5}, 0);
            expect(config::revision_of(*s.current(), "app") == app) << "sections are independent";
            expect(throws<config::revision_conflict>([&] { s.put_project("app", {}, 0); })) << "creating an existing project conflicts";
            s.put_project("app", {.ignore = {"a"}}, app);
            auto loser = [&] {
                try {
                    s.put_project("app", {.ignore = {"b"}}, app);
                    return false;
                } catch (const config::revision_conflict& e) {
                    expect(e.revision > app);
                    return true;
                }
            }();
            expect(loser);
            s.put_project("app", {.ignore = {"b"}}, config::revision_of(*s.current(), "app"));
            expect(s.current()->projects.at("app").ignore == std::vector<std::string>{"b"});
            auto after = config::load(file);
            expect(after.revisions.counter == s.current()->revisions.counter && config::revision_of(after, "app") == config::revision_of(*s.current(), "app"));
            s.remove_project("app");
            s.put_project("app", {}, 0);
            expect(s.current()->projects.contains("app")) << "a removed project is created again from revision 0";
        };

        "a configuration edit reaches every core before it returns"_test = [] {
            test::scratch tmp{"config"};
            cores::io_context<native::context> rt{3};
            execution::thread_pool pool{1};
            front::config_hub hub{rt, config::store{config::document{}, tmp.dir / "gateway.json"}, pool};
            std::vector<bool> seen;
            rt.run([&](scheduler&, std::size_t core) -> coroutine::task<> {
                if (core != 0)
                    return [](auto&) -> coroutine::task<> { co_return; }(core);
                return [](front::config_hub& hub, std::vector<bool>& seen, std::size_t cores) -> coroutine::task<> {
                    co_await hub.edit(0, [](config::store& s) { s.put_project("app", {}, 0); });
                    for (std::size_t c = 0; c < cores; ++c)
                        seen.push_back(hub.current(c)->projects.contains("app"));
                }(hub, seen, rt.size());
            });
            expect(seen == std::vector<bool>{true, true, true});
            expect(config::load(tmp.dir / "gateway.json").projects.contains("app")) << "and is on the disk";
        };
    };

    static constexpr auto usage_suite = [] {
        "monthly build time remains enforced after replay"_test = [] {
            test::scratch tmp{"monthly-usage"};
            config::effective_limits limits{.build_seconds_per_month = 3};
            {
                usage::ledger log{tmp.dir};
                auto ticket = log.admit_build("app", limits);
                log.record(ticket, {.project = "app", .ok = true, .seconds = 3});
                log.record(usage::call_event{.project = "app", .operation = "build", .reuse = "executed", .state = "succeeded",
                    .seconds = 3, .timings = {.sync_seconds = 0.25, .queue_seconds = 0.5, .dependency_seconds = 0.75, .worker_seconds = 1.5}});
                expect(log.calls("app", 1).size() == 1u && log.calls("other", 1).empty());
            }
            usage::ledger restored{tmp.dir};
            const auto calls = restored.calls("app", 1);
            expect(calls.size() == 1u >> fatal);
            expect(calls.front().timings.sync_seconds == 0.25 && calls.front().timings.queue_seconds == 0.5 &&
                calls.front().timings.dependency_seconds == 0.75 && calls.front().timings.worker_seconds == 1.5);
            expect(restored.build_seconds_this_month() == 3.0);
            expect(throws<usage::quota_exceeded>([&] { restored.admit_build("lib", limits); }));
            expect(restored.counters().projects.at("app").builds_ok == 1u);
        };
        "events are numbered, read after a cursor, and keep their numbering past pruning"_test = [] {
            test::scratch tmp{"usage-events"};
            {
                usage::ledger log{tmp.dir};
                log.record({.at = usage::now() - 100 * 86400, .project = "app", .tool = "read"});
                log.record({.project = "app", .tool = "edit"});
                log.record(log.admit_build("app", config::effective_limits{}), {.project = "app", .ok = true, .seconds = 2});
                auto first = log.events(0, 1);
                expect((first.events.size() == 1u) >> fatal);
                expect(first.events[0].seq == 2u && first.next == 2u && first.oldest == 2u) << "the event past keeping is not served";
                auto rest = log.events(first.next, 10);
                expect((rest.events.size() == 1u) >> fatal);
                expect(rest.events[0].build && rest.next == 3u);
                expect(log.events(rest.next, 10).events.empty());
            }
            {
                usage::ledger reopened{tmp.dir};
                auto all = reopened.events(0, 10);
                expect(all.oldest == 2u) << "the event past keeping is gone and the reader can tell";
                expect(all.events.size() == 2u && all.events.front().seq == 2u);
                reopened.record({.project = "app", .tool = "list"});
                expect(reopened.events(3, 10).events.at(0).seq == 4u) << "numbering continues after a restart";
            }
            test::scratch old{"usage-old"};
            {
                usage::ledger log{old.dir};
                log.record({.at = usage::now() - 100 * 86400, .project = "app", .tool = "read"});
                log.record({.at = usage::now() - 90 * 86400, .project = "app", .tool = "read"});
            }
            {
                usage::ledger pruned{old.dir};
                expect(pruned.events(0, 10).events.empty() && pruned.events(0, 10).oldest == 3u);
            }
            usage::ledger again{old.dir};
            again.record({.project = "app", .tool = "read"});
            expect(again.events(0, 10).events.at(0).seq == 3u) << "a log pruned to nothing still remembers the last number";
        };

        "ledger notifications follow visible committed state"_test = [] {
            test::scratch tmp{"usage-events"};
            usage::ledger ledger{tmp.dir};
            unsigned notifications = 0;
            ledger.changed = [&] { ++notifications; };
            auto ticket = ledger.admit_build("app", config::effective_limits{});
            expect(notifications == 1u && ledger.running() == 1u);
            ledger.record(ticket, {.project = "app", .ok = true});
            expect(notifications == 2u && ledger.running() == 0u);
            expect(ledger.events(0, 10).events.size() == 1u);
            ledger.record({.project = "app", .tool = "read"});
            expect(notifications == 3u && ledger.events(0, 10).events.size() == 2u);
            ledger.release(ticket);
            expect(notifications == 3u) << "no change does not emit a notification";
        };

        "build quotas: rate and build time"_test = [] {
            test::scratch tmp{"usage"};
            usage::ledger l{tmp.dir};
            config::effective_limits limits{.builds_per_hour = 2, .build_seconds_per_day = 100};
            auto t1 = l.admit_build("app", limits);
            l.record(t1, {.project = "app", .ok = true, .seconds = 30});
            auto t2 = l.admit_build("app", limits);
            l.release(t2);
            auto t3 = l.admit_build("app", limits);
            l.record(t3, {.project = "app", .ok = false, .seconds = 80});
            expect(throws<usage::quota_exceeded>([&] { l.admit_build("app", limits); })) << "2 builds this hour";
            config::effective_limits roomy = limits;
            roomy.builds_per_hour = 100;
            expect(throws<usage::quota_exceeded>([&] { l.admit_build("lib", roomy); })) << "110 of 100 build seconds";
        };

        "a build that never ran does not count against the rate"_test = [] {
            test::scratch tmp{"usage"};
            usage::ledger l{tmp.dir};
            config::effective_limits limits{.builds_per_hour = 1};
            l.record(l.admit_build("app", limits), {.project = "app", .error = "no worker holds 'linux'"});
            expect(l.builds_last_hour("app") == 0u);
            expect(nothrow([&] { l.release(l.admit_build("app", limits)); }));
            expect(l.days(1, "app")[0].failed_builds == 1u) << "still in the statistics";
        };

        "an unknown execution outcome is charged and is not classified as unrun"_test = [] {
            test::scratch tmp{"usage-unknown"};
            usage::ledger ledger{tmp.dir};
            config::effective_limits limits{.builds_per_hour = 1};
            ledger.record(ledger.admit_build("app", limits),
                          {.project = "app", .worker = "remote", .error = "connection lost", .error_code = "outcome_unknown"});
            expect(ledger.builds_last_hour("app") == 1u);
            expect(throws<usage::quota_exceeded>([&] { ledger.admit_build("app", limits); }));
            auto totals = ledger.counters().projects.at("app");
            expect(totals.builds_unknown == 1u && totals.builds_not_run == 0u && totals.builds_failed == 0u);
        };

        "the ledger survives a restart"_test = [] {
            test::scratch tmp{"usage"};
            {
                usage::ledger l{tmp.dir};
                config::effective_limits limits;
                l.record(l.admit_build("app", limits), {.project = "app", .ok = true, .seconds = 3, .bytes_sent = 100});
                l.record(l.admit_build("app", limits), {.project = "app", .ok = false, .seconds = 2});
                l.record(usage::tool_event{.project = "app", .tool = "edit"});
                l.record(usage::tool_event{.project = "lib", .tool = "read", .error = true});
            }
            usage::ledger again{tmp.dir};
            auto days = again.days(1);
            expect((days.size() == 2u) >> fatal);
            auto& app = days[0].project == "app" ? days[0] : days[1];
            expect(app.builds == 2u && app.failed_builds == 1u && app.build_seconds == 5.0 && app.bytes_synced == 100u);
            expect(app.tool_calls == 1u);
            expect(again.days(1, "lib")[0].tool_errors == 1u);
            expect(again.counters().tools.at("edit").calls == 1u && again.counters().tools.at("read").errors == 1u);
            expect(again.builds("app", 10).size() == 2u);
            expect(again.builds("app", 10)[0].ok == false) << "newest first";
            expect(again.builds_last_hour("app") == 2u);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"source_config">{cases};
    boost::ut::suite<"build_config">{config_suite};
    boost::ut::suite<"usage">{usage_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
