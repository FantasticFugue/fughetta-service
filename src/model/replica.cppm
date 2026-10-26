export module replica;
import constants;
import std;
import content;
import protocol;
import durability;
import archive;
import async;
import native;
import utility;

export namespace replica {
    struct limits {
        std::uint64_t max_cache_bytes = 20ull << 30;
        std::chrono::hours tree_ttl{24 * 14};
        std::chrono::hours offer_ttl{2};
        std::chrono::hours partial_ttl{24};
    };

    class store {
    public:
        explicit store(std::filesystem::path root, limits l = {}) : root_(std::move(root)), lock_(root_, "worker directory"), limits_(l) {
            std::filesystem::create_directories(root_ / "blobs" / "partial");
            std::filesystem::create_directories(root_ / "trees");
            index();
        }

        auto root() const -> const std::filesystem::path& { return root_; }
        auto cache_bytes() const -> std::uint64_t { return cache_bytes_; }

        static auto checked_project(std::string_view name) -> std::string_view { return content::checked_name(name); }

        static auto tree_key(std::string_view project, std::string_view target) -> std::string {
            return std::format("{}/{}", checked_project(project), content::checked_name(target.empty() ? "default" : target));
        }
        auto tree(std::string_view key) const -> std::filesystem::path { return root_ / "trees" / key; }

        auto offer(const jobs::sync_offer& o) -> jobs::sync_plan {
            checked_project(o.project);
            for (auto& e : o.tree.files) {
                content::checked_path(e.path);
                if (!content::is_hash(e.digest))
                    throw std::invalid_argument(std::format("bad digest for '{}'", e.path));
            }
            jobs::sync_plan plan{.snapshot = o.tree.id()};
            std::unordered_set<content::hash> seen;
            for (auto& e : o.tree.files) {
                if (!seen.insert(e.digest).second)
                    continue;
                if (has_blob(e.digest))
                    touch(e.digest);
                else
                    plan.missing.push_back(e.digest);
            }
            auto now = std::chrono::system_clock::now();
            auto& recent = offers_[o.project];
            std::erase_if(recent, [&](auto& r) { return r.snapshot == plan.snapshot || now - r.used > limits_.offer_ttl; });
            recent.push_back({plan.snapshot, o.tree, now});
            if (recent.size() > max_offers)
                recent.erase(recent.begin());
            return plan;
        }

        auto put(const jobs::source_bytes& push) -> jobs::authority_ack {
            jobs::authority_ack ack;
            for (auto& part : push.parts) {
                if (!content::is_hash(part.digest))
                    throw std::invalid_argument("bad digest");
                if (has_blob(part.digest))
                    continue;
                if (part.offset > part.total)
                    throw std::invalid_argument("blob part past the blob's end");
                std::string unpacked;
                if (part.packed)
                    unpacked = nodes::archive::unpack({reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()},
                                                      std::min(jobs::part_size, part.total - part.offset));
                auto bytes = part.packed ? std::as_bytes(std::span{unpacked}) : std::span<const std::byte>{part.bytes};
                if (part.offset + bytes.size() > part.total)
                    throw std::invalid_argument("blob part past the blob's end");
                auto partial = root_ / "blobs" / "partial" / part.digest.view();
                bool last = part.offset + bytes.size() == part.total;
                durability::write_part(partial, part.offset, bytes, last);
                if (!last)
                    continue;
                if (std::filesystem::file_size(partial) != part.total || hash_file(partial) != part.digest) {
                    std::filesystem::remove(partial);
                    throw transport::coded_error(jobs::why::stale_blob, std::format("blob {} does not match its digest", part.digest));
                }
                auto target = blob_path(part.digest);
                durability::durable_directories(target.parent_path());
                durability::durable_rename(partial, target);
                cache_bytes_ += part.total;
                touch(part.digest);
                ++ack.stored;
            }
            return ack;
        }

        // The digest of a file, read through the store's engine.
        static auto hash_of(const std::filesystem::path& p) -> content::hash { return hash_file(p); }

