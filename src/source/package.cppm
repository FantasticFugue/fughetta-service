export module package;
import std;
import content;
import durability;
import reflection;
import nodes;
import engine;
import codec;
import constants;

// A package is one version of a worldline written out of the node: a manifest and the bytes of each file, named by digest. It is
// restored as a new worldline of any project of the workspace, whose first batch creates exactly those files.
export namespace package {
    inline constexpr int format = 1;

    struct file {
        std::string path, digest, kind;
        std::uint64_t size = 0;
        bool executable = false;
    };
    struct manifest {
        // Required: a manifest that does not say its format is not read as this one.
        std::optional<int> format;
        std::string project, worldline, version;
        std::int64_t exported_at = 0;
        std::vector<file> files;
        // The project configuration at export, as JSON, with its digest; a restore returns it for the caller to apply.
        std::optional<std::string> configuration_digest;
    };

    // A directory name no package can have: package names start with a letter or digit.
    inline auto scratch(const std::filesystem::path& packages, std::string_view purpose) -> std::filesystem::path {
        return packages / std::format(".{}-{:016x}", purpose, std::random_device{}() * 0x100000001ull ^ std::random_device{}());
    }
    // A replacement in progress: the package's name, the old package moved aside, and the new one waiting for the name. Written before
    // the renames and removed after, so a crash between them is finished or undone when the package is next touched.
    struct replacement {
        std::string retired, staging;
    };
    inline auto journal_of(const std::filesystem::path& root) -> std::filesystem::path {
        return root.parent_path() / ("." + root.filename().string() + ".replacing");
    }
    inline void unlinked(const std::filesystem::path& path) {
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
            nodes::fail(nodes::code::invalid_record, "a package path goes through a symbolic link");
    }
    // The bytes of `path` if it is a regular file, not a link, of at most `limit` bytes; nothing else is opened or waited on.
    inline auto bounded(const std::filesystem::path& path, std::uint64_t limit) -> std::optional<std::string> {
        std::string bytes;
        try {
            for (auto piece : durability::read_plain(path, limit))
                bytes.append(reinterpret_cast<const char*>(piece.data()), piece.size());
        } catch (const durability::unreadable&) {
            return std::nullopt;
        }
        return bytes;
    }
    // Whether `path` is a regular file of exactly `size` bytes with digest `digest`, read in pieces, not held whole.
    inline auto holds(const std::filesystem::path& path, std::uint64_t size, const std::string& digest) -> bool {
        content::sha256 h;
        std::uint64_t seen = 0;
        try {
            for (auto piece : durability::read_plain(path, size)) {
                h.update(piece);
                seen += piece.size();
            }
        } catch (const durability::unreadable&) {
            return false;
        }
        return seen == size && content::to_hex(h.finish()) == digest;
    }
    // A name `scratch` gives for `purpose`, exactly.
    inline auto scratch_named(std::string_view name, std::string_view purpose) -> bool {
        auto prefix = std::format(".{}-", purpose);
        return name.size() == prefix.size() + 16 && name.starts_with(prefix) &&
               std::ranges::all_of(name.substr(prefix.size()), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
    // A real directory, not a link to one.
    inline auto directory(const std::filesystem::path& path) -> bool {
        std::error_code ec;
        return std::filesystem::is_directory(std::filesystem::symlink_status(path, ec)) && !ec;
    }
    // A journal is trusted only as a small regular file naming scratch directories as `scratch` names them.
    inline auto replacement_in(const std::filesystem::path& journal) -> std::optional<replacement> {
        auto text = bounded(journal, 4096);
        if (!text)
            return std::nullopt;
        auto r = rfl::json::read<replacement>(*text);
        if (!r || !scratch_named(r->retired, "retired") || !scratch_named(r->staging, "staging"))
            return std::nullopt;
        return *r;
    }

    struct contents {
        manifest kept;
        std::map<std::string, std::string> blobs;
        std::string configuration;
    };
    // The manifest of the package in directory `root` as it stands, checked as a whole: paths a worldline can hold together, one size
    // per digest. Nothing but the manifest is read.
    inline auto examine(const std::filesystem::path& root) -> manifest {
        unlinked(root);
        if (!std::filesystem::exists(std::filesystem::symlink_status(root)))
            nodes::fail(nodes::code::missing_objects, "no package has this name");
        if (!directory(root) || !directory(root / "blobs"))
            nodes::fail(nodes::code::invalid_record, "a package path goes through a symbolic link or is not a directory");
        auto text = bounded(root / "manifest.json", 64u << 20);
        if (!text)
            nodes::fail(nodes::code::invalid_record, "the package manifest is missing, not a plain file, or larger than any package holds");
        auto parsed = rfl::json::read<manifest>(*text);
        if (!parsed || parsed->format != format)
            nodes::fail(nodes::code::invalid_record, "the package manifest is unreadable or of another format");
        // Every entry of one digest declares the bytes that digest names, so limits see what is restored; no path is held twice or
        // both as a file and as a directory of another.
        std::map<std::string, std::uint64_t> sizes;
        std::set<std::string> paths, directories;
        for (auto& f : parsed->files) {
            content::checked_path(f.path);
            if (!content::is_hash(f.digest) || (f.kind != "text" && f.kind != "binary"))
                nodes::fail(nodes::code::invalid_record, "the package manifest names a file it cannot hold");
            if (auto [at, fresh] = sizes.emplace(f.digest, f.size); !fresh && at->second != f.size)
                nodes::fail(nodes::code::invalid_record, "the package manifest gives one digest two sizes");
            if (paths.contains(f.path) || directories.contains(f.path))
                nodes::fail(nodes::code::invalid_record, std::format("the package manifest holds {} twice or as a directory", f.path));
            for (auto slash = f.path.find('/'); slash != std::string::npos; slash = f.path.find('/', slash + 1)) {
                auto parent = f.path.substr(0, slash);
                if (paths.contains(parent))
                    nodes::fail(nodes::code::invalid_record, std::format("the package manifest holds {} both as a file and a directory", parent));
                directories.insert(parent);
            }
            paths.insert(f.path);
        }
        if (!parsed->configuration_digest && std::filesystem::exists(std::filesystem::symlink_status(root / "configuration.json")))
            nodes::fail(nodes::code::invalid_record, "the package holds a configuration its manifest does not declare");
        return std::move(*parsed);
    }
    inline constexpr std::uint64_t configuration_limit = 16u << 20;

    // Whether the package in directory `root` is whole: every blob and the configuration match the manifest. Nothing is held in memory.
    inline auto whole(const std::filesystem::path& root) -> bool {
        try {
            auto m = examine(root);
            std::set<std::string> seen;
            for (auto& f : m.files)
                if (seen.insert(f.digest).second && !holds(root / "blobs" / f.digest, f.size, f.digest))
                    return false;
            if (m.configuration_digest) {
                std::error_code ec;
                auto size = std::filesystem::file_size(root / "configuration.json", ec);
                if (ec || size > configuration_limit || !holds(root / "configuration.json", size, *m.configuration_digest))
                    return false;
            }
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // What `load`, `read` and `write` run when the caller has nothing to refuse.
    struct accept {
        void operator()(const auto&) const {}
    };

    // Loads the package in directory `root` as it stands. `check` sees the manifest before any file is loaded and may refuse it.
    template <class Check = accept> auto load(const std::filesystem::path& root, Check check = {}) -> contents {
        auto parsed = examine(root);
        check(parsed);
        std::map<std::string, std::string> blobs;
        nodes::error missing{nodes::code::missing_objects, "the package lacks or damages some of its files"};
        for (auto& f : parsed.files) {
            if (blobs.contains(f.digest))
                continue;
            auto bytes = bounded(root / "blobs" / f.digest, f.size);
            if (!bytes || bytes->size() != f.size || content::hash_of(*bytes) != f.digest) {
                missing.missing.push_back(f.path);
                continue;
            }
            blobs.emplace(f.digest, std::move(*bytes));
        }
        std::string configuration;
        if (parsed.configuration_digest) {
            auto pinned = bounded(root / "configuration.json", configuration_limit);
            if (!pinned || content::hash_of(*pinned) != *parsed.configuration_digest)
                missing.missing.push_back("configuration.json");
            else
                configuration = std::move(*pinned);
        }
        if (!missing.missing.empty())
            throw missing;
        return {std::move(parsed), std::move(blobs), std::move(configuration)};
    }

    // Finishes a replacement a crash interrupted: the new package takes the name if it loads whole, otherwise the old one gets it back.
    inline void recover(const std::filesystem::path& root) {
        unlinked(root.parent_path());
        auto journal = journal_of(root);
        if (!std::filesystem::exists(std::filesystem::symlink_status(journal)))
            return;
        auto r = replacement_in(journal);
        // A journal that cannot be trusted is kept: it may be all that names where the old package went.
        if (!r)
            nodes::fail(nodes::code::storage_failure, "the package's replacement journal is unreadable; it was kept for inspection");
        auto retired = root.parent_path() / r->retired, staging = root.parent_path() / r->staging;
        if (!std::filesystem::exists(std::filesystem::symlink_status(root))) {
            if (whole(staging))
                durability::durable_rename(staging, root);
            else if (whole(retired))
                durability::durable_rename(retired, root);
            else
                // Neither copy can take the name: what is left of them, and the journal naming it, are kept for inspection.
                nodes::fail(nodes::code::storage_failure, "neither copy of the package survived its replacement whole; both were kept for inspection");
        } else if (!whole(root))
            // A name held by something other than a whole package may be all that is left of neither copy: keep both and the journal.
            nodes::fail(nodes::code::storage_failure, "the package's name holds no whole package while a replacement is pending; it was kept for inspection");
        // Only the directories the journal names are removed, never what a link would lead to.
        std::error_code ignored;
        for (auto& dir : {retired, staging})
            if (directory(dir))
                std::filesystem::remove_all(dir, ignored);
            else if (std::filesystem::is_symlink(std::filesystem::symlink_status(dir, ignored)))
                std::filesystem::remove(dir, ignored);
        durability::durable_remove(journal);
    }

    // Removes what an export or replacement a crash interrupted left in `packages`: staging and retired directories that no
    // journal names. Run with the workspace's packages held, so no call of its own is using them.
    inline void sweep(const std::filesystem::path& packages) {
        unlinked(packages.parent_path());
        unlinked(packages);
        std::error_code ec;
        std::set<std::string> named;
        for (auto it = std::filesystem::directory_iterator(packages, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (auto name = it->path().filename().string(); name.starts_with(".") && name.ends_with(".replacing")) {
                auto r = replacement_in(it->path());
                // Next to a journal that cannot be trusted nothing is removed: what it named may be the only copy.
                if (!r)
                    return;
                named.insert(r->retired);
                named.insert(r->staging);
            }
        if (ec)
            return;
        for (auto it = std::filesystem::directory_iterator(packages, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            auto name = it->path().filename().string();
            std::error_code ignored;
            if ((scratch_named(name, "staging") || scratch_named(name, "retired")) && !named.contains(name)) {
                // A directory is removed with what it holds; anything else by its own name, never what it leads to.
                if (directory(it->path()))
                    std::filesystem::remove_all(it->path(), ignored);
                else
                    std::filesystem::remove(it->path(), ignored);
            } else if (name.starts_with(".") && name.ends_with(std::string{".replacing"} + std::string{constants::temporary_suffix}))
                // A journal a crash cut short while it was written was never trusted by anything.
                std::filesystem::remove(it->path(), ignored);
        }
    }

    // Writes `version` of `worldline` as package `root`; a package that exists is replaced only when asked.
    // `admit` sees the staged package before it takes its name and may refuse it; nothing it refuses is kept.
    template <class Admit = accept>
    auto write(nodes::local_node& node, const std::filesystem::path& root, const std::string& project, const std::string& worldline, const std::string& version,
               bool overwrite, const std::string& configuration = {}, Admit admit = {}) -> manifest {
        unlinked(root.parent_path());
        recover(root);
        unlinked(root);
        if (std::filesystem::exists(root) && !overwrite)
            nodes::fail(nodes::code::source_conflict, "a package with this name exists; export with force to replace it");
        auto tree = node.call({.action = "tree", .worldline = worldline, .revision = version});
        if (tree.code != "ok")
            nodes::fail(tree.code, tree.reason);
        manifest out{.format = format,
                     .project = project,
                     .worldline = worldline,
                     .version = tree.version,
                     .exported_at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
        auto staging = scratch(root.parent_path(), "staging");
        // A failed export leaves nothing behind to count against the workspace; a crashed one is swept on the next call.
        struct cleanup {
            std::filesystem::path dir;
            bool done = false;
            ~cleanup() {
                std::error_code ignored;
                if (!done)
                    std::filesystem::remove_all(dir, ignored);
            }
        } pending{staging};
        durability::durable_directories(staging / "blobs");
        for (auto& f : tree.files) {
            out.files.push_back({f.path, f.digest, f.kind, f.size, f.executable});
            if (!content::is_hash(f.digest))
                nodes::fail(nodes::code::invalid_record, "the node named " + f.path + " by something other than a digest");
            auto blob = staging / "blobs" / f.digest;
            if (std::filesystem::exists(blob))
                continue;
            std::string bytes;
            constexpr std::uint64_t chunk = 8u << 20;
            for (std::uint64_t at = 0; at < f.size || (f.size == 0 && at == 0); at += chunk) {
                nodes::node_request ask{.action = "content", .worldline = worldline, .revision = tree.version};
                ask.parts.push_back({f.digest, at, std::min(chunk, f.size - at), {}});
                auto read = node.call(ask);
                if (read.code != "ok" || read.parts.size() != 1)
                    nodes::fail(nodes::code::storage_failure, "reading " + f.path + " for the package failed: " + read.reason);
                bytes += nodes::unhex(read.parts[0].data);
                if (f.size == 0)
                    break;
            }
            // A package holds only bytes that match their names, checked before the old package is given up.
            if (bytes.size() != f.size || content::hash_of(bytes) != f.digest)
                nodes::fail(nodes::code::storage_failure, "the node's bytes for " + f.path + " do not match its digest");
            durability::durable_write(blob, bytes);
        }
        if (configuration.size() > configuration_limit)
            nodes::fail(nodes::code::resource_limit, "the project configuration is larger than a package holds");
        if (!configuration.empty()) {
            out.configuration_digest = content::hash_of(configuration);
            durability::durable_write(staging / "configuration.json", configuration);
        }
        durability::durable_write(staging / "manifest.json", rfl::json::write(out));
        // The staged package is read back as a restore will read it, so the old one is never given up for one that cannot be.
        examine(staging);
        admit(staging);
        // The old package stays whole until the new one has its name.
        if (std::filesystem::exists(std::filesystem::symlink_status(root))) {
            auto retired = scratch(root.parent_path(), "retired");
            auto journal = journal_of(root);
            durability::durable_write(journal, rfl::json::write(replacement{retired.filename().string(), staging.filename().string()}));
            durability::durable_rename(root, retired);
            durability::durable_rename(staging, root);
            std::filesystem::remove_all(retired);
            durability::durable_remove(journal);
        } else
            durability::durable_rename(staging, root);
        pending.done = true;
        return out;
    }

    // Reads package `root` back, after finishing any replacement a crash interrupted.
    template <class Check = accept> auto read(const std::filesystem::path& root, Check check = {}) -> contents {
        unlinked(root.parent_path());
        recover(root);
        return load(root, std::move(check));
    }

    // Creates worldline `name` holding exactly the package's files: an empty fork, then one batch that creates them. Both steps take
    // operation IDs derived from `operation`, so a retry replays them instead of repeating them.
    // What a restore records with its files, so a retry answers with what the first one restored.
    struct restored {
        std::string package, version;
        std::string configuration;
    };

    // Creates worldline `in.worldline` holding exactly the package's files: an empty fork, then one batch that creates them under the
    // caller's operation. Limits are checked before anything is created; a retry answers from the batch's receipt, whatever the
    // package holds by then.
    inline auto restore(nodes::local_node& node, const std::filesystem::path& root, const nodes::node_request& in) -> nodes::node_reply {
        auto done = node.call({.action = "receipt", .worldline = in.worldline, .operation = in.operation, .owner = node.node_id()});
        if (done.code == "ok") {
            // The operation ID answers only for this restore: the same package into the same worldline.
            auto kept = rfl::json::read<restored>(done.result);
            if (!kept || kept->package != in.target || done.worldline != in.worldline)
                nodes::fail(nodes::code::identity_conflict, "operation ID was used for another change");
            done.replayed = true;
            done.configuration = std::move(kept->configuration);
            return done;
        }
        if (done.code != nodes::code::outcome_unknown)
            return done;
        auto [kept, blobs, configuration] = read(root, [&in](const manifest& m) {
            std::uint64_t total = 0;
            for (auto& f : m.files) {
                if (in.file_limit && f.size > in.file_limit)
                    nodes::fail(nodes::code::resource_limit, std::format("{} is larger than the {} byte file limit", f.path, in.file_limit));
                total = f.size > std::numeric_limits<std::uint64_t>::max() - total ? std::numeric_limits<std::uint64_t>::max() : total + f.size;
            }
            if (in.project_limit && total > in.project_limit)
                nodes::fail(nodes::code::resource_limit,
                            std::format("the package holds {} bytes, more than the {} byte project limit", total, in.project_limit));
        });
        // An empty package is restored by its fork alone, which then holds the caller's operation and the restore's record.
        auto record = rfl::json::write(restored{in.target, kept.version, configuration});
        bool empty = kept.files.empty();
        auto fork = node.call({.action = "fork",
                               .worldline = in.worldline,
                               .operation = empty ? in.operation : content::hash_of(in.operation + ":fork").str(),
                               .owner = node.node_id(),
                               .actor = in.actor,
                               .caller_operation = in.caller_operation,
                               .intent = "restore package of " + kept.worldline + " at " + kept.version,
                               .result = empty ? record : std::string{}});
        if (fork.code != "ok")
            return fork;
        nodes::node_request files{.action = "apply",
                                  .worldline = in.worldline,
                                  .operation = in.operation,
                                  .owner = node.node_id(),
                                  .actor = in.actor,
                                  .caller_operation = in.caller_operation,
                                  .expected = fork.frontier,
                                  .intent = "restore package " + in.target,
                                  .result = record,
                                  .file_limit = in.file_limit,
                                  .project_limit = in.project_limit};
        for (auto& f : kept.files) {
            auto& bytes = blobs.at(f.digest);
            files.edits.push_back({.kind = f.kind == "text" ? "create" : "binary_create",
                                   .path = f.path,
                                   .text = f.kind == "text" ? bytes : nodes::hex(bytes),
                                   .executable = f.executable});
        }
        auto out = files.edits.empty() ? fork : node.call(files);
        out.configuration = std::move(configuration);
        return out;
    }
} // namespace package
