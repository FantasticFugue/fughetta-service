module worker;
import std;
import native;
import coroutine;
import signals;
import runtime;
import configuration;
import reflection;
import durability;
import enrollment;
import host;
import resolver;
import tls;
import httpant.tls;
import http;

namespace {
    struct join_options {
        std::string gateway, workspace, code, name = host::name(), ca_file;
        std::filesystem::path config, data_root, work_root;
        builds::toolset tools;
        std::uint32_t threads = 1, parallel = 1, inflight = 4;
        bool run = false;
    };
    auto number(std::string_view value) -> std::uint32_t {
        std::uint32_t result = 0;
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || !result)
            throw std::invalid_argument("Worker resource limits must be positive integers");
        return result;
    }
    auto default_config(const std::string& workspace) -> std::filesystem::path {
#ifdef _WIN32
        auto base = std::getenv("LOCALAPPDATA");
        if (!base || !*base)
            throw std::invalid_argument("--config is required when LOCALAPPDATA is absent");
        return std::filesystem::u8path(base) / "Fughetta" / "workers" / workspace / "credentials.json";
#else
        if (auto state = std::getenv("XDG_STATE_HOME"); state && *state)
            return std::filesystem::path{state} / "fughetta" / "workers" / workspace / "credentials.json";
        auto home = std::getenv("HOME");
        if (!home || !*home)
            throw std::invalid_argument("--config is required when HOME is absent");
        return std::filesystem::path{home} / ".local" / "state" / "fughetta" / "workers" / workspace / "credentials.json";
#endif
    }
    auto parse_join(int argc, char** argv) -> join_options {
        join_options out;
        for (int i = 2; i < argc; ++i) {
            std::string_view key = argv[i];
            if (key == "--run") {
                out.run = true;
                continue;
            }
            if (++i >= argc)
                throw std::invalid_argument("Worker join option needs a value");
            std::string value = argv[i];
            if (key == "--gateway")
                out.gateway = std::move(value);
            else if (key == "--workspace")
                out.workspace = std::move(value);
            else if (key == "--code")
                out.code = std::move(value);
            else if (key == "--name")
                out.name = std::move(value);
            else if (key == "--config")
                out.config = std::move(value);
            else if (key == "--ca-file")
                out.ca_file = std::move(value);
            else if (key == "--data-root")
                out.data_root = std::move(value);
            else if (key == "--work-root")
                out.work_root = std::move(value);
            else if (key == "--compute-threads")
                out.threads = number(value);
            else if (key == "--parallel")
                out.parallel = number(value);
            else if (key == "--max-inflight")
                out.inflight = number(value);
            else if (key == "--tool") {
                auto equal = value.find('=');
                if (equal == std::string::npos || !equal || equal + 1 == value.size())
                    throw std::invalid_argument("--tool needs NAME=PATH");
                auto name = value.substr(0, equal);
                config::check_name("tool", name);
                out.tools[name] = value.substr(equal + 1);
            } else
                throw std::invalid_argument("unknown Worker join option");
        }
        config::check_name("Workspace", out.workspace);
        if (out.code.size() != 32 || !std::ranges::all_of(out.code, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw std::invalid_argument("Worker pairing code is invalid");
        if (!out.gateway.starts_with("https://"))
            throw std::invalid_argument("Worker join gateway must use https://");
        while (out.gateway.ends_with('/'))
            out.gateway.pop_back();
        if (std::string_view{out.gateway}.substr(8).contains('/'))
            throw std::invalid_argument("Worker join gateway must be an HTTPS origin");
        if (out.config.empty())
            out.config = default_config(out.workspace);
        out.config = std::filesystem::absolute(out.config);
        if (std::filesystem::exists(out.config) || std::filesystem::is_symlink(out.config))
            throw std::invalid_argument("Worker configuration already exists");
        if (out.data_root.empty())
            out.data_root = out.config.parent_path() / "data";
        if (out.work_root.empty())
            out.work_root = out.config.parent_path() / "work";
        out.data_root = std::filesystem::absolute(out.data_root);
        out.work_root = std::filesystem::absolute(out.work_root);
        if (!out.ca_file.empty())
            out.ca_file = std::filesystem::absolute(out.ca_file).string();
        if (out.inflight > 64)
            throw std::invalid_argument("Worker max-inflight must be at most 64");
        return out;
    }

    auto redeem(native::scheduler& io, const join_options& options, const tls::address& target) -> coroutine::task<enrollment::credential> {
        auto endpoint = co_await resolver::resolve(io, target.host, static_cast<std::uint16_t>(std::stoul(target.port)));
        auto socket = co_await io::connect(io, endpoint);
        http::tls::context context{false};
        context.trust(options.ca_file);
        http::tls::stream secure{web::stream{socket}, context};
        secure.hostname(target.host);
        co_await secure.handshake();
        http::v1::client client{secure};
        struct request {
            std::string code, name;
        };
        auto body = rfl::json::write(request{options.code, options.name});
        http::request head{.method = http::method::POST,
                           .target = target.target,
                           .fields = {{"host", target.authority}, {"content-type", "application/json"}, {"connection", "close"}}};
        auto response = co_await http::coroutine::fetch(client, std::move(head), std::string_view{body}, http::fetch_options{.body_limit = 4096});
        co_await secure.close();
        if (response.head.status != 201)
            throw std::runtime_error(std::format("Worker enrollment was refused (HTTP {})", response.head.status.code()));
        try {
            co_return config::parse_as<enrollment::credential>(response.text(), "Worker credentials");
        } catch (const std::exception&) {
            throw std::runtime_error("Worker enrollment response is invalid");
        }
    }

    auto join(const join_options& options) -> std::filesystem::path {
        durability::dir_lock exclusive{options.config.parent_path(), "Worker enrollment"};
        if (std::filesystem::exists(options.config) || std::filesystem::is_symlink(options.config))
            throw std::invalid_argument("Worker configuration already exists");
        auto origin = std::string{"wss://"} + options.gateway.substr(8);
        auto target = tls::address::parse(origin + "/w/" + options.workspace + "/api/v1/worker-enrollments/redeem");
        config::worker saved;
        cores::io_context<runtime::context> context{1};
        cores::signal_watch signals{context};
        context.run([&](native::scheduler& io, std::size_t) -> coroutine::task<> {
            auto result = co_await coroutine::deadline(io, std::chrono::seconds{30}, redeem(io, options, target));
            if (!result)
                throw std::runtime_error("Worker enrollment timed out; its code may have been consumed");
            saved.gateway = origin + "/w/" + options.workspace + "/workers/tunnel";
            saved.ca_file = options.ca_file;
            saved.worker_id = result->worker_id;
            saved.workspace_id = result->workspace_id;
            saved.token = result->secret;
            saved.name = options.name;
            saved.data_root = options.data_root.string();
            saved.work_root = options.work_root.string();
            saved.compute_threads = options.threads;
            saved.parallel = options.parallel;
            saved.max_inflight = options.inflight;
            saved.tools = options.tools;
            saved = config::parse_worker(config::write(saved));
        });
        durability::durable_directories(options.config.parent_path());
        durability::durable_private_write(options.config, config::write(saved));
        std::println("Worker {} joined workspace {}. Configuration: {}", *saved.worker_id, *saved.workspace_id, options.config.string());
        return options.config;
    }
} // namespace

int worker_cli(int argc, char** argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument("use worker join or worker run --config PATH");
        std::filesystem::path file;
        bool run = true;
        if (std::string_view{argv[1]} == "join") {
            auto options = parse_join(argc, argv);
            file = join(options);
            run = options.run;
        } else if (std::string_view{argv[1]} == "run" && argc == 4 && std::string_view{argv[2]} == "--config")
            file = argv[3];
        else
            throw std::invalid_argument("use worker join or worker run --config PATH");
        if (!run)
            return 0;
        auto path = file.string();
        std::array<char*, 2> arguments{argv[0], path.data()};
        return run_worker(static_cast<int>(arguments.size()), arguments.data(), true);
    } catch (const std::exception& error) {
        logging::error(std::format("worker: {}", error.what()));
        return 1;
    }
}
