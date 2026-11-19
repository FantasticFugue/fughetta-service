export module delivery;
import std;
import content;
import protocol;
import constants;
import durability;
import json;
import projectmodel;

export namespace delivery {
    using pins = std::vector<std::shared_ptr<const std::string>>;
    // Callers serialize acquisition and collection with their storage lock.
    class retention {
        std::map<std::string, std::weak_ptr<const std::string>> live_;

    public:
        void acquire(const std::string& id, pins& held) {
            auto& slot = live_[id];
            auto pin = slot.lock();
            if (!pin)
                slot = pin = std::make_shared<const std::string>(id);
            held.push_back(std::move(pin));
        }
        auto roots() -> std::set<std::string> {
            std::erase_if(live_, [](const auto& item) { return item.second.expired(); });
            std::set<std::string> result;
            for (const auto& [id, _] : live_)
                result.insert(id);
            return result;
        }
    };
    inline void readonly(const std::filesystem::path& path) {
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    }
    inline auto model(const std::filesystem::path& storage, const content::hash& id) -> projectmodel::document {
        if (!content::is_hash(id))
            throw std::invalid_argument("invalid model digest");
        std::string bytes;
        for (auto part : durability::read_plain(storage / id.view(), projectmodel::format::max_bytes))
            bytes.append(reinterpret_cast<const char*>(part.data()), part.size());
        return projectmodel::read(bytes);
    }

    inline auto references(const std::filesystem::path& storage, const content::hash& id) -> std::vector<builds::exported_object> {
        if (!content::is_hash(id))
            throw std::invalid_argument("invalid model digest");
        const auto path = storage / "references" / id.view();
        if (!std::filesystem::exists(path))
            return {};
        std::ifstream input{path, std::ios::binary};
        return json::read<std::vector<builds::exported_object>>(std::string{std::istreambuf_iterator<char>{input}, {}}).value();
    }

    // The model bytes determine the reference graph; an untrusted sender's list is ignored.
    inline auto retain_model(const std::filesystem::path& storage, const content::hash& id) -> std::vector<builds::exported_object> {
        std::vector<builds::exported_object> refs;
        for (const auto& hash : projectmodel::objects(model(storage, id))) {
            const auto path = storage / hash;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
                throw std::runtime_error("missing model object: " + hash);
            refs.push_back({hash, std::filesystem::file_size(path)});
        }
        std::filesystem::create_directories(storage / "references");
        durability::durable_write(storage / "references" / id.view(), json::write(refs));
        return refs;
    }

    inline auto collect(const std::filesystem::path& storage, std::chrono::seconds retention = constants::export_retention,
                        std::uint64_t budget = constants::export_cache_bytes, const std::set<std::string>& receiving = {},
                        const std::set<std::string>& retained = {}) -> std::vector<std::string> {
        using clock = std::filesystem::file_time_type::clock;
        struct object {
            std::filesystem::path path;
            std::filesystem::file_time_type used;
            std::uint64_t size;
        };
        std::vector<object> objects;
        std::vector<std::string> errors;
        auto attempt = [&](const std::filesystem::path& path, auto&& action) {
            try {
                action();
                return true;
            } catch (const std::exception& failure) {
                errors.push_back(path.string() + ": " + failure.what());
                return false;
            }
        };
        std::uint64_t total = 0;
        std::error_code ec;
        const auto now = clock::now();
        for (const auto& entry : std::filesystem::directory_iterator(storage, ec)) {
            const auto status = entry.symlink_status(ec);
            if (ec || !std::filesystem::is_regular_file(status) || !content::is_hash(entry.path().filename().string()))
                continue;
            const auto size = entry.file_size(ec);
            if (ec)
                continue;
            const auto used = entry.last_write_time(ec);
            if (ec)
                continue;
            total += size;
            objects.push_back({entry.path(), used, size});
        }
        std::map<std::string, std::vector<builds::exported_object>> models;
        std::map<std::string, std::size_t> counts;
        std::set<std::string> children;
        std::vector<std::filesystem::path> orphaned;
        for (const auto& entry : std::filesystem::directory_iterator(storage / "references", ec)) {
            const auto id = entry.path().filename().string();
            if (!content::is_hash(id) || !entry.is_regular_file())
                continue;
            auto refs = references(storage, id);
            for (const auto& child : refs)
                children.insert(child.digest.str());
            if (std::filesystem::is_regular_file(storage / id)) {
                for (const auto& child : refs)
                    ++counts[child.digest.str()];
                models.emplace(id, std::move(refs));
            } else
                orphaned.push_back(entry.path());
        }
        auto remove = [&](const std::filesystem::path& path) {
            if (retained.contains(path.filename().string()))
                return false;
            return attempt(path, [&] {
                const auto size = std::filesystem::file_size(path, ec);
                if (ec == std::errc::no_such_file_or_directory)
                    return;
                if (ec)
                    throw std::filesystem::filesystem_error("measure export for collection", path, ec);
                // Reserve this eviction in the current pass even if deletion
                // fails. A blocked object must not evict newer entries in its
                // place; the next collection measures it again and retries.
                total -= size;
                durability::remove_file(path);
            });
        };
        for (const auto& child : children)
            if (!counts.contains(child))
                remove(storage / child);
        for (const auto& path : orphaned)
            attempt(path, [&] { durability::remove_file(path); });
        std::ranges::sort(objects, {}, &object::used);
        for (const auto& item : objects) {
            const auto id = item.path.filename().string();
            if (children.contains(id) || retained.contains(id))
                continue;
            if (now - item.used < constants::export_transfer_grace)
                continue;
            if (now - item.used < retention && total <= budget)
                continue;
            if (!remove(item.path))
                continue;
            if (auto found = models.find(id); found != models.end()) {
                for (const auto& child : found->second)
                    if (--counts.at(child.digest.str()) == 0)
                        remove(storage / child.digest.view());
                const auto path = storage / "references" / id;
                attempt(path, [&] { durability::remove_file(path); });
            }
        }
        // Interrupted receivers leave only partial files; unfinished capture directories are private scratch.
        for (const auto& entry : std::filesystem::directory_iterator(storage / "partial", ec)) {
            if (receiving.contains(entry.path().filename().string()))
                continue;
            if (!std::filesystem::is_regular_file(entry.symlink_status(ec)) || ec)
                continue;
            const auto used = entry.last_write_time(ec);
            if (!ec && now - used > std::chrono::hours{24})
                attempt(entry.path(), [&] { durability::remove_file(entry.path()); });
        }
        for (const auto& entry : std::filesystem::directory_iterator(storage, ec)) {
            if (!entry.path().filename().string().starts_with("pending-") || !std::filesystem::is_directory(entry.symlink_status(ec)) || ec)
                continue;
            const auto used = entry.last_write_time(ec);
            if (!ec && now - used > std::chrono::hours{24})
                attempt(entry.path(), [&] { durability::remove_tree(entry.path()); });
        }
        return errors;
    }

