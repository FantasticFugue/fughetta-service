export module publiccache;
import std;
import content;
import configuration;
import publicrecord;
import records;
import json;
import constants;
import durability;
import coroutine;
import native;
import resolver;
import http;
import httpant.tls;

export namespace public_shards {
    struct credentials {
        std::string id, secret;
        static auto environment(const char* id_name, const char* secret_name) -> credentials {
            const auto* id = std::getenv(id_name);
            const auto* secret = std::getenv(secret_name);
            if (bool(id && *id) != bool(secret && *secret))
                throw std::invalid_argument("R2 credentials require both environment variables");
            return {id ? id : "", secret ? secret : ""};
        }
        explicit operator bool() const { return !id.empty(); }
    };

    struct signed_request {
        std::string canonical, authorization;
    };
    inline auto sigv4(std::string_view method, std::string_view path, const std::map<std::string, std::string>& headers, std::string_view region,
                      const credentials& key) -> signed_request {
        std::string fields, names;
        for (const auto& [name, value] : headers) {
            fields += name + ":" + value + "\n";
            if (!names.empty())
                names += ";";
            names += name;
        }
        const auto& time = headers.at("x-amz-date");
        const auto date = time.substr(0, 8);
        const auto scope = std::format("{}/{}/s3/aws4_request", date, region);
        auto canonical = std::format("{}\n{}\n\n{}\n{}\n{}", method, path, fields, names, headers.at("x-amz-content-sha256"));
        auto signing = std::format("AWS4-HMAC-SHA256\n{}\n{}\n{}", time, scope, content::hash_of(canonical).view());
        auto mac = [](std::span<const unsigned char> secret, std::string_view message) {
            return content::hmac_sha256({reinterpret_cast<const char*>(secret.data()), secret.size()}, message);
        };
        auto signature = mac(mac(mac(mac(content::hmac_sha256("AWS4" + key.secret, date), region), "s3"), "aws4_request"), signing);
        return {std::move(canonical), std::format("AWS4-HMAC-SHA256 Credential={}/{},SignedHeaders={},Signature={}",
                                                 key.id, scope, names, content::hex_encode(signature))};
    }

    class bucket {
        std::string scheme_, authority_, host_, prefix_;
        std::uint16_t port_;
        static constexpr auto limit = constants::semantic_object_bytes + (1u << 20);

        auto transfer(native::scheduler& io, http::method method, std::string key, std::string_view bytes, const credentials& auth, bool create)
            const -> coroutine::task<web::reply> {
            const auto path = prefix_ + key;
            const auto address = co_await resolver::resolve(io, host_, port_);
            auto socket = co_await io::connect(io, address);
            web::stream transport{socket};
            std::map<std::string, std::string> fields{{"host", authority_}};
            if (create)
                fields.emplace("if-none-match", "*");
            if (auth) {
                fields.emplace("x-amz-content-sha256", content::hash_of(bytes).str());
                const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                fields.emplace("x-amz-date", std::format("{:%Y%m%dT%H%M%SZ}", now));
                fields.emplace("authorization", sigv4(method.token(), path, fields, "auto", auth).authorization);
            }
            http::request request{.method = method, .target = path, .scheme = scheme_, .authority = authority_};
            for (const auto& [name, value] : fields)
                if (name != "host")
                    request.fields.push_back({name, value});
            auto fetch = [&](auto& stream) -> coroutine::task<web::reply> {
                http::v1::client client{stream};
                auto response = co_await http::coroutine::fetch(client, std::move(request), bytes, {.body_limit = limit});
                co_return web::reply{std::move(response.head), std::string{response.text()}};
            };
            if (scheme_ == "https") {
                http::tls::context context{false};
                context.trust("");
                http::tls::stream session{transport, context, "http/1.1"};
                session.hostname(host_);
                co_await session.handshake(false, "http/1.1");
                auto reply = co_await fetch(session);
                co_await session.close();
                co_return reply;
            }
            co_return co_await fetch(transport);
        }
        auto request(native::scheduler io, http::method method, std::string key, std::string_view body, const credentials& auth, bool create = false)
            const -> coroutine::task<web::reply> {
            auto result = co_await coroutine::deadline(io, std::chrono::seconds{5}, transfer(io, method, std::move(key), body, auth, create));
            if (!result)
                throw std::runtime_error("public R2 request timed out");
            co_return std::move(*result);
        }
    public:
        bucket(std::string_view endpoint, std::string_view name) {
            const auto marker = endpoint.find("://");
            if (marker == std::string_view::npos || name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != name.npos)
                throw std::invalid_argument("invalid public shard endpoint or bucket");
            scheme_ = endpoint.substr(0, marker);
            endpoint.remove_prefix(marker + 3);
            if (endpoint.ends_with('/'))
                endpoint.remove_suffix(1);
            if (endpoint.empty() || endpoint.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-") != endpoint.npos)
                throw std::invalid_argument("public shard endpoint must contain only a host and optional port");
            const auto colon = endpoint.find(':');
            authority_ = endpoint;
            host_ = endpoint.substr(0, colon);
            if (host_.empty() || (scheme_ != "https" && (scheme_ != "http" || (host_ != "localhost" && host_ != "127.0.0.1"))))
                throw std::invalid_argument("public shard endpoint requires HTTPS or loopback HTTP");
            unsigned port = scheme_ == "https" ? 443 : 80;
            if (colon != endpoint.npos) {
                const auto value = endpoint.substr(colon + 1);
                const auto [end, ec] = std::from_chars(value.begin(), value.end(), port);
                if (ec != std::errc{} || end != value.end() || !port || port > 65535)
                    throw std::invalid_argument("invalid public shard port");
            }
            port_ = static_cast<std::uint16_t>(port);
            prefix_ = std::format("/{}/", name);
        }
        auto get(native::scheduler io, std::string key, const credentials& auth) const -> coroutine::task<std::optional<std::string>> {
            auto reply = co_await request(io, http::method::GET, std::move(key), {}, auth);
            if (reply.head.status == 404)
                co_return std::nullopt;
            if (reply.head.status != 200)
                throw std::runtime_error(std::format("public R2 GET returned {}", reply.head.status.code()));
            co_return std::move(reply.body);
        }
        auto put(native::scheduler io, std::string key, std::string_view body, const credentials& auth) const -> coroutine::task<bool> {
            if (!auth)
                throw std::invalid_argument("public R2 writes require credentials");
            auto reply = co_await request(io, http::method::PUT, std::move(key), body, auth, true);
            if (reply.head.status == 412)
                co_return false;
            if (reply.head.status != 200 && reply.head.status != 201 && reply.head.status != 204)
                throw std::runtime_error(std::format("public R2 PUT returned {}", reply.head.status.code()));
            co_return true;
        }
        auto erase(native::scheduler io, std::string key, const credentials& auth) const -> coroutine::task<> {
            auto reply = co_await request(io, http::method::DELETE_, std::move(key), {}, auth);
            if (reply.head.status != 204)
                throw std::runtime_error(std::format("public R2 DELETE returned {}", reply.head.status.code()));
        }
    };

