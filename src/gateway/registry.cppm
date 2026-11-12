export module registry;
import constants;
import std;
export import utility;
import coroutine;
import content;
import configuration;
import usage;
import durability;
import protocol;
import client;
import async;
import native;
import delivery;
import constants;

export namespace storage {
    struct project_ctx {
        const config::document& config;
        config::effective settings;
        usage::ledger& ledger;
    };

    class registry {
    public:
        registry(cores::context<native::scheduler>& rt, std::filesystem::path storage, const config::slots& config, execution::thread_pool& pool,
                 net::ip::tcp::endpoint source)
            : rt_(rt), storage_(std::move(storage)), lock_(storage_, "storage"), config_(config), pool_(pool), source_(source) {}

        std::function<void()> changed;

        auto has_export(std::size_t here, content::hash digest, std::uint64_t size) -> coroutine::task<bool> {
            if (!content::is_hash(digest)) co_return false;
            co_return co_await io::blocking(rt_.get_scheduler(here), pool_, [this, digest, size] {
                std::lock_guard held{exports_mutex_};
                const auto path = storage_ / ".exports/blobs" / digest.view();
                std::error_code ec;
                const auto status = std::filesystem::symlink_status(path, ec);
                if (ec || !std::filesystem::is_regular_file(status)) return false;
                const auto stored = std::filesystem::file_size(path, ec);
                if (ec || stored != size) return false;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
                return !ec;
            });
        }

        auto receive_export(std::size_t here, jobs::source_part part) -> coroutine::task<> {
            co_await io::blocking(rt_.get_scheduler(here), pool_, [this, part = std::move(part)] {
                if (!content::is_hash(part.digest)) throw std::invalid_argument("invalid export digest");
                std::shared_ptr<std::mutex> receiver;
                {
                    std::lock_guard held{exports_mutex_};
                    std::erase_if(export_receivers_, [](const auto& item) { return item.second.expired(); });
                    auto& slot = export_receivers_[part.digest.str()];
                    receiver = slot.lock();
                    if (!receiver) slot = receiver = std::make_shared<std::mutex>();
                    collect_exports();
                }
                std::lock_guard receiving{*receiver};
                const auto storage = storage_ / ".exports/blobs";
                {
                    std::lock_guard held{exports_mutex_};
                    const auto target = storage / part.digest.view();
                    if (std::filesystem::is_regular_file(std::filesystem::symlink_status(target)) &&
                        std::filesystem::file_size(target) == part.total) {
                        std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now());
                        return;
                    }
                }
                if (!delivery::stage(storage, part)) return;
                delivery::verify(storage, part.digest, part.total);
                std::lock_guard held{exports_mutex_};
                delivery::publish(storage, part.digest);
            });
        }

        auto grant_exports(std::size_t here, std::string project, std::vector<builds::exported_file> files) -> coroutine::task<std::vector<builds::exported_file>> {
            try {
                config::resolve(*config_.current(here), project);
                content::checked_name(project);
            } catch (const std::exception& error) {
                for (auto& file : files)
                    if (file.delivery_error.empty()) file.delivery_error = std::string{error.what()}.substr(0, 512);
                co_return files;
            }
            co_return co_await io::blocking(rt_.get_scheduler(here), pool_, [this, project, files = std::move(files)] mutable {
                std::lock_guard held{exports_mutex_};
                collect_exports();
                const auto directory = storage_ / ".exports/projects" / project;
                for (auto& file : files) {
                    if (!file.delivery_error.empty()) continue;
                    try {
                    std::filesystem::create_directories(directory);
                    if (!content::is_hash(file.digest)) throw std::invalid_argument("invalid export digest");
                    const auto blob = storage_ / ".exports/blobs" / file.digest.view();
                    if (std::filesystem::file_size(blob) != file.size) throw std::runtime_error("export size mismatch");
                    std::filesystem::last_write_time(blob, std::filesystem::file_time_type::clock::now());
                    std::error_code ec;
                    const auto granted = directory / file.digest.view();
                    if (std::filesystem::exists(granted) && !std::filesystem::equivalent(blob, granted)) std::filesystem::remove(granted);
                    std::filesystem::create_hard_link(blob, granted, ec);
                    if (ec && ec != std::errc::file_exists) throw std::filesystem::filesystem_error("grant export", blob, ec);
                    } catch (const std::exception& error) { file.delivery_error = std::string{error.what()}.substr(0, 512); }
                }
                return files;
            });
        }

