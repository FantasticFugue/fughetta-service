export module storage;
import constants;
import std;
import reflection;
import content;
import configuration;
import usage;
import durability;
import protocol;
import engine;
import codec;
import archive;
import async;
import native;

export namespace storage {
    struct stale_blob : std::runtime_error {
        explicit stale_blob(std::string_view hash) : std::runtime_error(std::format("blob {} is no longer in the project (files changed during sync)", hash)) {}
    };

    class project_files {
    public:
        project_files(std::string project, std::filesystem::path root, std::vector<std::string> ignore)
            : project_(std::move(project)), root_(std::move(root)), ignore_(std::move(ignore)) {
            std::filesystem::create_directories(root_);
            rescan();
        }

        auto project() const -> const std::string& { return project_; }
        auto root() const -> const std::filesystem::path& { return root_; }
        auto bytes() const -> std::uint64_t { return bytes_; }

        void set_ignore(std::vector<std::string> ignore) {
            if (ignore == ignore_)
                return;
            ignore_ = std::move(ignore);
            rescan();
        }

        auto contains(std::string_view path) const -> bool { return files_.contains(path); }
        auto size_of(std::string_view path) const -> std::uint64_t {
            auto it = files_.find(path);
            return it == files_.end() ? 0 : it->second.e.size;
        }

        auto manifest() const -> content::manifest {
            content::manifest m;
            m.files.reserve(files_.size());
            for (auto& [_, t] : files_)
                m.files.push_back(t.e);
            return m;
        }

        struct capture_record {
            std::string tool, request_digest, source_head;
        };
        auto capture_node(nodes::local_node& repository, std::string operation, std::string caller_operation, std::string actor,
                          const std::vector<std::string>& expected, std::string intent, std::uint64_t file_limit, std::uint64_t project_limit)
            -> nodes::node_reply {
            if (!content::is_hash(operation))
                return {.code = std::string{nodes::code::invalid_record}, .reason = "capture requires a 32-byte lowercase operation ID"};
            if (repository.project_id().empty()) {
                auto initialized = repository.initialize();
                if (initialized.code != "ok")
                    return initialized;
            }
            auto request_digest = nodes::object_id(nodes::value::map{{"expected", nodes::strings(expected)}, {"intent", intent}, {"operation", operation}});
            auto retained = repository.call({.action = "receipt", .operation = operation});
            if (retained.code == "ok") {
                auto record = rfl::json::read<capture_record, rfl::NoExtraFields>(retained.result);
                if (!record || record->tool != "workspace.capture/1" || record->request_digest != request_digest)
                    return {.code = std::string{nodes::code::identity_conflict}, .reason = "capture operation ID has another accepted input"};
                retained.replayed = true;
                return retained;
            }
            if (retained.code != nodes::code::outcome_unknown)
                return retained;
            auto state = repository.call({.action = "inspect"});
            if (state.code != "ok")
                return state;
            if (state.frontier != expected)
                return {.code = "stale_frontier", .reason = "selected main frontier changed"};
            if (!state.files.empty())
                return {.code = std::string{nodes::code::source_conflict},
                        .reason = "capture requires an empty worldline; edit the existing worldline through the node API"};
            rescan(true);
            auto tree = manifest();
            nodes::archive::files input;
            std::vector<nodes::edit> edits;
            for (auto& entry : tree.files) {
                auto bytes = input.read(root_ / entry.path);
                if (content::hash_of(bytes) != entry.digest)
                    throw stale_blob{entry.digest};
                bool binary = bytes.find('\0') != std::string::npos;
                if (!binary)
                    try {
                        (void)nodes::scalars(bytes);
                    } catch (const nodes::error& error) {
                        if (error.code != nodes::code::invalid_record)
                            throw;
                        binary = true;
                    }
                edits.push_back({.kind = binary ? "binary_create" : "create",
                                 .path = entry.path,
                                 .text = binary ? nodes::hex(bytes) : std::move(bytes),
                                 .executable = entry.executable});
            }
            return repository.call({.action = "apply",
                                    .operation = std::move(operation),
                                    .owner = repository.node_id(),
                                    .actor = std::move(actor),
                                    .caller_operation = std::move(caller_operation),
                                    .expected = expected,
                                    .edits = std::move(edits),
                                    .intent = std::move(intent),
                                    .result = rfl::json::write(capture_record{"workspace.capture/1", request_digest, tree.id().str()}),
                                    .tool = "workspace.capture/1",
                                    .capture = true,
                                    .file_limit = file_limit,
                                    .project_limit = project_limit});
        }