    class cache {
        config::public_shard_settings settings_;
        std::optional<bucket> bucket_;
        credentials read_, write_;
        std::optional<content::receipt_key> signer_;
        std::filesystem::path root_;
        std::mutex disk_;

        static auto read(const std::filesystem::path& path, std::uint64_t limit) -> std::string {
            std::string bytes;
            for (const auto& part : durability::read_plain(path, limit))
                bytes.append(reinterpret_cast<const char*>(part.data()), part.size());
            return bytes;
        }
        auto verify(std::string_view key, std::string_view text) const -> entry {
            auto index = json::read<entry>(text).value();
            if (index.key != key || !index.trusted(settings_.trusted_publishers))
                throw std::invalid_argument("untrusted public shard index");
            return index;
        }
        static void save(const std::filesystem::path& root, const entry& index, std::string_view text, std::string_view bytes) {
            for (auto name : {"shards", "index"})
                durability::durable_directories(root / name);
            durability::durable_write(root / "shards" / index.shard_id, bytes);
            durability::durable_write(root / "index" / index.key, text);
        }
        void trim() {
            struct file {
                std::filesystem::path path;
                std::filesystem::file_time_type used;
                std::uint64_t size;
            };
            std::vector<file> files;
            std::uint64_t total = 0;
            for (auto part : {"index", "shards"})
                for (const auto& item : std::filesystem::directory_iterator(root_ / part))
                    if (item.is_regular_file() && !item.is_symlink()) {
                        auto size = item.file_size();
                        files.push_back({item.path(), item.last_write_time(), size});
                        total += size;
                    }
            std::ranges::sort(files, {}, &file::used);
            for (const auto& item : files) {
                if (total <= settings_.cache_bytes)
                    break;
                std::filesystem::remove(item.path);
                total -= item.size;
            }
        }
        auto from_disk(const std::filesystem::path& root, bool touch, const std::string& key, const semantic::recipe& recipe, const inputs& expected)
            -> std::optional<semantic::shard> {
            try {
                const auto index_path = root / "index" / key;
                auto index = verify(key, read(index_path, 1u << 20));
                const auto shard_path = root / "shards" / index.shard_id;
                auto shard = decode(index, read(shard_path, constants::semantic_object_bytes + (1u << 20)), recipe, expected);
                if (shard && touch) {
                    const auto now = std::filesystem::file_time_type::clock::now();
                    std::error_code ec;
                    std::filesystem::last_write_time(index_path, now, ec);
                    std::filesystem::last_write_time(shard_path, now, ec);
                }
                return shard;
            } catch (const std::exception&) {
                return {};
            }
        }
    public:
        cache(config::public_shard_settings settings, const std::filesystem::path& state, bool publishing = true)
            : settings_(std::move(settings)), root_(settings_.cache_root ? std::filesystem::path{*settings_.cache_root} : state / ".public-shards") {
            if (!std::ranges::all_of(settings_.trusted_publishers, [](const auto& key) { return content::is_hash(key); }))
                throw std::invalid_argument("trusted public shard publishers must be hexadecimal Ed25519 public keys");
            if (root_.empty() || (settings_.shared_cache && (settings_.shared_cache->empty() ||
                std::filesystem::absolute(*settings_.shared_cache).lexically_normal() == std::filesystem::absolute(root_).lexically_normal())))
                throw std::invalid_argument("public shard cache directories must be distinct");
            if (settings_.endpoint.empty() != settings_.bucket.empty())
                throw std::invalid_argument("public shards require both endpoint and bucket");
            if (!settings_.endpoint.empty()) {
                bucket_.emplace(settings_.endpoint, settings_.bucket);
                read_ = credentials::environment("FUG_R2_READ_ACCESS_KEY_ID", "FUG_R2_READ_SECRET_ACCESS_KEY");
            }
            if (publishing && settings_.publish) {
                write_ = credentials::environment("FUG_R2_ACCESS_KEY_ID", "FUG_R2_SECRET_ACCESS_KEY");
                const auto* seed = std::getenv(settings_.publish->secret_key_env.c_str());
                if (!bucket_ || !write_ || !seed || !*seed)
                    throw std::invalid_argument("public shard publishing requires a bucket, write credentials and publisher seed");
                signer_.emplace(seed, content::receipt_key::encoding::hex_seed);
                if (!read_)
                    read_ = write_;
            }
        }
        auto signer() const -> const content::receipt_key* { return signer_ ? &*signer_ : nullptr; }