    // Serialize parts for the same digest through staging, verification, and publication.
    inline auto stage(const std::filesystem::path& storage, const jobs::source_part& part) -> bool {
        if (!content::is_hash(part.digest) || part.packed || part.total > constants::export_bytes || part.offset > part.total ||
            part.bytes.size() > jobs::part_size || part.bytes.size() > part.total - part.offset)
            throw std::invalid_argument("invalid export part");
        std::filesystem::create_directories(storage / "partial");
        const bool complete = part.offset + part.bytes.size() == part.total;
        durability::write_part(storage / "partial" / part.digest.view(), part.offset, part.bytes, complete);
        return complete;
    }

    inline void verify(const std::filesystem::path& storage, const content::hash& digest, std::uint64_t size) {
        const auto pending = storage / "partial" / digest.view();
        content::sha256 hash;
        std::uint64_t seen = 0;
        try {
            for (auto bytes : durability::read_plain(pending, size)) {
                hash.update(bytes);
                seen += bytes.size();
            }
            if (seen != size || content::to_hex(hash.finish()) != digest)
                throw std::invalid_argument("export content does not match its digest");
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(pending, ignored);
            throw;
        }
    }

    inline void publish(const std::filesystem::path& storage, const content::hash& digest) {
        readonly(storage / "partial" / digest.view());
        durability::durable_rename(storage / "partial" / digest.view(), storage / digest.view());
    }

    inline void accept(const std::filesystem::path& storage, const jobs::source_part& part) {
        if (stage(storage, part)) {
            verify(storage, part.digest, part.total);
            publish(storage, part.digest);
        }
    }