        auto blob_part(const content::hash& h, std::uint64_t offset, std::uint64_t length) const -> std::vector<std::byte> {
            auto it = by_hash_.find(h);
            if (it == by_hash_.end() || it->second.empty())
                throw stale_blob{h};
            auto path = root_ / *it->second.begin();
            auto size = std::filesystem::file_size(path);
            if (offset > size || length > size - offset)
                throw stale_blob{h};
            native::context resource;
            auto in = io::open_file(resource.get_scheduler(), path);
            std::vector<std::byte> out(length);
            for (std::size_t done = 0; done < out.size();) {
                auto part = std::span{out}.subspan(done, std::min<std::size_t>(1024 * 1024, out.size() - done));
                auto count = cores::run(resource, io::async_read_at(in, part, offset + done));
                if (!count || !*count)
                    throw stale_blob{h};
                done += *count;
            }
            return out;
        }

        void rescan(bool full = false) {
            std::map<std::string, tracked_file> seen;
            std::error_code ec;
            auto it = std::filesystem::recursive_directory_iterator(root_, ec);
            if (ec)
                throw std::system_error(ec, std::format("cannot scan {}", root_.string()));
            for (auto end = std::filesystem::recursive_directory_iterator{}; it != end; it.increment(ec)) {
                if (ec)
                    throw std::system_error(ec, std::format("cannot scan {}", root_.string()));
                auto rel = std::filesystem::relative(it->path(), root_).generic_string();
                auto status = it->symlink_status();
                if (std::filesystem::is_symlink(status))
                    continue;
                if (content::ignored(ignore_, rel) || rel.ends_with(temp_suffix)) {
                    if (std::filesystem::is_directory(status))
                        it.disable_recursion_pending();
                    continue;
                }
                if (!std::filesystem::is_regular_file(status))
                    continue;
                auto mtime = it->last_write_time();
                auto size = it->file_size();
                bool executable = durability::is_executable(it->path());
                if (auto old = files_.find(rel);
                    !full && old != files_.end() && old->second.mtime == mtime && old->second.e.size == size && old->second.e.executable == executable) {
                    seen.emplace(rel, old->second);
                    continue;
                }
                auto text = slurp(it->path());
                seen.emplace(rel, tracked_file{{.path = rel, .digest = content::hash_of(text), .size = text.size(), .executable = executable}, mtime});
            }
            if (ec)
                throw std::system_error(ec, std::format("cannot scan {}", root_.string()));
            files_.clear();
            by_hash_.clear();
            bytes_ = 0;
            for (auto& [rel, t] : seen)
                add(t.e, t.mtime);
        }

    private:
        static constexpr std::string_view temp_suffix = constants::temporary_suffix;

        struct tracked_file {
            content::entry e;
            std::filesystem::file_time_type mtime;
        };

        static auto slurp(const std::filesystem::path& p) -> std::string {
            std::ifstream in{p, std::ios::binary};
            if (!in)
                throw std::runtime_error(std::format("cannot read {}", p.string()));
            return {std::istreambuf_iterator<char>{in}, {}};
        }

        auto tracked(std::string_view path) const -> std::string {
            auto p = std::string{content::checked_path(path)};
            if (!contains(p))
                throw std::invalid_argument(std::format("no such file '{}'", p));
            return p;
        }