        // The files of an offered snapshot.
        auto files(std::string_view project, const content::hash& snapshot) -> std::vector<content::entry> { return find_offer(project, snapshot).files; }

        auto checkout(std::string_view project, const content::hash& snapshot, std::string_view target = {}) -> std::filesystem::path {
            auto& wanted = find_offer(project, snapshot);
            for (auto& e : wanted.files)
                if (!has_blob(e.digest))
                    throw std::runtime_error(std::format("snapshot {} needs blob {}, which was not pushed", snapshot, e.digest));

            auto key = tree_key(project, target);
            auto dir = tree(key);
            // Nothing between the cache root and the tree may be a link a build planted.
            unlinked(root_, dir.lexically_relative(root_).generic_string());
            std::filesystem::create_directories(dir);
            auto before = read_manifest(key);
            std::unordered_map<std::string, const placed*> had;
            for (auto& p : before)
                had.emplace(p.e.path, &p);

            std::vector<placed> now;
            for (auto& e : wanted.files) {
                auto target = dir / e.path;
                unlinked(dir, e.path);
                cleared(dir, e.path);
                touch(e.digest);
                auto it = had.find(e.path);
                std::error_code ec;
                bool same = it != had.end() && it->second->e.digest == e.digest && it->second->e.executable == e.executable &&
                            std::filesystem::is_regular_file(target, ec) && std::filesystem::file_size(target, ec) == e.size &&
                            stamp(std::filesystem::last_write_time(target, ec)) == it->second->mtime;
                if (!same) {
                    std::filesystem::create_directories(target.parent_path());
                    auto bytes = read_file(blob_path(e.digest));
                    write_atomically(target, std::as_bytes(std::span{bytes}), e.executable);
                }
                now.push_back({e, stamp(std::filesystem::last_write_time(target))});
            }
            std::unordered_set<std::string_view> keep;
            for (auto& e : wanted.files)
                keep.insert(e.path);
            for (auto& p : before)
                if (!keep.contains(p.e.path)) {
                    unlinked(dir, p.e.path);
                    std::error_code ignored;
                    std::filesystem::remove(dir / p.e.path, ignored);
                }
            durability::sync_filesystem(dir);
            write_manifest(key, now);
            return dir;
        }

        void collect(const std::set<std::string>& busy = {}) {
            auto now = std::chrono::system_clock::now();
            std::error_code ec;
            for (auto& f : std::filesystem::directory_iterator(root_ / "blobs" / "partial", ec))
                if (now - file_clock_to_sys(f.last_write_time()) > limits_.partial_ttl)
                    std::filesystem::remove(f.path(), ec);
            for (auto& [_, recent] : offers_)
                std::erase_if(recent, [&](auto& r) { return now - r.used > limits_.offer_ttl; });
            std::vector<std::string> stale_trees;
            for (auto& m : manifests())
                if (now - file_clock_to_sys(std::filesystem::last_write_time(m)) > limits_.tree_ttl && !busy.contains(key_of(m)))
                    stale_trees.push_back(key_of(m));
            for (auto& k : stale_trees) {
                std::error_code ignored;
                std::filesystem::remove_all(tree(k), ignored);
                std::filesystem::remove(manifest_path(k), ignored);
            }
            if (cache_bytes_ <= limits_.max_cache_bytes)
                return;
            std::unordered_set<content::hash> pinned;
            for (auto& m : manifests())
                for (auto& p : read_manifest(key_of(m)))
                    pinned.insert(p.e.digest);
            for (auto& [_, recent] : offers_)
                for (auto& r : recent)
                    for (auto& e : r.tree.files)
                        pinned.insert(e.digest);
            std::vector<std::pair<std::chrono::system_clock::time_point, content::hash>> order;
            for (auto& [h, used] : used_)
                order.emplace_back(used, h);
            std::ranges::sort(order);
            for (auto& [_, h] : order) {
                if (cache_bytes_ <= limits_.max_cache_bytes)
                    break;
                if (pinned.contains(h))
                    continue;
                std::error_code ec;
                auto size = std::filesystem::file_size(blob_path(h), ec);
                if (!ec && std::filesystem::remove(blob_path(h), ec))
                    cache_bytes_ -= size;
                used_.erase(h);
            }
        }

