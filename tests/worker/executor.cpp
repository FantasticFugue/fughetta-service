import std;
import toolchain;
import boost.ut;
import coroutine;
import native;
import content;
import protocol;
import replica;
import executor;
import runner;
import support;
import launch;
import archive;
import durability;
import host;
import dependencies;
import constants;
import reflection;
import knowledge;
import delivery;
import projectmodel;
import process;

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

    struct discard_unit {
        void operator()(language::evaluated_unit) const {}
    };
    struct reject_reuse {
        auto operator()(jobs::reuse_request) const -> jobs::reuse_reply { return {}; }
    };
    struct absent_shard {
        auto operator()(jobs::shard_read) const -> jobs::shard_piece { throw std::runtime_error("unexpected shard input"); }
    };
    struct absent_model {
        auto operator()(jobs::model_read) const -> jobs::source_bytes { throw std::runtime_error("unexpected model input"); }
    };
    template <class Unit = discard_unit, class Reuse = reject_reuse, class Shard = absent_shard, class Model = absent_model> struct record_checks {
        Unit unit{};
        Reuse reuse{};
        Shard shard{};
        Model model{};
    };
    template <class Channel, jobs::request Request, class Checks>
    auto analysis_call(Channel& channel, ctx io, const Request& request, Checks checks, jobs::progress_log* progress = nullptr)
        -> coroutine::task<transport::call_result<jobs::reply_t<Request>>> {
        auto inputs = std::make_shared<jobs::analysis_exchange>(io, io);
        auto serve = [&]() -> coroutine::task<> {
            while (auto item = co_await inputs->next()) {
                auto release = jobs::analysis_exchange::completion{item};
                try {
                    if (auto* unit = std::get_if<language::evaluated_unit>(&item->data)) {
                        checks.unit(std::move(*unit));
                        item->answer(std::monostate{});
                    } else if (auto* reuse = std::get_if<jobs::reuse_request>(&item->data))
                        item->answer(checks.reuse(std::move(*reuse)));
                    else if (auto* shard = std::get_if<jobs::shard_read>(&item->data))
                        item->answer(checks.shard(std::move(*shard)));
                    else
                        item->answer(checks.model(std::move(std::get<jobs::model_read>(item->data))));
                } catch (...) {
                    item->fail(std::current_exception());
                }
            }
        };
        auto run = [&](coroutine::scope& tasks) -> coroutine::task<transport::call_result<jobs::reply_t<Request>>> {
            auto close = jobs::analysis_exchange::lifetime{inputs};
            tasks.spawn(serve());
            co_return co_await channel.call(request, progress, inputs);
        };
        co_return co_await coroutine::with_scope(std::move(run));
    }

    template <class F>
    void exercise(F work, std::optional<std::filesystem::path> dependency_cache = {}, std::optional<std::filesystem::path> dependency_upstream = {},
                  std::optional<std::filesystem::path> dependency_publish = {}, std::optional<builds::dependency_recipe> recipe = {},
                  std::optional<std::uint64_t> memory_bytes = {}) {
        test::scratch tmp{"executor"};
        cores::io_context<native::context> rt{2};
        execution::thread_pool disk{2};
        replica::service service{rt,
                                 {.root = tmp.dir / "cache",
                                  .worker = "worker-test",
                                  .env = {{"VALUE", "worker"}},
                                  .inherit_env = false,
                                  .memory_bytes = memory_bytes,
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
                    static constexpr auto run_child = [](ctx& io, auto& listener, auto& service) -> coroutine::task<> {
                        auto [socket, endpoint] = co_await net::async_accept(listener);
                        transport::channel<native::socket> channel{socket};
                        transport::dispatcher<native::socket> dispatch;
                        service.install(dispatch, 0);
                        auto serve = [&](coroutine::scope& requests) -> coroutine::task<> {
                            co_await channel.pump([&](const transport::frame& frame) { requests.spawn(dispatch.handle(channel, frame)); });
                        };
                        co_await coroutine::with_scope(serve);
                    };
                    tasks.spawn(run_child(io, listener, service));
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
        "a cross-compiling publisher requires both target and host selections"_test = [] {
            test::scratch tmp{"publisher-triplets"};
            auto recipe = recipe_script(tmp.dir, "printf '%s\\n' \"$@\"");
            recipe.triplets = {"arm64-android", "x64-linux"};
            auto run = [](auto& channel, auto&) -> coroutine::task<> {
                expect(!(co_await channel.call(jobs::dependency_fill{})).has_value());
                expect(!(co_await channel.call(jobs::dependency_fill{.triplet = "arm64-android"})).has_value());
                expect(!(co_await channel.call(jobs::dependency_fill{.host_triplet = "x64-linux"})).has_value());
                auto reply = co_await channel.call(jobs::dependency_fill{.triplet = "arm64-android", .host_triplet = "x64-linux"});
                expect(reply.has_value() >> fatal);
                expect(reply->ok() && reply->steps.front().output.contains("--triplet=arm64-android") &&
                       reply->steps.front().output.contains("--host-triplet=x64-linux"));
            };
            exercise(run, tmp.dir / "private", {}, tmp.dir / "published", recipe);
            recipe.triplets = {"x64-linux"};
            auto single = [](auto& channel, auto&) -> coroutine::task<> {
                auto reply = co_await channel.call(jobs::dependency_fill{});
                expect(reply.has_value() >> fatal);
                expect(reply->ok() && reply->steps.front().output.contains("--triplet=x64-linux") &&
                       reply->steps.front().output.contains("--host-triplet=x64-linux"));
            };
            exercise(single, tmp.dir / "single-private", {}, tmp.dir / "single-published", recipe);
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
                // Both declared tasks share one managed process. A failed batch
                // keeps its real task names and full log instead of inventing
                // independent task outcomes or losing the earlier task's output.
                const std::string command = "for task do printf '%s\\n' \"$task\"; test \"$task\" != failed || exit 7; done";
                auto batched = co_await channel.call(jobs::build_request{
                    .project = "app",
                    .snapshot = tree.id(),
                    .steps = {{.name = "first", .argv = {test::shell(), "-c", command, "batch", "first"}, .preparation = true, .batch_prefix = 4},
                              {.name = "failed", .argv = {test::shell(), "-c", command, "batch", "failed"}, .preparation = true, .batch_prefix = 4}},
                    .output_limit = 4});
                expect((batched && batched->steps.size() == 1u) >> fatal);
                const auto& batch = batched->steps.front();
                expect(!batched->ok() && batch.exit_code == 7 && batch.name == "batch");
                expect(batch.tasks == std::vector<std::string>{"first", "failed"});
                expect(batch.log.has_value() && batch.truncated);
                auto bytes = co_await channel.call(jobs::export_read{batch.log.value().digest});
                expect(bytes.has_value() >> fatal);
                const auto& part = bytes->parts.at(0);
                expect(std::string(reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()) == "first\nfailed\n");
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

        "a model survives its producer and installs missing objects once on another worker"_test = [] {
            test::scratch gateway{"model-gateway"};
            const auto hash = content::hash_of("library");
            projectmodel::document model{
                .producer = "test",
                .producer_version = "current",
                .units = {{.id = "main",
                           .project = "app",
                           .language = "kotlin",
                           .directory = ".",
                           .references = {projectmodel::artifact_reference{.object = {hash.str(), "library.jar"}, .role = "classpath"}}}}};
            const auto text = rfl::json::write(model);
            builds::exported_file exported;
            auto produce = [&](auto& channel, auto& root) -> coroutine::task<> {
                content::manifest tree{{entry("model.json", text)}};
                co_await channel.call(jobs::sync_offer{"app", tree});
                co_await channel.call(jobs::source_bytes{{blob(text)}});
                jobs::build_request request{
                    .project = "app",
                    .snapshot = tree.id(),
                    .steps = {{.name = "model",
                               .argv = {test::shell(), "-c", "mkdir -p \"$FUGHETTA_STAGING\"; printf library > \"$FUGHETTA_STAGING/" + hash.str() + "\""},
                               .outputs = {{"model", "project_model", "model.json"}},
                               .preparation = true}}};
                auto report = co_await channel.call(request);
                expect(report.has_value() >> fatal);
                expect((report->models.size() == 1_u) >> fatal);
                exported = report->models.front();
                std::vector<builds::exported_object> objects = exported.objects;
                objects.push_back({exported.digest, exported.size});
                for (const auto& object : objects) {
                    auto reply = co_await channel.call(jobs::export_read{object.digest});
                    expect(reply.has_value() >> fatal);
                    delivery::accept(gateway.dir, reply->parts.at(0));
                }
                delivery::retain_model(gateway.dir, exported.digest);
                expect(!std::filesystem::exists(root / "cache/artifacts/objects"));
            };
            exercise(produce);
            auto consume = [&](auto& channel, auto& root, replica::service&, ctx& io) -> coroutine::task<> {
                content::manifest tree{{entry("model.json", text)}};
                co_await channel.call(jobs::sync_offer{"app", tree});
                co_await channel.call(jobs::source_bytes{{blob(text)}});
                std::size_t reads = 0;
                auto fetch = [&](jobs::model_read request) -> jobs::source_bytes {
                    expect(request.model == exported.digest && request.parent != 0_u);
                    ++reads;
                    return delivery::read(gateway.dir, request.object);
                };
                jobs::build_request request{.project = "app",
                                            .snapshot = tree.id(),
                                            .steps = {{.name = "consumer",
                                                       .argv = {test::shell(), "-c", "test -z \"$FUGHETTA_STAGING$FUGHETTA_SDKS\""},
                                                       .inputs = {{"model", "project_model", "model.json"}}}},
                                            .measure = {{"model", "project_model", "model.json"}}};
                for (int attempt = 0; attempt < 2; ++attempt) {
                    auto report = co_await analysis_call(channel, io, request, record_checks{.model = fetch});
                    expect(report.has_value() >> fatal);
                    expect(report->ok() && report->measured.size() == 1_u);
                }
                expect(reads == 1_u);
                expect(test::slurp(root / "cache/exports" / hash.view()) == "library");
                expect(delivery::references(root / "cache/exports", exported.digest).size() == 1_u);
            };
            exercise(consume);
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

        "generated contributions survive restart, retract per producer and recover interrupted writes"_test = [] {
            test::scratch tmp{"generated-ownership"};
            content::manifest source{{entry("source.txt", "source")}};
            const builds::generation first{.producer = "native/first", .outputs = {{"headers", "directory", "generated", "directory"}}};
            auto second = first;
            second.producer = "native/second";
            {
                replica::store store{tmp.dir};
                store.offer({"app", source});
                store.put({{blob("source")}});
                auto dir = store.checkout("app", source.id());
                store.begin_generation("app", "", first);
                test::spit(dir / "generated/old.hpp", "old");
                store.commit_generation("app", "", first, {"generated/old.hpp"});
                store.begin_generation("app", "", second);
                test::spit(dir / "generated/retained.hpp", "retained");
                store.commit_generation("app", "", second, {"generated/retained.hpp"});
            }
            {
                replica::store store{tmp.dir};
                store.offer({"app", source});
                auto dir = store.checkout("app", source.id());
                expect(store.generated_inputs("app", "").size() == 2u);
                store.begin_generation("app", "", first);
                store.commit_generation("app", "", first, {});
                expect(!std::filesystem::exists(dir / "generated/old.hpp") && test::slurp(dir / "generated/retained.hpp") == "retained");
                expect(store.generated_inputs("app", "").size() == 1u);
                store.begin_generation("app", "", first);
                test::spit(dir / "generated/old.hpp", "returned");
                store.commit_generation("app", "", first, {"generated/old.hpp"});
                store.reconcile_generations("app", "", {second.producer});
                expect(!std::filesystem::exists(dir / "generated/old.hpp") && test::slurp(dir / "generated/retained.hpp") == "retained" &&
                       store.generated_inputs("app", "").size() == 1u);

                store.begin_generation("app", "", first);
                test::spit(dir / "generated/partial.hpp", "incomplete");
            }
            {
                replica::store store{tmp.dir};
                store.offer({"app", source});
                auto dir = store.checkout("app", source.id());
                expect(test::slurp(dir / "source.txt") == "source" && !std::filesystem::exists(dir / "generated") && store.generated_inputs("app", "").empty());
            }
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
            expect(s.slots().size() == 1u && s.slots().front().project == "app" && s.slots().front().tree == "busy");
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

        "a hot directory keeps stage sessions across source changes and retires them together"_test = [] {
            static constexpr auto run_operation = [](auto& channel, const auto& root, replica::service& service, ctx& io) -> coroutine::task<> {
                const std::string python = std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "/usr/bin/python3";
                toolchain::identity runtime{.id = "lsp-runtime",
                                            .execution = "lsp-runtime-object",
                                            .platform = "platform-object",
                                            .executables = {{python, "python-object"}, {"/tools/unrelated", "unrelated-object"}}};
                service.set_runtime(runtime);
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
                jobs::analysis_request request{
                    .preparation = {.project = "app",
                                    .tree = "hot-language",
                                    .snapshot = first.id(),
                                    .steps = {{.name = "prepare", .argv = {test::shell(), "-c", "printf preparation-output"}}},
                                    .preparation_tasks = {{.name = "prepare", .first_step = 0, .step_count = 1}},
                                    .timeout_s = 5,
                                    .output_limit = 4,
                                    .max_memory_bytes = 64ull << 20},
                    .analysis = jobs::analysis_plan{.provider = "ty",
                                                    .argv = {std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "/usr/bin/python3", script.string()},
                                                    .execution = {"language-request", "language-computation", "python"},
                                                    .slot_id = "app/hot-language",
                                                    .demand = {.facts = {std::string{"diagnostics"}}}}};
                std::vector<language::evaluated_unit> received;
                auto receive = [&](language::evaluated_unit unit) -> void {
                    received.push_back(std::move(unit));
                    return;
                };
                auto initial = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect((initial && initial->analyzed && initial->analysis_session) >> fatal);
                expect(initial->analysis_session->epoch == 1u && initial->analysis_session->snapshot == first.id().str() &&
                       initial->analysis_session->phase == "ready");
                expect(initial->steps.at(0).truncated && !initial->steps.at(0).log);
                expect(received.back().shards.front().diagnostics.front().message == "x = 1");
                expect(received.back().shards.front().unit.coverage.available.front().facts == std::vector<semantic::fact_kind>{std::string{"diagnostics"}});
                auto invalid = request;
                invalid.analysis.query = {.action = std::string{"definitions"},
                                          .selector = semantic::selection{.name = semantic::named{.text = "missing", .path = "a.py"}}};
                auto refused = co_await analysis_call(channel, io, invalid, record_checks{.unit = receive});
                expect(refused && refused->analysis_error && refused->analysis_error_code == "invalid_record");
                auto valid = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect((valid && valid->analyzed && valid->analysis_session) >> fatal);
                expect(valid->analysis_session->id == initial->analysis_session->id && valid->analysis_session->epoch == initial->analysis_session->epoch);
                expect(test::slurp(root / "starts") == "start\n") << "a selection error keeps the warm language process";
                auto sibling = request;
                sibling.analysis.execution.stage_id = "review";
                sibling.analysis.options = "python-review";
                auto review = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive});
                expect((review && review->analyzed && review->analysis_session) >> fatal);
                expect(review->analysis_session->id != initial->analysis_session->id && review->analysis_session->slot == initial->analysis_session->slot);
                expect(review->steps.empty()) << "a sibling stage reuses completed preparation tasks";
                auto build = request.preparation;
                build.preparation_tasks.clear();
                build.output_limit = 1024;
                build.steps = {{.name = "build", .argv = {test::shell(), "-c", "echo built > artifact"}}};
                expect((co_await channel.call(build))->ok());
                auto rebuilt = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect(rebuilt && rebuilt->steps.size() == 1u) << "an intervening build invalidates preparation receipts";
                content::manifest second{{entry("a.py", "x = 2")}};
                expect((co_await channel.call(jobs::sync_offer{"app", second})).has_value());
                expect((co_await channel.call(jobs::source_bytes{{blob("x = 2")}})).has_value());
                request.preparation.snapshot = second.id();
                auto updated = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect((updated && updated->analyzed && updated->analysis_session) >> fatal);
                expect(updated->analysis_session->id == initial->analysis_session->id && updated->analysis_session->epoch == 2u &&
                       updated->analysis_session->snapshot == second.id().str());
                expect(received.back().shards.front().diagnostics.front().message == "x = 2");
                auto previous = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive});
                expect((previous && previous->analysis_session) >> fatal);
                expect(previous->analysis_session->id == review->analysis_session->id && previous->analysis_session->epoch == 1u);
                expect(received.back().shards.front().diagnostics.front().message == "x = 1");
                sibling.preparation.snapshot = second.id();
                auto reviewed = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive});
                expect((reviewed && reviewed->analysis_session) >> fatal);
                expect(reviewed->analysis_session->id == review->analysis_session->id && reviewed->analysis_session->epoch == 2u);
                auto returned = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect((returned && returned->analysis_session) >> fatal);
                expect(returned->analysis_session->id == updated->analysis_session->id && returned->analysis_session->epoch == 2u);
                expect(returned->steps.empty() && updated->steps.size() == 1u) << "preparation receipts follow the installed snapshot";
                expect(received.back().shards.front().diagnostics.front().message == "x = 2");
                expect(test::slurp(root / "starts") == "start\nstart\n") << "stage switches and a regular build preserve both language processes";
                auto warm = co_await service.status(0);
                expect(warm.slots.size() == 1u && warm.slots.front().sessions.size() == 2u && !warm.slots.front().busy &&
                       std::ranges::all_of(warm.slots.front().sessions,
                                           [&](const auto& session) { return session.snapshot == second.id().str() && session.epoch == 2u; }));
                expect(warm.memory.limit.has_value() && warm.memory.resident.has_value() && warm.memory.baseline.has_value());
                expect(std::ranges::all_of(warm.slots.front().sessions, [](const auto& session) {
                    return session.usage && session.usage->resident_bytes > 0 && session.usage->processes > 0 &&
                           session.high_water_bytes >= session.usage->resident_bytes && session.rebuild_seconds > 0;
                })) << "warm sessions report their real child group resident memory";
                co_await service.reclaim(0, true);
                auto reclaimed = co_await service.status(0);
                expect(reclaimed.slots.size() == 1u && reclaimed.slots.front().sessions.empty())
                    << "reclamation retains the directory but withdraws its provider sessions";
                auto recovered = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect((recovered && recovered->analyzed && recovered->analysis_session) >> fatal);
                expect(recovered->analysis_session->id != updated->analysis_session->id && recovered->analysis_session->epoch == 1u);
                expect(recovered->steps.size() == 1u) << "reclamation discards preparation receipts";
                expect(received.back().shards.front().diagnostics.front().message == "x = 2");
                auto recovered_review = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive});
                expect((recovered_review && recovered_review->analysis_session) >> fatal);
                expect(recovered_review->analysis_session->id != reviewed->analysis_session->id && recovered_review->analysis_session->epoch == 1u);
                expect(test::slurp(root / "starts") == "start\nstart\nstart\nstart\n") << "memory pressure retires every idle stage session";
                expect(std::filesystem::exists(std::filesystem::path{recovered->tree} / "artifact")) << "regular builds keep generated outputs";
                build.snapshot = second.id();
                build.fresh = true;
                build.steps = {{.name = "clean build", .argv = {test::shell(), "-c", "test ! -e artifact && cat a.py"}}};
                auto clean = co_await channel.call(build);
                expect((clean && clean->ok()) >> fatal);
                expect(clean->steps.front().output == "x = 2");
                auto restarted = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect((restarted && restarted->analyzed && restarted->analysis_session) >> fatal);
                expect(restarted->analysis_session->id != recovered->analysis_session->id && restarted->analysis_session->epoch == 1u);
                expect(received.back().shards.front().diagnostics.front().message == "x = 2");
                auto restarted_review = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive});
                expect((restarted_review && restarted_review->analysis_session) >> fatal);
                expect(restarted_review->analysis_session->id != recovered_review->analysis_session->id && restarted_review->analysis_session->epoch == 1u);
                expect(test::slurp(root / "starts") == "start\nstart\nstart\nstart\nstart\nstart\n")
                    << "a clean build retires all of the directory's stage sessions";
                sibling.analysis.execution.computation_id = "next-computation";
                auto next = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive});
                expect(next && next->steps.size() == 1u && next->analysis_session->id == restarted_review->analysis_session->id)
                    << "a new computation prepares again while retaining the stage session";

                const auto& evaluated = received.back();
                const semantic::recipe recipe{evaluated.request.unit, evaluated.session.options};
                auto saved_unit = evaluated.shards.front().unit;
                saved_unit.direct_inputs = evaluated.session.inputs;
                saved_unit.coverage = {.requested = sibling.analysis.demand, .available = {sibling.analysis.demand}};
                sibling.analysis.candidates = {{.id = "saved-python-unit", .recipe = recipe, .units = {saved_unit}, .nodes = 1}};
                auto validate = [&](jobs::reuse_request input) -> jobs::reuse_reply {
                    semantic::source_vector sources;
                    for (const auto& file : second.files)
                        sources["app"].emplace(file.path, file.digest.str());
                    reuse::channel_values channels;
                    const auto validators =
                        language::providers::dispatch(input.session.provider, []<class Provider> { return language::input_validators<Provider>(); });
                    reuse::environment current{sources, input.inputs, channels, "app", validators};
                    jobs::reuse_reply reply;
                    for (const auto& candidate : sibling.analysis.candidates)
                        if (reuse::accept(candidate, input.session, sibling.analysis.demand, current))
                            reply.accepted.push_back({candidate.id, semantic::recipe_identifier(candidate.recipe)});
                    return reply;
                };
                runtime.id = "lsp-unrelated-changed";
                runtime.executables["/tools/unrelated"] = "unrelated-changed";
                service.set_runtime(runtime);
                auto reused = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive, .reuse = validate});
                expect(reused && !reused->analysis_error && received.back().reused == std::optional<std::string>{"saved-python-unit"})
                    << "unaffected LSP candidates survive changes to unused tools";
                runtime.id = "lsp-python-changed";
                runtime.executables[python] = "python-changed";
                service.set_runtime(runtime);
                auto reevaluated = co_await analysis_call(channel, io, sibling, record_checks{.unit = receive, .reuse = validate});
                expect(reevaluated && !reevaluated->analysis_error && !received.back().reused && received.back().shards.front().unit.cacheable)
                    << "the selected executable invalidates the other LSP candidates";
                auto uncapped = sibling;
                uncapped.preparation.max_memory_bytes.reset();
                auto admitted = co_await analysis_call(channel, io, uncapped, record_checks{.unit = receive});
                expect(admitted && admitted->analyzed && !admitted->analysis_error) << "an undeclared task allowance uses the whole available worker budget";
                auto exclusive = co_await service.status(0);
                expect(exclusive.slots.front().sessions.size() == 1u && exclusive.memory.reserved == 0_u)
                    << "full-budget admission reclaims idle siblings and releases its reservation on completion";
            };
            auto usage = native_process::self_usage();
            expect(usage.has_value() >> fatal);
            exercise(run_operation, {}, {}, {}, {}, usage->resident_bytes + (256ull << 20));
        };

        "semantic evaluation accounts exported units and recovers after exceeding its budget"_test = [] {
            static constexpr auto run_operation = [](auto& channel, const auto& root, replica::service& service, ctx& io) -> coroutine::task<> {
                const std::string python = std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "/usr/bin/python3";
                toolchain::identity runtime{
                    .id = "worker-runtime",
                    .execution = "runtime-object",
                    .platform = "platform-object",
                    .executables = {{python, "python-object"}, {"/tools/compiler", "compiler-object"}, {"/tools/unrelated", "unrelated-object"}},
                    .inputs = {{.name = "sdk/native", .object = "sdk-object", .roots = {(root / "sdk").generic_string()}},
                               {.name = "sdk/headers", .object = "headers-object", .roots = {(root / "sdk/include").generic_string()}}}};
                const auto execution = toolchain::select(runtime, {{"main", python}, {"command:index", python}, {"command:query", python}},
                                                         {{"VALUE", "worker"}, {"OMP_NUM_THREADS", "1"}});
                service.set_runtime(runtime);
                test::spit(root / "configurations", R"({"a.cpp":"command-a.cpp","b.cpp":"command-b.cpp"})");
                auto script = root / "semantic.py";
                test::spit(script, R"python(
import json, os, sys, time
if os.name == "posix":
    import resource
    assert resource.getrlimit(resource.RLIMIT_CPU)[0] == 20
    assert resource.getrlimit(resource.RLIMIT_AS)[0] == 512 * 1024 * 1024
def configuration(path):
    with open("commands.json") as source: return next(entry["configuration"] for entry in json.load(source) if entry["file"] == path)
def publication(path):
    sdk = os.path.join(os.path.dirname(__file__), "sdk", "include")
    return {"configurations":{"commands":{path:configuration(path)},"complete":True}, "tools":{"executables":["/tools/compiler"],"complete":True}, "column_encoding":"utf8", "complete":True,
                  "dependencies":[path, "shared.inc", os.path.join(sdk, "header.h")] + (["external/header.h"] if path == "a.cpp" else []),
                  "absent":[os.path.join(sdk, "optional.h")],
                  "directories":[],
                  "symbols":[], "occurrences":[], "scopes":[], "relations":[], "diagnostics":[], "identities":[],
                  "types":[{"id":"int", "kind":"primitive", "name":"int" + "é" * 600000, "arguments":[],
                            "parameters":[], "qualifiers":[], "type_parameters":[], "template_arguments":[]}]}
if len(sys.argv) > 1:
    with open(os.path.join(os.path.dirname(__file__), "commands"), "a") as log: log.write(" ".join(sys.argv[1:3]) + "\n")
    if sys.argv[1] == "index": result = {"failed":[]}
    elif sys.argv[2] == "project-files": result = {"result":{"files":[{"path":"a.cpp","translationUnit":True,"translationUnits":["a.cpp"],"configuration":{"a.cpp":configuration("a.cpp")}},{"path":"b.cpp","translationUnit":False,"translationUnits":[],"configuration":{"b.cpp":configuration("b.cpp")}}]},"stale":[]}
    elif sys.argv[2] == "semantic-contributions": result = {"result":[{"path":"a.cpp","contributions":[{"key":"records","kind":"records","file":"a.cpp","value":"published-a"}]}],"stale":[]}
    else: result = {"result":publication(sys.argv[3]),"stale":[]}
    print(json.dumps(result))
    sys.exit(0)
with open(os.path.join(os.path.dirname(__file__), "semantic-starts"), "a", newline="\n") as f: f.write("start\n")
while True:
    length = 0
    while True:
        line = sys.stdin.buffer.readline()
        if not line: sys.exit(0)
        if not line.strip(): break
        if line.lower().startswith(b"content-length:"): length = int(line.split(b":")[1])
    m = json.loads(sys.stdin.buffer.read(length))
    if "id" not in m: continue
    result = None
    if m.get("method") == "initialize": result = {"capabilities": {"documentSymbolProvider": True}}
    if m.get("method") == "clice/semanticExport":
        if m["params"]["uri"].endswith("b.cpp") and os.path.exists(os.path.join(os.path.dirname(__file__), "delay")): time.sleep(10)
        result = publication(m["params"]["uri"].rsplit("/",1)[-1])
    body = json.dumps({"jsonrpc":"2.0", "id":m["id"], "result":result}).replace("column_encoding", "columnEncoding").replace("template_arguments", "templateArguments").replace("type_parameters", "typeParameters").encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()
)python");
                content::manifest tree{{entry("a.cpp", "int a;"), entry("b.cpp", "int b;"), entry("shared.inc", "// shared")}};
                expect((co_await channel.call(jobs::sync_offer{"app", tree})).has_value() >> fatal);
                expect((co_await channel.call(jobs::source_bytes{{blob("int a;"), blob("int b;"), blob("// shared")}})).has_value() >> fatal);

                jobs::analysis_request request{
                    .preparation =
                        {.project = "app",
                         .tree = "semantic-budget",
                         .snapshot = tree.id(),
                         .steps = {{.name = "prepare",
                                    .argv = {python, "-c",
                                             "import json,sys; data=json.load(open(sys.argv[1])); json.dump([dict(file=path,directory='.',command='c++ "
                                             "'+value,configuration=value) for path,value in data.items()],open('commands.json','w'))",
                                             (root / "configurations").string()}}},
                         .timeout_s = 5,
                         .max_memory_bytes = 512u * 1024 * 1024,
                         .max_cpu_seconds = 20,
                         .measure = {{"commands", "compilation_database", "commands.json"}}},
                    .analysis =
                        jobs::analysis_plan{.provider = "clice",
                                            .options = "cpp",
                                            .artifacts = {{"commands", "compilation_database", "commands.json"}},
                                            .argv = {std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "/usr/bin/python3", script.string()},
                                            .budget = {.units = 2, .nodes = 1},
                                            .execution = {"semantic-request", "semantic-computation", "cpp"},
                                            .slot_id = "app/semantic-budget",
                                            .demand = {.facts = {std::string{"types"}}},
                                            .commands = {{"index", {python, script.string(), "index"}}, {"query", {python, script.string(), "query"}}}}};
                request.analysis.initialization = *rfl::json::read<rfl::Generic>(R"({"project":{}})");
                semantic::store stored{root / "semantic"};
                std::vector<semantic::view_id> committed;
                auto receive = [&](language::evaluated_unit unit) -> void {
                    expect(unit.session.snapshot == tree.id().str() && unit.session.options == "cpp" && unit.request.execution == request.analysis.execution &&
                           unit.request.slot_id == unit.session.slot && unit.request.demand == request.analysis.demand &&
                           std::ranges::equal(unit.request.inputs, unit.session.inputs));
                    const auto& contribution = unit.shards.front().unit;
                    const auto key = rfl::json::read<language::cpp::contribution_key>(unit.request.unit.name).value();
                    const auto& path = key.translation_unit;
                    expect(unit.request.unit.kind == semantic::unit_kind::group);
                    const auto source = std::ranges::find(tree.files, path, &content::entry::path);
                    std::vector<semantic::input_object> inputs{{semantic::input_kind::runtime, "toolchain", execution},
                                                               {semantic::input_kind::provider, path, "command-" + path, "clice/configuration"},
                                                               {semantic::input_kind::tool, "/tools/compiler", "compiler-object"}};
                    if (key.contribution == "immediate")
                        inputs.insert(inputs.begin(), {semantic::input_kind::file, path, source->digest.str()});
                    else
                        inputs.push_back({semantic::input_kind::provider, unit.request.unit.name, "published-a", "clice/contribution"});
                    expect(std::ranges::equal(contribution.direct_inputs, inputs));
                    const auto& reads = contribution.observed_dependencies;
                    expect(reads.size() == (path == "a.cpp" ? 5u : 4u) >> fatal);
                    expect(reads[0].kind == semantic::dependency_kind::source && reads[0].value == source->digest.str() &&
                           reads[1].kind == semantic::dependency_kind::source && reads[1].value == tree.files.back().digest.str());
                    expect(reads[2].kind == semantic::dependency_kind::artifact && reads[2].value == "headers-object" && reads[2].scope == "sdk/headers" &&
                           reads[2].key == "header.h");
                    expect(reads.back().kind == semantic::dependency_kind::negative_lookup && reads.back().value == "headers-object" &&
                           reads.back().scope == "sdk/headers" && reads.back().key == "optional.h");
                    expect(unit.shards.front().unit.coverage.missing.empty() && unit.shards.front().unit.cacheable == (path == "b.cpp"))
                        << "an unbound external read keeps its facts but prevents result reuse";
                    expect(rfl::json::write(unit).size() > constants::semantic_batch_bytes) << "one unit spans multiple bounded frames";
                    semantic::view selected{.snapshots = {{"app", "work", "v1", tree.id().str()}}, .configuration = "cpp"};
                    committed.push_back(stored.publish_records(std::move(selected), std::move(unit.shards), {}));
                    return;
                };
                auto limited = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                if (!limited)
                    transport::raise_call_error(std::move(limited.error()));
                expect(!limited->analysis_error && limited->analyzed && !limited->missing.empty() && limited->missing.size() == 1u &&
                       limited->missing.front().reason == "semantic node budget exhausted")
                    << rfl::json::write(*limited);
                expect(limited->committed_units == 1u && committed.size() == 1u) << "node exhaustion publishes the completed first unit and defers the second";
                request.analysis.budget.nodes = 2;
                jobs::progress_log progress;
                auto recovered = co_await analysis_call(channel, io, request, record_checks{.unit = receive}, &progress);
                std::size_t seen = 0;
                auto events = progress.since(seen);
                expect(!events.empty() && events.back().execution == request.analysis.execution && events.back().stage == "analysis");
                expect((recovered && recovered->analyzed && !recovered->analysis_error) >> fatal) << (recovered ? rfl::json::write(*recovered) : "no reply");
                expect(recovered->committed_units == 2u && committed.size() == 3u);
                for (const auto& id : committed) {
                    auto view = stored.load(id);
                    expect(view->pieces.size() == 1u && view->pieces.front()->types().front().kind == "primitive");
                }
                expect(test::slurp(root / "semantic-starts") == "start\n") << "a completed response exceeding the output budget keeps its session";
                expect(test::slurp(root / "commands") ==
                       "index --workspace\nquery project-files\nquery semantic-contributions\nquery semantic-export\nquery semantic-export\n")
                    << "one index sweep serves repeated evaluations of the installed epoch";
                request.analysis.budget.units = 1;
                auto partial = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect(partial && !partial->analysis_error && partial->analyzed && !partial->missing.empty() && partial->committed_units == 1u &&
                       partial->missing.size() == 1u && partial->missing.front().deferred &&
                       partial->missing.front().units ==
                           std::vector<std::string>{semantic::unit_identifier({"app", "cpp", "clice", "evaluate", semantic::unit_kind::group,
                                                                               rfl::json::write(language::cpp::contribution_key{"b.cpp", "immediate"})})});
                request.analysis.budget.units = 2;
                test::spit(root / "delay", "delay second unit");
                coroutine::stop_source stop;
                auto cancel_after_commit = [&](language::evaluated_unit unit) {
                    receive(std::move(unit));
                    stop.request_stop();
                };
                auto cancelled = co_await coroutine::under(stop.get_token(), analysis_call(channel, io, request, record_checks{.unit = cancel_after_commit}));
                expect(!cancelled && committed.size() == 5u) << "cancel stops the in-flight unit after the first is committed";
                semantic::store reopened{root / "semantic"};
                expect(reopened.load(committed.back())->pieces.front()->types().front().kind == "primitive")
                    << "the committed unit survives cancellation and store restart";
                request.analysis.budget.milliseconds = 1000;
                auto timed = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect(timed && timed->ok() && !timed->analysis_error && timed->analyzed && !timed->missing.empty() && timed->committed_units == 1u &&
                       timed->missing.size() == 1u && timed->missing.front().scope.files.front() == "b.cpp" && committed.size() == 6u)
                    << "time exhaustion keeps the committed prefix and identifies the interrupted unit";
                std::filesystem::remove(root / "delay");
                request.analysis.budget.milliseconds.reset();
                auto resumed = co_await analysis_call(channel, io, request, record_checks{.unit = receive});
                expect(resumed && resumed->analysis_session && resumed->analysis_session->id != recovered->analysis_session->id)
                    << "the identified cancellation retires its active provider before the next request";
                expect(resumed && resumed->ok() && resumed->analyzed && resumed->missing.empty() && committed.size() == 8u)
                    << "evaluation completes after replacing the interrupted provider";

                auto saved = stored.load(committed.back());
                request.analysis.demand.files = {"b.cpp"};
                request.analysis.candidates = {
                    {.id = committed.back(), .recipe = saved->pieces.front()->unit.recipe, .units = {saved->pieces.front()->unit}, .nodes = 1}};
                std::vector<language::evaluated_unit> evaluated;
                auto collect = [&](language::evaluated_unit unit) -> void {
                    evaluated.push_back(std::move(unit));
                    return;
                };
                auto validate = [&](jobs::reuse_request input) -> jobs::reuse_reply {
                    semantic::source_vector sources;
                    for (const auto& file : tree.files)
                        sources["app"].emplace(file.path, file.digest.str());
                    reuse::channel_values channels;
                    const auto validators =
                        language::providers::dispatch(input.session.provider, []<class Provider> { return language::input_validators<Provider>(); });
                    reuse::environment current{sources, input.inputs, channels, "app", validators};
                    jobs::reuse_reply reply;
                    for (const auto& candidate : request.analysis.candidates)
                        if (reuse::accept(candidate, input.session, request.analysis.demand, current))
                            reply.accepted.push_back({candidate.id, semantic::recipe_identifier(candidate.recipe)});
                    return reply;
                };
                runtime.id = "worker-other-sdk";
                runtime.inputs.front().object = "unused-sdk-changed";
                service.set_runtime(runtime);
                auto unrelated = co_await analysis_call(channel, io, request, record_checks{.unit = collect, .reuse = validate});
                expect((unrelated && !unrelated->analysis_error && evaluated.size() == 1u) >> fatal);
                expect(evaluated.back().reused == committed.back() && evaluated.back().shards.empty())
                    << "changing an unread SDK retains the persisted TU after its provider session restarts";

                runtime.id = "worker-unrelated-tool";
                runtime.executables.at("/tools/unrelated") = "unrelated-changed";
                service.set_runtime(runtime);
                auto unrelated_tool = co_await analysis_call(channel, io, request, record_checks{.unit = collect, .reuse = validate});
                expect((unrelated_tool && !unrelated_tool->analysis_error && evaluated.size() == 2u) >> fatal);
                expect(evaluated.back().reused == committed.back() && evaluated.back().shards.empty())
                    << "the TU reuses its exported tool reads after an unrelated tool changes";
                runtime.id = "worker-consumed-tool";
                runtime.executables.at("/tools/compiler") = "compiler-changed";
                service.set_runtime(runtime);
                auto consumed_tool = co_await analysis_call(channel, io, request, record_checks{.unit = collect, .reuse = validate});
                expect((consumed_tool && !consumed_tool->analysis_error && evaluated.size() == 3u && !evaluated.back().shards.empty()) >> fatal);
                expect(!evaluated.back().reused && std::ranges::any_of(evaluated.back().shards.front().unit.direct_inputs, [](const auto& input) {
                    return input.kind == semantic::input_kind::tool && input.name == "/tools/compiler" && input.object == "compiler-changed";
                })) << "the TU evaluates again when its exported compiler input changes";
                runtime.executables.at("/tools/compiler") = "compiler-object";
                runtime.id = "worker-consumed-sdk";
                runtime.inputs.back().object = "headers-changed";
                service.set_runtime(runtime);
                auto consumed = co_await analysis_call(channel, io, request, record_checks{.unit = collect, .reuse = validate});
                expect((consumed && !consumed->analysis_error && evaluated.size() == 4u && !evaluated.back().shards.empty()) >> fatal);
                expect(!evaluated.back().reused && std::ranges::any_of(evaluated.back().shards.front().unit.observed_dependencies, [](const auto& read) {
                    return read.value == "headers-changed";
                })) << "changing a consumed SDK evaluates the TU and replaces its SDK read";
                runtime.inputs.back().object = "headers-object";
                service.set_runtime(runtime);
                test::spit(root / "configurations", R"({"a.cpp":"changed-a","b.cpp":"command-b.cpp"})");
                auto other_command = co_await analysis_call(channel, io, request, record_checks{.unit = collect, .reuse = validate});
                expect((other_command && !other_command->analysis_error && evaluated.size() == 5u) >> fatal);
                expect(evaluated.back().reused == committed.back() && evaluated.back().shards.empty())
                    << "changed preparation output with only another TU's command edit retains this TU";
                test::spit(root / "configurations", R"({"a.cpp":"changed-a","b.cpp":"changed-b"})");
                auto own_command = co_await analysis_call(channel, io, request, record_checks{.unit = collect, .reuse = validate});
                expect((own_command && !own_command->analysis_error && evaluated.size() == 6u && !evaluated.back().shards.empty()) >> fatal);
                expect(!evaluated.back().reused && std::ranges::any_of(evaluated.back().shards.front().unit.direct_inputs, [](const auto& input) {
                    return input.kind == semantic::input_kind::provider && input.type == "clice/configuration" && input.name == "b.cpp" &&
                           input.object == "changed-b";
                })) << "the consumed command's edit evaluates and replaces the TU contribution";
                const auto commands_before_diagnostics = test::slurp(root / "commands");
                request.analysis.demand.facts = {std::string{"diagnostics"}};
                request.analysis.candidates.clear();
                evaluated.clear();
                auto diagnostics = co_await analysis_call(channel, io, request, record_checks{.unit = collect});
                expect((diagnostics && !diagnostics->analysis_error && evaluated.size() == 1u) >> fatal);
                const auto immediate = rfl::json::read<language::cpp::contribution_key>(evaluated.front().request.unit.name).value();
                expect(evaluated.front().request.unit.kind == semantic::unit_kind::group && immediate.translation_unit == "b.cpp" &&
                       immediate.contribution == "immediate" && test::slurp(root / "commands") == commands_before_diagnostics)
                    << "immediate diagnostics use canonical contribution keys without running index or CLI queries";
            };
            exercise(run_operation);
        };

        "language analysis honors both preparation and evaluation deadlines"_test = [] {
            static constexpr auto run_operation = [](auto& channel, const auto&) -> coroutine::task<> {
                content::manifest tree{{entry("a.cpp", "int a;")}};
                auto offered = co_await channel.call(jobs::sync_offer{"app", tree});
                expect(offered.has_value() >> fatal);
                expect((co_await channel.call(jobs::source_bytes{{blob("int a;")}})).has_value());
                jobs::analysis_request request{.preparation = {.project = "app",
                                                               .tree = "analysis-deadline",
                                                               .snapshot = tree.id(),
                                                               .steps = {{.name = "prepare", .argv = {test::shell(), "-c", "sleep 0.7"}}},
                                                               .timeout_s = 1},
                                               .analysis = jobs::analysis_plan{.provider = "clice",
                                                                               .argv = {test::shell(), "-c", "sleep 10"},
                                                                               .seconds = 30,
                                                                               .execution = {"deadline-request", "deadline-computation", "cpp"},
                                                                               .slot_id = "app/analysis-deadline"}};
                auto started = std::chrono::steady_clock::now();
                auto report = co_await channel.call(request);
                expect(report.has_value() >> fatal);
                expect(!report->ok() && report->steps.back().name == "analysis" && report->steps.back().timed_out);
                expect(std::chrono::steady_clock::now() - started < 3s) << "the survey receives only the remaining execution time";
                request.preparation.steps.clear();
                request.preparation.timeout_s = 30;
                request.analysis.budget.milliseconds = 100;
                started = std::chrono::steady_clock::now();
                auto bounded = co_await channel.call(request);
                expect(bounded.has_value() >> fatal);
                expect(bounded->ok() && bounded->analyzed && !bounded->missing.empty() && bounded->committed_units == 0u && bounded->missing.size() == 1u &&
                       bounded->missing.front().deferred && !bounded->analysis_session);
                expect(std::chrono::steady_clock::now() - started < 2s) << "the analysis budget limits a longer build deadline";
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
    if (argc == 7) {
        test::require(std::string_view{argv[1]}.compare("--fsharp") == 0, "unknown executor integration command");
        const std::filesystem::path fixture{argv[2]};
        const std::string dotnet{argv[3]}, host{argv[4]}, adapter{argv[5]}, sdk{argv[6]};
        auto run = [&](auto& channel, auto&, replica::service& service, ctx& io) -> coroutine::task<> {
            service.set_runtime({.inputs = {{"sdk/dotnet", "dotnet", {sdk}}}});
            const auto model = test::slurp(fixture / "model/model.json");
            const auto consumer = test::slurp(fixture / "App.fs");
            jobs::analysis_request request{.preparation = {.project = "project", .tree = "fsharp", .measure = {{"model", "project_model", "model/model.json"}}},
                                           .analysis = {.provider = "fsharp",
                                                        .options = "fsharp-test",
                                                        .artifacts = {{"model", "project_model", "model/model.json"}},
                                                        .argv = {dotnet, host, "--adapter", adapter},
                                                        .env = {{"TMPDIR", fixture.string()}},
                                                        .execution = {"request", "computation", "fsharp"},
                                                        .slot_id = "project/fsharp",
                                                        .demand = {.files = {"App.fs"}, .facts = {std::string{"symbols"}, std::string{"types"}}}}};
            std::size_t reads = 0;
            auto fetch = [&](jobs::model_read request) -> jobs::source_bytes {
                ++reads;
                return delivery::read(fixture / "objects", request.object);
            };
            std::string session;
            for (const auto& value : {std::string{"42"}, std::string{"\"updated\""}}) {
                const auto source = "namespace Sample\nmodule Library =\n    let value = " + value + "\n";
                content::manifest tree{{entry("model/model.json", model), entry("Library.fs", source), entry("App.fs", consumer)}};
                test::require((co_await channel.call(jobs::sync_offer{"project", tree})).has_value(), "offer failed");
                test::require((co_await channel.call(jobs::source_bytes{{blob(model), blob(source), blob(consumer)}})).has_value(), "source transfer failed");
                request.preparation.snapshot = tree.id();
                std::vector<language::evaluated_unit> units;
                auto receive = [&](language::evaluated_unit unit) -> void {
                    units.push_back(std::move(unit));
                    return;
                };
                auto report = co_await analysis_call(channel, io, request, record_checks{.unit = receive, .model = fetch});
                test::require(report.has_value() && report->analyzed && report->analysis_session.has_value(),
                              report ? rfl::json::write(*report) : "analysis transport failed");
                test::require(!units.empty() && !units.front().shards.empty(), "worker returned no F# shard");
                const auto& shard = units.front().shards.front();
                const auto result = std::ranges::find(shard.symbols, "result", &semantic::records::symbol::name);
                test::require(result != shard.symbols.end() && result->type.has_value(), "F# consumer has no inferred type");
                const auto type = std::ranges::find(shard.types, *result->type, &semantic::records::type::id);
                test::require(type != shard.types.end() && type->name == (value == "42" ? "Microsoft.FSharp.Core.int" : "Microsoft.FSharp.Core.string"),
                              "F# predecessor edit did not update consumer type");
                if (!session.empty())
                    test::require(session == report->analysis_session->id, "source edit replaced FCS helper session");
                session = report->analysis_session->id;
            }
            test::require(reads > 0, "worker did not fetch generated model objects");
        };
        exercise(run);
        return 0;
    }
    if (argc == 8) {
        test::require(std::string_view{argv[1]} == "--kotlin", "unknown executor integration command");
        const std::filesystem::path fixture{argv[2]};
        const std::string java{argv[3]}, host{argv[4]}, adapter{argv[5]}, jdk{argv[6]}, stdlib{argv[7]};
        auto run = [&](auto& channel, auto&, replica::service& service, ctx& io) -> coroutine::task<> {
            service.set_runtime({.inputs = {{"jdk", "jdk", {jdk}}, {"stdlib", "stdlib", {stdlib}}}});
            const auto model = test::slurp(fixture / "model.json");
            jobs::analysis_request request{.preparation = {.project = "project", .tree = "kotlin", .measure = {{"model", "project_model", "model.json"}}},
                                           .analysis = {.provider = "kotlin",
                                                        .options = "kotlin-test",
                                                        .artifacts = {{"model", "project_model", "model.json"}},
                                                        .argv = {java, "-Djava.io.tmpdir=" + fixture.string(), "-jar", host, "--adapter", adapter},
                                                        .execution = {"request", "computation", "kotlin"},
                                                        .slot_id = "project/kotlin",
                                                        .demand = {.files = {"Main.kt"}, .facts = {std::string{"symbols"}, std::string{"types"}}}}};
            std::size_t reads = 0;
            auto fetch = [&](jobs::model_read request) -> jobs::source_bytes {
                ++reads;
                return delivery::read(fixture / "objects", request.object);
            };
            std::string session;
            for (const auto& source : {std::string{"fun answer() = listOf(42).first()\n"}, std::string{"fun answer() = listOf(\"updated\").first()\n"}}) {
                const auto kotlin_source = source + "fun javaAnswer() = JavaApi.value()\n";
                const auto java_source = source.contains("42") ? std::string{"public class JavaApi { public static int value() { return 42; } }"}
                                                               : std::string{"public class JavaApi { public static String value() { return \"updated\"; } }"};
                content::manifest tree{{entry("model.json", model), entry("Main.kt", kotlin_source), entry("JavaApi.java", java_source)}};
                test::require((co_await channel.call(jobs::sync_offer{"project", tree})).has_value(), "offer failed");
                test::require((co_await channel.call(jobs::source_bytes{{blob(model), blob(kotlin_source), blob(java_source)}})).has_value(),
                              "source transfer failed");
                request.preparation.snapshot = tree.id();
                std::vector<language::evaluated_unit> units;
                auto receive = [&](language::evaluated_unit unit) -> void {
                    units.push_back(std::move(unit));
                    return;
                };
                auto report = co_await analysis_call(channel, io, request, record_checks{.unit = receive, .model = fetch});
                test::require(report.has_value(), "analysis transport failed");
                test::require(report->analyzed && report->analysis_session.has_value(), rfl::json::write(*report));
                test::require(!units.empty() && !units.front().shards.empty(), "worker returned no Kotlin shard");
                const auto records = rfl::json::write(units.front().shards);
                test::require(records.contains("answer") && records.contains(source.contains("42") ? "kotlin.Int" : "kotlin.String"), records);
                const auto& shard = units.front().shards.front();
                const auto function = std::ranges::find(shard.symbols, "javaAnswer", &semantic::records::symbol::name);
                test::require(function != shard.symbols.end() && function->type.has_value(), "Java consumer has no function type");
                auto type = [&](const semantic::type_id& id) -> const semantic::records::type& {
                    const auto found = std::ranges::find(shard.types, id, &semantic::records::type::id);
                    test::require(found != shard.types.end(), "Java consumer type is absent");
                    return *found;
                };
                const auto& signature = type(*function->type);
                test::require(signature.return_type.has_value(), "Java consumer has no return type");
                const auto& result = type(*signature.return_type);
                const auto& value = result.kind == "flexible" ? type(*result.lower_bound) : result;
                test::require(value.name == (source.contains("42") ? "kotlin.Int" : "kotlin.String"), "Java edit did not update Kotlin inference");
                if (!session.empty())
                    test::require(session == report->analysis_session->id, "source edit replaced the helper session");
                session = report->analysis_session->id;
            }
            test::require(reads > 0, "worker did not fetch the model library object");
        };
        exercise(run);
        return 0;
    }
    boost::ut::suite<"cache">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