        auto read_export(std::size_t here, std::string project, jobs::export_read request) -> coroutine::task<jobs::source_bytes> {
            config::resolve(*config_.current(here), project);
            content::checked_name(project);
            co_return co_await io::blocking(rt_.get_scheduler(here), pool_, [this, project, request] {
                std::lock_guard held{exports_mutex_};
                collect_exports();
                return delivery::read(storage_ / ".exports/projects" / project, request);
            });
        }

        auto node(std::size_t here, std::string project, nodes::node_request request, worlds::actor actor, bool reading) -> coroutine::task<nodes::node_reply> {
            // The source opens a repository for any project it is asked about, so only configured projects reach it.
            auto doc = config_.current(here);
            if (!doc->find(project))
                throw config::not_found(std::format("unknown project '{}'", project));
            // Limits come from the configuration, whatever the caller sent.
            auto limits = config::resolve(*doc, project).limits;
            request.file_limit = limits.max_file_bytes;
            request.project_limit = limits.max_project_bytes;
            request.workspace_limit = limits.max_workspace_bytes;
            auto out = co_await remote(here, jobs::node_call{std::move(project), std::move(actor), std::move(request), reading});
            if (!reading && changed)
                changed();
            co_return out;
        }

        // What the source stores for each project.
        auto usage(std::size_t here, worlds::actor actor) -> coroutine::task<jobs::node_usage_reply> {
            co_return co_await remote(here, jobs::node_usage{std::move(actor)});
        }

        auto capture(std::size_t here, std::string project, std::string operation, std::vector<std::string> expected, std::string intent, worlds::actor actor)
            -> coroutine::task<nodes::node_reply> {
            auto settings = config::resolve(*config_.current(here), project);
            auto out = co_await remote(here, jobs::node_capture{std::move(project), settings.ignore, std::move(actor), std::move(operation),
                                                                std::move(expected), std::move(intent), settings.limits.max_file_bytes,
                                                                settings.limits.max_project_bytes, settings.limits.max_workspace_bytes});
            if (changed)
                changed();
            co_return out;
        }

        template <class F>
            requires std::copy_constructible<F>
        auto with(std::size_t here, std::string project, F f) -> coroutine::task<std::invoke_result_t<F&, project_ctx&>> {
            co_return co_await with_ledger(here, [project = std::move(project), f](usage::ledger& ledger, const config::document& d) mutable {
                project_ctx ctx{d, config::resolve(d, project), ledger};
                return f(ctx);
            });
        }

