export module dependencies;
import std;
import content;
import durability;
import constants;

export namespace dependencies {
    // The caller holds the publisher lock and only runs trusted inputs in its private cache.
    inline auto publish(const std::filesystem::path& cache, const std::filesystem::path& destination, std::chrono::steady_clock::time_point deadline)
        -> std::size_t {
        const auto source = cache / "vcpkg", target = destination / "vcpkg";
        if (!std::filesystem::exists(source))
            return 0;
        auto check_time = [&] {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("dependency publication exceeded the build deadline");
        };
        check_time();
        durability::durable_directories(target);
        static std::atomic<std::uint64_t> sequence{0};
        std::filesystem::path temporary;
        do {
            check_time();
            temporary = target / (".publish-" + std::to_string(sequence.fetch_add(1)));
        } while (!std::filesystem::create_directory(temporary));
        struct cleanup {
            std::filesystem::path path;
            ~cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } guard{temporary};
        std::size_t seen = 0, published = 0;
        for (const auto& bucket : std::filesystem::directory_iterator(source)) {
            check_time();
            const auto prefix = bucket.path().filename().string();
            if (prefix.size() != 2 || prefix.find_first_not_of("0123456789abcdef") != std::string::npos)
                continue;
            if (!std::filesystem::is_directory(bucket.symlink_status()))
                throw std::runtime_error("dependency cache bucket is not a directory");
            for (const auto& archive : std::filesystem::directory_iterator(bucket.path())) {
                check_time();
                if (++seen > constants::dependency_archive_count)
                    throw std::runtime_error("dependency publication archive count limit exceeded");
                const auto name = archive.path().filename();
                if (name.extension() != ".zip")
                    continue;
                const auto abi = name.stem().string();
                if (!content::is_hash(abi) || !abi.starts_with(prefix))
                    throw std::runtime_error("dependency archive name is not a vcpkg ABI");
                if (!std::filesystem::is_regular_file(archive.symlink_status()))
                    throw std::runtime_error("dependency archive is not a regular file");
                const auto output = target / prefix / name;
                if (std::filesystem::exists(std::filesystem::symlink_status(output))) {
                    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(output)))
                        throw std::runtime_error("published dependency archive is not a regular file");
                    continue;
                }
                const auto staged = temporary / name;
                std::ofstream file{staged, std::ios::binary | std::ios::trunc};
                if (!file)
                    throw std::runtime_error("cannot stage dependency archive");
                for (auto bytes : durability::read_plain(archive.path(), constants::dependency_archive_bytes)) {
                    check_time();
                    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    if (!file)
                        throw std::runtime_error("cannot copy dependency archive");
                }
                file.close();
                if (!file)
                    throw std::runtime_error("cannot finish dependency archive");
                durability::write_part(staged, 0, {}, true);
                durability::durable_directories(output.parent_path());
                check_time();
                std::error_code error;
                std::filesystem::create_hard_link(staged, output, error);
                if (error && error != std::errc::file_exists)
                    throw std::filesystem::filesystem_error("publish dependency archive", staged, output, error);
                if (!error) {
                    durability::sync_dir(output.parent_path());
                    ++published;
                }
                std::filesystem::remove(staged);
            }
        }
        return published;
    }
} // namespace dependencies
