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
import json;
import codec;

export namespace replica {
    struct limits {
        std::uint64_t max_cache_bytes = 20ull << 30;
        std::chrono::hours tree_ttl{24 * 14};
        std::chrono::hours offer_ttl{2};
        std::chrono::hours partial_ttl{24};
    };

    // What a worker directory holds, read without its lock: the store that owns the directory writes it, and an analysis
    // process on the same host reads the trees an execute process prepared there.
    class view {
    public:
        explicit view(std::filesystem::path root) : root_(std::move(root)) {}

        auto root() const -> const std::filesystem::path& { return root_; }

        auto slots() const -> std::vector<jobs::worker_slot> {
            std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> recent;
            for (const auto& manifest : manifests()) {
                std::error_code error;
                auto directory = manifest;
                directory.replace_extension();
                if (!std::filesystem::is_directory(directory, error))
                    continue;
                const auto modified = std::filesystem::last_write_time(manifest, error);
                if (!error)
                    recent.emplace_back(modified, manifest);
            }
            std::ranges::sort(recent, std::greater{});
            std::vector<jobs::worker_slot> out;
            for (const auto& [_, manifest] : recent) {
                if (out.size() == constants::worker_slot_reports)
                    break;
                std::ifstream input{manifest};
                std::string header;
                if (!input)
                    continue;
                std::getline(input, header);
                constexpr std::string_view prefix = "# snapshot ";
                out.push_back({.project = manifest.parent_path().filename().string(),
                               .tree = manifest.stem().string(),
                               .snapshot = header.starts_with(prefix) ? header.substr(prefix.size()) : std::string{}});
            }
            return out;
        }

        static auto checked_project(std::string_view name) -> std::string_view { return content::checked_name(name); }

        static auto tree_key(std::string_view project, std::string_view target) -> std::string {
            return std::format("{}/{}", checked_project(project), content::checked_name(target.empty() ? "default" : target));
        }
        auto tree(std::string_view key) const -> std::filesystem::path { return root_ / "trees" / key; }
        // Where the steps on a tree write their reports, outside the tree so they never become part of it.
        auto reports(std::string_view key) const -> std::filesystem::path { return root_ / "reports" / key; }
        // What the analysis sessions of a tree write: their caches live here, so the tree stays the build's alone.
        auto analysis(std::string_view key) const -> std::filesystem::path { return root_ / "analysis" / key; }

        // The files a checkout placed in the tree, in the order of their snapshot.
        auto placed_files(std::string_view key) const -> std::vector<content::entry> {
            std::vector<content::entry> out;
            for (auto& p : read_manifest(key))
                out.push_back(std::move(p.e));
            return out;
        }

        // The digest of a file, read through the store's engine.
        static auto hash_of(const std::filesystem::path& p) -> content::hash { return hash_file(p); }

        auto generated_inputs(std::string_view project, std::string_view target) const -> std::map<std::string, std::string> {
            std::map<std::string, std::string> out;
            for (const auto& [producer, value] : read_generation(tree_key(project, target)).producers)
                for (const auto& path : value.files)
                    out.emplace(path, value.object);
            return out;
        }
        auto unfinished_generation(std::string_view project, std::string_view target) const -> bool {
            return !read_generation(tree_key(project, target)).pending.empty();
        }

    protected:
        struct placed {
            content::entry e;
            std::int64_t mtime = 0;
        };
        static auto stamp(std::filesystem::file_time_type t) -> std::int64_t {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        }

        auto manifest_path(std::string_view key) const -> std::filesystem::path { return root_ / "trees" / (std::string{key} + ".manifest"); }
        auto generation_path(std::string_view key) const -> std::filesystem::path { return root_ / "trees" / (std::string{key} + ".generated"); }
        struct generated {
            struct contribution {
                std::string object;
                std::set<std::string> files;
            };
            std::map<std::string, contribution> producers;
            std::set<std::string> pending;
        };
        auto read_generation(std::string_view key) const -> generated {
            if (!std::filesystem::exists(generation_path(key)))
                return {};
            std::string bytes;
            for (auto part : durability::read_plain(generation_path(key), constants::generated_manifest_bytes))
                bytes.append(reinterpret_cast<const char*>(part.data()), part.size());
            auto value = json::read<generated>(bytes);
            if (!value)
                throw std::runtime_error("invalid generated output ownership: " + value.error().what());
            return std::move(*value);
        }
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