        // The usage ledger lives on core zero; every use of it runs there, one at a time, on the disk pool.
        template <class F>
            requires std::copy_constructible<F>
        auto with_ledger(std::size_t here, F f) -> coroutine::task<std::invoke_result_t<F&, usage::ledger&, const config::document&>> {
            if (here == 0)
                co_return co_await serial(std::move(f));
            co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), [this, f] { return serial(f); });
        }

        // Runs `f` on the ledger later, without waiting for it.
        template <class F> void post(std::size_t here, F f) {
            auto start = [this, f = std::move(f)] mutable {
                pending_.emplace_back(std::move(f));
                if (!draining_) {
                    draining_ = true;
                    rt_.spawn(0, drain());
                }
            };
            if (here == 0)
                start();
            else
                rt_.post(0, std::move(start));
        }

    private:
        void collect_exports() {
            const auto now = std::chrono::steady_clock::now();
            if (now < export_collect_) return;
            export_collect_ = now + constants::export_collect_interval;
            const auto blobs = storage_ / ".exports/blobs";
            std::set<std::string> receiving;
            for (const auto& [digest, lock] : export_receivers_) if (!lock.expired()) receiving.insert(digest);
            delivery::collect(blobs, constants::export_retention, constants::export_cache_bytes, receiving);
            std::error_code ec;
            for (const auto& project : std::filesystem::directory_iterator(storage_ / ".exports/projects", ec)) {
                if (!std::filesystem::is_directory(project.symlink_status(ec)) || ec) continue;
                for (const auto& file : std::filesystem::directory_iterator(project.path(), ec)) {
                    if (!std::filesystem::is_regular_file(file.symlink_status(ec)) || ec || !content::is_hash(file.path().filename().string())) continue;
                    const auto blob = blobs / file.path().filename();
                    if (!std::filesystem::exists(blob, ec) && !ec) std::filesystem::remove(file.path(), ec);
                    else if (!ec && !std::filesystem::equivalent(blob, file.path(), ec) && !ec) {
                        std::filesystem::remove(file.path(), ec);
                        if (!ec) std::filesystem::create_hard_link(blob, file.path(), ec);
                    }
                }
                std::filesystem::remove(project.path(), ec);
            }
        }

        template <class Request> auto remote(std::size_t here, Request request) -> coroutine::task<jobs::reply_t<Request>> {
            auto stop = co_await coroutine::current_token;
            try {
                co_return co_await io::blocking(rt_.get_scheduler(here), pool_, [this, request = std::move(request), stop]() mutable {
                    return source::call(source_, std::move(request), stop);
                });
            } catch (const source::stopped&) {
            }
            co_await coroutine::stop;
            std::unreachable();
        }

        auto drain() -> coroutine::task<> {
            struct reset {
                bool& value;
                ~reset() { value = false; }
            } done{draining_};
            do {
                co_await serial([](usage::ledger&, const config::document&) {});
            } while (!pending_.empty());
        }

        template <class F> auto serial(F f) -> coroutine::task<std::invoke_result_t<F&, usage::ledger&, const config::document&>> {
            auto held = co_await ledger_lock_.lock();
            auto doc = config_.current(0);
            auto pending = std::exchange(pending_, {});
            co_return co_await io::blocking(rt_.get_scheduler(0), pool_, [this, f = std::move(f), doc, pending = std::move(pending)]() mutable {
                if (!ledger_) {
                    ledger_ = std::make_unique<usage::ledger>(storage_ / constants::source_ledger_directory);
                    ledger_->changed = [this] {
                        if (changed)
                            changed();
                    };
                    if (auto& r = ledger_->recovery())
                        logging::warn("usage ledger: " + *r);
                }
                // A refusal leaves the ledger as it was; any other failure reopens it from its log, so running builds stay counted.
                try {
                    if (!pending.empty())
                        ledger_->batch([&] {
                            for (auto& record : pending)
                                record(*ledger_);
                        });
                    return f(*ledger_, *doc);
                } catch (const usage::quota_exceeded&) {
                    throw;
                } catch (const std::invalid_argument&) {
                    throw;
                } catch (...) {
                    ledger_.reset();
                    throw;
                }
            });
        }

        cores::context<native::scheduler>& rt_;
        std::filesystem::path storage_;
        durability::dir_lock lock_;
        const config::slots& config_;
        execution::thread_pool& pool_;
        net::ip::tcp::endpoint source_;
        coroutine::mutex ledger_lock_;
        std::vector<std::function<void(usage::ledger&)>> pending_;
        bool draining_ = false;
        std::unique_ptr<usage::ledger> ledger_;
        std::mutex exports_mutex_;
        std::map<std::string, std::weak_ptr<std::mutex>> export_receivers_;
        std::chrono::steady_clock::time_point export_collect_{};
    };
} // namespace storage