        auto lookup(native::scheduler io, execution::thread_pool& pool, semantic::recipe recipe, inputs expected) -> coroutine::task<std::optional<semantic::shard>> {
            const auto key = lookup_key(recipe, expected);
            if (key.empty() || settings_.trusted_publishers.empty())
                co_return std::nullopt;
            auto local = co_await io::blocking(io, pool, [&, this] {
                if (settings_.shared_cache)
                    if (auto shard = from_disk(*settings_.shared_cache, false, key, recipe, expected))
                        return shard;
                std::lock_guard held{disk_};
                return settings_.cache_bytes ? from_disk(root_, true, key, recipe, expected) : std::nullopt;
            });
            if (local || !bucket_)
                co_return local;
            try {
                auto text = co_await bucket_->get(io, "index/" + key, read_);
                if (!text || text->size() > (1u << 20))
                    co_return std::nullopt;
                auto index = verify(key, *text);
                if (index.input_set != canonical(expected))
                    co_return std::nullopt;
                auto bytes = co_await bucket_->get(io, "shards/" + index.shard_id, read_);
                if (!bytes)
                    co_return std::nullopt;
                co_return co_await io::blocking(io, pool, [&, this] {
                    auto shard = decode(index, *bytes, recipe, expected);
                    if (shard && settings_.cache_bytes && text->size() + bytes->size() <= settings_.cache_bytes) {
                        try {
                            std::lock_guard held{disk_};
                            save(root_, index, *text, *bytes);
                            trim();
                        } catch (const std::exception&) {
                        }
                    }
                    return shard;
                });
            } catch (const std::exception&) {
                co_return std::nullopt;
            }
        }

        auto publish(native::scheduler io, const object& value) const -> coroutine::task<> {
            const auto key = "shards/" + value.index.shard_id;
            if (!co_await bucket_->put(io, key, value.bytes, write_)) {
                auto previous = co_await bucket_->get(io, key, write_);
                if (previous != value.bytes)
                    throw std::runtime_error("public shard object already has different bytes");
            }
            co_await bucket_->put(io, "index/" + value.index.key, json::write(value.index), write_);
        }

        auto fill(native::scheduler io, const std::filesystem::path& root, const std::string& key) const -> coroutine::task<> {
            if (!bucket_)
                throw std::invalid_argument("public cache fill requires an R2 endpoint");
            if (shard_name(key)) {
                auto bytes = co_await bucket_->get(io, "shards/" + key, read_);
                if (!bytes)
                    throw std::invalid_argument("missing public shard: " + key);
                auto shard = decode_shard(key, *bytes);
                if (!rebind(shard, std::string{neutral_project}))
                    throw std::invalid_argument("invalid or missing public shard: " + key);
                durability::durable_directories(root / "shards");
                durability::durable_write(root / "shards" / key, *bytes);
                co_return;
            }
            if (!content::is_hash(key))
                throw std::invalid_argument("invalid public shard key: " + key);
            auto text = co_await bucket_->get(io, "index/" + key, read_);
            if (!text || text->size() > (1u << 20))
                throw std::invalid_argument("invalid or missing public shard index: " + key);
            auto index = verify(key, *text);
            auto bytes = co_await bucket_->get(io, "shards/" + index.shard_id, read_);
            if (!bytes)
                throw std::invalid_argument("missing public shard: " + index.shard_id);
            auto shard = decode_shard(index.shard_id, *bytes);
            if (!decode(index, *bytes, shard.unit.recipe, index.input_set))
                throw std::invalid_argument("invalid public shard: " + index.shard_id);
            save(root, index, *text, *bytes);
        }
    };
}
