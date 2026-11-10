export module server;
import std;
import native;
import coroutine;
import protocol;
import configuration;
import content;
import storage;
import engine;
import codec;
import durability;
import usage;
import package;
import constants;
import reflection;

export namespace source_server {
    class service {
    public:
        using socket = native::socket;

        service(cores::context<native::scheduler>& rt, std::filesystem::path storage, execution::thread_pool& pool)
            : rt_(rt), storage_(std::move(storage)), lock_(storage_, "source"), pool_(pool), cores_(rt.size()), token_(jobs::source_token()) {}

        auto serve(native::scheduler& io, socket peer, std::size_t core) -> coroutine::task<> {
            transport::channel<socket> chan{peer};
            transport::dispatcher<socket> dispatch;
            dispatch.on<jobs::node_call>([this, core](jobs::node_call request, auto&) { return node(core, std::move(request)); });
            dispatch.on<jobs::node_capture>([this, core](jobs::node_capture request, auto&) { return capture(core, std::move(request)); });
            dispatch.on<jobs::node_usage>([this, core](jobs::node_usage request, auto&) { return usage(core, std::move(request)); });
            try {
                auto first = co_await coroutine::deadline(io, std::chrono::seconds{10}, chan.recv());
                if (!first || first->kind != jobs::code<jobs::hello>)
                    co_return;
                auto hello = transport::decode<jobs::hello>(*first);
                if (!content::secure_equal(hello.auth, token_))
                    co_return;
                if (hello.protocol != jobs::protocol_version) {
                    co_await chan.fail(0, "gateway/source protocol mismatch");
                    co_return;
                }
                co_await coroutine::with_scope([&](coroutine::scope& handlers) -> coroutine::task<> {
                    // A well-behaved gateway sends one call per connection; the cap keeps a looping or compromised peer from growing handlers and payload memory without bound.
                    std::atomic<std::size_t> inflight{0};
                    for (;;) {
                        auto frame = co_await chan.recv();
                        // Cancellation is cheap and must always reach the running call, so it never waits for a slot.
                        if (frame.kind == jobs::code<jobs::cancel>) {
                            handlers.spawn(dispatch.handle(chan, std::move(frame)));
                            continue;
                        }
                        if (inflight.load(std::memory_order_relaxed) >= constants::source_handler_limit) {
                            co_await chan.fail(frame.corr, "source handler limit reached", "source_overloaded");
                            continue;
                        }
                        inflight.fetch_add(1, std::memory_order_relaxed);
                        handlers.spawn(tracked(dispatch, chan, std::move(frame), inflight));
                    }
                });
            } catch (const std::exception&) {
            }
        }

    private:
        template <class S>
        static auto tracked(transport::dispatcher<S>& dispatch, transport::channel<S>& chan, transport::frame frame, std::atomic<std::size_t>& inflight)
            -> coroutine::task<> {
            struct release {
                std::atomic<std::size_t>& inflight;
                ~release() { inflight.fetch_sub(1, std::memory_order_relaxed); }
            } counted{inflight};
            co_await dispatch.handle(chan, std::move(frame));
        }

        // The gateway authenticated the caller; a workspace role authorizes source access.
        static void authorize(const worlds::actor& actor) {
            if (actor.issuer == "source" && actor.role == "service")
                return;
            if (actor.user.empty() || !worlds::rank(actor.role))
                throw transport::coded_error(nodes::code::permission_denied, "the caller has no role in the workspace");
        }

        struct per_core {
            std::map<std::string, std::unique_ptr<storage::project_files>> projects;
            std::map<std::string, std::unique_ptr<coroutine::mutex>> locks;
        };

        template <class F>
        auto with(std::size_t here, std::string project, std::vector<std::string> ignore, F work)
            -> coroutine::task<std::invoke_result_t<F&, storage::project_files&>> {
            config::check_name("project", project);
            auto owner = std::hash<std::string_view>{}(project) % rt_.size();
            if (owner == here)
                co_return co_await serial(owner, std::move(project), std::move(ignore), std::move(work));
            co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(owner),
                                           [this, owner, project, ignore = std::move(ignore), work] { return serial(owner, project, ignore, work); });
        }

