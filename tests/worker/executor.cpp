import std;
import boost.ut;
import coroutine;
import native;
import content;
import protocol;
import replica;
import executor;
import runner;
import support;
import archive;
import durability;
import host;
import dependencies;
import constants;
import reflection;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = native::scheduler;

    auto part(std::string_view text, std::uint64_t offset, std::uint64_t length, std::string_view whole) -> jobs::source_part {
        auto bytes = std::as_bytes(std::span{text.substr(offset, length)});
        return {content::hash_of(whole).str(), offset, whole.size(), {bytes.begin(), bytes.end()}};
    }
    auto blob(std::string_view text) -> jobs::source_part { return part(text, 0, text.size(), text); }

    auto entry(std::string path, std::string_view text, bool exec = false) -> content::entry {
        return {.path = std::move(path), .digest = content::hash_of(text), .size = text.size(), .executable = exec};
    }

    template <class F>
    void exercise(F work, std::optional<std::filesystem::path> dependency_cache = {}, std::optional<std::filesystem::path> dependency_upstream = {},
                  std::optional<std::filesystem::path> dependency_publish = {}, std::optional<builds::dependency_recipe> recipe = {}) {
        test::scratch tmp{"executor"};
        cores::io_context<native::context> rt{2};
        execution::thread_pool disk{2};
        replica::service service{rt,
                                 {.root = tmp.dir / "cache",
                                  .worker = "worker-test",
                                  .env = {{"VALUE", "worker"}},
                                  .inherit_env = false,
                                  .dependency_cache = dependency_cache,
                                  .dependency_upstream = dependency_upstream,
                                  .dependency_publish = dependency_publish,
                                  .dependency_recipe = recipe},
                                 disk,
                                 1};
        auto run_core = [&](ctx& io, std::size_t core) -> coroutine::task<> {
            if (core != 0)
                co_return;
            auto run_operation = [&]() -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto run_tasks = [&](coroutine::scope& tasks) -> coroutine::task<> {
                    static constexpr auto run_child = [](ctx& io, auto& listener, auto& service, coroutine::scope& tasks) -> coroutine::task<> {
                        auto [socket, endpoint] = co_await net::async_accept(listener);
                        transport::channel<native::socket> channel{socket};
                        transport::dispatcher<native::socket> dispatch;
                        service.install(dispatch, 0);
                        for (;;)
                            tasks.spawn(dispatch.handle(channel, co_await channel.recv()));
                    };
                    tasks.spawn(run_child(io, listener, service, tasks));
                    auto socket = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
                    transport::channel<native::socket> channel{socket};
                    auto run_pump = [&](coroutine::scope& pump) -> coroutine::task<> {
                        pump.spawn(channel.pump([](const transport::frame&) {}));
                        if constexpr (std::invocable<F&, decltype(channel)&, decltype(tmp.dir)&, replica::service&, ctx&>)
                            co_await work(channel, tmp.dir, service, io);
                        else if constexpr (std::invocable<F&, decltype(channel)&, decltype(tmp.dir)&, replica::service&>)
                            co_await work(channel, tmp.dir, service);
                        else
                            co_await work(channel, tmp.dir);
                    };
                    co_await coroutine::with_scope(std::move(run_pump));
                };
                co_await coroutine::with_scope(std::move(run_tasks));
            };
            co_await service.run(0, run_operation());
        };
        rt.run(std::move(run_core));
    }

    auto recipe_script(const std::filesystem::path& root, std::string_view body) -> builds::dependency_recipe {
        auto script = root / "trusted-vcpkg";
        if (host::os == "windows") {
            // Windows runs no scripts directly: a copy of the helper runs the script beside it.
            script += ".exe";
            const auto* helper = std::getenv("TEST_COMMAND_HELPER");
            if (!helper)
                throw std::runtime_error("TEST_COMMAND_HELPER is required");
            std::filesystem::copy_file(helper, script, std::filesystem::copy_options::overwrite_existing);
            test::spit(script.string() + ".sh", "#!" + test::shell() + "\n" + std::string{body} + "\n");
        } else
            test::spit(script, "#!/bin/sh\n" + std::string{body} + "\n");
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        return {.executable = script.string(), .root = root.string(), .triplets = {"x64-linux"}};
    }

    static constexpr auto cases = [] {
        "dependency RPC rejects oversized payloads and undeclared registry fields before execution"_test = [] {
            jobs::dependency_fill valid{.manifest = builds::dependency_manifest(R"({"dependencies":["fmt",{"name":"zlib","features":["core"]}]})")};
            expect(rfl::json::write(transport::codec<jobs::dependency_fill>::decode(transport::codec<jobs::dependency_fill>::encode(valid))) ==
                   rfl::json::write(valid));
            std::vector<std::byte> oversized(constants::dependency_request_bytes + 1);
            expect(throws<std::invalid_argument>([&] { transport::codec<jobs::dependency_fill>::decode(oversized); }));
            auto text = rfl::json::write(jobs::dependency_fill{});
            text.insert(1, "\"vcpkg-configuration\":{},");
            expect(throws([&] { transport::codec<jobs::dependency_fill>::decode(std::as_bytes(std::span{text})); }));
        };
        if (const auto* executable = std::getenv("TEST_VCPKG"); executable && std::getenv("TEST_VCPKG_DOWNLOADS")) {
            "concurrent trusted workers publish one real vcpkg compilation"_test = [executable] {
                test::scratch shared{"vcpkg-publishers"};
                const auto port = shared.dir / "ports/fug-cache-probe";
                test::spit(port / "vcpkg.json", R"({"name":"fug-cache-probe","version-string":"1.0.0"})");
                test::spit(port / "probe.cpp", "int cache_probe() { return 42; }\n");
                test::spit(port / "portfile.cmake", R"cmake(file(APPEND "$ENV{PROBE_LEDGER}" "compiled\n")
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include" "${CURRENT_PACKAGES_DIR}/lib" "${CURRENT_PACKAGES_DIR}/debug/lib" "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_execute_required_process(COMMAND /usr/bin/c++ -c "${CMAKE_CURRENT_LIST_DIR}/probe.cpp" -o "${CURRENT_BUILDTREES_DIR}/probe.o" WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}" LOGNAME compile)
vcpkg_execute_required_process(COMMAND /usr/bin/ar rcs "${CURRENT_PACKAGES_DIR}/lib/libprobe.a" "${CURRENT_BUILDTREES_DIR}/probe.o" WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}" LOGNAME archive)
file(COPY "${CURRENT_PACKAGES_DIR}/lib/libprobe.a" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
file(WRITE "${CURRENT_PACKAGES_DIR}/include/probe.h" "int cache_probe();\n")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" "Test fixture, public domain.\n")
)cmake");
                test::spit(shared.dir / "triplets/fug-cache-probe.cmake", R"cmake(set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED PROBE_LEDGER)
)cmake");
                std::atomic<int> ready{0};
                auto worker = [&](int index) {
                    std::string library;
                    auto run_operation = [&](auto& channel, auto&, replica::service&, ctx& io) -> coroutine::task<> {
                        const std::string manifest = R"({"name":"cache-consumer","version-string":"1","dependencies":["fug-cache-probe"]})";
                        ++ready;
                        const auto deadline = std::chrono::steady_clock::now() + 10s;
                        while (ready.load() != 2) {
                            if (std::chrono::steady_clock::now() >= deadline)
                                throw std::runtime_error("other publisher did not reach the start barrier");
                            co_await execution::schedule_after(io, 10ms);
                        }
                        jobs::dependency_fill request{
                            .manifest = builds::dependency_manifest(manifest), .triplet = "fug-cache-probe", .host_triplet = "x64-linux", .timeout_s = 100};
                        auto result = co_await channel.call(request);
                        if (!result)
                            transport::raise_call_error(std::move(result.error()));
                        if (!result->ok() || !result->steps.front().cache_error.empty())
                            throw std::runtime_error(result->steps.front().output + result->steps.front().cache_error);
                        library = test::slurp(std::filesystem::path{result->tree} / "installed/fug-cache-probe/lib/libprobe.a");
                    };
                    exercise(std::move(run_operation), shared.dir / ("private-" + std::to_string(index)), {}, shared.dir / "published",
                             builds::dependency_recipe{.executable = executable,
                                                       .root = std::filesystem::path{executable}.parent_path().string(),
                                                       .triplets = {"x64-linux", "fug-cache-probe"},
                                                       .overlay_ports = {(shared.dir / "ports").string()},
                                                       .overlay_triplets = {(shared.dir / "triplets").string()},
                                                       .env = {{"PATH", "/usr/local/bin:/usr/bin:/bin"},
                                                               {"HOME", std::getenv("HOME") ? std::getenv("HOME") : ""},
                                                               {"VCPKG_FORCE_SYSTEM_BINARIES", "1"},
                                                               {"VCPKG_DISABLE_METRICS", "1"},
                                                               {"VCPKG_DOWNLOADS", std::getenv("TEST_VCPKG_DOWNLOADS")},
                                                               {"PROBE_LEDGER", (shared.dir / "compiled").string()}},
                                                       .timeout_s = 100,
                                                       .offline = true});
                    return library;
                };
                auto first = std::async(std::launch::async, worker, 1);
                auto second = std::async(std::launch::async, worker, 2);
                const auto a = first.get(), b = second.get();
                expect(!a.empty() && a == b);
                expect(test::slurp(shared.dir / "compiled") == "compiled\n");
                std::size_t archives = 0;
                for (const auto& item : std::filesystem::recursive_directory_iterator(shared.dir / "published/vcpkg"))
                    if (item.path().extension() == ".zip")
                        ++archives;
                expect(archives == 1_u);
            };
        } else {
            skip / "concurrent trusted workers publish one real vcpkg compilation"_test = [] {};
        }
        "dependency publication preserves the first archive and separates it from mutable staging"_test = [] {
            test::scratch tmp{"dependency-publication"};
            const auto cache = tmp.dir / "private", published = tmp.dir / "published";
            const std::string abi(64, 'a');
            const auto relative = std::filesystem::path{"vcpkg/aa"} / (abi + ".zip");
            test::spit(cache / relative, "first archive");
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            expect(dependencies::publish(cache, published, deadline) == 1_u);
            expect(test::slurp(published / relative) == "first archive");
            test::spit(cache / relative, "replacement");
            expect(dependencies::publish(cache, published, deadline) == 0_u);
            expect(test::slurp(published / relative) == "first archive");
            expect(!std::filesystem::equivalent(cache / relative, published / relative));
            test::spit(cache / "vcpkg/aa/invalid.zip", "bad");
            expect(throws<std::runtime_error>([&] { dependencies::publish(cache, published, deadline); }));
            expect(!std::filesystem::exists(published / "vcpkg/aa/invalid.zip"));
            expect(throws<std::runtime_error>([&] { dependencies::publish(cache, published, std::chrono::steady_clock::now()); }));
            for (const auto& item : std::filesystem::directory_iterator(published / "vcpkg"))
                expect(!item.path().filename().string().starts_with(".publish-"));
        };
        "expired publication does not prepare storage even for an empty cache"_test = [] {
            test::scratch tmp{"expired-publication"};
            const auto cache = tmp.dir / "private", published = tmp.dir / "published";
            std::filesystem::create_directories(cache / "vcpkg");
            expect(throws<std::runtime_error>([&] { dependencies::publish(cache, published, std::chrono::steady_clock::now()); }));
            expect(!std::filesystem::exists(published));
        };
        "trusted publisher only accepts selectors and preserves results on publication failure"_test = [] {
            test::scratch tmp{"publisher-worker"};
            const auto cache = tmp.dir / "private", published = tmp.dir / "published";
            const std::string abi(64, 'b');
            const auto relative = std::filesystem::path{"vcpkg/bb"} / (abi + ".zip");
            test::spit(cache / relative, "staged archive");
            auto recipe = recipe_script(tmp.dir, "printf done; exit 7");
            recipe.env["EXPECTED"] =
                "clear;files," + (published / "vcpkg").generic_string() + ",read;files," + (cache / "vcpkg").generic_string() + ",readwrite";
            auto run_operation = [&](auto& channel, auto&) -> coroutine::task<> {
                expect(!(co_await channel.call(jobs::sync_offer{"app", {}})).has_value());
                expect(!(co_await channel.call(jobs::source_bytes{{blob("tenant bytes")}})).has_value());
                expect(!(co_await channel.call(jobs::build_request{.project = "app"})).has_value());
                expect(!(co_await channel.call(jobs::export_read{content::hash_of("tenant bytes")})).has_value());
                auto invalid = co_await channel.call(jobs::dependency_fill{.triplet = "../escape"});
                expect(!invalid.has_value());
                jobs::dependency_fill excessive;
                excessive.features.resize(constants::dependency_manifest_entries + 1, "feature");
                expect(!(co_await channel.call(excessive)).has_value());
                jobs::dependency_fill request;
                auto failed = co_await channel.call(request);
                expect(failed.has_value() >> fatal);
                expect(!failed->ok() && !std::filesystem::exists(published / relative));
                recipe_script(tmp.dir, "test \"$VCPKG_BINARY_SOURCES\" = \"$EXPECTED\" && printf done");
                auto success = co_await channel.call(request);
                expect(success.has_value() >> fatal);
                expect(success->ok() && success->steps.front().cache_error.empty());
                expect(test::slurp(published / relative) == "staged archive");
                test::spit(cache / "vcpkg/bb/invalid.zip", "bad");
                auto partial = co_await channel.call(request);
                expect(partial.has_value() >> fatal);
                expect(partial->ok() && partial->steps.front().output == "done");
                expect(partial->steps.front().cache_error.contains("not a vcpkg ABI"));
                durability::dir_lock unlocked{published / "vcpkg", "publisher released"};
            };
            exercise(std::move(run_operation), cache, {}, published, recipe);
        };
        "worker binds a read-only upstream and never locks it"_test = [] {
            test::scratch shared{"dependency-upstream"};
            auto upstream = shared.dir / "published", cache = shared.dir / "private";
            auto run_operation = [&](auto& channel, auto&) -> coroutine::task<> {
                content::manifest tree;
                co_await channel.call(jobs::sync_offer{"app", tree});
                auto result =
                    co_await channel.call(jobs::build_request{.project = "app",
                                                              .snapshot = tree.id(),
                                                              .steps = {{.name = "sources",
                                                                         .argv = {test::shell(), "-c", "printf '%s' \"$VCPKG_BINARY_SOURCES\""},
                                                                         .env = {{"VCPKG_BINARY_SOURCES", "clear;files,${cache:vcpkg},readwrite"}}}}});
                expect(result.has_value() >> fatal);
                expect(result->ok());
                expect(result->steps.front().output ==
                       "clear;files," + (upstream / "vcpkg").generic_string() + ",read;files," + (cache / "vcpkg").generic_string() + ",readwrite");
                expect(!std::filesystem::exists(upstream));
                expect(std::filesystem::is_directory(cache / "vcpkg"));
            };
            exercise(std::move(run_operation), cache, upstream);
        };
        "publisher defaults never import the older general cache"_test = [] {
            test::scratch shared{"publisher-default"};
            auto run_operation = [&](auto& channel, auto& root) -> coroutine::task<> {
                const std::string abi(64, 'c');
                const auto relative = std::filesystem::path{"vcpkg/cc"} / (abi + ".zip");
                test::spit(root / "cache/dependencies" / relative, "old untrusted archive");
                auto result = co_await channel.call(jobs::dependency_fill{});
                expect(result.has_value() >> fatal);
                expect(result->ok() && result->steps.front().cache_error.empty());
                expect(result->steps.front().output == "clear;files," + (shared.dir / "vcpkg").generic_string() + ",read;files," +
                                                           (root / "cache/publisher/dependencies/vcpkg").generic_string() + ",readwrite");
                expect(!std::filesystem::exists(shared.dir / relative));
                expect(test::slurp(root / "cache/dependencies" / relative) == "old untrusted archive");
            };
            exercise(std::move(run_operation), {}, {}, shared.dir, recipe_script(shared.dir, "printf '%s' \"$VCPKG_BINARY_SOURCES\""));
        };
        "overlapping worker caches fail before creating a lock or running a command"_test = [] {
            test::scratch shared{"dependency-overlap"};
            const auto cache = shared.dir / "cache";
            for (bool publishing : {false, true}) {
                auto run_operation = [&](auto& channel, auto& root) -> coroutine::task<> {
                    content::manifest tree;
                    if (!publishing)
                        co_await channel.call(jobs::sync_offer{"app", tree});
                    auto result = publishing ? co_await channel.call(jobs::dependency_fill{})
                                             : co_await channel.call(
                                                   jobs::build_request{.project = "app",
                                                                       .snapshot = tree.id(),
                                                                       .steps = {{.name = "install",
                                                                                  .argv = {test::shell(), "-c", "touch installed"},
                                                                                  .env = {{"VCPKG_BINARY_SOURCES", "clear;files,${cache:vcpkg},readwrite"}}}}});
                    expect(!result.has_value() >> fatal);
                    const auto* failure = std::get_if<jobs::failure>(&result.error());
                    expect((failure != nullptr) >> fatal);
                    expect(failure->message.contains("must not overlap"));
                    expect(!std::filesystem::exists(cache));
                    expect(!std::filesystem::exists(root / "cache/trees/app/default/installed"));
                };
                exercise(std::move(run_operation), cache, publishing ? std::nullopt : std::optional{cache}, publishing ? std::optional{cache} : std::nullopt,
                         publishing ? std::optional{recipe_script(shared.dir, "touch installed")} : std::nullopt);
            }
        };
        "dependency cache lock waiting respects the build deadline and releases for retry"_test = [] {
            test::scratch shared{"dependency-lock"};
            auto held = std::make_unique<durability::dir_lock>(shared.dir / "vcpkg", "test cache");
            auto run_operation = [&](auto& channel, auto& root) -> coroutine::task<> {
                content::manifest tree;
                co_await channel.call(jobs::sync_offer{"app", tree});
                jobs::build_request request{.project = "app",
                                            .snapshot = tree.id(),
                                            .steps = {{.name = "install",
                                                       .argv = {test::shell(), "-c", "printf ready > installed; printf installed"},
                                                       .env = {{"VCPKG_BINARY_SOURCES", "clear;files,${cache:vcpkg},readwrite"}},
                                                       .preparation = true}},
                                            .timeout_s = 1};
                const auto start = std::chrono::steady_clock::now();
                auto blocked = co_await channel.call(request);
                expect(blocked.has_value() >> fatal);
                expect(!blocked->ok() && blocked->steps.at(0).timed_out);
                expect(blocked->preparation_seconds.value_or(-1) > 0 && blocked->execution_seconds == 0);
                expect(blocked->steps.at(0).output.contains("shared vcpkg cache lock held by another install"));
                expect(std::chrono::steady_clock::now() - start < 3s);
                expect(!std::filesystem::exists(root / "cache/trees/app/default/installed"));
                held.reset();
                auto retried = co_await channel.call(request);
                expect(retried.has_value() >> fatal);
                expect(retried->ok() && retried->steps.at(0).output == "installed");
                durability::dir_lock available{shared.dir / "vcpkg", "released cache"};
            };
            exercise(std::move(run_operation), shared.dir);
        };
        "a waiting cache install acquires the released lock and cancellation prevents execution"_test = [] {
            test::scratch shared{"dependency-wait"};
            auto held = std::make_unique<durability::dir_lock>(shared.dir / "vcpkg", "test cache");
            auto run_operation = [&](auto& channel, auto& root, replica::service&, ctx& io) -> coroutine::task<> {
                content::manifest tree;
                co_await channel.call(jobs::sync_offer{"app", tree});
                jobs::build_request request{.project = "app",
                                            .snapshot = tree.id(),
                                            .steps = {{.name = "install",
                                                       .argv = {test::shell(), "-c", "printf ready > installed; printf installed"},
                                                       .env = {{"VCPKG_BINARY_SOURCES", "clear;files,${cache:vcpkg},readwrite"}},
                                                       .preparation = true}},
                                            .timeout_s = 5};
                const auto installed = root / "cache/trees/app/default/installed";
                auto run_timers_2 = [&](coroutine::scope& timers) -> coroutine::task<> {
                    static constexpr auto run_child = [](ctx& io, auto& held, const auto& installed) -> coroutine::task<> {
                        co_await execution::schedule_after(io, 150ms);
                        expect(!std::filesystem::exists(installed));
                        held.reset();
                    };
                    timers.spawn(run_child(io, held, installed));
                    auto result = co_await channel.call(request);
                    expect(result.has_value() >> fatal);
                    expect(result->ok() && result->steps.front().output == "installed");
                };
                co_await coroutine::with_scope(std::move(run_timers_2));
                expect(std::filesystem::exists(installed));
                std::filesystem::remove(installed);
                held = std::make_unique<durability::dir_lock>(shared.dir / "vcpkg", "test cache");
                coroutine::stop_source stop;
                auto run_timers = [&](coroutine::scope& timers) -> coroutine::task<> {
                    static constexpr auto run_child = [](ctx& io, coroutine::stop_source& stop) -> coroutine::task<> {
                        co_await execution::schedule_after(io, 150ms);
                        stop.request_stop();
                    };
                    timers.spawn(run_child(io, stop));
                    auto result = co_await coroutine::under(stop.get_token(), channel.call(request));
                    expect(!result);
                };
                co_await coroutine::with_scope(std::move(run_timers));
                held.reset();
                request.steps = {{.name = "barrier", .argv = {test::shell(), "-c", "test ! -e installed && printf stopped"}}};
                auto barrier = co_await channel.call(request);
                expect(barrier.has_value() >> fatal);
                expect(barrier->ok() && barrier->steps.front().output == "stopped");
                expect(!std::filesystem::exists(installed));
                durability::dir_lock available{shared.dir / "vcpkg", "released cache"};
            };
            exercise(std::move(run_operation), shared.dir);
        };
        "workers bind the shared dependency cache outside fresh checkout trees"_test = [] {
            test::scratch shared{"dependency-cache"};
            for (int worker = 0; worker < 2; ++worker) {
                auto run_operation = [&](auto& channel, auto&) -> coroutine::task<> {
                    content::manifest tree;
                    const auto project = "project-" + std::to_string(worker);
                    co_await channel.call(jobs::sync_offer{project, tree});
                    jobs::build_request request{
                        .project = project,
                        .snapshot = tree.id(),
                        .steps =
                            {{.name = "cache probe",
                              .argv =
                                  {test::shell(), "-c",
                                   R"(cache="${VCPKG_BINARY_SOURCES#clear;files,}"; cache="${cache%,readwrite}"; test -f "$cache/package" || printf compiled > "$cache/package"; cat "$cache/package")"},
                              .env = {{"VCPKG_BINARY_SOURCES", "clear;files,${cache:vcpkg},readwrite"}}}},
                        .fresh = true};
                    auto result = co_await channel.call(request);
                    expect(result.has_value() >> fatal);
                    expect(result->ok() && result->steps.front().output == "compiled");
                    expect(test::slurp(shared.dir / "vcpkg/package") == "compiled");
                };
                exercise(std::move(run_operation), shared.dir);
            }
        };
        "worker preserves full step logs when inline output is truncated and a build fails"_test = [] {
            static constexpr auto run_operation = [](auto& channel, auto&) -> coroutine::task<> {
                content::manifest tree;
                expect((co_await channel.call(jobs::sync_offer{"app", tree})).has_value());
                auto report = co_await channel.call(
                    jobs::build_request{.project = "app",
                                        .snapshot = tree.id(),
                                        .steps = {{.name = "first", .argv = {test::shell(), "-c", "printf first-complete-output"}, .preparation = true},
                                                  {.name = "failed", .argv = {test::shell(), "-c", "printf second-failure-output; exit 7"}}},
                                        .output_limit = 4});
                expect(report.has_value() >> fatal);
                expect(!report->ok() && report->steps.size() == 2u);
                expect(report->preparation_seconds.value_or(-1) >= report->steps[0].seconds);
                expect(report->execution_seconds.value_or(-1) >= report->steps[1].seconds);
                const std::array expected{"first-complete-output", "second-failure-output"};
                for (std::size_t index = 0; index < report->steps.size(); ++index) {
                    const auto& step = report->steps[index];
                    expect(step.truncated && !step.log_truncated && step.log_error.empty());
                    expect(step.output.size() == 4u && step.log.has_value());
                    auto bytes = co_await channel.call(jobs::export_read{step.log.value().digest});
                    expect(bytes.has_value() >> fatal);
                    const auto& part = bytes->parts.at(0);
                    expect(std::string(reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()) == expected[index]);
                }
            };
            exercise(run_operation);
        };
        "log capture limits and storage failure preserve process results"_test = [] {
            test::scratch tmp{"runner-logs"};
            cores::io_context<native::context> rt{1};
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto capped = co_await ::runner::run_step(io, {.argv = {test::shell(), "-c", "printf abcdef; exit 7"}, .cwd = tmp.dir}, 5s, 4, "capped",
                                                          tmp.dir / "capped.log", 3);
                expect(capped.exit_code == 7 && capped.truncated && capped.log_truncated) << capped.exit_code << capped.output << capped.log_error;
                expect(test::slurp(tmp.dir / "capped.log") == "abc");
                auto failed = co_await ::runner::run_step(io, {.argv = {test::shell(), "-c", "printf kept; exit 7"}, .cwd = tmp.dir}, 5s, 100, "failed",
                                                          tmp.dir / "missing/log");
                expect(failed.exit_code == 7 && failed.output == "kept" && !failed.log_error.empty()) << failed.exit_code << failed.output << failed.log_error;
            };
            rt.run(std::move(run_core));
        };

        "worker exports remain readable after the hot tree changes"_test = [] {
            static constexpr auto run_operation = [](auto& channel, auto& root) -> coroutine::task<> {
                content::manifest manifest{{entry("output.bin", "stable output")}};
                auto plan = co_await channel.call(jobs::sync_offer{"app", manifest});
                co_await channel.call(jobs::source_bytes{{blob("stable output")}});
                auto report = co_await channel.call(jobs::build_request{.project = "app", .snapshot = plan->snapshot, .exports = {{"binary", "output.bin"}}});
                expect(report->ok() && report->exports.size() == 1u);
                if (report->exports.size() != 1)
                    throw std::runtime_error("worker did not return exported file");
                test::spit(root / "cache/trees/app/default/output.bin", "changed tree");
                auto bytes = co_await channel.call(jobs::export_read{report->exports.front().digest});
                expect(bytes->parts.size() == 1u);
                auto& part = bytes->parts.at(0);
                expect(std::string(reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()) == "stable output");
            };
            exercise(run_operation);
        };
        "export failures preserve execution status and failed runs still export files"_test = [] {
            static constexpr auto run_operation = [](auto& channel, auto&) -> coroutine::task<> {
                content::manifest tree;
                co_await channel.call(jobs::sync_offer{"app", tree});
                jobs::build_request request{.project = "app",
                                            .snapshot = tree.id(),
                                            .steps = {{.name = "run", .argv = {test::shell(), "-c", "printf crash-report > crash.txt; exit 7"}}},
                                            .exports = {{"crash", "crash.txt"}, {"absent", "not-produced/app"}}};
                auto failed = co_await channel.call(request);
                expect(failed.has_value() >> fatal);
                expect(!failed->ok() && failed->steps.size() == 1u && failed->steps.front().exit_code == 7);
                expect(failed->export_error.empty() && failed->exports.size() == 1u);
                request.steps = {{.name = "run", .argv = {test::shell(), "-c", "printf completed"}}};
                request.exports = {{"missing", "absent.txt"}};
                auto missing = co_await channel.call(request);
                expect(missing.has_value() >> fatal);
                expect(missing->ok() && missing->steps.size() == 1u && missing->steps.front().output == "completed");
                expect(!missing->export_error.empty() && missing->exports.empty());
            };
            exercise(run_operation);
        };
        "a checkout writes what changed and keeps build outputs"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            content::manifest v1{{entry("bin/run.sh", "#!/bin/sh\n", true), entry("src/a.txt", "a1"), entry("src/old.txt", "old")}};

            auto plan = s.offer({"app", v1});
            expect(plan.snapshot == v1.id());
            expect(plan.missing.size() == 3u);
            expect(throws<std::runtime_error>([&] { s.checkout("app", plan.snapshot); }));
            expect(s.put({{blob("#!/bin/sh\n"), blob("a1"), blob("old")}}).stored == 3u);
            expect(s.offer({"app", v1}).missing.empty());

            auto dir = s.checkout("app", plan.snapshot);
            expect(dir == tmp.dir / "trees/app/default");
            expect(test::slurp(dir / "src/a.txt") == "a1");
            expect(durability::is_executable(dir / "bin/run.sh"));
            test::spit(dir / "build.out", "made by the build");

            content::manifest v2{{entry("bin/run.sh", "#!/bin/sh\n", true), entry("src/a.txt", "a2")}};
            auto plan2 = s.offer({"app", v2});
            expect(plan2.missing == std::vector<content::hash>{content::hash_of("a2")});
            s.put({{blob("a2")}});
            s.checkout("app", plan2.snapshot);
            expect(test::slurp(dir / "src/a.txt") == "a2");
            expect(!std::filesystem::exists(dir / "src/old.txt"));
            expect(std::filesystem::exists(dir / "build.out"));
        };

        "detached build trees can be reclaimed without changing a replacement tree"_test = [] {
            test::scratch tmp{"retired-tree"};
            replica::store store{tmp.dir};
            content::manifest source{{entry("a.txt", "source")}};
            auto offer = store.offer({"app", source});
            store.put({{blob("source")}});
            auto directory = store.checkout("app", offer.snapshot);
            std::filesystem::create_directories(directory / "build/readonly");
            test::spit(directory / "build/readonly/output", "old");
            std::filesystem::permissions(directory / "build/readonly", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
            auto retired = store.detach("app", "");
            expect(std::filesystem::exists(retired / "tree/build/readonly/output"));
            auto replacement = store.checkout("app", offer.snapshot);
            test::spit(replacement / "new-output", "new");
            replica::store::discard(retired);
            expect(!std::filesystem::exists(retired));
            expect(test::slurp(replacement / "a.txt") == "source");
            expect(test::slurp(replacement / "new-output") == "new");
            expect(!std::filesystem::exists(replacement / "build"));
        };

        "a file changed in the tree is restored, even at the same size"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            content::manifest m{{entry("a.txt", "good")}};
            auto plan = s.offer({"app", m});
            s.put({{blob("good")}});
            auto dir = s.checkout("app", plan.snapshot);
            std::this_thread::sleep_for(20ms);
            test::spit(dir / "a.txt", "evil");
            s.checkout("app", plan.snapshot);
            expect(test::slurp(dir / "a.txt") == "good");
        };

        "a large blob arrives in parts and is checked whole"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            std::string big(10000, 'x');
            big[1234] = 'y';
            content::manifest m{{entry("big.bin", big)}};
            auto plan = s.offer({"app", m});
            expect(s.put({{part(big, 0, 4000, big)}}).stored == 0u) << "not complete yet";
            expect(s.put({{part(big, 4000, 4000, big), part(big, 8000, 2000, big)}}).stored == 1u);
            expect(test::slurp(s.checkout("app", plan.snapshot) / "big.bin") == big);

            std::string other(10, 'a'), changed(10, 'b');
            auto first = part(other, 0, 5, other);
            auto second = part(changed, 5, 5, changed);
            second.digest = first.digest;
            s.put({{first}});
            try {
                s.put({{second}});
                expect(false) << "a mismatching blob was kept";
            } catch (const transport::coded_error& e) {
                expect(e.code == jobs::why::stale_blob);
            }
        };

        "the cache evicts unused blobs past its limit"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir, {.max_cache_bytes = 25}};
            std::string a(10, 'a'), b(10, 'b'), c(10, 'c');
            content::manifest m{{entry("c.txt", c)}};
            auto plan = s.offer({"app", m});
            s.put({{blob(a)}});
            std::this_thread::sleep_for(5ms);
            s.put({{blob(b)}});
            s.put({{blob(c)}});
            s.checkout("app", plan.snapshot);
            s.collect();
            expect(s.cache_bytes() <= 25u);
            auto missing = s.offer({"app", {{entry("a", a), entry("b", b), entry("c", c)}}}).missing;
            expect(std::ranges::contains(missing, content::hash_of(a))) << "the least recently used went first";
            expect(!std::ranges::contains(missing, content::hash_of(c))) << "a tree's blob stays";
        };

        "a compressed piece is stored as its plain bytes"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            std::string text(4096, 'p');
            auto plan = s.offer({"app", {{entry("a", text)}}});
            auto frame = nodes::archive::pack(text);
            jobs::source_part packed{
                .digest = content::hash_of(text),
                .total = text.size(),
                .bytes = {reinterpret_cast<const std::byte*>(frame.data()), reinterpret_cast<const std::byte*>(frame.data()) + frame.size()},
                .packed = true};
            expect(s.put({{packed}}).stored == 1u);
            expect(test::slurp(s.checkout("app", plan.snapshot) / "a") == text);
            auto damaged = packed;
            damaged.digest = content::hash_of("other");
            damaged.bytes.resize(damaged.bytes.size() / 2);
            expect(throws([&] { s.put({{damaged}}); })) << "a damaged frame is refused";
            auto inflated = packed;
            inflated.digest = content::hash_of("short");
            inflated.total = 100;
            expect(throws([&] { s.put({{inflated}}); })) << "a frame is not unpacked past what the piece may hold";
        };

        "the store refuses what does not check out"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            expect(throws<std::runtime_error>([&] { s.checkout("app", content::hash_of("nothing")); }));
            expect(throws<std::invalid_argument>([&] { s.offer({"../app", {}}); }));
            expect(throws<std::invalid_argument>([&] { s.offer({"app", {{entry("../x", "x")}}}); }));
        };

        "the cache keeps what an offer needs until its build checks out"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir, {.max_cache_bytes = 15}};
            std::string a(10, 'a'), b(10, 'b');
            auto first = s.offer({"app", {{entry("a", a)}}});
            s.put({{blob(a)}});
            auto second = s.offer({"app", {{entry("b", b)}}});
            s.put({{blob(b)}});
            s.collect();
            expect(s.offer({"app", {{entry("a", a)}}}).missing.empty()) << "pushed, not checked out yet: kept";
            expect(std::filesystem::exists(s.checkout("app", first.snapshot, "one") / "a"));
            expect(std::filesystem::exists(s.checkout("app", second.snapshot, "two") / "b"));
            for (int i = 0; i < 20; ++i)
                s.offer({"app", {{entry("c", std::string(i + 1, 'c'))}}});
            expect(std::filesystem::exists(s.checkout("app", first.snapshot, "one") / "a")) << "more than 8 offers since: still known";
        };

        "stale trees go, unless a build holds them; so do abandoned parts"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir, {.tree_ttl = 0h, .partial_ttl = 0h}};
            auto plan = s.offer({"app", {{entry("a", "x")}}});
            s.put({{blob("x")}});
            s.checkout("app", plan.snapshot, "busy");
            s.checkout("app", plan.snapshot, "idle");
            std::string big(100, 'z');
            auto part = blob(big);
            part.bytes.resize(10);
            s.put({{part}});
            std::this_thread::sleep_for(10ms);
            s.collect({"app/busy"});
            expect(std::filesystem::exists(s.tree("app/busy") / "a")) << "a build holds it";
            expect(!std::filesystem::exists(s.tree("app/idle")));
            expect(std::filesystem::is_empty(tmp.dir / "blobs" / "partial"));
        };

        "the cache lock and blob index survive reopening"_test = [] {
            test::scratch tmp{"cache-index"};
            {
                replica::store cache{tmp.dir};
                cache.put({{blob("retained")}});
                expect(throws<std::runtime_error>([&] { replica::store second{tmp.dir}; }));
            }
            replica::store cache{tmp.dir};
            expect(cache.cache_bytes() == 8u);
            auto offered = cache.offer({"app", {{entry("a", "retained")}}});
            expect(offered.missing.empty());
            expect(test::slurp(cache.checkout("app", offered.snapshot) / "a") == "retained");
        };
        "service dispatch syncs bytes, executes steps and stops at failure"_test = [] {
            static constexpr auto run_operation = [](auto& channel, const auto&) -> coroutine::task<> {
                content::manifest tree{{entry("input.txt", "hello")}};
                auto offer = co_await channel.call(jobs::sync_offer{"app", tree});
                expect(offer.has_value() >> fatal);
                expect(offer->missing == std::vector<content::hash>{content::hash_of("hello")});
                auto first = co_await channel.call(jobs::source_bytes{{part("hello", 0, 2, "hello")}});
                expect(first.has_value() && first->stored == 0u);
                auto last = co_await channel.call(jobs::source_bytes{{part("hello", 2, 3, "hello")}});
                expect(last.has_value() && last->stored == 1u);
                jobs::build_request request{.project = "app",
                                            .tree = "debug",
                                            .snapshot = tree.id(),
                                            .steps = {{.name = "first",
                                                       .argv = {test::shell(), "-c", "cat input.txt; echo $VALUE; echo output > artifact"},
                                                       .env = {{"VALUE", "step"}}},
                                                      {.name = "fail", .argv = {test::shell(), "-c", "exit 3"}},
                                                      {.name = "unreached", .argv = {test::shell(), "-c", "echo bad > unreached"}}},
                                            .env = {{"VALUE", "build"}}};
                auto report = co_await channel.call(request);
                expect(report.has_value() >> fatal);
                expect(report->worker == "worker-test" && report->steps.size() == 2u);
                expect(!report->ok() && report->steps[0].output == "hellostep\n" && report->steps[1].exit_code == 3);
                expect(test::slurp(std::filesystem::path{report->tree} / "artifact") == "output\n");
                expect(!std::filesystem::exists(std::filesystem::path{report->tree} / "unreached"));
                request.steps.resize(1);
                request.steps[0].argv = {test::shell(), "-c", "cat artifact; cat input.txt"};
                auto again = co_await channel.call(request);
                expect(again.has_value() && again->ok() && again->steps[0].output == "output\nhello");
                request.max_cpu_seconds = 1;
                request.output_limit = 4;
                request.steps[0].argv = {test::shell(), "-c", "touch completed"};
                auto limited = co_await channel.call(request);
                expect(limited.has_value() && limited->ok());
                expect(std::filesystem::exists(std::filesystem::path{report->tree} / "completed"));
                request.max_cpu_seconds.reset();
                request.timeout_s = 1;
                request.steps[0].argv = {test::shell(), "-c", "echo start; sleep 30"};
                auto timeout = co_await channel.call(request);
                expect(timeout.has_value() && timeout->steps[0].timed_out && !timeout->ok());
            };
            exercise(run_operation);
        };

        "a hot directory keeps its language process across source changes and builds"_test = [] {
            static constexpr auto run_operation = [](auto& channel, const auto& root, replica::service& service) -> coroutine::task<> {
                auto script = root / "language.py";
                test::spit(script, R"python(
import json, os, sys
with open(os.path.join(os.path.dirname(__file__), "starts"), "a", newline="\n") as f: f.write("start\n")
texts = {}
while True:
    length = 0
    while True:
        line = sys.stdin.buffer.readline()
        if not line: sys.exit(0)
        if not line.strip(): break
        if line.lower().startswith(b"content-length:"): length = int(line.split(b":")[1])
    m = json.loads(sys.stdin.buffer.read(length))
    method = m.get("method")
    p = m.get("params", {})
    if method == "textDocument/didOpen": texts[p["textDocument"]["uri"]] = p["textDocument"]["text"]
    if method == "textDocument/didChange": texts[p["textDocument"]["uri"]] = p["contentChanges"][0]["text"]
    if "id" not in m: continue
    result = None
    if method == "initialize": result = {"capabilities": {"diagnosticProvider": True, "definitionProvider": True}}
    if method == "textDocument/diagnostic":
        result = {"kind": "full", "items": [{"range": {"start":{"line":0,"character":0}, "end":{"line":0,"character":1}}, "message":texts[p["textDocument"]["uri"]]}]}
    body = json.dumps({"jsonrpc":"2.0", "id":m["id"], "result":result}).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()
)python");
                content::manifest first{{entry("a.py", "x = 1")}};
                expect((co_await channel.call(jobs::sync_offer{"app", first})).has_value());
                expect((co_await channel.call(jobs::source_bytes{{blob("x = 1")}})).has_value());
                jobs::build_request request{
                    .project = "app",
                    .tree = "hot-language",
                    .snapshot = first.id(),
                    .steps = {{.name = "prepare", .argv = {test::shell(), "-c", "printf preparation-output"}}},
                    .timeout_s = 5,
                    .output_limit = 4,
                    .survey = jobs::survey{.provider = "ty",
                                           .argv = {std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "/usr/bin/python3", script.string()}}};
                auto initial = co_await channel.call(request);
                expect((initial && initial->outline) >> fatal);
                expect(initial->steps.at(0).truncated && !initial->steps.at(0).log);
                expect(initial->outline->diagnostics.front().message == "x = 1");
                auto invalid = request;
                invalid.survey->query = {.action = "definitions", .path = "a.py", .symbol = "missing"};
                auto refused = co_await channel.call(invalid);
                expect(refused && refused->survey_error && refused->survey_error_code == "invalid_record");
                auto valid = co_await channel.call(request);
                expect(valid && valid->outline);
                expect(test::slurp(root / "starts") == "start\n") << "a selection error keeps the warm language process";
                auto build = request;
                build.survey.reset();
                build.output_limit = 1024;
                build.steps = {{.name = "build", .argv = {test::shell(), "-c", "echo built > artifact"}}};
                expect((co_await channel.call(build))->ok());
                content::manifest second{{entry("a.py", "x = 2")}};
                expect((co_await channel.call(jobs::sync_offer{"app", second})).has_value());
                expect((co_await channel.call(jobs::source_bytes{{blob("x = 2")}})).has_value());
                request.snapshot = second.id();
                auto updated = co_await channel.call(request);
                expect((updated && updated->outline) >> fatal);
                expect(updated->outline->diagnostics.front().message == "x = 2");
                expect(test::slurp(root / "starts") == "start\n") << "the language process survives the intervening build";
                co_await service.reclaim(0, true);
                auto recovered = co_await channel.call(request);
                expect((recovered && recovered->outline) >> fatal);
                expect(recovered->outline->diagnostics.front().message == "x = 2");
                expect(test::slurp(root / "starts") == "start\nstart\n") << "memory pressure retires the idle process and the next query recreates it";
                expect(std::filesystem::exists(std::filesystem::path{recovered->tree} / "artifact")) << "regular builds keep generated outputs";
                build.snapshot = second.id();
                build.fresh = true;
                build.steps = {{.name = "clean build", .argv = {test::shell(), "-c", "test ! -e artifact && cat a.py"}}};
                auto clean = co_await channel.call(build);
                expect((clean && clean->ok()) >> fatal);
                expect(clean->steps.front().output == "x = 2");
                auto restarted = co_await channel.call(request);
                expect((restarted && restarted->outline) >> fatal);
                expect(restarted->outline->diagnostics.front().message == "x = 2");
                expect(test::slurp(root / "starts") == "start\nstart\nstart\n") << "a clean build retires the directory's language process";
            };
            exercise(run_operation);
        };

        "language analysis shares the preparation execution deadline"_test = [] {
            static constexpr auto run_operation = [](auto& channel, const auto&) -> coroutine::task<> {
                content::manifest tree{{entry("a.cpp", "int a;")}};
                auto offered = co_await channel.call(jobs::sync_offer{"app", tree});
                expect(offered.has_value() >> fatal);
                expect((co_await channel.call(jobs::source_bytes{{blob("int a;")}})).has_value());
                jobs::build_request request{.project = "app",
                                            .tree = "analysis-deadline",
                                            .snapshot = tree.id(),
                                            .steps = {{.name = "prepare", .argv = {test::shell(), "-c", "sleep 0.7"}}},
                                            .timeout_s = 1,
                                            .survey = jobs::survey{.provider = "clice", .argv = {test::shell(), "-c", "sleep 10"}, .seconds = 30}};
                auto started = std::chrono::steady_clock::now();
                auto report = co_await channel.call(request);
                expect(report.has_value() >> fatal);
                expect(!report->ok() && report->steps.back().name == "analysis" && report->steps.back().timed_out);
                expect(std::chrono::steady_clock::now() - started < 3s) << "the survey receives only the remaining execution time";
            };
            exercise(run_operation);
        };

        "service rejects a damaged push without caching its contents"_test = [] {
            static constexpr auto run_operation = [](auto& channel, const auto&) -> coroutine::task<> {
                auto corrupt = blob("bad");
                corrupt.digest = content::hash_of("yes");
                auto pushed = co_await channel.call(jobs::source_bytes{{corrupt}});
                expect(!pushed.has_value());
                auto offered = co_await channel.call(jobs::sync_offer{"app", {{entry("a", "yes")}}});
                expect(offered.has_value() && offered->missing.size() == 1u);
            };
            exercise(run_operation);
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"cache">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