    // Export storage is outside mutable build trees. Files are copied, never hard-linked to a build output.
    auto capture(const std::filesystem::path& tree, const std::map<std::string, std::string>& requested, const std::filesystem::path& storage,
                        bool allow_missing = false, std::vector<std::string>* skipped = nullptr) -> std::vector<builds::exported_file> {
        std::vector<builds::exported_file> result;
        if (requested.empty())
            return result;
        std::filesystem::create_directories(storage);
        static std::atomic<std::uint64_t> next{0};
        std::filesystem::path temporary;
        do {
            temporary = storage / ("pending-" + std::to_string(next.fetch_add(1)));
        } while (!std::filesystem::create_directory(temporary));
        struct cleanup {
            std::filesystem::path path;
            ~cleanup() {
                std::error_code ignored;
                durability::remove_tree(path, ignored);
            }
        } guard{temporary};
        std::uint64_t total = 0;
        auto checked = [&](const std::filesystem::path& path) {
            const auto relative = path.lexically_relative(tree);
            content::checked_path(relative.generic_string());
            auto at = tree;
            std::filesystem::file_status status;
            for (const auto& component : relative) {
                at /= component;
                std::error_code ec;
                status = std::filesystem::symlink_status(at, ec);
                if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory)
                    return std::filesystem::file_status{std::filesystem::file_type::not_found};
                if (ec)
                    throw std::runtime_error("cannot inspect export: " + relative.generic_string());
                if (std::filesystem::is_symlink(status))
                    throw std::invalid_argument("export path contains a symbolic link: " + relative.generic_string());
            }
            return status;
        };
        auto copy = [&](const std::string& name, const std::filesystem::path& path, std::string relative) {
            const auto status = checked(path);
            if (!std::filesystem::is_regular_file(status))
                throw std::invalid_argument("export is not a regular file: " + path.lexically_relative(tree).generic_string());
            if (result.size() >= constants::export_files)
                throw std::length_error("export file count exceeds the delivery limit");
            const auto staged = temporary / std::to_string(result.size());
            std::ofstream out{staged, std::ios::binary | std::ios::trunc};
            if (!out)
                throw std::runtime_error("cannot store export: " + path.lexically_relative(tree).generic_string());
            content::sha256 hash;
            std::uint64_t size = 0;
            for (auto bytes : durability::read_plain(path, constants::export_bytes - total)) {
                const auto count = bytes.size();
                if (count > constants::export_bytes - total)
                    throw std::length_error("export bytes exceed the delivery limit");
                total += count;
                size += count;
                hash.update(bytes);
                out.write(reinterpret_cast<const char*>(bytes.data()), count);
                if (!out)
                    throw std::runtime_error("cannot store export");
            }
            out.close();
            if (!out)
                throw std::runtime_error("cannot finish export");
            auto digest = content::to_hex(hash.finish());
            const auto target = storage / digest.view();
            readonly(staged);
            // Concurrent captures of identical bytes can share an already published object.
            std::error_code ec;
            std::filesystem::create_hard_link(staged, target, ec);
            if (ec && ec != std::errc::file_exists)
                throw std::filesystem::filesystem_error("publish export", staged, target, ec);
            durability::remove_file(staged);
            std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now());
            auto executable = durability::is_executable(path);
            auto refs = references(storage, digest);
            result.push_back({name, std::move(relative), std::move(digest), size, executable, {}, std::move(refs)});
        };
        for (const auto& [name, path] : requested) {
            content::checked_name(name);
            content::checked_path(path);
            const auto source = tree / path;
            const auto status = checked(source);
            if (allow_missing && !std::filesystem::exists(status))
                continue;
            if (std::filesystem::is_directory(status)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
                    if (std::filesystem::is_symlink(entry.symlink_status())) {
                        if (skipped) {
                            if (skipped->size() >= constants::export_files)
                                throw std::length_error("too many skipped export paths");
                            skipped->push_back(entry.path().lexically_relative(tree).generic_string());
                        }
                        continue;
                    }
                    const auto state = checked(entry.path());
                    if (std::filesystem::is_directory(state))
                        continue;
                    copy(name, entry.path(), entry.path().lexically_relative(source).generic_string());
                }
            } else
                copy(name, source, source.filename().generic_string());
        }
        std::ranges::sort(result, {}, [](const auto& file) { return std::pair{file.name, file.path}; });
        return result;
    }

    // Build processes only write staging. Capture hashes a private copy before publishing it
    // under the computed digest; a false claim can never overwrite another object's bytes.
    inline auto capture_model(const std::filesystem::path& tree, const builds::context& output, const std::filesystem::path& staging,
                              const std::filesystem::path& storage) -> builds::exported_file {
        auto file = capture(tree, {{"model", output.path}}, storage).at(0);
        file.name = output.name;
        for (const auto& expected : projectmodel::objects(model(storage, file.digest))) {
            const auto object = capture(staging, {{"object", expected}}, storage).at(0);
            if (object.digest != expected)
                throw std::invalid_argument("staged model object does not match its digest: " + expected);
        }
        file.objects = retain_model(storage, file.digest);
        return file;
    }

    inline auto read(const std::filesystem::path& storage, const jobs::export_read& request) -> jobs::source_bytes {
        if (!content::is_hash(request.digest) || request.length > jobs::part_size)
            throw std::invalid_argument("invalid export read");
        const auto path = storage / request.digest.view();
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
            throw std::invalid_argument("export is not available");
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now());
        const auto size = std::filesystem::file_size(path);
        if (request.offset > size)
            throw std::invalid_argument("export offset is past its end");
        jobs::source_part part{.digest = request.digest, .offset = request.offset, .total = size};
        part.bytes.resize(std::min(request.length, size - request.offset));
        std::ifstream in{path, std::ios::binary};
        if (!in || !in.seekg(request.offset))
            throw std::runtime_error("cannot open exported content");
        if (!part.bytes.empty() && !in.read(reinterpret_cast<char*>(part.bytes.data()), part.bytes.size()))
            throw std::runtime_error("cannot read exported content");
        return {{std::move(part)}};
    }
} // namespace delivery