        static auto hash_file(const std::filesystem::path& p) -> content::hash {
            content::sha256 h;
            read_pieces(p, [&](std::span<const std::byte> bytes) { h.update(bytes); });
            return content::to_hex(h.finish());
        }

        auto read_manifest(std::string_view key, std::string* header = nullptr) const -> std::vector<placed> {
            std::vector<placed> out;
            std::ifstream in{manifest_path(key)};
            std::string line;
            while (std::getline(in, line)) {
                if (line.starts_with("# snapshot ")) {
                    if (header)
                        *header = line;
                    continue;
                }
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

        std::filesystem::path root_;
    };

    class store : public view {
    public:
        explicit store(std::filesystem::path root, limits l = {}) : view(std::move(root)), lock_(root_, "worker directory"), limits_(l) {
            std::filesystem::create_directories(root_ / "blobs" / "partial");
            std::filesystem::create_directories(root_ / "trees");
            discard(root_ / "discarded");
            index();
        }

        auto cache_bytes() const -> std::uint64_t { return cache_bytes_; }

        auto offer(const jobs::sync_offer& o) -> jobs::sync_plan {
            checked_project(o.project);
            auto now = std::chrono::system_clock::now();
            if (o.retained) {
                if (auto found = offers_.find(o.project); found != offers_.end())
                    for (auto& offer : found->second)
                        if (offer.snapshot == *o.retained && now - offer.used <= limits_.offer_ttl) {
                            offer.used = now;
                            jobs::sync_plan plan{.snapshot = offer.snapshot};
                            for (const auto& digest : offer.blobs)
                                if (auto blob = used_.find(digest); blob != used_.end())
                                    blob->second = now;
                                else
                                    plan.missing.push_back(digest);
                            return plan;
                        }
                return {.snapshot = *o.retained, .needs_tree = true};
            }
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
            auto& recent = offers_[o.project];
            std::erase_if(recent, [&](auto& r) { return r.snapshot == plan.snapshot || now - r.used > limits_.offer_ttl; });
            recent.push_back({plan.snapshot, o.tree, now, std::move(seen)});
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
                try {
                    protect_blob(target);
                } catch (...) {
                    durability::remove_file(target);
                    throw;
                }
                cache_bytes_ += part.total;
                touch(part.digest);
                ++ack.stored;
            }
            return ack;
        }

        // The files of an offered snapshot.
        auto files(std::string_view project, const content::hash& snapshot) -> std::vector<content::entry> { return find_offer(project, snapshot).files; }

        auto capture_worktree(const std::filesystem::path& dir, std::span<const content::entry> files, const jobs::worktree_capture& scope) const
            -> std::vector<jobs::worktree_change> {
            namespace fs = std::filesystem;
            if (!fs::is_directory(fs::symlink_status(dir)))
                throw std::runtime_error("capture tree is no longer a directory");
            std::map<std::string, const content::entry*, std::less<>> remaining;
            for (const auto& file : files)
                if (scope.includes(file.path))
                    remaining.emplace(file.path, &file);
            auto ignore_subtree = [&](const std::string& path) {
                remaining.erase(path);
                const auto prefix = path + '/';
                auto first = remaining.lower_bound(prefix), last = first;
                while (last != remaining.end() && last->first.starts_with(prefix))
                    ++last;
                remaining.erase(first, last);
            };
            std::vector<jobs::worktree_change> changes;
            std::uint64_t used = 0;
            auto add = [&](jobs::worktree_change change) {
                if (changes.size() == constants::worktree_capture_files)
                    throw std::length_error("worktree capture file count exceeded");
                changes.push_back(std::move(change));
            };
            auto scan = [&](this const auto& self, const fs::path& directory) -> void {
                for (const auto& entry : fs::directory_iterator(directory)) {
                    const auto path = entry.path().lexically_relative(dir).generic_string();
                    if (scope.excludes(path) || !std::ranges::any_of(scope.prefixes, [&](const auto& prefix) {
                            return jobs::worktree_capture::within(path, prefix) || jobs::worktree_capture::within(prefix, path);
                        }))
                        continue;
                    const auto status = entry.symlink_status();
                    if (fs::is_directory(status)) {
                        self(entry.path());
                        continue;
                    }
                    if (!fs::is_regular_file(status) || fs::hard_link_count(entry.path()) != 1) {
                        ignore_subtree(path);
                        continue;
                    }
                    if (!scope.includes(path))
                        continue;
                    content::checked_path(path);
                    const auto found = remaining.find(path);
                    const auto* before = found == remaining.end() ? nullptr : found->second;
                    if (before)
                        remaining.erase(found);
                    const auto limit = std::min(scope.file_limit, constants::worktree_capture_bytes - used);
                    const auto executable = durability::is_executable(entry.path());
                    bool same = before && fs::file_size(entry.path()) == before->size;
                    std::ifstream original;
                    if (same) {
                        original.open(blob_path(before->digest), std::ios::binary);
                        if (!original)
                            throw std::runtime_error("input snapshot file unavailable for capture: " + path);
                    }
                    std::string bytes;
                    std::vector<char> comparison;
                    std::uint64_t size = 0;
                    for (auto part : durability::read_plain(entry.path(), std::max(limit, before ? before->size : 0))) {
                        size += part.size();
                        if (same) {
                            comparison.resize(part.size());
                            if (!original.read(comparison.data(), static_cast<std::streamsize>(part.size())))
                                throw std::runtime_error("input snapshot file unreadable for capture: " + path);
                            same = std::ranges::equal(part, std::as_bytes(std::span{comparison}));
                        }
                        if (size <= limit)
                            bytes.append(reinterpret_cast<const char*>(part.data()), part.size());
                    }
                    if (same && size == before->size) {
                        if (before->executable != executable)
                            add({.path = path, .executable = executable});
                        continue;
                    }
                    if (size > limit)
                        throw std::length_error("worktree capture byte limit exceeded by " + path);
                    used += size;
                    add({.path = path, .bytes = nodes::hex(bytes), .executable = executable});
                }
            };
            scan(dir);
            for (const auto& [path, _] : remaining)
                add({.path = path});
            std::ranges::sort(changes, {}, &jobs::worktree_change::path);
            return changes;
        }

        // Detach under the store lock; traverse and delete after releasing that lock.
        auto detach(std::string_view project, std::string_view target) -> std::filesystem::path {
            const auto key = tree_key(project, target);
            auto dir = tree(key);
            unlinked(root_, dir.lexically_relative(root_).generic_string());
            forget_warm_build(key);
            const auto caches = analysis(key);
            std::filesystem::path retired;
            if (std::filesystem::exists(dir) || std::filesystem::exists(caches)) {
                const auto parent = root_ / "discarded";
                std::filesystem::create_directories(parent);
                static std::atomic<std::uint64_t> next{0};
                do {
                    retired = parent / std::to_string(next.fetch_add(1));
                } while (!std::filesystem::create_directory(retired));
                if (std::filesystem::exists(dir))
                    std::filesystem::rename(dir, retired / "tree");
                if (std::filesystem::exists(caches))
                    std::filesystem::rename(caches, retired / "analysis");
            }
            std::filesystem::remove(manifest_path(key));
            std::filesystem::remove(generation_path(key));
            return retired;
        }

        void remember_warm_build(std::string_view key, const content::hash& snapshot, const content::hash& recipe) const {
            durability::sync_filesystem(tree(key));
            durability::durable_write(warm_path(key), std::format("{} {}\n", snapshot, recipe));
        }

        auto warm_build_time(std::string_view key, const content::hash& snapshot, const content::hash& recipe) const
            -> std::optional<std::filesystem::file_time_type> {
            std::error_code error;
            const auto path = warm_path(key);
            const auto modified = std::filesystem::last_write_time(path, error);
            if (error || std::filesystem::file_time_type::clock::now() - modified > limits_.tree_ttl ||
                !std::filesystem::is_directory(tree(key), error))
                return std::nullopt;
            const auto expected = std::format("{} {}\n", snapshot, recipe);
            std::string stored;
            try {
                for (auto part : durability::read_plain(path, expected.size()))
                    stored.append(reinterpret_cast<const char*>(part.data()), part.size());
            } catch (const durability::unreadable&) {
                return std::nullopt;
            }
            return stored == expected ? std::optional{modified} : std::nullopt;
        }

        auto warm_parents(std::string_view project, std::string_view pool, const content::hash& snapshot, const content::hash& recipe) const
            -> std::vector<std::string> {
            std::vector<std::pair<std::filesystem::file_time_type, std::string>> recent;
            std::error_code error;
            for (auto& file : std::filesystem::directory_iterator(root_ / "trees" / checked_project(project), error)) {
                if (file.path().extension() != ".warm")
                    continue;
                auto key = tree_key(project, file.path().stem().string());
                if (!pool.empty() && !key.starts_with(pool))
                    continue;
                if (auto time = warm_build_time(key, snapshot, recipe))
                    recent.emplace_back(*time, std::move(key));
            }
            std::ranges::sort(recent, std::greater{});
            std::vector<std::string> parents;
            for (auto& [_, key] : recent)
                parents.push_back(std::move(key));
            return parents;
        }

        static void discard(const std::filesystem::path& path) {
            if (path.empty())
                return;
            std::error_code ec;
            auto status = std::filesystem::symlink_status(path, ec);
            if (ec)
                return;
            if (!std::filesystem::exists(status))
                return;
            if (std::filesystem::is_directory(status)) {
                std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
                if (ec)
                    return;
                auto it = std::filesystem::directory_iterator(path, ec);
                while (!ec && it != std::filesystem::directory_iterator{}) {
                    discard(it->path());
                    it.increment(ec);
                }
            }
            std::filesystem::remove(path, ec);
        }

        auto checkout(std::string_view project, const content::hash& snapshot, std::string_view target = {}, std::string_view parent = {},
                      const std::vector<std::string>& build_dirs = {}) -> std::filesystem::path {
            auto& wanted = find_offer(project, snapshot);
            for (auto& e : wanted.files)
                if (!has_blob(e.digest))
                    throw std::runtime_error(std::format("snapshot {} needs blob {}, which was not pushed", snapshot, e.digest));

            auto key = tree_key(project, target);
            if (!read_generation(key).pending.empty())
                discard(detach(project, target));
            auto dir = tree(key);
            // Nothing between the cache root and the tree may be a link a build planted.
            unlinked(root_, dir.lexically_relative(root_).generic_string());
            forget_warm_build(key);
            const auto seed = parent.empty() || std::filesystem::exists(dir) ? std::vector<placed>{} : seed_tree(parent, dir, build_dirs, wanted.files);
            std::map<std::string, const placed*> inherited;
            for (const auto& file : seed)
                inherited.emplace(file.e.path, &file);
            std::filesystem::create_directories(dir);
            std::string header;
            auto before = read_manifest(key, &header);
            bool changed = header != std::format("# snapshot {}", snapshot) || before.size() != wanted.files.size() ||
                           !std::filesystem::is_regular_file(std::filesystem::symlink_status(manifest_path(key)));
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
                    changed = true;
                    std::filesystem::create_directories(target.parent_path());
                    write_atomically(blob_path(e.digest), target, e.executable);
                    if (const auto old = inherited.find(e.path); old != inherited.end() && old->second->e.digest == e.digest &&
                        old->second->e.executable == e.executable) {
                        const auto source = tree(parent) / e.path;
                        const auto time = std::filesystem::last_write_time(source, ec);
                        if (!ec && stamp(time) == old->second->mtime && std::filesystem::file_size(source, ec) == e.size && !ec)
                            std::filesystem::last_write_time(target, time);
                    }
                }
                now.push_back({e, stamp(std::filesystem::last_write_time(target))});
            }
            std::unordered_set<std::string_view> keep;
            for (auto& e : wanted.files)
                keep.insert(e.path);
            for (auto& p : before)
                if (!keep.contains(p.e.path)) {
                    changed = true;
                    unlinked(dir, p.e.path);
                    std::error_code ignored;
                    std::filesystem::remove(dir / p.e.path, ignored);
                }
            if (changed) {
                durability::sync_filesystem(dir);
                write_manifest(key, now, snapshot);
            } else
                durability::touch_file(manifest_path(key));
            return dir;
        }