        auto writable(std::string_view path) const -> std::string {
            auto p = std::string{content::checked_path(path)};
            if (content::ignored(ignore_, p))
                throw std::invalid_argument(std::format("'{}' is ignored by the project's configuration", p));
            auto at = root_;
            for (auto part : std::filesystem::path{p}) {
                at /= part;
                std::error_code ec;
                auto st = std::filesystem::symlink_status(at, ec);
                if (ec || !std::filesystem::exists(st))
                    break;
                if (std::filesystem::is_symlink(st))
                    throw std::invalid_argument(std::format("'{}' goes through a symlink", p));
            }
            return p;
        }

        void store(const std::string& rel, std::string_view text, bool executable) const {
            auto target = root_ / rel;
            durability::durable_directories(target.parent_path());
            inside(target.parent_path());
            durability::durable_write(target, text, executable, temp_suffix);
        }

        void inside(const std::filesystem::path& dir) const {
            auto real = std::filesystem::canonical(dir);
            auto base = std::filesystem::canonical(root_);
            auto [b, r] = std::ranges::mismatch(base, real);
            if (b != base.end())
                throw std::invalid_argument(std::format("'{}' leads out of the project", dir.string()));
        }

        auto track(const std::string& rel, std::string_view text, bool executable) -> content::entry {
            if (auto it = files_.find(rel); it != files_.end())
                untrack(it);
            content::entry e{.path = rel, .digest = content::hash_of(text), .size = text.size(), .executable = executable};
            add(e, std::filesystem::last_write_time(root_ / rel));
            return e;
        }

        void add(const content::entry& e, std::filesystem::file_time_type mtime) {
            by_hash_[e.digest].insert(e.path);
            bytes_ += e.size;
            files_[e.path] = tracked_file{e, mtime};
        }

        void untrack(std::map<std::string, tracked_file>::iterator it) {
            auto& paths = by_hash_[it->second.e.digest];
            paths.erase(it->first);
            if (paths.empty())
                by_hash_.erase(it->second.e.digest);
            bytes_ -= it->second.e.size;
            files_.erase(it);
        }

        std::string project_;
        std::filesystem::path root_;
        std::vector<std::string> ignore_;
        std::map<std::string, tracked_file, std::less<>> files_;
        std::unordered_map<content::hash, std::set<std::string>> by_hash_;
        std::uint64_t bytes_ = 0;
    };

    class native_nodes {
        struct entry {
            std::mutex mutex, read_mutex, initialization;
            std::unique_ptr<nodes::local_node> node, reader;
            std::uint64_t used = 0;
        };
        std::mutex mutex_;
        std::map<std::filesystem::path, std::shared_ptr<entry>> entries_;
        std::uint64_t clock_ = 0;

    public:
        template <class F> auto with(const std::filesystem::path& root, F f, bool reading = false) -> std::invoke_result_t<F&, nodes::local_node&> {
            std::shared_ptr<entry> selected;
            {
                std::lock_guard lock{mutex_};
                auto& slot = entries_[root];
                if (!slot)
                    slot = std::make_shared<entry>();
                selected = slot;
                selected->used = ++clock_;
                while (entries_.size() > 2) {
                    auto oldest = entries_.end();
                    for (auto it = entries_.begin(); it != entries_.end(); ++it)
                        if (it->second.use_count() == 1 && (oldest == entries_.end() || it->second->used < oldest->second->used))
                            oldest = it;
                    if (oldest == entries_.end())
                        break;
                    entries_.erase(oldest);
                }
            }
            std::lock_guard lock{reading ? selected->read_mutex : selected->mutex};
            {
                std::lock_guard initialize{selected->initialization};
                // Every project's node starts with an empty main worldline the first time it is opened.
                if (!selected->node) {
                    selected->node = std::make_unique<nodes::local_node>(root);
                    if (selected->node->project_id().empty())
                        selected->node->initialize();
                }
                if (reading && !selected->reader)
                    selected->reader = std::make_unique<nodes::local_node>(root, std::string{}, false, true);
            }
            return f(reading ? *selected->reader : *selected->node);
        }
    };

} // namespace storage