        template <class F>
        auto serial(std::size_t core, std::string project, std::vector<std::string> ignore, F work)
            -> coroutine::task<std::invoke_result_t<F&, storage::project_files&>> {
            auto& state = cores_[core];
            auto& lock = state.locks[project];
            if (!lock)
                lock = std::make_unique<coroutine::mutex>();
            auto held = co_await lock->lock();
            auto& slot = state.projects[project];
            co_return co_await io::blocking(rt_.get_scheduler(core), pool_, [this, &slot, project, ignore, work]() mutable {
                if (!slot) {
                    auto root = storage_ / project;
                    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root)))
                        throw std::invalid_argument("source project root goes through a symlink");
                    slot = std::make_unique<storage::project_files>(project, root, std::move(ignore));
                } else
                    slot->set_ignore(std::move(ignore));
                try {
                    return work(*slot);
                } catch (...) {
                    slot.reset();
                    throw;
                }
            });
        }

        // Node history names the signed-in user, whichever credential it used, and operation IDs are chosen by callers,
        // so each user gets its own operation space in the node.
        static auto author_key(const worlds::actor& who) -> std::string { return who.issuer + ":" + who.user; }
        static auto scoped(const worlds::actor& who, const std::string& operation) -> std::string {
            return content::hash_of(worlds::identity_key({.issuer = who.issuer, .user = who.user}) + "\n" + (who.run.empty() ? std::string{} : who.run + "\n") + operation).str();
        }

        // The bytes a project's node repository holds on disk, its whole history included.
        static auto stored(const std::filesystem::path& repository) -> std::uint64_t {
            std::uint64_t n = 0;
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(repository, ec); !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec))
                if (it->is_regular_file(ec) && !it->is_symlink(ec))
                    if (auto size = it->file_size(ec); !ec)
                        n += size;
            return n;
        }
        // What the workspace stores: its node repositories and the packages exported from them.
        auto stored_all() -> std::uint64_t {
            std::uint64_t n = 0;
            std::error_code ec;
            for (auto it = std::filesystem::directory_iterator(storage_ / ".nodes", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
                if (it->is_directory(ec) && !it->is_symlink(ec))
                    n += stored(it->path());
            return n + stored(storage_ / ".packages");
        }

        // While it lives, each write of `node` commits, with a storage limit, only while the workspace stores less than it. A replay commits
        // nothing and is never refused here.
        class confirmation final : public nodes::commit_check {
        public:
            confirmation(service& owner, nodes::local_node& node, const worlds::actor& actor, std::uint64_t storage_limit)
                : owner_(owner), node_(node), actor_(actor), storage_limit_(storage_limit) {
                node_.confirm_with(this);
            }
            confirmation(const confirmation&) = delete;
            ~confirmation() { node_.confirm_with(nullptr); }

            void check() override {
                owner_.authorize(actor_);
                if (storage_limit_) {
                    if (auto used = owner_.stored_all(); used >= storage_limit_)
                        nodes::fail(nodes::code::resource_limit, std::format("the workspace stores {} bytes of history (limit {}); history is kept, so only a "
                                                                             "higher storage limit or removing a project makes room",
                                                                             used, storage_limit_));
                }
            }

        private:
            service& owner_;
            nodes::local_node& node_;
            const worlds::actor& actor_;
            std::uint64_t storage_limit_;
        };

        // Packages live beside the repositories: `target` names one; an export writes the selected version into it, and a
        // restore creates worldline `worldline` from it.
        auto packaged(nodes::local_node& node, const jobs::node_call& call) -> nodes::node_reply {
            auto& in = call.request;
            nodes::node_reply out;
            try {
                config::check_name("package", in.target);
                auto root = storage_ / ".packages" / in.target;
                // Packages are written and read one call at a time, so the storage is checked against what is there.
                std::lock_guard busy{packages_mutex_};
                // Nothing is moved or removed through a link out of the storage.
                package::unlinked(storage_ / ".packages");
                package::sweep(root.parent_path());
                // A replacement a crash interrupted is finished first, so it is neither counted twice nor in the way.
                package::recover(root);
                if (in.action == "export") {
                    // The staged package is measured as written, manifest and configuration included; the one it replaces is freed.
                    auto refuse = [&] {
                        nodes::fail(nodes::code::resource_limit,
                                    std::format("the package would bring the workspace past its {} byte storage limit", in.workspace_limit));
                    };
                    // The package this export replaces is freed by it. A new package is refused once the workspace is at its limit; a
                    // replacement only when it grows the workspace past it, so a smaller one is always possible.
                    bool replacing = std::filesystem::exists(std::filesystem::symlink_status(root));
                    if (in.workspace_limit && !replacing && stored_all() >= in.workspace_limit)
                        refuse();
                    durability::durable_directories(root.parent_path());
                    auto admit = [&](const std::filesystem::path& staged) {
                        auto total = stored_all(), old = stored(root), fresh = stored(staged);
                        auto after = total - std::min(total, old);
                        if (in.workspace_limit && after > in.workspace_limit && fresh > old)
                            refuse();
                    };
                    auto kept = package::write(node, root, call.project, in.worldline, in.revision, in.force, in.configuration, admit);
                    out.version = kept.version;
                    for (auto& f : kept.files)
                        out.files.push_back({.path = f.path, .kind = f.kind, .digest = f.digest, .size = f.size, .executable = f.executable});
                    return out;
                }
                if (!content::is_hash(in.operation))
                    nodes::fail(nodes::code::invalid_record, "a restore names its operation");
                return package::restore(node, root, in);
            } catch (const nodes::error& e) {
                out.code = e.code;
                out.reason = e.what();
                out.missing = e.missing;
            } catch (const std::invalid_argument& e) {
                out.code = nodes::code::invalid_record;
                out.reason = e.what();
            } catch (const std::exception& e) {
                // A filesystem failure is a definite refusal: the old package keeps its name until the new one takes it.
                out.code = nodes::code::storage_failure;
                out.reason = e.what();
            }
            return out;
        }

        auto usage(std::size_t core, jobs::node_usage request) -> coroutine::task<jobs::node_usage_reply> {
            co_return co_await io::blocking(rt_.get_scheduler(core), pool_, [this, request = std::move(request)] {
                authorize(request.actor);
                jobs::node_usage_reply out;
                std::error_code ec;
                for (auto& entry : std::filesystem::directory_iterator(storage_ / ".nodes", ec)) {
                    if (!entry.is_directory(ec) || entry.is_symlink(ec))
                        continue;
                    jobs::project_usage u{.project = entry.path().filename().string(), .storage_bytes = stored(entry.path())};
                    auto tree =
                        repositories_.with(entry.path(), [](nodes::local_node& node) { return node.call({.action = "tree", .worldline = "main"}); }, true);
                    for (auto& file : tree.files)
                        u.source_bytes += file.size;
                    out.projects.push_back(std::move(u));
                }
                std::ranges::sort(out.projects, {}, &jobs::project_usage::project);
                out.packages_bytes = stored(storage_ / ".packages");
                return out;
            });
        }

        auto node(std::size_t core, jobs::node_call request) -> coroutine::task<nodes::node_reply> {
            config::check_name("project", request.project);
            auto root = storage_ / ".nodes" / request.project;
            co_return co_await io::blocking(rt_.get_scheduler(core), pool_, [this, root, request = std::move(request)]() mutable {
                authorize(request.actor);
                // A workspace at its storage limit accepts reversals but no additional source history.
                static const std::set<std::string, std::less<>> growing{"mail_send", "apply", "promote", "fork",    "integrate", "admit",
                                                                        "resolve",   "redo",  "receive", "restore"};
                auto stores = growing.contains(request.request.action) ? request.request.workspace_limit : 0;
                return repositories_.with(
                    root,
                    [&](nodes::local_node& node) {
                        confirmation confirmed{*this, node, request.actor, stores};
                        if (request.request.owner.empty())
                            request.request.owner = node.node_id();
                        // History names the authenticated user.
                        request.request.actor = author_key(request.actor) + (request.actor.run.empty() ? "" : "/task/" + request.actor.task + "/run/" + request.actor.run);
                        request.request.lease_owner = worlds::identity_key(request.actor) + "\n" + request.actor.client.value_or("");
                        request.request.execution_owner = worlds::identity_key({.issuer = request.actor.issuer, .user = request.actor.user});
                        if (request.request.action == "task_lookup" && !(request.actor.issuer == "source" && request.actor.role == "service" && request.actor.caller == "execution"))
                            throw transport::coded_error(nodes::code::permission_denied, "execution lookup is internal");
                        if (!request.actor.execution_token.empty()) {
                            if (request.actor.project != request.project)
                                throw transport::coded_error(nodes::code::permission_denied, "execution belongs to another project");
                            request.request.execution_token = request.actor.execution_token;
                            request.request.run_id = request.actor.run;
                            request.request.lease_owner = request.actor.run;
                        } else if (request.request.action != "task_lookup") {
                            request.request.execution_token.clear();
                        }
                        if (request.request.action == "task_start")
                            request.request.execution_identity = rfl::json::write(request.actor);
                        request.request.capture = false;
                        // The gateway may name the caller's own operation ID for display; the engine keys on the scoped identity.
                        if (content::is_hash(request.request.operation)) {
                            if (request.request.caller_operation.empty())
                                request.request.caller_operation = request.request.operation;
                            request.request.operation = scoped(request.actor, request.request.operation);
                        } else
                            request.request.caller_operation.clear();
                        if (request.request.action == "export" || request.request.action == "restore")
                            return packaged(node, request);
                        return node.call(request.request);
                    },
                    request.reading);
            });
        }

        auto capture(std::size_t core, jobs::node_capture request) -> coroutine::task<nodes::node_reply> {
            auto project = request.project;
            auto ignore = request.ignore;
            co_return co_await with(core, std::move(project), std::move(ignore), [this, request = std::move(request)](storage::project_files& files) {
                authorize(request.actor);
                config::check_name("project", request.project);
                return repositories_.with(storage_ / ".nodes" / request.project, [&](nodes::local_node& node) {
                    confirmation confirmed{*this, node, request.actor, request.workspace_limit};
                    auto reply = files.capture_node(node, content::is_hash(request.operation) ? scoped(request.actor, request.operation) : request.operation,
                                                    request.operation, author_key(request.actor), request.expected, request.intent, request.file_limit,
                                                    request.project_limit);
                    reply.operation = request.operation;
                    return reply;
                });
            });
        }

        storage::native_nodes repositories_;
        std::mutex packages_mutex_;
        cores::context<native::scheduler>& rt_;
        std::filesystem::path storage_;
        durability::dir_lock lock_;
        execution::thread_pool& pool_;
        std::vector<per_core> cores_;
        std::string token_;
    };
} // namespace source_server
