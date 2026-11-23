export module configuration;
import utility;
import constants;
import std;
import json;
import durability;
import async;
import host;
import toolchain;
export import language;
export import allocation;

export namespace config {
    struct not_found : std::invalid_argument, util::retained_exception {
        using std::invalid_argument::invalid_argument;
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto not_found::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    struct revision_conflict : std::runtime_error, util::retained_exception {
        std::uint64_t revision;
        std::string value;
        revision_conflict(std::uint64_t r, std::string v)
            : std::runtime_error("configuration changed underneath; rebase onto the current revision"), revision(r), value(std::move(v)) {}
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto revision_conflict::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    inline auto valid_name(std::string_view n) -> bool {
        if (n.empty() || n.size() > 64 || !std::isalnum(static_cast<unsigned char>(n.front())))
            return false;
        return std::ranges::all_of(n, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'; });
    }

    inline void check_name(std::string_view what, std::string_view n) {
        if (!valid_name(n))
            throw std::invalid_argument(std::format("bad {} name '{}': 1-64 of [A-Za-z0-9._-], starting with a letter or digit", what, n));
    }

    struct limits {
        std::optional<std::uint64_t> max_file_bytes;
        std::optional<std::uint64_t> max_project_bytes;
        std::optional<std::uint64_t> max_workspace_bytes;
        std::optional<std::uint32_t> max_projects;
        std::optional<std::uint32_t> builds_per_hour;
        std::optional<std::uint32_t> build_seconds_per_day;
        std::optional<std::uint32_t> build_seconds_per_month;
        std::optional<std::uint32_t> concurrent_builds;
        std::optional<std::uint32_t> read_max_lines;
        std::optional<std::uint64_t> read_max_bytes;
        std::optional<std::uint32_t> list_max_entries;
        std::optional<std::uint32_t> search_max_matches;

        friend auto operator==(const limits&, const limits&) -> bool = default;
    };

    using requirements = builds::requirements;
    using run_limits = builds::limits;

    struct run_configuration {
        std::string operation = "run";
        builds::parameters parameters;
        std::vector<std::string> configurations;
        builds::environment env;
        run_limits limits;
    };

    struct settings {
        std::vector<std::string> ignore;
        std::map<std::string, std::string> env;
        std::map<std::string, builds::configuration> build_configurations;
        std::map<std::string, run_configuration> run_configurations;
        std::vector<std::string> default_configurations;
        std::map<std::string, language::configuration> analysis;
        language::defaults analysis_defaults;
    };

    // A bearer token of this core: its SHA-256 hash, the name its writes carry, and the role it acts with.
    enum class auth_mode : std::uint8_t { token, trusted };
    enum class token_role : std::uint8_t { owner, member };

    struct token {
        std::string name;
        std::string hash;
        token_role role = token_role::owner;
        friend auto operator==(const token&, const token&) -> bool = default;
    };

    // `token` checks bearer tokens; `trusted` takes the caller from the headers a front on the private network sets.
    struct auth {
        auth_mode mode = auth_mode::token;
        std::string workspace;
        std::optional<std::string> front_token;
        std::vector<config::token> tokens;
        friend auto operator==(const auth&, const auth&) -> bool = default;
    };

    struct server {
        std::string http_bind = "127.0.0.1";
        std::uint16_t http_port = 8080;
        std::string worker_bind = "127.0.0.1";
        std::uint16_t worker_port = 9090;
        std::string remote_worker_bind = "127.0.0.1";
        std::uint16_t remote_worker_port = 0;
        bool remote_worker_enrollment = false;
        std::uint32_t overflow_wait_seconds = 0;
        std::string source = "127.0.0.1:9091";
        std::string storage = "state";
        std::vector<std::string> allowed_origins;
        std::uint64_t max_request_bytes = 8u << 20;
        config::auth auth;
    };

    inline auto public_server(server s) -> server {
        s.auth.front_token.reset();
        return s;
    }

    // Resource bounds for the workspace's execute child process.
    struct worker_backend {
        std::optional<std::uint64_t> max_memory_bytes;
        std::optional<std::uint32_t> max_cpu_seconds;
        friend auto operator==(const worker_backend&, const worker_backend&) -> bool = default;
    };

    inline void validate(const worker_backend& b) {
        if ((b.max_memory_bytes && !*b.max_memory_bytes) || (b.max_cpu_seconds && !*b.max_cpu_seconds))
            throw std::invalid_argument("worker resource limits must be positive");
    }

    struct revisions {
        std::uint64_t counter = 0;
        std::uint64_t server = 0;
        std::uint64_t limits = 0;
        std::uint64_t granted = 0;
        std::uint64_t runtime = 0;
        std::map<std::string, std::uint64_t, std::less<>> projects;
    };

    struct public_shard_publisher {
        std::string secret_key_env;
    };

    struct public_shard_settings {
        std::string endpoint, bucket;
        std::vector<std::string> trusted_publishers;
        std::optional<std::string> shared_cache, cache_root;
        std::uint64_t cache_bytes = 1ull << 30;
        std::optional<public_shard_publisher> publish;
    };

    // The configuration of this core: its one workspace and the projects in it.
    struct document {
        allocation::budget runtime;
        builds::toolset tools;
        // The SDKs a managed Worker maps into its toolchain, by name.
        std::map<std::string, toolchain::sdk> sdk_manifests;
        config::worker_backend worker_backend;
        config::server server;
        config::limits limits;
        std::map<std::string, config::settings, std::less<>> projects;
        std::optional<std::string> worker_token;
        // Limits set by whoever hosts this workspace; they cap its own limits.
        std::optional<config::limits> granted;
        config::revisions revisions;
        std::optional<public_shard_settings> public_shards;

        auto find(std::string_view project) const -> const config::settings* {
            auto p = projects.find(project);
            return p == projects.end() ? nullptr : &p->second;
        }
    };

    // The revision of a project; zero while it does not exist, the revision that creating one names.
    inline auto revision_of(const document& d, std::string_view project) -> std::uint64_t {
        if (auto it = d.revisions.projects.find(project); it != d.revisions.projects.end())
            return it->second;
        return 0;
    }

    struct effective_limits {
        std::uint64_t max_file_bytes = 4u << 20;
        std::uint64_t max_project_bytes = 1ull << 30;
        std::uint64_t max_workspace_bytes = 10ull << 30;
        std::uint32_t max_projects = 100;
        std::uint32_t builds_per_hour = 120;
        std::uint32_t build_seconds_per_day = 10 * 3600;
        std::uint32_t build_seconds_per_month = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t concurrent_builds = 4;
        std::uint32_t read_max_lines = 2000;
        std::uint64_t read_max_bytes = 256u << 10;
        std::uint32_t list_max_entries = 2000;
        std::uint32_t search_max_matches = 200;
    };

    struct effective_limits_run {
        std::uint32_t timeout_s = 1800;
        std::uint64_t output_limit = 1u << 20;
        std::optional<std::uint64_t> max_memory_bytes;
        std::optional<std::uint32_t> max_cpu_seconds;
    };

    struct effective_config {
        std::string name;
        builds::configuration build;
        json::named<"requires", std::optional<requirements>> needs;
        builds::environment env;
        effective_limits_run limits;
        bool enabled = true;
    };

    struct effective_run {
        std::string name;
        run_configuration spec;
        std::vector<std::string> configurations;
    };

    struct effective {
        std::string project;
        std::vector<std::string> ignore;
        std::map<std::string, std::string> env;
        std::map<std::string, effective_config, std::less<>> configs;
        std::map<std::string, effective_run, std::less<>> runs;
        std::vector<std::string> default_configurations;
        effective_limits limits;
        std::map<std::string, language::configuration> analysis;
        language::defaults analysis_defaults;

        auto config(std::string_view name) const -> const effective_config& {
            auto it = configs.find(name);
            if (it == configs.end())
                throw not_found(std::format("project '{}' has no build configuration '{}'", project, name));
            return it->second;
        }

        auto pick(const std::vector<std::string>& names, std::string_view operation) const -> std::vector<const effective_config*> {
            std::vector<const effective_config*> out;
            auto add = [&](const effective_config& c) {
                if (!std::ranges::contains(out, &c))
                    out.push_back(&c);
            };
            if (std::ranges::contains(names, std::string{"*"})) {
                for (auto& [_, c] : configs)
                    if (c.enabled && c.build.operations.contains(operation))
                        add(c);
            } else if (!names.empty()) {
                for (auto& n : names)
                    add(config(n));
            } else if (!default_configurations.empty()) {
                for (auto& n : default_configurations)
                    add(config(n));
            } else if (configs.size() == 1) {
                add(configs.begin()->second);
            }
            if (out.empty())
                throw std::invalid_argument(
                    std::format("project '{}': name the configurations to use ({} configured, no default_configurations)", project, configs.size()));
            return out;
        }

        auto run(std::string_view name) const -> const effective_run& {
            auto it = runs.find(name);
            if (it == runs.end())
                throw not_found(std::format("project '{}' has no run configuration '{}'", project, name));
            return it->second;
        }
    };

    namespace detail {
        template <class T> void over(T& into, const std::optional<T>& from) {
            if (from)
                into = *from;
        }

        inline void apply(effective_limits& e, const limits& l) {
            over(e.max_file_bytes, l.max_file_bytes);
            over(e.max_project_bytes, l.max_project_bytes);
            over(e.max_workspace_bytes, l.max_workspace_bytes);
            over(e.max_projects, l.max_projects);
            over(e.builds_per_hour, l.builds_per_hour);
            over(e.build_seconds_per_day, l.build_seconds_per_day);
            over(e.build_seconds_per_month, l.build_seconds_per_month);
            over(e.concurrent_builds, l.concurrent_builds);
            over(e.read_max_lines, l.read_max_lines);
            over(e.read_max_bytes, l.read_max_bytes);
            over(e.list_max_entries, l.list_max_entries);
            over(e.search_max_matches, l.search_max_matches);
        }

        // Holds each limit of `e` within the one `ceiling` names, if it names it.
        inline void cap(effective_limits& e, const limits& ceiling) {
            auto low = [](auto& value, const auto& most) {
                if (most)
                    value = std::min(value, *most);
            };
            low(e.max_file_bytes, ceiling.max_file_bytes);
            low(e.max_project_bytes, ceiling.max_project_bytes);
            low(e.max_workspace_bytes, ceiling.max_workspace_bytes);
            low(e.max_projects, ceiling.max_projects);
            low(e.builds_per_hour, ceiling.builds_per_hour);
            low(e.build_seconds_per_day, ceiling.build_seconds_per_day);
            low(e.build_seconds_per_month, ceiling.build_seconds_per_month);
            low(e.concurrent_builds, ceiling.concurrent_builds);
            low(e.read_max_lines, ceiling.read_max_lines);
            low(e.read_max_bytes, ceiling.read_max_bytes);
            low(e.list_max_entries, ceiling.list_max_entries);
            low(e.search_max_matches, ceiling.search_max_matches);
        }

        inline void apply(effective_limits_run& e, const run_limits& l) {
            over(e.timeout_s, l.timeout_s);
            over(e.output_limit, l.output_limit);
            if (l.max_memory_bytes)
                e.max_memory_bytes = l.max_memory_bytes;
            if (l.max_cpu_seconds)
                e.max_cpu_seconds = l.max_cpu_seconds;
        }

        template <class Map> auto with(std::map<std::string, std::string> env, const Map& over) -> std::map<std::string, std::string> {
            for (auto& [k, v] : over)
                env[k] = v;
            return env;
        }

        inline auto resolve(const settings& s, std::string project, const effective_limits& limits) -> effective {
            effective e{.project = std::move(project),
                        .ignore = s.ignore,
                        .env = s.env,
                        .limits = limits,
                        .analysis = s.analysis,
                        .analysis_defaults = s.analysis_defaults};
            auto where = e.project;
            for (auto& [name, build] : s.build_configurations) {
                effective_config c{.name = name, .build = build, .env = with(s.env, build.env), .enabled = build.enabled};
                c.build.env = c.env;
                apply(c.limits, build.limits);
                e.configs[name] = std::move(c);
            }
            e.default_configurations = s.default_configurations;
            for (auto& n : e.default_configurations) {
                if (!e.configs.contains(n))
                    throw std::invalid_argument(std::format("{}: default configuration '{}' is not configured", where, n));
                if (!e.configs.at(n).enabled)
                    throw std::invalid_argument(std::format("{}: default configuration '{}' is disabled", where, n));
            }
            for (auto& [name, r] : s.run_configurations) {
                effective_run run{.name = name, .spec = r, .configurations = r.configurations.empty() ? e.default_configurations : r.configurations};
                for (auto& n : run.configurations) {
                    if (!e.configs.contains(n))
                        throw std::invalid_argument(std::format("{}: run configuration '{}' uses '{}', which is not configured", where, name, n));
                    if (!e.configs.at(n).build.operations.contains(r.operation))
                        throw std::invalid_argument(
                            std::format("{}: run configuration '{}' runs '{}', which configuration '{}' does not offer", where, name, r.operation, n));
                }
                e.runs[name] = std::move(run);
            }
            return e;
        }
    } // namespace detail

    // The defaults, then the workspace's own limits, each capped by what the host granted.
    inline auto resolve_limits(const document& d) -> effective_limits {
        effective_limits l;
        detail::apply(l, d.limits);
        if (d.granted)
            detail::cap(l, *d.granted);
        return l;
    }

    inline auto resolve(const document& d, std::string_view project) -> effective {
        auto* s = d.find(project);
        if (!s)
            throw not_found(std::format("unknown project '{}'", project));
        return detail::resolve(*s, std::string{project}, resolve_limits(d));
    }

    inline void validate(const run_limits& l, std::string_view where) {
        if (l.timeout_s && *l.timeout_s == 0)
            throw std::invalid_argument(std::format("{}: timeout_s must be positive", where));
        if (l.max_memory_bytes && *l.max_memory_bytes == 0)
            throw std::invalid_argument(std::format("{}: max_memory_bytes must be positive or omitted", where));
        if (l.max_cpu_seconds && *l.max_cpu_seconds == 0)
            throw std::invalid_argument(std::format("{}: max_cpu_seconds must be positive or omitted", where));
    }

    inline void validate(const settings& s, std::string_view where) {
        builds::validate_environment(s.env);
        for (auto& [name, a] : s.analysis) {
            check_name("analysis", name);
            language::expand(a, s.build_configurations, {}, s.analysis_defaults);
        }
        for (auto& [name, c] : s.build_configurations) {
            check_name("build configuration", name);
            builds::validate(c);
            validate(c.limits, where);
        }
        for (auto& [name, r] : s.run_configurations) {
            check_name("run configuration", name);
            if (r.operation.empty())
                throw std::invalid_argument("run configuration has no operation");
            if (r.parameters.parallel && !*r.parameters.parallel)
                throw std::invalid_argument("run parallelism must be positive");
            validate(r.limits, where);
            builds::validate_environment(r.env);
        }
        for (auto& p : s.ignore)
            if (p.empty())
                throw std::invalid_argument(std::format("{}: empty ignore pattern", where));
    }

    inline void validate(const auth& a) {
        if (a.mode == auth_mode::trusted && a.workspace.empty())
            throw std::invalid_argument("server.auth.workspace is required in trusted mode");
        if (a.mode == auth_mode::trusted && (!a.front_token || a.front_token->empty()))
            throw std::invalid_argument("server.auth.front_token is required in trusted mode");
        std::set<std::string_view> names;
        for (auto& t : a.tokens) {
            check_name("token", t.name);
            if (!names.insert(t.name).second)
                throw std::invalid_argument(std::format("server.auth.tokens: '{}' is named twice", t.name));
            if (t.hash.size() != 64 ||
                !std::ranges::all_of(t.hash, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f'); }))
                throw std::invalid_argument(std::format("server.auth.tokens: '{}' needs a lowercase hex SHA-256 hash", t.name));
        }
    }

    inline void validate_sdk_keys(const std::map<std::string, toolchain::sdk>& manifests) {
        for (const auto& [key, _] : manifests)
            (void)toolchain::installation(key, true);
    }

    inline void validate(const document& d) {
        allocation::distribute(d.runtime);
        validate_sdk_keys(d.sdk_manifests);
        if (d.server.storage.empty())
            throw std::invalid_argument("server.storage is empty");
        validate(d.server.auth);
        if (d.server.overflow_wait_seconds > 600 ||
            (d.server.overflow_wait_seconds && (d.server.auth.mode != auth_mode::trusted || !d.worker_token || d.worker_token->empty())))
            throw std::invalid_argument("overflow waiting requires a hosted Core, a private Worker token, and a deadline of at most 600 seconds");
        io::parse_endpoint(d.server.http_bind, d.server.http_port);
        io::parse_endpoint(d.server.worker_bind, d.server.worker_port);
        if (d.server.remote_worker_port) {
            io::parse_endpoint(d.server.remote_worker_bind, d.server.remote_worker_port);
            if (d.server.auth.workspace.empty())
                throw std::invalid_argument("remote Worker listener needs a Core workspace identity");
            if (d.server.remote_worker_port == d.server.worker_port || d.server.remote_worker_port == d.server.http_port)
                throw std::invalid_argument("remote Worker listener needs a separate port");
        }
        validate(d.worker_backend);
        if (d.server.remote_worker_enrollment && !d.server.remote_worker_port)
            throw std::invalid_argument("remote Worker enrollment needs a private listener");
        io::parse_endpoint(d.server.source);
        for (auto& [name, p] : d.projects) {
            check_name("project", name);
            validate(p, name);
            resolve(d, name);
        }
    }

    template <class T> auto parse_as(std::string_view json, std::string_view what) -> T {
        auto out = json::read<T, json::defaults, json::strict>(json);
        if (!out)
            throw std::invalid_argument(std::format("{}: {}", what, out.error().what()));
        return std::move(*out);
    }

    inline auto parse(std::string_view json, std::optional<allocation::budget> self_hosted_runtime = {}) -> document {
        auto d = parse_as<document>(json, "config");
        if (self_hosted_runtime && d.server.auth.mode != auth_mode::trusted) {
            auto fields = parse_as<json::value>(json, "config").to_object();
            if (fields && !fields->get("runtime"))
                d.runtime = *self_hosted_runtime;
        }
        validate(d);
        return d;
    }

    template <class T> auto write(const T& value) -> std::string { return json::write(value); }

    inline auto load(const std::filesystem::path& file, std::optional<allocation::budget> self_hosted_runtime = {}) -> document {
        std::ifstream in{file, std::ios::binary};
        if (!in)
            throw std::runtime_error(std::format("config: cannot open {}", file.string()));
        std::string text{std::istreambuf_iterator<char>{in}, {}};
        return parse(text, self_hosted_runtime);
    }

    inline constexpr std::string_view runc = "io.containerd.runc.v2";
    inline constexpr std::string_view runsc = "io.containerd.runsc.v1";
    inline constexpr std::string_view kata = "io.containerd.kata.v2";

    struct host_limits {
        std::uint32_t cpu_millis = 0;
        std::uint64_t memory_bytes = 0, storage_bytes = 0;
        friend bool operator==(const host_limits&, const host_limits&) = default;
    };

    struct host_policy {
        std::vector<std::string> runtimes;
        bool exclusive = false;
        std::vector<std::string> devices;
        friend bool operator==(const host_policy&, const host_policy&) = default;
    };

    struct host_settings {
        std::string bind = "127.0.0.1";
        std::uint16_t port = 8090;
        std::string token, front_token, host_id, socket = "/run/containerd/containerd.sock";
        std::string name_space, storage, rootfs, toolchain_root, work_root, shared_cache;
        std::string volume_devices = "/dev/disk/by-id/";
        host_policy isolation;
        host_limits capacity;
        std::string network_prefix = "10.203";
        std::vector<std::string> gateway_sources, egress;
        bool remote_worker_enrollment = false;
        std::uint32_t mapped_id = 65534, pids = 512, timeout_seconds = 60, operation_timeout_seconds = 300;
        builds::toolset tools;
        friend bool operator==(const host_settings&, const host_settings&) = default;
    };

    struct host_document {
        host_settings host;
    };

    inline auto private_ipv4(net::ip::address_v4 address) -> bool {
        const auto bytes = address.to_bytes();
        return bytes[0] == 10 || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
               (bytes[0] == 192 && bytes[1] == 168);
    }

    inline auto private_listener(net::ip::address address) -> bool {
        if (address.is_v4()) {
            const auto ipv4 = address.to_v4();
            return ipv4.is_loopback() || private_ipv4(ipv4);
        }
        const auto ipv6 = address.to_v6();
        return ipv6.is_loopback() || (ipv6.to_bytes().front() & 0xfe) == 0xfc;
    }

    inline auto tcp_egress(std::string_view authority) -> std::pair<std::string_view, unsigned> {
        const auto colon = authority.find(':');
        if (colon == authority.npos)
            throw std::invalid_argument("egress endpoint requires host:port");
        const auto name = authority.substr(0, colon), port_text = authority.substr(colon + 1);
        unsigned port = 0;
        const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (error != std::errc{} || end != port_text.data() + port_text.size() || !port || port > 65535 || name.empty() ||
            !std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                                          (c >= '0' && c <= '9') || c == '-' || c == '.'; }))
            throw std::invalid_argument("invalid TCP egress endpoint");
        return {name, port};
    }

    inline void validate(const host_settings& h) {
        if (!h.port || !private_listener(io::parse_endpoint(h.bind, h.port).address()))
            throw std::invalid_argument("host.bind requires a private or loopback address and a positive port");
        const auto valid_token = [](std::string_view token) {
            return !token.empty() && std::ranges::all_of(token, [](unsigned char c) { return c > 32 && c < 127; });
        };
        if (!valid_token(h.token) || !valid_token(h.front_token) || h.token == h.front_token)
            throw std::invalid_argument("host.token and host.front_token require distinct bearer secrets");
        check_name("host ID", h.host_id);
        if (h.name_space.empty() || h.name_space.size() > 76 || h.name_space.front() == '-' || h.name_space.back() == '-' ||
            !std::ranges::all_of(h.name_space, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
            throw std::invalid_argument("host.name_space requires lowercase letters, digits and interior hyphens");
        const auto path = [](std::string_view value) {
            return !value.contains(char{}) && std::filesystem::path{value}.is_absolute();
        };
        if (!path(h.socket) || !path(h.storage) || !path(h.rootfs) || !path(h.toolchain_root) || !path(h.work_root) ||
            (!h.shared_cache.empty() && !path(h.shared_cache)))
            throw std::invalid_argument("host socket and directories require absolute paths");
        const std::filesystem::path devices{h.volume_devices};
        if (!path(h.volume_devices) || devices == devices.root_path() || devices != devices.lexically_normal())
            throw std::invalid_argument("host.volume_devices requires a normalized absolute directory");
        std::error_code error;
        const auto prefix = net::ip::make_address_v4(h.network_prefix + ".0.0", error);
        if (error || !private_ipv4(prefix))
            throw std::invalid_argument("host.network_prefix requires a private IPv4 /16 prefix");
        for (const auto& source : h.gateway_sources) {
            net::ip::make_address_v4(source, error);
            if (error || source.starts_with(h.network_prefix + "."))
                throw std::invalid_argument("host.gateway_sources requires IPv4 addresses outside the workspace prefix");
        }
        for (const auto& endpoint : h.egress)
            tcp_egress(endpoint);
        if (!h.capacity.cpu_millis || !h.capacity.memory_bytes || h.capacity.storage_bytes < constants::host_minimum_storage ||
            !h.mapped_id || !h.pids || !h.timeout_seconds || !h.operation_timeout_seconds)
            throw std::invalid_argument("host capacity, mapped ID, PID limit, and timeouts must be positive and storage at least 16 MiB");
        if (h.isolation.runtimes.empty() || !std::ranges::all_of(h.isolation.runtimes, [&](std::string_view runtime) {
                return runtime == runsc || runtime == kata || (runtime == runc && h.isolation.exclusive);
            }))
            throw std::invalid_argument("host isolation requires supported runtimes and an exclusive host for runc");
        for (const auto& device : h.isolation.devices)
            if (!device.starts_with("/dev/") || !path(device) || std::filesystem::path{device}.lexically_normal() != device)
                throw std::invalid_argument("host isolation devices must name paths under /dev");
        for (const auto& [name, file] : h.tools)
            if (name.empty() || !path(file))
                throw std::invalid_argument("host tools require a name and an absolute container path");
    }

    template <class T> auto read_host_file(const std::filesystem::path& file, std::string_view what, std::uint64_t limit = 1u << 20) -> T {
        std::string text;
        for (auto bytes : durability::read_private(file, limit))
            text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return parse_as<T>(text, what);
    }

    inline auto load_host(const std::filesystem::path& file) -> host_settings {
        auto settings = read_host_file<host_document>(file, "host settings").host;
        validate(settings);
        for (auto* directory : {&settings.storage, &settings.rootfs, &settings.toolchain_root, &settings.work_root, &settings.shared_cache})
            if (!directory->empty())
                *directory = std::filesystem::weakly_canonical(*directory).string();
        namespace fs = std::filesystem;
        const auto contains = [](const fs::path& parent, const fs::path& child) {
            return std::ranges::mismatch(parent, child).in1 == parent.end();
        };
        for (const auto& directory : {settings.rootfs, settings.toolchain_root, settings.shared_cache})
            if (!directory.empty() && (contains(directory, settings.storage) ||
                contains(directory, settings.work_root) || contains(settings.work_root, directory)))
                throw std::invalid_argument("host image, toolchain, and cache must be separate from the work root and cannot contain host storage");
        return settings;
    }

    struct source {
        std::string bind = "127.0.0.1";
        std::uint16_t port = 9091;
        std::string storage = "storage";
        std::uint32_t disk_threads = allocation::plan{}.source_disk;
    };

    inline void validate(const source& s) {
        if (s.disk_threads < allocation::plan{}.source_disk)
            throw std::invalid_argument("source requires at least two disk threads");
        io::parse_endpoint(s.bind, s.port);
        if (s.storage.empty())
            throw std::invalid_argument("source.storage is empty");
    }

    using worker_role = json::literal<"execute", "analyze">;
    inline constexpr auto execute_role = worker_role::make<"execute">();
    inline constexpr auto analyze_role = worker_role::make<"analyze">();

    struct worker {
        std::string gateway = "127.0.0.1:9090";
        std::optional<std::string> front_token;
        std::optional<std::string> worker_id;
        std::optional<std::string> workspace_id;
        std::uint32_t heartbeat_seconds = 15;
        std::uint32_t reconnect_attempts = 30;
        std::optional<std::string> token;
        std::string name;
        std::string data_root = std::string{host::default_data_root};
        std::string work_root = "work";
        std::uint32_t compute_threads = 1;
        std::uint32_t parallel = 1;
        std::optional<std::uint64_t> memory_bytes;
        std::uint32_t max_inflight = 4;
        // Analyses that read project data run beside the builds, on their own limit.
        std::uint32_t max_analyses = 4;
        std::vector<worker_role> roles{execute_role, analyze_role};
        std::uint64_t max_cache_bytes = 20ull << 30;
        std::uint32_t tree_ttl_days = 14;
        std::map<std::string, std::string> env;
        bool inherit_env = true;
        builds::toolset tools;
        config::worker_backend backend;
        bool execute_stdio = false;
        std::map<std::string, toolchain::sdk> sdk_manifests;
        std::optional<std::string> dependency_cache;
        std::optional<std::string> dependency_upstream;
        std::optional<std::string> dependency_publish;
        std::optional<builds::dependency_recipe> dependency_recipe;
    };

    inline constexpr std::string_view worker_tunnel_scheme = "ws://";
    inline auto worker_gateway_address(std::string_view gateway) -> std::string_view {
        if (!gateway.starts_with(worker_tunnel_scheme))
            return gateway;
        gateway.remove_prefix(worker_tunnel_scheme.size());
        if (!gateway.ends_with(constants::worker_tunnel_path))
            throw std::invalid_argument("Worker WebSocket gateway must use /workers/tunnel");
        return gateway.substr(0, gateway.size() - constants::worker_tunnel_path.size());
    }

    inline auto parse_worker(std::string_view text) -> worker {
        worker w;
        try {
            w = parse_as<worker>(text, "Worker configuration");
        } catch (const std::exception&) {
            throw std::invalid_argument("Worker configuration is invalid");
        }
        validate_sdk_keys(w.sdk_manifests);
        if (!w.execute_stdio)
            io::parse_endpoint(worker_gateway_address(w.gateway));
        if (w.work_root.empty())
            throw std::invalid_argument("Worker work_root is empty");
        if (w.dependency_cache && (w.dependency_cache->empty() || !builds::absolute_path(*w.dependency_cache)))
            throw std::invalid_argument("Worker dependency_cache must be an absolute path");
        if (w.dependency_upstream && (w.dependency_upstream->empty() || !builds::absolute_path(*w.dependency_upstream)))
            throw std::invalid_argument("Worker dependency_upstream must be an absolute path");
        if (w.dependency_publish && (w.dependency_publish->empty() || !builds::absolute_path(*w.dependency_publish)))
            throw std::invalid_argument("Worker dependency_publish must be an absolute path");
        if (w.dependency_publish && w.dependency_upstream)
            throw std::invalid_argument("Worker dependency_publish already supplies the upstream cache");
        if (w.dependency_publish.has_value() != w.dependency_recipe.has_value())
            throw std::invalid_argument("Worker dependency_publish and dependency_recipe must be configured together");
        if (w.dependency_recipe)
            builds::validate(*w.dependency_recipe);
        if (w.compute_threads == 0 || w.parallel == 0)
            throw std::invalid_argument("worker thread and parallel budgets must be positive");
        if (w.memory_bytes && !*w.memory_bytes)
            throw std::invalid_argument("Worker memory_bytes must be positive");
        if (w.max_inflight == 0 || w.max_analyses == 0)
            throw std::invalid_argument("Worker max_inflight and max_analyses must be positive");
        if (w.roles.empty())
            throw std::invalid_argument("Worker roles are execute, analyze or both");
        if (w.dependency_publish && !std::ranges::contains(w.roles, execute_role))
            throw std::invalid_argument("a dependency publisher takes the execute role");
        if (w.tree_ttl_days == 0)
            throw std::invalid_argument("Worker tree_ttl_days must be positive");
        validate(w.backend);
        if (w.execute_stdio && (w.worker_id || w.workspace_id || w.roles != std::vector<worker_role>{execute_role}))
            throw std::invalid_argument("execute_stdio requires an unregistered execute role");
        if (w.worker_id || w.workspace_id) {
            if (!w.worker_id || !w.workspace_id || !w.token || w.token->size() != 64)
                throw std::invalid_argument("Registered Worker requires its identity and credentials");
            check_name("Worker identity", *w.worker_id);
            check_name("Workspace identity", *w.workspace_id);
            if (!w.heartbeat_seconds || w.heartbeat_seconds > 30 || !w.reconnect_attempts || w.reconnect_attempts > 120 || w.max_inflight > 64 ||
                w.max_analyses > 64)
                throw std::invalid_argument("Registered Worker heartbeat, reconnect, or inflight limit is invalid");
        }
        return w;
    }

    inline auto load_worker(const std::filesystem::path& file, bool private_file = false) -> worker {
        auto read = [&](bool secret) {
            std::string text;
            try {
                for (auto bytes : secret ? durability::read_private(file, 1u << 20) : durability::read_plain(file, 1u << 20))
                    text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            } catch (const durability::unreadable&) {
                throw std::runtime_error(secret ? "Worker credentials must be an owner-only regular file" : "cannot read Worker configuration");
            }
            return text;
        };
        auto w = parse_worker(read(private_file));
        if (!private_file && w.worker_id)
            w = parse_worker(read(true));
        return w;
    }

    namespace detail {
        template <class Json> void check_revision(std::uint64_t current, std::uint64_t expected, Json current_json) {
            if (current != expected)
                throw revision_conflict(current, current_json());
        }

        inline void bump(document& d, std::uint64_t& section) { section = ++d.revisions.counter; }
    } // namespace detail

    class store {
    public:
        using snapshot = std::shared_ptr<const document>;

        store(document d, std::optional<std::filesystem::path> file) : current_(std::make_shared<const document>(std::move(d))), file_(std::move(file)) {}

        auto current() const -> snapshot { return current_; }

        void put_limits(limits l, std::uint64_t expected) {
            change([&](document& d) {
                detail::check_revision(d.revisions.limits, expected, [&] { return write(d.limits); });
                d.limits = std::move(l);
                detail::bump(d, d.revisions.limits);
            });
        }

        void put_granted(std::optional<limits> g, std::uint64_t expected) {
            change([&](document& d) {
                detail::check_revision(d.revisions.granted, expected, [&] { return write(d.granted); });
                d.granted = std::move(g);
                detail::bump(d, d.revisions.granted);
            });
        }

        void put_runtime(allocation::budget value, std::uint64_t expected) {
            change([&](document& d) {
                detail::check_revision(d.revisions.runtime, expected, [&] { return write(d.runtime); });
                allocation::distribute(value);
                if (d.runtime != value) {
                    d.runtime = value;
                    detail::bump(d, d.revisions.runtime);
                }
            });
        }

        void put_server(server s, std::uint64_t expected) {
            change([&](document& d) {
                detail::check_revision(d.revisions.server, expected, [&] { return write(public_server(d.server)); });
                d.server = std::move(s);
                detail::bump(d, d.revisions.server);
            });
        }

        void put_project(const std::string& name, settings s, std::uint64_t expected) {
            change([&](document& d) {
                check_name("project", name);
                bool exists = d.projects.contains(name);
                detail::check_revision(revision_of(d, name), expected, [&] { return exists ? write(d.projects.at(name)) : std::string{"{}"}; });
                auto limit = resolve_limits(d).max_projects;
                if (!exists && d.projects.size() >= limit)
                    throw std::invalid_argument(std::format("the workspace is at its limit of {} projects", limit));
                d.projects[name] = std::move(s);
                detail::bump(d, d.revisions.projects[name]);
            });
        }

        void remove_project(const std::string& name) {
            change([&](document& d) {
                if (!d.projects.erase(name))
                    throw not_found(std::format("unknown project '{}'", name));
                d.revisions.projects.erase(name);
            });
        }

    private:
        template <class Edit> void change(Edit edit) {
            auto next = *current_;
            edit(next);
            validate(next);
            if (file_)
                save(next);
            current_ = std::make_shared<const document>(std::move(next));
        }

        void save(const document& d) const {
            auto text = write(d);
            if (d.server.auth.front_token)
                durability::durable_private_write(*file_, text);
            else
                durability::durable_write(*file_, text, false, ".tmp");
        }

        snapshot current_;
        std::optional<std::filesystem::path> file_;
    };

    // One snapshot per core, so a core reads its configuration without a lock. The owner on core 0 publishes a new snapshot to each core
    // on that core; a reader keeps the pointer it took for as long as it needs that version.
    class slots {
    public:
        slots(std::size_t cores, store::snapshot first) : slots_(cores, std::move(first)) {}

        auto slot(std::size_t core) const -> const store::snapshot* { return &slots_[core]; }
        auto current(std::size_t core) const -> store::snapshot { return slots_[core]; }
        void set(std::size_t core, store::snapshot snapshot) { slots_[core] = std::move(snapshot); }

    private:
        std::vector<store::snapshot> slots_;
    };
    // Field descriptions are shared by generated configuration schemas.
    inline auto field_descriptions() -> const std::map<std::string, std::string>& {
        static const std::map<std::string, std::string> descriptions{
            {"max_file_bytes", "Maximum bytes per source file; unsigned integer, 0 allowed. Example: 4194304."},
            {"max_project_bytes", "Maximum stored bytes per project; unsigned integer. Example: 1073741824."},
            {"max_workspace_bytes", "Maximum stored bytes of all projects, history included; unsigned integer. Example: 10737418240."},
            {"max_projects", "Maximum projects; unsigned integer. Example: 100."},
            {"builds_per_hour", "Maximum build requests per rolling hour per project; unsigned integer. Example: 120."},
            {"build_seconds_per_day", "Daily build time budget in seconds; unsigned integer. Example: 36000."},
            {"build_seconds_per_month", "Monthly build time budget in seconds; unsigned integer. Example: 360000."},
            {"concurrent_builds", "Maximum concurrent builds; unsigned integer. Example: 4."},
            {"read_max_lines", "Maximum lines per file read; unsigned integer. Example: 2000."},
            {"read_max_bytes", "Maximum bytes per file read; unsigned integer. Example: 262144."},
            {"list_max_entries", "Maximum directory entries returned; unsigned integer. Example: 2000."},
            {"search_max_matches", "Maximum search matches returned; unsigned integer. Example: 200."},
            {"http_bind", "HTTP listen address. Example: 127.0.0.1."},
            {"http_port", "HTTP listen port, integer 0-65535. Example: 8080."},
            {"worker_bind", "Worker listen address. Example: 127.0.0.1."},
            {"worker_port", "Worker listen port, integer 0-65535. Example: 9090."},
            {"remote_worker_bind", "Private WebSocket tunnel listen address. Example: 127.0.0.1."},
            {"remote_worker_port", "Private WebSocket tunnel port. Set to 0 to disable the listener."},
            {"source", "Source service host and port. Example: 127.0.0.1:9091."},
            {"storage", "Nonempty gateway storage directory. Example: state."},
            {"allowed_origins", "Allowed browser origins, exact strings. Example: [https://console.example.com]."},
            {"max_request_bytes", "Maximum HTTP request body bytes; unsigned integer. Example: 8388608."},
            {"auth", "How callers authenticate; the next request uses the saved value."},
            {"mode", "token checks caller bearer tokens. trusted checks the front bearer token and reads caller identity from front headers."},
            {"front_token", "Shared bearer secret required in trusted mode. Set it in the Core configuration file and the front gateway."},
            {"tokens", "Bearer tokens: name, lowercase hex SHA-256 hash of the token, role owner or member."},
            {"hash", "Lowercase hex SHA-256 hash of a bearer token."},
            {"role", "owner or member."},
            {"ignore", "Nonempty source ignore patterns. Example: [build, *.o]."},
            {"env", "Environment variable map; names and values must pass environment validation. Example: {CC: clang}."},
            {"build_configurations", "Named build configurations; names use 1-64 letters, digits, dots, underscores or hyphens, starting with a letter or "
                                     "digit. Example key: Debug."},
            {"run_configurations", "Named run configurations selecting a build operation and configurations. Example key: app."},
            {"default_configurations", "Existing enabled build configuration names used when no selection is supplied. Example: [Debug]."},
            {"analysis", "Named language analysis configurations. Example key: Cpp."},
            {"configurations", "Existing build configuration names; empty uses project defaults. Example: [Debug]."},
            {"operation", "Operation name provided by the selected build or adapter. Examples: build, run, prepare."},
            {"parameters", "Operation parameters; only parameters supported by the selected operation may be supplied."},
            {"limits", "Resource limits; omitted fields retain the configured default or parent value."},
            {"builders", "Named adapter instances. Example key: main."},
            {"adapter", "Registered build adapter ID. Examples: command, cmake."},
            {"options", "Settings of the selected adapter or language server provider."},
            {"requires", "Required executable tools and operating system/architecture constraints."},
            {"tasks", "Named task graph or operation task names; dependencies must form an acyclic graph."},
            {"builder", "Existing builder name. Example: main."},
            {"after", "Existing task names to run before this task. Example: [configure]."},
            {"inputs", "In a task, input names mapped to task output references, such as {binary_dir: {task: build, output: build_tree}}; in an analysis, the "
                       "build outputs its language server reads, such as [compile_commands]."},
            {"outputs", "Named output references. Example: {binary: {task: build, output: executable}}."},
            {"operations", "Named operations exposing graph tasks. Example: {build: {tasks: [build]}}."},
            {"receiver", "Task receiving operation parameters; must be in the selected operation graph."},
            {"task", "Existing producer task name. Example: build."},
            {"output", "Producer output name. Example: build_tree."},
            {"enabled", "Whether a build configuration is selectable by defaults or wildcard; boolean. Example: true."},
            {"tools", "Required executable names. Example: [clang, cmake]."},
            {"platform", "Optional operating system and architecture constraints."},
            {"os", "Required operating system identifier. Examples: linux, windows, darwin."},
            {"arch", "Required architecture identifier. Examples: x86_64, aarch64."},
            {"timeout_s", "Wall time limit in seconds; positive integer. Example: 1800."},
            {"output_limit", "Maximum captured output bytes; unsigned integer. Example: 1048576."},
            {"max_memory_bytes", "Memory limit in bytes; positive integer or omitted. Example: 1073741824."},
            {"max_cpu_seconds", "CPU time limit in seconds; positive integer or omitted. Example: 600."},
            {"targets", "Build targets requested from the operation. Example: [app]."},
            {"args", "Arguments appended to supported operations. Example: [--verbose]."},
            {"filter", "Test name filter supported by the adapter. Example: smoke."},
            {"labels", "Test label filter supported by the adapter. Example: unit."},
            {"parallel", "Requested parallel jobs; positive integer. Example: 2."},
            {"provider", "Language server provider ID. Example: clice."},
            {"cache_dir", "Project-relative directory for language server caches; omitted, a clice directory beside the first input. Example: build/clice."},
            {"build", "Build graph producing language analysis inputs."},
            {"server", "Language server provider and its options, or gateway server settings, according to the enclosing object."},
            {"argv", "Executable and arguments. Example: [make, -j4]."},
            {"value", "Complete replacement section value; read the section and preserve fields that should remain."},
            {"body", "REST request body; section saves require revision and value."},
        };
        return descriptions;
    }

    inline auto annotate(json::value value) -> json::value {
        if (auto object = value.to_object()) {
            auto out = *object;
            for (auto& [key, child] : out) {
                child = annotate(std::move(child));
                if (key == "properties") {
                    auto properties = child.to_object().value();
                    for (auto& [name, property] : properties) {
                        if (auto found = field_descriptions().find(name); found != field_descriptions().end()) {
                            auto fields = property.to_object().value();
                            fields["description"] = found->second;
                            property = fields;
                        }
                    }
                    child = properties;
                }
            }
            return out;
        }
        if (auto array = value.to_array()) {
            auto out = *array;
            for (auto& child : out)
                child = annotate(std::move(child));
            return out;
        }
        return value;
    }

    template <class T> auto schema() -> json::value { return annotate(json::schema<T>()); }

} // namespace config
