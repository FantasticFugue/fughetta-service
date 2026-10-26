import launch;
import boost.ut;
import fixture;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "HTTP command workflow remains available after gateway restart"_test = [] {
            test::scratch tmp{"workflow"};
            auto port = test::port();
            auto doc = test::document(tmp.dir, port, test::port());
            auto file = tmp.dir / "config.json";
            test::spit(file, rfl::json::write(doc));
            auto workflow = fixture::commands({"npm", "run", "build"}, {"npm", "ci"}, {"npm", "test"}, "app");
            config::settings settings;
            settings.build_configurations["npm"] = workflow;
            settings.default_configurations = std::vector<std::string>{"npm"};
            auto start = [&] {
                auto process = std::make_unique<test::child>(test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {file.string()}));
                test::ready(*process, port);
                return process;
            };
            auto process = start();
            auto saved = test::save(port, std::string{test::project}, rfl::json::write(settings));
            test::require(saved.status == 200, saved.body);
            auto check = [&] {
                auto stored = test::request(port, http::method::GET, std::string{test::project});
                test::require(stored.status == 200, stored.body);
                struct section { config::settings value; std::uint64_t revision; };
                auto parsed = test::decode<section>(stored.body);
                const auto& build = parsed.value.build_configurations.at("npm");
                expect(rfl::json::write(build.builders.at("main")) == rfl::json::write(workflow.builders.at("main")));
                expect(build.tasks.at("test").after == std::vector<std::string>{"build"});
                expect(build.tasks.at("build").after == std::vector<std::string>{"prepare"});
                auto effective = test::request(port, http::method::GET, std::string{test::project} + "/effective");
                test::require(effective.status == 200, effective.body);
                struct resolved { builds::configuration build; };
                struct view { std::map<std::string, resolved> configs; };
                auto current = test::decode<view>(effective.body).configs.at("npm").build;
                auto& commands = fixture::options<"command">(current.builders.at("main"));
                expect(commands.operations.at("test").argv == std::vector<std::string>{"npm", "test"});
                expect(commands.operations.at("build").working_dir == "app");
                auto plan = test::request(port, http::method::POST, std::string{test::project} + "/build-configurations/npm/plan", R"({"operation":"test"})");
                test::require(plan.status == 200, plan.body);
                auto prepared = test::decode<builds::plan>(plan.body);
                expect(prepared.steps.size() == 3u);
                expect(prepared.steps.back().argv == std::vector<std::string>{builds::tool("npm"), "test"});
                return parsed.revision;
            };
            auto revision = check();
            expect(process->stop() == 0);
            process = start();
            expect(check() == revision);
            expect(process->stop() == 0);
        };
    };
}

int main(int argc, const char** argv) {
    boost::ut::suite<"workflow">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