        void begin_generation(std::string_view project, std::string_view target, const builds::generation& producer) {
            if (producer.producer.empty() || producer.outputs.empty())
                throw std::invalid_argument("generated output requires a producer and output scope");
            const auto key = tree_key(project, target);
            auto state = read_generation(key);
            state.pending.insert(producer.producer);
            write_generation(key, state);
        }

        void commit_generation(std::string_view project, std::string_view target, const builds::generation& producer, const std::vector<std::string>& files) {
            const auto key = tree_key(project, target);
            auto state = read_generation(key);
            if (!state.pending.contains(producer.producer))
                throw std::logic_error("generated output has no active producer");
            const auto dir = tree(key);
            std::set<std::string> source;
            for (const auto& placed : read_manifest(key))
                source.insert(placed.e.path);
            std::set<std::string> selected;
            for (const auto& file : files) {
                content::checked_path(file);
                if (source.contains(file) || !std::ranges::any_of(producer.outputs, [&](const auto& output) {
                        return file == output.path || (output.kind == "directory" && file.starts_with(output.path + "/"));
                    }))
                    throw std::invalid_argument("generated file is outside the producer's output scope: " + file);
                for (const auto& [owner, value] : state.producers)
                    if (owner != producer.producer && value.files.contains(file))
                        throw std::invalid_argument("generated file already belongs to another producer: " + file);
                unlinked(dir, file);
                if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(dir / file)))
                    throw std::invalid_argument("generated manifest names no regular file: " + file);
                selected.insert(file);
            }
            auto& previous = state.producers[producer.producer];
            source.insert(selected.begin(), selected.end());
            withdraw_generated(dir, previous.files, source);
            durability::sync_filesystem(dir);
            previous = {.object = "generation-" + nodes::hex(nodes::random_bytes(16)), .files = std::move(selected)};
            state.pending.erase(producer.producer);
            write_generation(key, state);
        }

        void reconcile_generations(std::string_view project, std::string_view target, const std::set<std::string>& producers) {
            const auto key = tree_key(project, target);
            auto state = read_generation(key);
            std::set<std::string> removed;
            for (const auto& [producer, value] : state.producers)
                if (!producers.contains(producer))
                    removed.insert(producer);
            if (removed.empty())
                return;
            state.pending.insert(removed.begin(), removed.end());
            write_generation(key, state);
            std::set<std::string> source;
            for (const auto& placed : read_manifest(key))
                source.insert(placed.e.path);
            const auto dir = tree(key);
            for (const auto& producer : removed) {
                withdraw_generated(dir, state.producers.at(producer).files, source);
                state.producers.erase(producer);
                state.pending.erase(producer);
            }
            durability::sync_filesystem(dir);
            write_generation(key, state);
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
                std::filesystem::remove_all(analysis(k), ignored);
                std::filesystem::remove(manifest_path(k), ignored);
                std::filesystem::remove(generation_path(k), ignored);
                std::filesystem::remove(warm_path(k), ignored);
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
                if (!ec && durability::remove_file(blob_path(h))) {
                    cache_bytes_ -= size;
                    used_.erase(h);
                }
            }
        }

    private:
        static auto inside(std::string_view path, std::string_view directory) -> bool {
            return path == directory || (path.starts_with(directory) && path[directory.size()] == '/');
        }

        auto warm_path(std::string_view key) const -> std::filesystem::path {
            return root_ / "trees" / (std::string{key} + ".warm");
        }

        void forget_warm_build(std::string_view key) const {
            const auto path = warm_path(key);
            if (std::filesystem::exists(path))
                durability::durable_remove(path);
        }

        auto seed_tree(std::string_view parent, const std::filesystem::path& dir, const std::vector<std::string>& build_dirs,
                       const std::vector<content::entry>& wanted) const -> std::vector<placed> {
            auto original = read_manifest(parent);
            if (original.empty() || !read_generation(parent).pending.empty())
                return {};
            const auto base = tree(parent);
            auto scratch = root_ / "discarded";
            std::filesystem::create_directories(scratch);
            static std::atomic<std::uint64_t> next{0};
            std::filesystem::path staging;
            do {
                staging = scratch / ("seed-" + std::to_string(next.fetch_add(1)));
            } while (!std::filesystem::create_directory(staging));
            bool seeded = false;
            std::uint64_t cloned_files = 0, copied_files = 0, copied_bytes = 0;
            try {
                // Visit a containing directory before any listed descendants.
                std::vector<std::string_view> directories{build_dirs.begin(), build_dirs.end()};
                std::ranges::stable_sort(directories, {}, [](std::string_view name) { return std::ranges::count(name, '/'); });
                for (const auto name : directories) {
                    builds::relative(name, false);
                    auto overlaps = [&](const auto& file) { return inside(file.path, name) || inside(name, file.path); };
                    if (std::ranges::any_of(wanted, overlaps) || std::ranges::any_of(original, [&](const auto& file) { return overlaps(file.e); }))
                        continue;
                    auto source = base;
                    bool directory = std::filesystem::is_directory(std::filesystem::symlink_status(source));
                    for (const auto& segment : std::filesystem::path(name)) {
                        source /= segment;
                        if (directory)
                            directory = std::filesystem::is_directory(std::filesystem::symlink_status(source));
                    }
                    if (!directory)
                        continue;
                    auto destination = staging / name;
                    if (std::filesystem::exists(destination))
                        continue;
                    std::filesystem::create_directories(destination);
                    for (const auto& item : std::filesystem::recursive_directory_iterator(source)) {
                        auto target = destination / item.path().lexically_relative(source);
                        const auto status = item.symlink_status();
                        if (std::filesystem::is_directory(status)) {
                            std::filesystem::create_directory(target);
                        } else if (std::filesystem::is_regular_file(status)) {
                            if (durability::try_clone_file(item.path(), target)) {
                                ++cloned_files;
                            } else {
                                std::filesystem::copy_file(item.path(), target);
                                ++copied_files;
                                copied_bytes += std::filesystem::file_size(target);
                            }
                            std::filesystem::permissions(target, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
                            durability::set_executable(target, durability::is_executable(item.path()));
                            std::filesystem::last_write_time(target, item.last_write_time());
                        } else {
                            discard(staging);
                            return {};
                        }
                    }
                    seeded = true;
                }
                if (seeded)
                    std::filesystem::rename(staging, dir);
                else
                    discard(staging);
            } catch (const std::system_error&) {
                discard(staging);
                return {};
            }
            if (seeded)
                logging::info(std::format("worker: derived build tree from {} into {}: native_clones={} fallback_copies={} fallback_bytes={}",
                                          parent, dir.lexically_relative(root_ / "trees").generic_string(), cloned_files, copied_files, copied_bytes));
            return seeded ? std::move(original) : std::vector<placed>{};
        }

        static constexpr std::size_t max_offers = 256;
        struct offered {
            content::hash snapshot;
            content::manifest tree;
            std::chrono::system_clock::time_point used;
            std::unordered_set<content::hash> blobs;
        };

        auto blob_path(std::string_view h) const -> std::filesystem::path { return root_ / "blobs" / h.substr(0, 2) / h; }
        static void protect_blob(const std::filesystem::path& path) {
            using std::filesystem::perms;
            constexpr auto write = perms::owner_write | perms::group_write | perms::others_write;
            const auto status = std::filesystem::symlink_status(path);
            if (!std::filesystem::is_regular_file(status))
                throw std::runtime_error(std::format("worker blob is not a regular file: {}", path.string()));
            if ((status.permissions() & write) != perms::none)
                std::filesystem::permissions(path, write, std::filesystem::perm_options::remove);
        }
        auto has_blob(std::string_view h) const -> bool { return std::filesystem::is_regular_file(std::filesystem::symlink_status(blob_path(h))); }
        void touch(const content::hash& h) { used_[h] = std::chrono::system_clock::now(); }
        static void withdraw_generated(const std::filesystem::path& dir, const std::set<std::string>& files, const std::set<std::string>& retained) {
            for (const auto& file : files)
                if (!retained.contains(file)) {
                    content::checked_path(file);
                    unlinked(dir, file);
                    // A former generated file may now contain source or output files.
                    if (!std::filesystem::is_directory(std::filesystem::symlink_status(dir / file)))
                        std::filesystem::remove(dir / file);
                }
        }
        void write_generation(std::string_view key, const generated& state) const {
            auto bytes = json::write(state);
            if (bytes.size() > constants::generated_manifest_bytes)
                throw std::length_error("generated output ownership exceeds storage limit");
            durability::durable_write(generation_path(key), bytes);
        }

        void index() {
            std::error_code ec;
            for (auto& d : std::filesystem::directory_iterator(root_ / "blobs", ec)) {
                if (!std::filesystem::is_directory(d.symlink_status()) || d.path().filename() == "partial")
                    continue;
                for (auto& f : std::filesystem::directory_iterator(d.path(), ec)) {
                    auto h = f.path().filename().string();
                    if (!content::is_hash(h))
                        continue;
                    protect_blob(f.path());
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

        static void write_atomically(const std::filesystem::path& source, const std::filesystem::path& target, bool executable) {
            auto temp = target;
            temp += constants::temporary_suffix;
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            // Trees are rebuilt from blobs, so a tree file is not synced to disk.
            try {
                if (!durability::try_clone_file(source, temp)) {
                    auto file = io::open_file(engine().get_scheduler(), temp, true);
                    std::uint64_t offset = 0;
                    read_pieces(source, [&](std::span<const std::byte> bytes) {
                        while (!bytes.empty()) {
                            const auto count = cores::run(engine(), io::async_write_at(file, bytes, offset));
                            if (!count || !*count)
                                throw std::runtime_error(std::format("cannot write {}", target.string()));
                            bytes = bytes.subspan(*count);
                            offset += *count;
                        }
                    });
                }
                durability::set_executable(temp, executable);
                std::filesystem::rename(temp, target);
            } catch (...) {
                std::filesystem::remove(temp, ignored);
                throw;
            }
        }

        void write_manifest(std::string_view key, const std::vector<placed>& files, const content::hash& snapshot) const {
            std::string text = std::format("# snapshot {}\n", snapshot);
            for (auto& p : files)
                text += std::format("{} {} {} {} {}\n", p.e.digest, p.e.size, p.e.executable ? 1 : 0, p.mtime, p.e.path);
            durability::durable_directories(manifest_path(key).parent_path());
            durability::durable_write(manifest_path(key), text);
        }

        durability::dir_lock lock_;
        limits limits_;
        std::unordered_map<std::string, std::vector<offered>, util::transparent_string_hash, std::equal_to<>> offers_;
        std::unordered_map<content::hash, std::chrono::system_clock::time_point> used_;
        std::uint64_t cache_bytes_ = 0;
    };

} // namespace replica
