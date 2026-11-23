import launch;
import boost.ut;
import publiccache;
import publicrecord;
import records;
import content;
import host;
import durability;
import enrollment;
import fixture;
import language;
import tools;
import websocket;
import constants;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    const std::string seed(64, '1');
    const public_shards::credentials test_key{"test-id", "test-secret"};

    auto recipe_for(std::string project) -> semantic::recipe {
        return {{std::move(project), "stdlib", "jvm-artifact", "evaluate", semantic::unit_kind::artifact, "Object.class"}, "options"};
    }

    auto class_file() -> std::string {
        std::string bytes;
        auto hex = [&](std::string_view digits) {
            for (std::size_t i = 0; i < digits.size(); i += 2) {
                unsigned value = 0;
                std::from_chars(digits.data() + i, digits.data() + i + 2, value, 16);
                bytes.push_back(static_cast<char>(value));
            }
        };
        hex("cafebabe000000340003010010");
        bytes += "java/lang/Object";
        hex("0700010021000200000000000000000000");
        return bytes;
    }

    class local_bucket {
        std::mutex mutex_;
        std::map<std::string, std::string> objects_;
        std::size_t requests_ = 0, writes_ = 0;
        test::listener listener_;
    public:
        local_bucket() : listener_([this](native::scheduler&, native::socket socket) { return serve(std::move(socket)); }) {}
        auto endpoint() const -> std::string { return std::format("http://127.0.0.1:{}", listener_.endpoint().port()); }
        auto counts() -> std::pair<std::size_t, std::size_t> {
            std::lock_guard held{mutex_};
            return {requests_, writes_};
        }
        void damage_shard() {
            std::lock_guard held{mutex_};
            for (auto& [path, bytes] : objects_)
                if (path.starts_with("/public-test/shards/"))
                    bytes[0] ^= 1;
        }
    private:
        auto serve(native::socket socket) -> coroutine::task<> {
            web::stream stream{socket};
            http::v1::server server{stream};
            auto incoming = co_await http::coroutine::receive(server);
            auto body = co_await web::read_all(incoming.body);
            const auto& key = incoming.head.target;
            http::status status = 400;
            {
                std::lock_guard held{mutex_};
                ++requests_;
                if (incoming.head.method == http::method::GET && key.starts_with("/public-test/")) {
                    const auto found = objects_.find(key);
                    status = found == objects_.end() ? http::status{404} : http::status{200};
                    body = found == objects_.end() ? "" : found->second;
                } else if (incoming.head.method == http::method::PUT && key.starts_with("/public-test/") &&
                           web::header(incoming.head, "if-none-match") == "*" && web::header(incoming.head, "authorization")) {
                    ++writes_;
                    status = objects_.try_emplace(key, std::move(body)).second ? http::status{200} : http::status{412};
                    body.clear();
                }
            }
            http::response response{.status = status, .fields = {{"connection", "close"}}};
            co_await web::respond(server, std::move(incoming.token), std::move(response), std::move(body));
        }
    };

    struct result {
        std::uint64_t reused;
        std::vector<std::string> names;
    };

    auto reject_tunnel(std::uint16_t port, std::string_view protocol, std::optional<std::string_view> origin, http::status status) -> void {
        native::context context;
        auto io = context.get_scheduler();
        auto request = [&]() -> coroutine::task<http::status> {
            auto peer = co_await io::connect(io, {net::ip::address_v4::loopback(), port});
            test::http_stream transport{peer};
            http::v1::client client{transport};
            http::request head{.method = http::method::GET,
                               .target = std::string{constants::worker_tunnel_path},
                               .scheme = "http",
                               .authority = "localhost",
                               .fields = {{"connection", "Upgrade"}, {"upgrade", "websocket"},
                                          {"sec-websocket-key", web::ws::random_key()}, {"sec-websocket-version", "13"},
                                          {"sec-websocket-protocol", std::string{protocol}}}};
            if (origin)
                head.fields.push_back({"origin", std::string{*origin}});
            auto reply = co_await http::coroutine::fetch(client, std::move(head));
            co_return reply.head.status;
        };
        auto result = cores::run(context, coroutine::deadline(io, 5s, request()));
        test::require(result && *result == status, std::format("expected worker tunnel status {}", status.code()));
    }

    auto run_core(const std::filesystem::path& root, const std::string& project, const std::string& endpoint, bool publisher, bool private_input = false)
        -> result {
        std::filesystem::create_directories(root / "sdk");
        test::spit(root / "sdk" / "Object.class", class_file());
        test::spit(root / "sdk" / "manifest.json", R"({"sdk":"fixture"})");
        const auto source_port = test::port(), port = test::port();
        auto source_args = std::vector<std::string>{"--port", std::to_string(source_port), "--storage", (root / "source").string()};
        test::child source{test::source_command(SERVICE_EXECUTABLE, root, std::move(source_args))};
        test::ready(source, source_port);
        auto doc = test::document(root, port, source_port);
        doc.projects.clear();
        doc.server.auth.workspace = project;
        auto& settings = doc.projects[project];
        const std::string build = publisher ? "publisher-local" : "local";
        settings.build_configurations[build] = {};
        settings.default_configurations = {build};
        language::stage stage{.server = language::with<"jvm-artifact">{.options = {.module = "stdlib", .sdk = "fixture", .sdk_archive = "Object.class"}},
                              .build_configuration = build};
        config::worker worker;
        worker.gateway = std::format("127.0.0.1:{}", doc.server.worker_port);
        worker.name = "test-worker";
        worker.work_root = (root / "worker").string();
        worker.sdk_manifests["fixture"] = {.manifest = (root / "sdk" / "manifest.json").string(), .roots = {(root / "sdk").string()}};
        if (project == "reader")
            worker.tools["unrelated"] = std::getenv("TEST_PYTHON");
        if (private_input) {
            worker.tools["python"] = std::getenv("TEST_PYTHON");
            builds::config<"command"> builder;
            builder.operations["prepare"] = {
                .argv = {"python", "-c", "from pathlib import Path; Path('Object.class').write_bytes(bytes.fromhex(Path('class.txt').read_text()))"},
                .outputs = {{"class", {"jvm_artifact", "Object.class"}}}};
            settings.build_configurations["local"].builders["command"] = builds::with<"command">{.options = std::move(builder)};
            settings.build_configurations["local"].tasks["prepare"] = {.builder = "command", .operation = "prepare"};
            stage.server = language::with<"jvm-artifact">{.options = {.module = "stdlib"}};
            stage.inputs = {{"artifact", {language::build_input{"local", "prepare", "class"}}}};
        }
        settings.analysis["stdlib"] = fixture::profile(std::move(stage));
        const content::receipt_key signing{seed, content::receipt_key::encoding::hex_seed};
        doc.public_shards = config::public_shard_settings{.endpoint = endpoint, .bucket = "public-test", .trusted_publishers = {signing.public_key()}};
        if (publisher) {
            doc.server.remote_worker_port = test::port();
            doc.server.remote_worker_enrollment = true;
            enrollment::store registry{doc.server.storage, doc.server.auth.workspace};
            auto key = registry.add(worker.name, "operator");
            worker.gateway = std::format("ws://127.0.0.1:{}{}", doc.server.remote_worker_port, constants::worker_tunnel_path);
            worker.workspace_id = std::move(key.workspace_id);
            worker.worker_id = std::move(key.worker_id);
            worker.token = std::move(key.secret);
            doc.public_shards->publish = config::public_shard_publisher{"FUG_TEST_SHARD_SEED"};
        }
        test::spit(root / "config.json", config::write(doc));
        durability::durable_private_write(root / "worker.json", config::write(worker));
        auto command = test::gateway_command(SERVICE_EXECUTABLE, root, {"config.json"});
        command.env = host::environment();
        for (auto name : {"FUG_R2_READ_ACCESS_KEY_ID", "FUG_R2_READ_SECRET_ACCESS_KEY", "FUG_R2_ACCESS_KEY_ID", "FUG_R2_SECRET_ACCESS_KEY"})
            command.env->erase(name);
        if (publisher) {
            (*command.env)["FUG_TEST_SHARD_SEED"] = seed;
            (*command.env)["FUG_R2_ACCESS_KEY_ID"] = test_key.id;
            (*command.env)["FUG_R2_SECRET_ACCESS_KEY"] = test_key.secret;
        }
        test::child gateway{std::move(command)};
        test::ready(gateway, port);
        if (publisher) {
            reject_tunnel(doc.server.remote_worker_port, constants::worker_tunnel_protocol, "https://example.com", 403);
            reject_tunnel(doc.server.remote_worker_port, "wrong-protocol", std::nullopt, 400);
        }
        test::child executor{test::worker_command(SERVICE_EXECUTABLE, root, {"worker.json"})};
        struct worker_status { std::string name; bool online = false; };
        test::eventually([&] {
            test::require(!executor.finished(), executor.output());
            auto reply = test::request(port, http::method::GET, "/api/v1/workers");
            return std::ranges::any_of(test::decode<std::vector<worker_status>>(reply.body), [&](const auto& status) {
                return status.online && status.name == worker.name;
            });
        });
        auto inspect = test::decode<tools::worldline::output>(test::tool(port, "worldline",
            json::write(tools::worldline::input{.project = project, .action = "inspect"})));
        const auto data = class_file();
        auto text = private_input ? content::hex_encode({reinterpret_cast<const unsigned char*>(data.data()), data.size()}) : project;
        auto edit = tools::worldline::input{.project = project, .action = "apply", .observed = inspect.version, .operation_id = "initial",
            .changes = std::vector<tools::change>{{.kind = "create", .path = "class.txt", .text = std::move(text)}}};
        test::tool(port, "worldline", json::write(edit));
        auto response = test::tool(port, "language",
            std::format(R"({{"project":{},"configuration":"stdlib","action":"symbols"}})", json::write(project)));
        auto output = test::decode<tools::language_query::output>(response);
        test::require(output.view.has_value() && output.status == "complete" && !output.items.empty(), response);
        result summary{output.reused_units, {}};
        for (const auto& symbol : output.items)
            summary.names.push_back(symbol.name);
        executor.stop();
        gateway.stop();
        source.stop();
        return summary;
    }

    constexpr auto cases = [] {
        "public key uses only the complete unit input set and project-independent recipe"_test = [] {
            auto hash = content::hash_of("class").str();
            public_shards::inputs values{{semantic::input_kind::sdk, "Object.class", hash}, {semantic::input_kind::tool, "reader", hash}};
            auto first = recipe_for("first");
            auto second = recipe_for("second");
            auto key = public_shards::lookup_key(first, values);
            std::ranges::reverse(values);
            expect(!key.empty() && key == public_shards::lookup_key(second, values));
            values.push_back({semantic::input_kind::file, "private.txt", hash});
            expect(public_shards::lookup_key(second, values).empty());
        };
        "signed index and compressed records detect changes"_test = [] {
            auto recipe = recipe_for("first");
            auto values = public_shards::inputs{{semantic::input_kind::sdk, "Object.class", content::hash_of("class").str()}};
            semantic::shard shard{.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe))};
            shard.unit.direct_inputs = values;
            const content::receipt_key signing{seed, content::receipt_key::encoding::hex_seed};
            auto value = public_shards::encode(shard, values, signing).value();
            expect(value.index.trusted({signing.public_key()}));
            expect(!value.index.trusted({content::receipt_key{"other"}.public_key()}));
            auto changed = value.index;
            changed.input_set[0].object = content::hash_of("changed").str();
            expect(!changed.trusted({signing.public_key()}));
            expect(public_shards::decode(value.index, value.bytes, recipe, values).has_value());
            value.bytes.back() ^= 1;
            expect(!public_shards::decode(value.index, value.bytes, recipe, values));
            shard.unit.observed_dependencies.push_back({.kind = semantic::dependency_kind::source, .key = "private.txt"});
            expect(!public_shards::encode(shard, values, signing));
        };
        "S3 SigV4 matches the AWS GET object example"_test = [] {
            const std::string empty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
            auto request = public_shards::sigv4("GET", "/test.txt", {{"host", "examplebucket.s3.amazonaws.com"}, {"range", "bytes=0-9"},
                                                      {"x-amz-content-sha256", empty}, {"x-amz-date", "20130524T000000Z"}},
                                              "us-east-1", {"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"});
            const auto canonical = "GET\n/test.txt\n\nhost:examplebucket.s3.amazonaws.com\nrange:bytes=0-9\nx-amz-content-sha256:" + empty +
                                   "\nx-amz-date:20130524T000000Z\n\nhost;range;x-amz-content-sha256;x-amz-date\n" + empty;
            expect(request.canonical == canonical);
            expect(request.authorization.ends_with("Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));
        };
        "independent Cores share public JVM records and compute on corrupt objects"_test = [] {
            test::scratch scratch{"public-shards"};
            local_bucket s3;
            auto first = run_core(scratch.dir / "first", "publisher", s3.endpoint(), true);
            expect(first.reused == 0_u && !first.names.empty());
            expect(s3.counts().second == 2_u);
            auto second = run_core(scratch.dir / "second", "reader", s3.endpoint(), false);
            expect(second.reused == 1_u && first.names == second.names);
            expect(s3.counts().second == 2_u);
            s3.damage_shard();
            auto third = run_core(scratch.dir / "third", "repair", s3.endpoint(), false);
            expect(third.reused == 0_u && first.names == third.names);
            const auto before = s3.counts();
            auto private_result = run_core(scratch.dir / "private", "private", s3.endpoint(), false, true);
            expect(private_result.reused == 0_u && !private_result.names.empty());
            expect(s3.counts() == before);
        };
    };
}

