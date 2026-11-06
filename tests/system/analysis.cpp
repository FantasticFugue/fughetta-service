import launch;
import boost.ut;
import client;
import protocol;
import language;
import fixture;
import content;
import nodes;
import semantic;

namespace {
    using namespace boost::ut;
    const worlds::actor actor{.issuer = "source", .user = "analysis-test", .caller = "suite", .role = "service"};

    auto node(net::ip::tcp::endpoint endpoint, nodes::node_request request, bool reading = false) -> nodes::node_reply {
        auto action = request.action;
        auto reply = source::call(endpoint, jobs::node_call{.project = "app", .actor = actor, .request = std::move(request), .reading = reading});
        test::require(reply.code == "ok", action + ": " + reply.code + ": " + reply.reason);
        return reply;
    }
    // A change in main against its observed source version.
    auto create(net::ip::tcp::endpoint endpoint, std::string path, std::string text) -> nodes::node_reply {
        auto tree = node(endpoint, {.action = "tree", .worldline = "main"}, true);
        return node(endpoint, {.action = "apply",
                               .worldline = "main",
                               .operation = content::hash_of("create " + path).str(),
                               .expected = tree.frontier,
                               .edits = {{.kind = "create", .path = std::move(path), .text = std::move(text)}}});
    }
    auto start_source(const std::filesystem::path& root, std::uint16_t port) {
        std::vector<std::string> args{"--port", std::to_string(port), "--storage", (root / "source").string()};
        auto process = std::make_unique<test::child>(test::source_command(SERVICE_EXECUTABLE, root, std::move(args)));
        test::eventually([&] {
            test::require(!process->finished(), process->output());
            return process->output().contains("source: internal file service");
        });
        test::ready(*process, port);
        return process;
    }

    static constexpr auto cases = [] {
        "language preparation requires a worker and never executes on source"_test = [] {
            test::scratch tmp{"analysis-routing"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            auto doc = test::document(tmp.dir, port, source_port);
            auto& settings = doc.projects.at("app");
            settings.build_configurations["native"] = fixture::cmake("build/native");
            fixture::options<"cmake">(settings.build_configurations["native"].builders.at("main")).export_compile_commands = true;
            settings.analysis["cpp"] = {.build_configuration = "native"};
            test::spit(tmp.dir / "config.json", rfl::json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            auto error = test::tool(port, "language", R"({"project":"app","configuration":"cpp"})", true);
            expect(error.contains("no_worker")) << error;
            expect(!std::filesystem::exists(tmp.dir / "source/.analysis"));
            for (auto action : {"plan", "prepare", "receipt"}) {
                auto response = test::request(port, action == std::string_view{"receipt"} ? http::method::GET : http::method::POST,
                                              std::string{test::project} + "/analysis/cpp/" + action);
                expect(response.status == 404);
            }
            expect(gateway.stop() == 0 && source->stop() == 0);
        };

        "C++ language queries automatically prepare on workers and reuse the bound build tree"_test = [] {
            auto clice = std::getenv("TEST_CLICE");
            if (!clice || !std::filesystem::exists("/sys/fs/cgroup/workers")) {
                skip / "real worker analysis requires TEST_CLICE and delegated cgroups"_test = [] {};
                return;
            }
            test::scratch tmp{"worker-analysis"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            net::ip::tcp::endpoint endpoint{net::ip::address_v4::loopback(), source_port};
            create(endpoint, "main.cpp", "int twice(int x) { return x * 2; }\nint main() { return twice(0); }\n");
            create(endpoint, "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(probe CXX)\nadd_executable(app main.cpp)\n");
            auto doc = test::document(tmp.dir, port, source_port);
            auto& settings = doc.projects.at("app");
            settings.build_configurations["native"] = fixture::cmake("build/native");
            auto& cmake = fixture::options<"cmake">(settings.build_configurations["native"].builders.at("main"));
            cmake.export_compile_commands = true;
            settings.analysis["cpp"] = {.server = fixture::clice("clice", {"--log-level", "off"}), .build_configuration = "native"};
            test::spit(tmp.dir / "config.json", rfl::json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            config::worker worker_config;
            worker_config.gateway = std::format("127.0.0.1:{}", doc.server.worker_port);
            worker_config.work_root = (tmp.dir / "worker").string();
            worker_config.tools["clice"] = clice;
            test::spit(tmp.dir / "worker.json", rfl::json::write(worker_config));
            test::child worker{test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"})};
            test::eventually([&] {
                test::require(!worker.finished(), worker.output());
                auto workers = test::request(port, http::method::GET, "/api/v1/workers");
                return workers.status == 200 && workers.body != "[]";
            });
            auto symbols = test::tool(port, "language", R"({"project":"app","configuration":"cpp","action":"symbols"})");
            expect(symbols.contains("twice") && symbols.contains(R"("analyzed":true)")) << symbols;
            auto repeated = test::tool(port, "language", R"({"project":"app","configuration":"cpp","action":"symbols"})");
            expect(repeated.contains("twice") && !repeated.contains(R"("preparation":{)"));
            auto built = test::tool(port, "build", R"({"project":"app","configurations":["native"]})");
            expect(built.contains(R"("ok":true)")) << built;
            expect(!std::filesystem::exists(tmp.dir / "source/.analysis"));
            expect(worker.stop() == 0 && gateway.stop() == 0 && source->stop() == 0);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"analysis">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
