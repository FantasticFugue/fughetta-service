import std;
import boost.ut;
import coroutine;
import native;
import meta;
import protocol;
import mcp;
import http;
import configuration;
import registry;
import support;
import tools;
import fleet;
import world;
import reflection;

namespace {
    using namespace boost::ut;
    using ctx = io::scheduler_of_t<native::context>;

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

    static_assert(meta::fnv1a("") == 0x811c9dc5u);
    static_assert(meta::fnv1a("a") == 0xe40c292cu);
    static_assert(meta::fixed_string{"abc"}.view() == "abc");
    static_assert(meta::unique<int, long, char>);
    static_assert(!meta::unique<int, long, int>);
    static_assert(meta::distinct(std::array{1, 2, 3}));
    static_assert(!meta::distinct(std::array{1, 2, 1}));

    template <class T> struct is_long : std::bool_constant<std::same_as<T, long>> {};
    static_assert(std::same_as<meta::find_t<is_long, meta::list<int, long, char>>, long>);
    static_assert(std::same_as<meta::find_t<is_long, meta::list<int, char>>, void>);

    static_assert(jobs::code<jobs::node_call> == meta::fnv1a("node.call"));
    static_assert(std::same_as<jobs::reply_t<jobs::node_usage>, jobs::node_usage_reply>);
    static_assert(jobs::request<jobs::node_call> && !jobs::request<jobs::hello>);
    static_assert(jobs::one_way<jobs::progress>);

    struct echo : tools::tool<"echo", "Say it back."> {
        struct input {
            std::string text;
        };
        struct output {
            std::string said;
        };
        static auto run(tools::services&, input in, mcp::notifier&, const worlds::actor&) -> coroutine::task<output> {
            co_return output{in.text};
        }
    };

    struct shout : tools::tool<"shout", "Say it back, louder."> {
        using input = echo::input;
        using output = echo::output;
        static auto run(tools::services&, input in, mcp::notifier&, const worlds::actor&) -> coroutine::task<output> {
            std::ranges::transform(in.text, in.text.begin(), [](unsigned char c) { return std::toupper(c); });
            co_return output{in.text};
        }
    };

    struct tick : tools::tool<"tick", "Count to two."> {
        struct input {
            std::optional<bool> fail;
        };
        struct output {
            int count = 0;
            bool failed_ = false;
            auto failed() const -> bool { return failed_; }
        };
        static auto run(tools::services&, input in, mcp::notifier& note, const worlds::actor&) -> coroutine::task<output> {
            for (std::int64_t i = 1; i <= 2; ++i)
                note({.method = "notifications/progress", .params = mcp::object({{"progress", i}, {"total", std::int64_t{2}}})});
            co_return output{2, in.fail.value_or(false)};
        }
    };

    using box = tools::toolbox<echo, shout, tick>;
    static_assert(tools::valid_name("build") && !tools::valid_name("has space") && !tools::valid_name(""));
    static_assert(echo::name == "echo");

    struct gateway_parts {
        cores::io_context<native::context> rt{1};
        routing::router router{rt};
        config::slots config{1, std::make_shared<const config::document>()};
        execution::thread_pool pool{1};
        test::scratch tmp{"meta"};
        test::source_service source{tmp.dir / "source"};
        storage::registry files{rt, tmp.dir / "state", config, pool, source.endpoint()};
        tools::services services{config.slot(0), router, files, 0};
    };

    auto request(std::string_view json) -> mcp::request { return rfl::json::read<mcp::request>(json).value(); }

    static constexpr auto meta_suite = [] {
        "an MCP server lists and calls the toolbox's tools"_test = [] {
            gateway_parts g;
            mcp::server<box::bound> server{"test", "0", box::bound{g.services}};
            auto& listed = server.tools.list();
            expect((listed.size() == 3u) >> fatal);
            expect(listed[0].name == "echo" && listed[1].name == "shout");
            expect(listed[1].description == "Say it back, louder.");
            expect(listed[0].outputSchema.has_value()) << "derived from the output struct";

            native::context context;
            auto io = context.get_scheduler();
            mcp::notifier drop;
            const worlds::actor someone{.issuer = "token", .user = "tester", .caller = "test-suite", .role = "owner"};
            auto said = *cores::run(context, server.tools.call("shout", mcp::object({{"text", std::string{"hey"}}}), drop, someone));
            expect(said.text == R"({"said":"HEY"})" && !said.error && said.structured.has_value());
            auto unknown = *cores::run(context, server.tools.call("whisper", mcp::object({}), drop, someone));
            expect(unknown.error && unknown.text == "unknown tool: whisper");
            auto bad = *cores::run(context, server.tools.call("echo", mcp::object({}), drop, someone));
            expect(bad.error && bad.text.contains(R"("code":"invalid_record")") && bad.text.contains("echo: bad arguments")) << bad.text;
            auto failed = *cores::run(context, server.tools.call("tick", mcp::object({{"fail", true}}), drop, someone));
            expect(failed.error) << "an output that says failed()";
        };

        "progress goes out only with the client's progress token"_test = [] {
            gateway_parts g;
            mcp::server<box::bound> server{"test", "0", box::bound{g.services}};
            native::context context;
            auto io = context.get_scheduler();
            web::event_stream stream;
            mcp::notifier keep{stream};
            const worlds::actor someone{.issuer = "token", .user = "tester", .caller = "test-suite", .role = "owner"};

            auto plain = request(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"tick","arguments":{}}})");
            auto r = *cores::run(context, server.handle(plain, keep, someone));
            expect(notifications(stream).empty()) << "no token, no progress";
            expect(!r->error.has_value());

            auto asked = request(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tick","arguments":{},"_meta":{"progressToken":"t-7"}}})");
            cores::run(context, server.handle(asked, keep, someone));
            auto sent = notifications(stream);
            expect((sent.size() == 2u) >> fatal);
            auto params = sent[1].params.to_object().value();
            expect(params.get("progressToken").value().to_string().value() == "t-7");
            expect(params.get("progress").value().to_int().value() == 2);

            auto hello = request(R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
            auto init = *cores::run(context, server.handle(hello, keep, someone));
            auto version = init->result->to_object().value().get("protocolVersion").value().to_string().value();
            expect(version == "2025-03-26") << "a version both speak is agreed on";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"meta">{meta_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
