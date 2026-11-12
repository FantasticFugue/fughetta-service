export module delivery;
import std;
import content;
import protocol;
import constants;
import durability;

export namespace delivery {
    inline void collect(const std::filesystem::path& storage, std::chrono::seconds retention = constants::export_retention,
                        std::uint64_t budget = constants::export_cache_bytes, const std::set<std::string>& receiving = {}) {
        using clock = std::filesystem::file_time_type::clock;
        struct object {
            std::filesystem::path path;
            std::filesystem::file_time_type used;
            std::uint64_t size;
        };
        std::vector<object> objects;
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
        std::ranges::sort(objects, {}, &object::used);
        for (const auto& item : objects) {
            if (now - item.used < constants::export_transfer_grace)
                continue;
            if (now - item.used < retention && total <= budget)
                continue;
            if (std::filesystem::remove(item.path, ec))
                total -= item.size;
        }
        // Interrupted receivers leave only partial files; unfinished capture directories are private scratch.
        for (const auto& entry : std::filesystem::directory_iterator(storage / "partial", ec)) {
            if (receiving.contains(entry.path().filename().string()))
                continue;
            if (!std::filesystem::is_regular_file(entry.symlink_status(ec)) || ec)
                continue;
            const auto used = entry.last_write_time(ec);
            if (!ec && now - used > std::chrono::hours{24})
                std::filesystem::remove(entry.path(), ec);
        }
        for (const auto& entry : std::filesystem::directory_iterator(storage, ec)) {
            if (!entry.path().filename().string().starts_with("pending-") || !std::filesystem::is_directory(entry.symlink_status(ec)) || ec)
                continue;
            const auto used = entry.last_write_time(ec);
            if (!ec && now - used > std::chrono::hours{24})
                std::filesystem::remove_all(entry.path(), ec);
        }
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
        durability::durable_rename(storage / "partial" / digest.view(), storage / digest.view());
    }

    inline void accept(const std::filesystem::path& storage, const jobs::source_part& part) {
        if (stage(storage, part)) {
            verify(storage, part.digest, part.total);
            publish(storage, part.digest);
        }
    }

    // Export storage is outside mutable build trees. Files are copied, never hard-linked to a build output.
    inline auto capture(const std::filesystem::path& tree, const std::map<std::string, std::string>& requested, const std::filesystem::path& storage,
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
                std::filesystem::remove_all(path, ignored);
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
            // Concurrent captures of identical bytes can share an already published object.
            std::error_code ec;
            std::filesystem::create_hard_link(staged, target, ec);
            if (ec && ec != std::errc::file_exists)
                throw std::filesystem::filesystem_error("publish export", staged, target, ec);
            std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now());
            auto executable = durability::is_executable(path);
            result.push_back({name, std::move(relative), std::move(digest), size, executable});
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