    private:
        static constexpr std::size_t max_offers = 256;
        struct offered {
            content::hash snapshot;
            content::manifest tree;
            std::chrono::system_clock::time_point used;
        };

        struct placed {
            content::entry e;
            std::int64_t mtime = 0;
        };
        static auto stamp(std::filesystem::file_time_type t) -> std::int64_t {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        }

        auto blob_path(std::string_view h) const -> std::filesystem::path { return root_ / "blobs" / h.substr(0, 2) / h; }
        auto has_blob(std::string_view h) const -> bool { return std::filesystem::exists(blob_path(h)); }
        void touch(const content::hash& h) { used_[h] = std::chrono::system_clock::now(); }
        auto manifest_path(std::string_view key) const -> std::filesystem::path { return root_ / "trees" / (std::string{key} + ".manifest"); }

        auto manifests() const -> std::vector<std::filesystem::path> {
            std::vector<std::filesystem::path> out;
            std::error_code ec;
            for (auto& project : std::filesystem::directory_iterator(root_ / "trees", ec))
                if (project.is_directory())
                    for (auto& f : std::filesystem::directory_iterator(project.path(), ec))
                        if (f.path().extension() == ".manifest")
                            out.push_back(f.path());
            return out;
        }
        auto key_of(const std::filesystem::path& manifest) const -> std::string {
            auto rel = std::filesystem::relative(manifest, root_ / "trees");
            rel.replace_extension();
            return rel.generic_string();
        }

        static auto file_clock_to_sys(std::filesystem::file_time_type t) -> std::chrono::system_clock::time_point {
            return std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - std::filesystem::file_time_type::clock::now() +
                                                                                     std::chrono::system_clock::now());
        }

        void index() {
            std::error_code ec;
            for (auto& d : std::filesystem::directory_iterator(root_ / "blobs", ec)) {
                if (!d.is_directory() || d.path().filename() == "partial")
                    continue;
                for (auto& f : std::filesystem::directory_iterator(d.path(), ec)) {
                    auto h = f.path().filename().string();
                    if (!content::is_hash(h))
                        continue;
                    cache_bytes_ += f.file_size();
                    used_[h] = file_clock_to_sys(f.last_write_time());
                }
            }
        }

        auto find_offer(std::string_view project, const content::hash& snapshot) -> const content::manifest& {
            if (auto it = offers_.find(project); it != offers_.end())
                for (auto& r : it->second)
                    if (r.snapshot == snapshot) {
                        r.used = std::chrono::system_clock::now();
                        return r.tree;
                    }
            throw std::runtime_error(std::format("unknown snapshot {} of '{}'; offer it first", snapshot, project));
        }

        // Blob and tree files go through the native asynchronous engine, driven from the disk-pool thread that runs the store.
        static auto engine() -> native::context& {
            static thread_local native::context resource;
            return resource;
        }
        static constexpr std::size_t piece = 1024 * 1024;

        // Calls `take` with each piece of the file at `p`, in order.
        template <class Take> static void read_pieces(const std::filesystem::path& p, Take take) {
            auto size = std::filesystem::file_size(p);
            auto file = io::open_file(engine().get_scheduler(), p);
            std::vector<std::byte> buffer(std::min<std::uint64_t>(size, piece));
            for (std::uint64_t offset = 0; offset < size;) {
                auto span = std::span{buffer}.first(std::min<std::uint64_t>(piece, size - offset));
                auto count = cores::run(engine(), io::async_read_at(file, span, offset));
                if (!count || !*count)
                    throw std::runtime_error(std::format("short read of {}", p.string()));
                take(span.first(*count));
                offset += *count;
            }
        }

