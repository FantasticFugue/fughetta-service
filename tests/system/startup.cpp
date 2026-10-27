import launch;
import boost.ut;
import host;
import content;
import environment;

namespace {
    using namespace boost::ut;
    auto clean_command(const std::filesystem::path& root, std::vector<std::string> args = {}) -> io::command {
        auto command = test::gateway_command(SERVICE_EXECUTABLE, root, std::move(args));
        command.env = host::environment();
        return command;
    }

    static constexpr auto cases = [] {
        "one entry starts the source and stops it with the gateway"_test = [] {
            test::scratch tmp{"managed"};
            auto port = test::port(), source_port = test::port();
            auto doc = test::document(tmp.dir, port, source_port);
            doc.runtime = {.threads = 6, .jobs = 1, .workers = 0};
            test::spit(tmp.dir / "config.json", config::write(doc));
            test::child process{test::command(SERVICE_EXECUTABLE, tmp.dir)};
            test::ready(process, port);
            auto health = test::request(port, http::method::GET, "/healthz");
            expect(health.status == 200 && health.body.contains(R"("source":true)")) << health.body;
            expect(process.stop() == 0) << process.output();
            native::context context;
            expect(
                throws<std::system_error>([&] { cores::run(context, io::connect(context.get_scheduler(), {net::ip::address_v4::loopback(), source_port})); }));
        };
        "the managed entry restarts after its parent is killed"_test = [] {
            test::scratch tmp{"managed-restart"};
            auto port = test::port(), source_port = test::port();
            auto doc = test::document(tmp.dir, port, source_port);
            doc.runtime = {.threads = 6, .jobs = 1, .workers = 0};
            test::spit(tmp.dir / "config.json", config::write(doc));
            {
                auto process = std::make_unique<test::child>(test::command(SERVICE_EXECUTABLE, tmp.dir));
                test::ready(*process, port);
                expect(test::request(port, http::method::GET, "/healthz").body.contains(R"("source":true)"));
                process.reset();
            }
            test::eventually([&] {
                try {
                    native::context context;
                    cores::run(context, io::connect(context.get_scheduler(), {net::ip::address_v4::loopback(), source_port}));
                    return false;
                } catch (const std::system_error&) {
                    return true;
                }
            });
            test::child process{test::command(SERVICE_EXECUTABLE, tmp.dir)};
            test::ready(process, port);
            expect(test::request(port, http::method::GET, "/healthz").body.contains(R"("source":true)"));
            expect(process.stop() == 0) << process.output();
        };
        "a source startup failure fails the managed entry"_test = [] {
            test::scratch tmp{"managed-failure"};
            auto doc = test::document(tmp.dir, test::port(), test::port());
            doc.runtime.workers = 0;
            test::spit(tmp.dir / "state" / "source", "not a directory");
            test::spit(tmp.dir / "config.json", config::write(doc));
            test::child process{test::command(SERVICE_EXECUTABLE, tmp.dir)};
            expect(process.wait() == 1) << process.output();
        };
        "default entry reads config.json and checks bearer tokens"_test = [] {
            test::scratch tmp{"startup"};
            auto port = test::port();
            auto doc = test::document(tmp.dir, port, test::port());
            test::spit(tmp.dir / "config.json", config::write(doc));
            {
                test::child process{clean_command(tmp.dir)};
                test::ready(process, port);
                expect(test::request(port, http::method::GET, "/healthz", {}, "").status == 200);
                expect(test::request(port, http::method::GET, "/api/v1/config", {}, "").status == 401);
                expect(test::request(port, http::method::GET, "/api/v1/config", {}, "invalid-token").status == 401);
                expect(test::request(port, http::method::GET, "/api/v1/config").status == 200);
                expect(process.stop() == 0);
            }
            {
                test::child process{clean_command(tmp.dir, {"missing.json"})};
                expect(process.wait() == 1);
                auto output = process.output();
                expect(output.contains("missing.json"));
            }
        };

        "token subcommand adds a token the gateway accepts"_test = [] {
            test::scratch tmp{"token"};
            auto port = test::port();
            test::spit(tmp.dir / "config.json", config::write(test::document(tmp.dir, port, test::port())));
            std::string token;
            {
                test::child process{clean_command(tmp.dir, {"token", "agent", "member"})};
                auto status = process.wait();
                auto output = process.output();
                expect(status == 0) << output;
                token = output.substr(0, output.find('\n'));
            }
            expect(token.size() == 64_u) << token;
            auto saved = config::load(tmp.dir / "config.json");
            auto found = std::ranges::find(saved.server.auth.tokens, "agent", &config::token::name);
            expect(found != saved.server.auth.tokens.end() && found->role == "member" && found->hash == content::hash_of(token).str());
            {
                test::child process{clean_command(tmp.dir)};
                test::ready(process, port);
                auto me = test::request(port, http::method::GET, "/api/v1/me", {}, token);
                expect(me.status == 200 && me.body.contains(R"("role":"member")")) << me.body;
                expect(test::request(port, http::method::PUT, "/api/v1/config/limits", R"({"revision":0,"value":{}})", token).status == 403);
                expect(process.stop() == 0);
            }
            {
                test::child process{clean_command(tmp.dir, {"token", "agent", "owner"})};
                expect(process.wait() == 1);
                auto output = process.output();
                expect(output.contains("named twice")) << output;
            }
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"startup">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