int main(int argc, const char** argv) {
    if (argc > 1 && std::string_view{argv[1]}.compare("r2") == 0) {
        const auto* endpoint = std::getenv("FUG_R2_ENDPOINT");
        const auto* name = std::getenv("FUG_R2_BUCKET");
        if (!endpoint || !*endpoint || !name || !*name || !std::getenv("FUG_R2_ACCESS_KEY_ID") || !std::getenv("FUG_R2_SECRET_ACCESS_KEY"))
            return 77;
        const public_shards::bucket bucket{endpoint, name};
        const auto auth = public_shards::credentials::environment("FUG_R2_ACCESS_KEY_ID", "FUG_R2_SECRET_ACCESS_KEY");
        const auto key = "test/" + enrollment::mint(16);
        native::context context;
        auto run = [&]() -> coroutine::task<bool> {
            auto io = context.get_scheduler();
            if (!co_await bucket.put(io, key, "public-shard-test", auth))
                co_return false;
            bool matched = false;
            try {
                matched = (co_await bucket.get(io, key, auth)) == "public-shard-test";
            } catch (const std::exception&) {
            }
            co_await bucket.erase(io, key, auth);
            co_return matched;
        };
        return cores::run(context, run()).value_or(false) ? 0 : 1;
    }
    boost::ut::suite<"public_shards">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