        static auto read_file(const std::filesystem::path& p) -> std::string {
            std::string out;
            read_pieces(p, [&](std::span<const std::byte> bytes) { out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size()); });
            return out;
        }

        static auto hash_file(const std::filesystem::path& p) -> content::hash {
            content::sha256 h;
            read_pieces(p, [&](std::span<const std::byte> bytes) { h.update(bytes); });
            return content::to_hex(h.finish());
        }

        // A tree's paths are its own: whatever a build left as a symbolic link on the way to a tracked file is removed, so nothing
        // written or deleted for the tree ends up outside it.
        static void unlinked(const std::filesystem::path& dir, std::string_view relative) {
            auto path = dir;
            for (const auto& component : std::filesystem::path(relative)) {
                path /= component;
                std::error_code ec;
                if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)))
                    std::filesystem::remove(path);
                else if (ec || !std::filesystem::exists(std::filesystem::symlink_status(path, ec)))
                    return;
            }
        }

        // A path that changed between a file and a directory: what stands in the new file's way goes, files where its directories
        // must be and a directory where it must be.
        static void cleared(const std::filesystem::path& dir, std::string_view relative) {
            auto path = dir;
            std::filesystem::path wanted{relative};
            for (auto it = wanted.begin(); it != wanted.end(); ++it) {
                path /= *it;
                auto status = std::filesystem::symlink_status(path);
                if (std::next(it) == wanted.end()) {
                    if (std::filesystem::is_directory(status))
                        std::filesystem::remove_all(path);
                } else if (std::filesystem::exists(status) && !std::filesystem::is_directory(status))
                    std::filesystem::remove(path);
            }
        }

        static void write_atomically(const std::filesystem::path& target, std::span<const std::byte> bytes, bool executable) {
            auto temp = target;
            temp += constants::temporary_suffix;
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            // Trees are rebuilt from blobs, so a tree file is not synced to disk.
            {
                auto file = io::open_file(engine().get_scheduler(), temp, true);
                for (std::size_t offset = 0; offset < bytes.size();) {
                    auto count = cores::run(engine(), io::async_write_at(file, bytes.subspan(offset, std::min(piece, bytes.size() - offset)), offset));
                    if (!count || !*count)
                        throw std::runtime_error(std::format("cannot write {}", target.string()));
                    offset += *count;
                }
            }
            durability::set_executable(temp, executable);
            std::filesystem::rename(temp, target);
        }

        auto read_manifest(std::string_view key) const -> std::vector<placed> {
            std::vector<placed> out;
            std::ifstream in{manifest_path(key)};
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream fields{line};
                placed p;
                int exec = 0;
                std::string digest_text;
                fields >> digest_text >> p.e.size >> exec >> p.mtime;
                p.e.digest = content::hash{digest_text};
                fields.get();
                std::getline(fields, p.e.path);
                p.e.executable = exec != 0;
                // The manifest sits where a build can write; a line whose path is not the tree's own is ignored.
                bool own = !p.e.path.empty();
                try {
                    content::checked_path(p.e.path);
                } catch (const std::invalid_argument&) {
                    own = false;
                }
                if (content::is_hash(p.e.digest) && own)
                    out.push_back(std::move(p));
            }
            return out;
        }

        void write_manifest(std::string_view key, const std::vector<placed>& files) const {
            std::string text;
            for (auto& p : files)
                text += std::format("{} {} {} {} {}\n", p.e.digest, p.e.size, p.e.executable ? 1 : 0, p.mtime, p.e.path);
            durability::durable_directories(manifest_path(key).parent_path());
            durability::durable_write(manifest_path(key), text);
        }

        std::filesystem::path root_;
        durability::dir_lock lock_;
        limits limits_;
        std::unordered_map<std::string, std::vector<offered>, util::transparent_string_hash, std::equal_to<>> offers_;
        std::unordered_map<content::hash, std::chrono::system_clock::time_point> used_;
        std::uint64_t cache_bytes_ = 0;
    };

} // namespace replica
