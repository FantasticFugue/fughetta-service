module;
#include <sodium.h>
export module tools;
import std;
import reflection;
import coroutine;
import meta;
import protocol;
import mcp;
import configuration;
import usage;
import world;
import codec;
import archive;
import registry;
import fleet;
import commands;
export import templates;
import native;

export namespace tools {
    class backlog;
    struct execution_identity;

    struct services {
        const config::store::snapshot* config;
        routing::router& workers;
        storage::registry& files;
        std::size_t core;
        // The build, test and run queue of the workspace, shared by every core.
        tools::backlog* queue = nullptr;

        auto snapshot() const -> config::store::snapshot { return *config; }
    };

    consteval auto valid_name(std::string_view n) -> bool {
        if (n.empty() || n.size() > 64)
            return false;
        for (char c : n)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return false;
        return true;
    }

    template <meta::fixed_string Name, meta::fixed_string Description> struct tool {
        static_assert(valid_name(Name), "an MCP tool name is 1-64 of [A-Za-z0-9_-]");
        static_assert(!Description.view().empty(), "describe the tool: the agent reads it to decide when to call it");
        static constexpr std::string_view name = Name;
        static constexpr std::string_view description = Description;
    };

    template <class T>
    concept tool_for = requires(services& s, typename T::input in, mcp::notifier& out, const worlds::actor& actor) {
        { T::name } -> std::convertible_to<std::string_view>;
        { T::description } -> std::convertible_to<std::string_view>;
        { T::run(s, std::move(in), out, actor) } -> std::same_as<coroutine::task<typename T::output>>;
    };

    template <class In>
    concept on_project = requires(const In& in) {
        { in.project.value() } -> std::convertible_to<std::string>;
    };

    template <on_project In> auto project_of(const In& in) -> std::string {
        config::check_name("project", in.project.value());
        return in.project.value();
    }

    // A refusal from the node engine, kept with its code so MCP callers get { code, message }.
    struct tool_error : std::runtime_error {
        tool_error(std::string_view code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
        std::string code;
    };

    template <class... Tools> struct toolbox {
        static_assert(meta::distinct(std::array<std::string_view, sizeof...(Tools)>{Tools::name...}), "two tools share a name");
        static_assert((tool_for<Tools> && ...), "every tool runs on the gateway's services");

        class bound {
        public:
            explicit bound(services& s) : s_(&s) {}

            auto list() const -> const std::vector<mcp::tool>& {
                static const std::vector<mcp::tool> all{describe<Tools>()...};
                return all;
            }

            auto call(std::string_view name, rfl::Generic args, mcp::notifier& out, const worlds::actor& who) -> coroutine::task<mcp::tool_outcome> {
                std::optional<coroutine::task<mcp::tool_outcome>> run;
                (void)((name == Tools::name && (run.emplace(invoke<Tools>(std::move(args), out, who)), true)) || ...);
                if (!run)
                    co_return mcp::tool_outcome{.text = "unknown tool: " + std::string{name}, .error = true};
                co_return co_await std::move(*run);
            }

        private:
            template <class T> static auto describe() -> mcp::tool {
                return {std::string{T::name}, std::string{T::description}, rfl::json::read<rfl::Generic>(rfl::json::to_schema<typename T::input>()).value(),
                        rfl::json::read<rfl::Generic>(rfl::json::to_schema<typename T::output>()).value()};
            }

            struct refusal {
                std::string_view code;
                std::string message;
            };
            static auto refused(refusal r) -> mcp::tool_outcome {
                return {.text = rfl::json::write(r), .structured = rfl::to_generic(r), .error = true};
            }

            template <class T> auto invoke(rfl::Generic args, mcp::notifier& note, const worlds::actor& who) -> coroutine::task<mcp::tool_outcome> {
                auto started = std::chrono::steady_clock::now();
                auto in = rfl::from_generic<typename T::input>(args);
                if (!in)
                    co_return refused({nodes::code::invalid_record, std::format("{}: bad arguments: {}", T::name, in.error().what())});
                std::optional<std::string> where;
                if constexpr (on_project<typename T::input>)
                    if (config::valid_name(in->project.value()))
                        where = in->project.value();
                auto& actor = who;

                std::optional<typename T::output> value;
                std::string error, code;
                try {
                    value.emplace(co_await T::run(*s_, std::move(*in), note, actor));
                } catch (const tool_error& e) {
                    error = e.what();
                    code = e.code;
                } catch (const transport::remote_error& e) {
                    error = e.what();
                    code = e.code;
                } catch (const config::not_found& e) {
                    error = e.what();
                    code = nodes::code::missing_objects;
                } catch (const std::invalid_argument& e) {
                    error = e.what();
                    code = nodes::code::invalid_record;
                } catch (const usage::quota_exceeded& e) {
                    error = e.what();
                    code = nodes::code::resource_limit;
                } catch (const std::exception& e) {
                    error = e.what();
                }
                mcp::tool_outcome outcome;
                if (value) {
                    outcome.text = rfl::json::write(*value);
                    outcome.structured = rfl::to_generic(*value);
                    if constexpr (requires { value->failed(); })
                        outcome.error = value->failed();
                } else
                    outcome = refused({code.empty() ? "failed" : code, std::move(error)});
                if (where) {
                    usage::tool_event e{.project = *where,
                                        .tool = std::string{T::name},
                                        .error = outcome.error,
                                        .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
                    s_->files.post(s_->core, [e](usage::ledger& ledger) { ledger.record(e); });
                }
                co_return outcome;
            }

            services* s_;
        };
    };

    inline auto progress_note(const jobs::progress& p) -> mcp::notification {
        return {.method = "notifications/progress",
                .params = mcp::object({{"progress", static_cast<std::int64_t>(p.done)}, {"total", static_cast<std::int64_t>(p.total)}, {"message", p.stage}})};
    }

    namespace lines {
        inline auto base64(std::string_view bytes) -> std::string {
            static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((bytes.size() + 2) / 3 * 4);
            for (std::size_t i = 0; i < bytes.size(); i += 3) {
                std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
                if (i + 1 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
                if (i + 2 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 2]);
                out += alphabet[(n >> 18) & 63];
                out += alphabet[(n >> 12) & 63];
                out += i + 1 < bytes.size() ? alphabet[(n >> 6) & 63] : '=';
                out += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
            }
            return out;
        }
        inline auto unbase64(std::string_view text) -> std::string {
            auto value = [](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (c == '+') return 62;
                if (c == '/') return 63;
                return -1;
            };
            if (text.size() % 4)
                throw tool_error(nodes::code::invalid_record, "data_base64 is not base64");
            std::string out;
            for (std::size_t i = 0; i < text.size(); i += 4) {
                int v[4];
                for (int k = 0; k < 4; ++k) {
                    v[k] = text[i + k] == '=' && i + 4 == text.size() && k >= 2 ? 0 : value(text[i + k]);
                    if (v[k] < 0)
                        throw tool_error(nodes::code::invalid_record, "data_base64 is not base64");
                }
                std::uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
                out += static_cast<char>(n >> 16);
                if (text[i + 2] != '=') out += static_cast<char>((n >> 8) & 255);
                if (text[i + 3] != '=') out += static_cast<char>(n & 255);
            }
            return out;
        }
        inline auto hex(std::string_view bytes) -> std::string {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out(bytes.size() * 2, '0');
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                auto c = static_cast<unsigned char>(bytes[i]);
                out[2 * i] = digits[c >> 4];
                out[2 * i + 1] = digits[c & 0x0f];
            }
            return out;
        }
        inline auto unhex(std::string_view text) -> std::string {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            std::string out;
            out.reserve(text.size() / 2);
            for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
                int hi = nibble(text[i]), lo = nibble(text[i + 1]);
                if (hi < 0 || lo < 0)
                    throw tool_error(nodes::code::invalid_record, "data_hex is not hexadecimal");
                out += static_cast<char>((hi << 4) | lo);
            }
            return out;
        }
        // Bytes the engine keeps as text: valid UTF-8 without NUL.
        inline auto textual(std::string_view bytes) -> bool {
            if (bytes.find('\0') != std::string_view::npos)
                return false;
            for (std::size_t i = 0; i < bytes.size();) {
                auto c = static_cast<unsigned char>(bytes[i]);
                std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
                if (!n || i + n > bytes.size())
                    return false;
                for (std::size_t k = 1; k < n; ++k)
                    if ((static_cast<unsigned char>(bytes[i + k]) >> 6) != 2)
                        return false;
                i += n;
            }
            return true;
        }
        inline auto bytes_of(const nodes::file_view& f) -> std::string { return f.encoding == "hex" ? unhex(f.text) : f.text; }
        // Caller-chosen operation IDs become the engine's 32-byte operation identities.
        inline auto operation_of(std::string_view id) -> std::string {
            if (id.empty() || id.size() > 256)
                throw tool_error(nodes::code::invalid_record, "an operation_id of 1 through 256 characters is required");
            return content::hash_of("operation:" + std::string{id}).str();
        }
        // The length of the longest prefix of `text` within `most` bytes that ends between characters.
        inline auto cut(std::string_view text, std::uint64_t most) -> std::size_t {
            if (text.size() <= most)
                return text.size();
            auto n = static_cast<std::size_t>(most);
            while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
                --n;
            return n;
        }
        inline auto scalars(std::string_view text) -> std::uint64_t {
            return static_cast<std::uint64_t>(std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        }

        struct entry {
            std::string path, digest, kind;
            std::uint64_t size = 0;
            bool executable = false;
        };
        inline auto manifest(const nodes::node_reply& tree) -> std::vector<entry> {
            std::vector<entry> out;
            for (auto& f : tree.files)
                out.push_back({f.path, f.digest, f.kind, f.size, f.executable});
            std::ranges::sort(out, {}, &entry::path);
            return out;
        }
    } // namespace lines

    auto node_call(services& s, const std::string& project, nodes::node_request request, const worlds::actor& actor, bool reading = false)
        -> coroutine::task<nodes::node_reply> {
        // The source refuses some requests before they reach the node; those carry a code too, and nothing was done.
        std::optional<tool_error> refused;
        nodes::node_reply reply;
        try {
            reply = co_await s.files.node(s.core, project, std::move(request), actor, reading);
        } catch (const transport::remote_error& e) {
            if (e.code.empty())
                throw;
            refused.emplace(e.code, e.what());
        }
        if (refused)
            throw *refused;
        if (reply.code != "ok")
            throw tool_error(reply.code, reply.reason);
        co_return reply;
    }
    // The frontier of a version of a worldline, or of its current state when no version is named.
    auto selected(services& s, const std::string& project, const std::string& worldline, const std::string& version, const worlds::actor& actor)
        -> coroutine::task<nodes::node_reply> {
        co_return co_await node_call(s, project, {.action = "tree", .worldline = worldline, .revision = version}, actor, true);
    }

    struct change {
        rfl::Description<"edit, write, create, remove, move or mode", std::string> kind;
        rfl::Description<"Path relative to the project root", std::string> path;
        rfl::Description<"edit: first Unicode scalar to replace, in the observed version", std::optional<std::uint64_t>> offset;
        rfl::Description<"edit: number of Unicode scalars to replace (0 inserts)", std::optional<std::uint64_t>> count;
        rfl::Description<"edit/write/create: new text", std::optional<std::string>> text;
        rfl::Description<"write/create: new bytes, base64", std::optional<std::string>> data_base64;
        rfl::Description<"create/mode: executable flag", std::optional<bool>> executable;
        rfl::Description<"move: new path", std::optional<std::string>> destination;
    };

    // Engine edits for one batch. A whole new content is checked against the file limit here; the node checks the state it publishes.
    inline auto edits_of(const std::vector<change>& changes, const nodes::node_reply& observed, const config::effective_limits& limits) -> std::vector<nodes::edit> {
        std::map<std::string, const nodes::file_view*> files;
        for (auto& f : observed.files)
            files.emplace(f.path, &f);
        std::vector<nodes::edit> out;
        for (auto& c : changes) {
            auto& kind = c.kind.value();
            auto& path = c.path.value();
            auto found = files.find(path);
            auto content = [&] {
                if (c.text.value() && c.data_base64.value())
                    throw tool_error(nodes::code::invalid_record, "a change holds text or data_base64, not both");
                auto bytes = c.data_base64.value() ? lines::unbase64(*c.data_base64.value()) : c.text.value().value_or("");
                if (bytes.size() > limits.max_file_bytes)
                    throw tool_error(nodes::code::resource_limit, std::format("{} exceeds the {} byte file limit", path, limits.max_file_bytes));
                return bytes;
            };
            if (kind == "edit") {
                if (!c.offset.value() || !c.text.value())
                    throw tool_error(nodes::code::invalid_record, "an edit names offset and text");
                out.push_back({.kind = "edit", .path = path, .text = *c.text.value(), .offset = *c.offset.value(), .count = c.count.value().value_or(0)});
            } else if (kind == "write" || kind == "create") {
                auto bytes = content();
                bool text = lines::textual(bytes);
                if (kind == "create")
                    out.push_back({.kind = text ? "create" : "binary_create", .path = path, .text = text ? bytes : lines::hex(bytes),
                                   .executable = c.executable.value().value_or(false)});
                else {
                    if (found == files.end())
                        throw tool_error(nodes::code::invalid_record, path + " does not exist; create it");
                    if (c.executable.value() && *c.executable.value() != found->second->executable)
                        throw tool_error(nodes::code::invalid_record, "change the executable flag with a mode change");
                    if (text != (found->second->kind == "text"))
                        throw tool_error(nodes::code::invalid_record, "a write keeps a file text or binary; remove and create it to change that");
                    out.push_back({.kind = text ? "write" : "binary", .path = path, .text = text ? bytes : lines::hex(bytes)});
                }
            } else if (kind == "remove")
                out.push_back({.kind = "remove", .path = path});
            else if (kind == "move") {
                if (!c.destination.value())
                    throw tool_error(nodes::code::invalid_record, "a move names its destination");
                out.push_back({.kind = "move", .path = path, .target = *c.destination.value()});
            } else if (kind == "mode") {
                if (!c.executable.value())
                    throw tool_error(nodes::code::invalid_record, "a mode change names executable");
                out.push_back({.kind = "mode", .path = path, .executable = *c.executable.value()});
            } else
                throw tool_error(nodes::code::invalid_record, "unknown change kind " + kind);
        }
        return out;
    }

    struct lock_view {
        std::uint64_t id = 0;
        std::string holder, scope, symbol, environment;
        std::optional<semantic::place> place;
    };
    inline auto locks_of(const nodes::node_reply& r) -> std::vector<lock_view> {
        std::vector<lock_view> out;
        for (auto& l : r.locks)
            out.push_back({l.id, l.holder, l.scope, l.symbol, l.environment, l.place});
        return out;
    }

    // A retried file tool answers from its receipt: recomputing the change on the moved worldline would be another input.
    // What a file tool records with its batch: the digest of the tool input it was asked for, and its count of replacements.
    struct record {
        std::string input;
        std::uint64_t replacements = 0;
    };
    auto recorded(services& s, const std::string& project, const worlds::actor& actor, const std::optional<std::string>& operation_id,
                  const std::string& input, std::string worldline = "main") -> coroutine::task<std::optional<std::pair<nodes::node_reply, record>>> {
        if (!operation_id)
            co_return std::nullopt;
        auto r = co_await s.files.node(s.core, project, {.action = "receipt", .worldline = std::move(worldline), .operation = lines::operation_of(*operation_id), .caller_operation = std::string{*operation_id}}, actor, true);
        if (r.code == nodes::code::outcome_unknown)
            co_return std::nullopt;
        if (r.code != "ok")
            throw tool_error(r.code, r.reason);
        auto kept = rfl::json::read<record>(r.result);
        if (!kept || kept->input != input)
            throw tool_error(nodes::code::identity_conflict, "operation ID reused with different input");
        co_return std::pair{std::move(r), *kept};
    }
    // An operation ID's answer is its receipt. A retry that overlapped its original can meet the original's own change while it derives
    // its batch, and fail on it; it then answers from the receipt the original left.
    template <class T, class Fresh, class Replay>
    auto once(services& s, const std::string& project, const worlds::actor& actor, std::optional<std::string> operation_id, std::string input,
              Fresh fresh, Replay replay, std::string worldline = "main") -> coroutine::task<T> {
        if (auto done = co_await recorded(s, project, actor, operation_id, input, worldline))
            co_return replay(*done);
        std::exception_ptr failed;
        try {
            co_return co_await fresh();
        } catch (const tool_error&) {
            failed = std::current_exception();
        }
        // When even the receipt cannot be read, the caller hears the first answer, which may be outcome_unknown.
        std::optional<std::pair<nodes::node_reply, record>> done;
        try {
            done = co_await recorded(s, project, actor, operation_id, input, worldline);
        } catch (const std::exception&) {
        }
        if (done)
            co_return replay(*done);
        std::rethrow_exception(failed);
    }

    struct worldline
        : tool<"worldline",
               "Work in a worldline of a project: list, inspect, read, fork, lock, unlock, locks, apply, receipt, history, reverse or redo. Many agents "
               "work in one worldline at once and their edits converge; worldlines never merge. Every read reports a version; a write names the "
               "version it observed (observed) and may observe an earlier one. Before editing, lock the syntax scopes you change: scope ids from the "
               "language tool (action scopes) or \"::\" for code outside any class, function or namespace and for every file-level change (write, "
               "create, remove, move, mode). A scope another holder locked fails with scope_locked naming the holder. Edits use Unicode scalar "
               "offsets of the observed version. Resending an operation_id with identical input returns the recorded result; receipt recovers it "
               "after a lost reply. Errors carry a code: stale_frontier, scope_locked, worldline_boundary, identity_conflict, outcome_unknown, "
               "missing_objects, invalid_record or permission_denied."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"list, inspect, read, fork, lock, unlock, locks, apply, receipt, history, contribution, reverse, redo, export or restore", std::string> action;
            rfl::Description<"Worldline name; main when absent", std::optional<std::string>> worldline;
            rfl::Description<"inspect/read/fork/export: the version to use; the current one when absent", std::optional<std::string>> version;
            rfl::Description<"apply/reverse/redo: the version this change was made against", std::optional<std::string>> observed;
            rfl::Description<"read: file path", std::optional<std::string>> path;
            rfl::Description<"fork/restore: name of the new worldline", std::optional<std::string>> name;
            rfl::Description<"fork: worldline to fork from; this call's worldline when absent", std::optional<std::string>> from;
            rfl::Description<"apply/fork/reverse/redo/restore: caller-chosen ID of this operation", std::optional<std::string>> operation_id;
            rfl::Description<"apply/fork/reverse/redo: what this change is for", std::optional<std::string>> intent;
            rfl::Description<"apply: one atomic batch", std::optional<std::vector<change>>> changes;
            rfl::Description<"lock/unlock: scope ids, or \"::\"", std::optional<std::vector<std::string>>> scopes;
            rfl::Description<"lock/unlock: the analysis environment the scope ids come from", std::optional<std::string>> environment;
            rfl::Description<"unlock: one lock number instead of scopes", std::optional<std::uint64_t>> lock_id;
            rfl::Description<"unlock: release another holder's lock (owners only); export: replace a package of that name", std::optional<bool>> force;
            rfl::Description<"export/restore: package name, shared by the workspace's projects", std::optional<std::string>> package;
            rfl::Description<"reverse/redo/contribution: the history entry to undo, restore or describe", std::optional<std::string>> target;
            rfl::Description<"history: page size", std::optional<std::uint64_t>> limit;
            rfl::Description<"history: the last entry of the previous page", std::optional<std::string>> cursor;
        };
        struct line_entry {
            std::string name, version;
            std::optional<std::string> base_version, created_by;
        };
        struct history_entry {
            std::string id, operation_id, actor, intent, tool, outcome;
            std::vector<std::string> parents;
        };
        // One side of a changed file. Text is included up to the project's read limit; binary content never is.
        struct file_side {
            std::string path, digest, kind;
            std::uint64_t size = 0;
            bool executable = false;
            std::optional<std::string> text;
            bool text_truncated = false;
        };
        struct file_change {
            std::string type;
            std::optional<file_side> before, after;
        };
        struct output {
            std::optional<std::string> worldline, version, base_version, path, digest, kind, data_base64, receipt, operation_id;
            std::optional<std::uint64_t> size;
            std::optional<bool> executable, replayed, truncated;
            std::optional<std::vector<line_entry>> worldlines;
            std::optional<std::vector<lines::entry>> files;
            std::optional<std::vector<std::string>> conflicts;
            std::optional<std::vector<lock_view>> locks;
            std::optional<std::vector<history_entry>> entries;
            std::optional<std::vector<file_change>> changes;
            // contribution: the worldlines whose current version contains the entry.
            std::optional<std::vector<std::string>> contained_in;
            // restore: the project configuration the package held, as JSON, for the caller to apply where it wants it.
            std::optional<std::string> configuration;
        };

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto line = in.worldline.value().value_or("main");
            auto& action = in.action.value();
            output out{.worldline = line};
            if (action == "list") {
                auto r = co_await node_call(s, project, {.action = "worldlines"}, actor, true);
                auto most = config::resolve(*s.snapshot(), project).limits.list_max_entries;
                std::vector<line_entry> listed;
                for (auto& l : r.lines) {
                    if (listed.size() == most)
                        break;
                    listed.push_back({l.name, l.version, l.base_version.empty() ? std::nullopt : std::optional{l.base_version},
                                      l.creator.empty() ? std::nullopt : std::optional{l.creator}});
                }
                out.truncated = listed.size() < r.lines.size();
                out.worldlines = std::move(listed);
            } else if (action == "inspect") {
                auto tree = co_await selected(s, project, line, in.version.value().value_or(""), actor);
                out.version = tree.version;
                out.files = lines::manifest(tree);
                out.conflicts = tree.conflicts;
            } else if (action == "read") {
                if (!in.path.value())
                    throw tool_error(nodes::code::invalid_record, "read names a path");
                auto r = co_await node_call(s, project, {.action = "read", .worldline = line, .revision = in.version.value().value_or(""), .target = *in.path.value()},
                                            actor, true);
                auto file = std::ranges::find(r.files, *in.path.value(), &nodes::file_view::path);
                if (file == r.files.end())
                    throw tool_error(nodes::code::missing_objects, *in.path.value() + " is not in this version");
                auto bytes = lines::bytes_of(*file);
                out = {.worldline = line, .version = r.version, .path = file->path, .digest = file->digest, .kind = file->kind,
                       .data_base64 = lines::base64(bytes), .size = bytes.size(), .executable = file->executable};
            } else if (action == "fork") {
                if (!in.name.value())
                    throw tool_error(nodes::code::invalid_record, "fork names the new worldline");
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "fork names an operation_id");
                // A retry answers from the receipt: the source worldline may have moved on since the original resolved its version.
                auto name = *in.name.value();
                auto id = *in.operation_id.value();
                std::string input = content::hash_of(rfl::json::write(std::array{name, in.from.value().value_or(line), in.version.value().value_or(""), in.intent.value().value_or("")})).str();
                auto r = co_await once<nodes::node_reply>(s, project, actor, id, input, [&]() -> coroutine::task<nodes::node_reply> {
                    auto base = co_await selected(s, project, in.from.value().value_or(line), in.version.value().value_or(""), actor);
                    co_return co_await node_call(s, project, {.action = "fork", .worldline = name, .operation = lines::operation_of(id), .caller_operation = id,
                                                          .source = base.frontier, .intent = in.intent.value().value_or(""), .result = rfl::json::write(record{input})}, actor);
                }, [](const auto& done) { auto r = done.first; r.replayed = true; return r; }, name);
                out = {.worldline = name, .version = r.version, .receipt = r.receipt, .operation_id = id, .replayed = r.replayed};
            } else if (action == "lock" || action == "unlock" || action == "locks") {
                nodes::node_request request{.action = action, .worldline = line, .environment = in.environment.value().value_or(""),
                                            .scopes = in.scopes.value().value_or(std::vector<std::string>{}), .force = in.force.value().value_or(false)};
                if (in.lock_id.value())
                    request.target = std::to_string(*in.lock_id.value());
                auto r = co_await node_call(s, project, std::move(request), actor, action == "locks");
                out.version = r.version;
                out.locks = locks_of(r);
            } else if (action == "apply" || action == "reverse" || action == "redo") {
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, action + " names an operation_id");
                if (!in.observed.value())
                    throw tool_error(nodes::code::invalid_record, action + " names the version it observed");
                auto observed = co_await selected(s, project, line, *in.observed.value(), actor);
                nodes::node_request request{.action = action, .worldline = line, .operation = lines::operation_of(in.operation_id.value().value_or("")), .caller_operation = std::string{in.operation_id.value().value_or("")},
                                            .expected = observed.frontier, .target = in.target.value().value_or(""), .intent = in.intent.value().value_or("")};
                if (action == "apply") {
                    auto limits = config::resolve(*s.snapshot(), project).limits;
                    request.edits = edits_of(in.changes.value().value_or(std::vector<change>{}), observed, limits);
                    if (request.edits.empty())
                        throw tool_error(nodes::code::invalid_record, "apply needs at least one change");
                }
                auto r = co_await node_call(s, project, std::move(request), actor);
                auto tree = co_await selected(s, project, line, r.version, actor);
                out = {.worldline = line, .version = r.version, .receipt = r.receipt, .operation_id = in.operation_id.value(), .replayed = r.replayed};
                out.files = lines::manifest(tree);
            } else if (action == "receipt") {
                auto r = co_await node_call(s, project, {.action = "receipt", .worldline = line, .operation = lines::operation_of(in.operation_id.value().value_or("")), .caller_operation = std::string{in.operation_id.value().value_or("")}},
                                            actor, true);
                auto tree = co_await selected(s, project, line, r.version, actor);
                out = {.worldline = r.worldline, .version = r.version, .receipt = r.receipt, .operation_id = in.operation_id.value(), .replayed = true};
                out.files = lines::manifest(tree);
            } else if (action == "history") {
                auto limit = std::min<std::uint64_t>(in.limit.value().value_or(100), config::resolve(*s.snapshot(), project).limits.list_max_entries);
                auto r = co_await node_call(s, project, {.action = "history", .worldline = line, .target = in.cursor.value().value_or(""), .limit = limit}, actor, true);
                std::vector<history_entry> entries;
                for (auto& h : r.history)
                    entries.push_back({h.event, h.operation, h.actor, h.intent, h.tool, h.outcome, h.parents});
                out.version = r.version;
                out.entries = std::move(entries);
                out.truncated = r.truncated;
            } else if (action == "export") {
                // One version of this worldline written out as a package, which any project of the workspace can restore.
                if (!in.package.value())
                    throw tool_error(nodes::code::invalid_record, "an export names its package");
                // The package keeps the project's configuration beside its files.
                auto* settings = s.snapshot()->find(project);
                auto r = co_await node_call(s, project, {.action = "export", .worldline = line, .revision = in.version.value().value_or(""), .target = *in.package.value(),
                                                     .configuration = settings ? config::write(*settings) : std::string{}, .force = in.force.value().value_or(false)},
                                            actor);
                out.version = r.version;
                out.files = lines::manifest(r);
            } else if (action == "restore") {
                if (!in.package.value() || !in.name.value() || !in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "a restore names its package, the new worldline and an operation_id");
                auto r = co_await node_call(s, project, {.action = "restore", .worldline = *in.name.value(), .operation = lines::operation_of(*in.operation_id.value()),
                                                     .caller_operation = *in.operation_id.value(), .target = *in.package.value()}, actor);
                auto tree = co_await selected(s, project, *in.name.value(), r.version, actor);
                out = {.worldline = *in.name.value(), .version = r.version, .receipt = r.receipt, .operation_id = in.operation_id.value(), .replayed = r.replayed};
                out.files = lines::manifest(tree);
                if (!r.configuration.empty())
                    out.configuration = r.configuration;
            } else if (action == "contribution") {
                if (!in.target.value())
                    throw tool_error(nodes::code::invalid_record, "a contribution names a history entry");
                auto most = config::resolve(*s.snapshot(), project).limits.read_max_bytes;
                nodes::node_request ask{.action = "contribution", .worldline = line, .target = *in.target.value()};
                ask.max_bytes = most;
                auto r = co_await node_call(s, project, std::move(ask), actor, true);
                auto side = [most](const nodes::file_view& f) {
                    file_side out{.path = f.path, .digest = f.digest, .kind = f.kind, .size = f.size, .executable = f.executable};
                    if (f.kind == "text") {
                        out.text_truncated = f.text.size() > most || f.text.size() < f.size;
                        out.text = f.text.substr(0, lines::cut(f.text, most));
                    }
                    return out;
                };
                std::vector<file_change> changes;
                for (auto& c : r.changes)
                    changes.push_back({c.type, c.before ? std::optional{side(*c.before)} : std::nullopt, c.after ? std::optional{side(*c.after)} : std::nullopt});
                std::vector<history_entry> entries;
                for (auto& h : r.history)
                    entries.push_back({h.event, h.operation, h.actor, h.intent, h.tool, h.outcome, h.parents});
                std::vector<std::string> lines;
                for (auto& l : r.lines)
                    lines.push_back(l.name);
                out.version = r.version;
                out.base_version = r.base_version;
                out.entries = std::move(entries);
                out.changes = std::move(changes);
                out.contained_in = std::move(lines);
            } else
                throw tool_error(nodes::code::invalid_record, "unknown worldline action " + action);
            co_return out;
        }
    };

    // File tools for the main worldline. They read its current version and write through the same locks as worldline apply.
    auto main_tree(services& s, const std::string& project, const worlds::actor& actor, bool text = false) -> coroutine::task<nodes::node_reply> {
        co_return co_await node_call(s, project, {.action = text ? "inspect" : "tree", .worldline = "main"}, actor, true);
    }
    auto main_apply(services& s, const std::string& project, const worlds::actor& actor, const nodes::node_reply& observed, std::vector<change> changes,
                    std::string operation_id, std::string intent, std::string result = {}, std::vector<std::string> scopes = {}) -> coroutine::task<nodes::node_reply> {
        auto limits = config::resolve(*s.snapshot(), project).limits;
        co_return co_await node_call(s, project, {.action = "apply", .worldline = "main", .operation = lines::operation_of(operation_id), .caller_operation = std::string{operation_id}, .expected = observed.frontier,
                                              .edits = edits_of(changes, observed, limits), .intent = std::move(intent), .result = std::move(result), .scopes = std::move(scopes)},
                                     actor);
    }

    struct read : tool<"read", "Read a text file of a project's main worldline, optionally a range of lines. Long files are cut at the "
                               "project's limits; ask for a later range with offset. The result names the version read."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Path relative to the project root", std::string> path;
            rfl::Description<"First line to return, 1-based", std::optional<std::uint64_t>> offset;
            rfl::Description<"Number of lines to return", std::optional<std::uint64_t>> limit;
        };
        struct output {
            std::string content;
            std::uint64_t first_line = 1;
            std::uint64_t lines = 0;
            std::uint64_t total_lines = 0;
            bool truncated = false;
            std::string version;
        };

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto r = co_await node_call(s, project, {.action = "read", .worldline = "main", .target = in.path.value()}, actor, true);
            auto file = std::ranges::find(r.files, in.path.value(), &nodes::file_view::path);
            if (file == r.files.end())
                throw tool_error(nodes::code::missing_objects, in.path.value() + " is not in the project");
            if (file->kind != "text")
                throw tool_error(nodes::code::invalid_record, in.path.value() + " is a binary file");
            auto l = config::resolve(*s.snapshot(), project).limits;
            auto out = slice(file->text, std::max<std::uint64_t>(1, in.offset.value().value_or(1)),
                             std::min<std::uint64_t>(in.limit.value().value_or(l.read_max_lines), l.read_max_lines), l.read_max_bytes);
            out.version = r.version;
            co_return out;
        }

        static auto slice(std::string_view text, std::uint64_t first, std::uint64_t count, std::uint64_t max_bytes) -> output {
            output out{.first_line = first};
            std::uint64_t line = 1;
            for (std::size_t pos = 0; pos < text.size(); ++line) {
                auto end = text.find('\n', pos);
                auto next = end == std::string_view::npos ? text.size() : end + 1;
                if (line >= first && !out.truncated) {
                    auto piece = text.substr(pos, next - pos);
                    if (out.lines == count || out.content.size() + piece.size() > max_bytes)
                        out.truncated = true;
                    else {
                        out.content.append(piece);
                        ++out.lines;
                    }
                }
                pos = next;
            }
            out.total_lines = line - 1;
            return out;
        }
    };

    struct mutation {
        std::string version, receipt, operation_id;
        bool replayed = false;
    };

    struct write : tool<"write", "Create a file in the main worldline of a project, or replace its whole content. Needs the \"::\" lock (worldline "
                                 "lock). Resubmitting the same operation_id returns the recorded result instead of writing again."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Path relative to the project root", std::string> path;
            rfl::Description<"The new content", std::string> content;
            rfl::Description<"Caller-chosen ID of this change; resending it with identical input returns the recorded result", std::string> operation_id;
        };
        using output = mutation;

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            std::string input = content::hash_of(rfl::json::write(in)).str();
            co_return co_await once<output>(s, project, actor, std::optional{in.operation_id.value()}, input, [&]() -> coroutine::task<output> {
                auto observed = co_await main_tree(s, project, actor);
                bool exists = std::ranges::contains(observed.files, in.path.value(), &nodes::file_view::path);
                auto id = in.operation_id.value();
                auto r = co_await main_apply(s, project, actor, observed, {change{.kind = exists ? "write" : "create", .path = in.path.value(), .text = in.content.value()}}, id,
                                             "write " + in.path.value(), rfl::json::write(record{input}));
                co_return mutation{r.version, r.receipt, id, r.replayed};
            }, [&](const auto& done) { return mutation{done.first.version, done.first.receipt, in.operation_id.value(), true}; });
        }
    };

    struct edit : tool<"edit", "Replace text in a file of a project's main worldline. The old text must occur exactly once unless replace_all is "
                               "set. Lock the scope that contains it first (worldline lock). Resubmitting the same operation_id returns the "
                               "recorded result."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Path relative to the project root", std::string> path;
            rfl::Description<"Text to replace, exactly as in the file", std::string> old_string;
            rfl::Description<"Replacement text", std::string> new_string;
            rfl::Description<"Replace every occurrence", std::optional<bool>> replace_all;
            rfl::Description<"Caller-chosen ID of this change; resending it with identical input returns the recorded result", std::string> operation_id;
        };
        struct output {
            std::string version, receipt, operation_id;
            std::uint64_t replacements = 0;
            bool replayed = false;
        };

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            std::string input = content::hash_of(rfl::json::write(in)).str();
            co_return co_await once<output>(s, project, actor, std::optional{in.operation_id.value()}, input, [&]() { return fresh(s, project, in, actor, input); },
                                            [&](const auto& done) { return output{done.first.version, done.first.receipt, in.operation_id.value(), done.second.replacements, true}; });
        }
        static auto fresh(services& s, const std::string& project, const input& in, const worlds::actor& actor, const std::string& input) -> coroutine::task<output> {
            auto r = co_await node_call(s, project, {.action = "read", .worldline = "main", .target = in.path.value()}, actor, true);
            auto file = std::ranges::find(r.files, in.path.value(), &nodes::file_view::path);
            if (file == r.files.end() || file->kind != "text")
                throw tool_error(nodes::code::missing_objects, in.path.value() + " is not a text file of the project");
            auto& from = in.old_string.value();
            if (from.empty())
                throw tool_error(nodes::code::invalid_record, "old_string is empty");
            std::vector<std::size_t> at;
            for (auto i = file->text.find(from); i != std::string::npos; i = file->text.find(from, i + from.size()))
                at.push_back(i);
            if (at.empty())
                throw tool_error(nodes::code::invalid_record, "old_string does not occur in " + in.path.value());
            if (at.size() > 1 && !in.replace_all.value().value_or(false))
                throw tool_error(nodes::code::invalid_record, std::format("old_string occurs {} times; set replace_all or give more context", at.size()));
            // One edit per occurrence would edit the path twice in one batch, so the edit spans from the first to the last occurrence.
            std::string replaced;
            std::size_t last = at.front();
            for (auto i : at) {
                replaced += file->text.substr(last, i - last);
                replaced += in.new_string.value();
                last = i + from.size();
            }
            auto begin = lines::scalars(std::string_view{file->text}.substr(0, at.front()));
            auto count = lines::scalars(std::string_view{file->text}.substr(at.front(), last - at.front()));
            auto id = in.operation_id.value();
            auto tree = co_await selected(s, project, "main", r.version, actor);
            auto done = co_await main_apply(s, project, actor, tree,
                                            {change{.kind = "edit", .path = in.path.value(), .offset = begin, .count = count, .text = replaced}}, id,
                                            "edit " + in.path.value(), rfl::json::write(record{input, at.size()}));
            co_return output{done.version, done.receipt, id, at.size(), done.replayed};
        }
    };

    struct remove : tool<"remove", "Delete a file of a project's main worldline. Needs the \"::\" lock (worldline lock). Resubmitting the same "
                                   "operation_id returns the recorded result."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Path relative to the project root", std::string> path;
            rfl::Description<"Caller-chosen ID of this change; resending it with identical input returns the recorded result", std::string> operation_id;
        };
        using output = mutation;

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            std::string input = content::hash_of(rfl::json::write(in)).str();
            co_return co_await once<output>(s, project, actor, std::optional{in.operation_id.value()}, input, [&]() -> coroutine::task<output> {
                auto observed = co_await main_tree(s, project, actor);
                auto id = in.operation_id.value();
                auto r = co_await main_apply(s, project, actor, observed, {change{.kind = "remove", .path = in.path.value()}}, id, "remove " + in.path.value(),
                                             rfl::json::write(record{input}));
                co_return mutation{r.version, r.receipt, id, r.replayed};
            }, [&](const auto& done) { return mutation{done.first.version, done.first.receipt, in.operation_id.value(), true}; });
        }
    };

    struct move : tool<"move", "Rename or move a file within a project's main worldline. Needs the \"::\" lock (worldline lock). Resubmitting "
                               "the same operation_id returns the recorded result."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Current path, relative to the project root", std::string> from;
            rfl::Description<"New path, relative to the project root; must not exist", std::string> to;
            rfl::Description<"Caller-chosen ID of this change; resending it with identical input returns the recorded result", std::string> operation_id;
        };
        using output = mutation;

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            std::string input = content::hash_of(rfl::json::write(in)).str();
            co_return co_await once<output>(s, project, actor, std::optional{in.operation_id.value()}, input, [&]() -> coroutine::task<output> {
                auto observed = co_await main_tree(s, project, actor);
                auto id = in.operation_id.value();
                auto r = co_await main_apply(s, project, actor, observed, {change{.kind = "move", .path = in.from.value(), .destination = in.to.value()}}, id,
                                             "move " + in.from.value(), rfl::json::write(record{input}));
                co_return mutation{r.version, r.receipt, id, r.replayed};
            }, [&](const auto& done) { return mutation{done.first.version, done.first.receipt, in.operation_id.value(), true}; });
        }
    };

    struct file {
        std::string path;
        std::uint64_t size = 0;
    };

    struct list : tool<"list", "List the files of a project's main worldline, optionally under a directory."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Directory to list, relative to the project root", std::optional<std::string>> prefix;
        };
        struct output {
            std::vector<file> files;
            bool truncated = false;
            std::string version;
        };

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto tree = co_await main_tree(s, project, actor);
            auto prefix = in.prefix.value().value_or("");
            co_return cap(tree, [&](std::string_view p) { return prefix.empty() || p == prefix || (p.starts_with(prefix) && p[prefix.size()] == '/'); },
                          config::resolve(*s.snapshot(), project).limits.list_max_entries);
        }

        template <class F> static auto cap(const nodes::node_reply& tree, F keep, std::size_t max) -> output {
            output out{.version = tree.version};
            for (auto& e : lines::manifest(tree)) {
                if (!keep(e.path))
                    continue;
                if (out.files.size() == max) {
                    out.truncated = true;
                    break;
                }
                out.files.push_back({e.path, e.size});
            }
            return out;
        }
    };

    struct glob : tool<"glob", "Find files of a project's main worldline by path pattern: * and ? within a directory name, ** across "
                               "directories (\"src/**/*.cpp\")."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Path pattern", std::string> pattern;
        };
        using output = list::output;

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto tree = co_await main_tree(s, project, actor);
            co_return list::cap(tree, [&](std::string_view p) { return content::glob(in.pattern.value(), p); },
                                config::resolve(*s.snapshot(), project).limits.list_max_entries);
        }
    };

    struct search : tool<"search", "Search the text files of a project's main worldline for lines containing a string (or matching a "
                                   "regular expression), optionally only in files matching a path pattern."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Text to find", std::string> query;
            rfl::Description<"Treat the query as an ECMAScript regular expression", std::optional<bool>> regex;
            rfl::Description<"Only files matching this path pattern", std::optional<std::string>> files;
        };
        struct match {
            std::string path;
            std::uint64_t line = 0;
            std::string text;
        };
        struct output {
            std::vector<match> matches;
            bool truncated = false;
            std::string version;
        };

        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto limit = config::resolve(*s.snapshot(), project).limits.search_max_matches;
            auto tree = co_await main_tree(s, project, actor, true);
            auto pattern = in.files.value().value_or("");
            std::optional<std::regex> expression;
            if (in.regex.value().value_or(false))
                expression.emplace(in.query.value(), std::regex::ECMAScript);
            output out{.version = tree.version};
            std::ranges::sort(tree.files, {}, &nodes::file_view::path);
            for (auto& f : tree.files) {
                if (f.kind != "text" || (!pattern.empty() && !content::glob(pattern, f.path)))
                    continue;
                std::istringstream text{f.text};
                std::uint64_t number = 0;
                for (std::string line; std::getline(text, line);) {
                    ++number;
                    if (expression ? !std::regex_search(line, *expression) : line.find(in.query.value()) == std::string::npos)
                        continue;
                    if (out.matches.size() == limit) {
                        out.truncated = true;
                        co_return out;
                    }
                    out.matches.push_back({f.path, number, line});
                }
            }
            co_return out;
        }
    };

    struct step_result {
        std::string name;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::string output;
        bool truncated = false;
    };

    struct diagnostic {
        std::string file;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        std::string severity;
        std::string message;
    };

    struct test_summary {
        std::uint32_t total = 0;
        std::uint32_t failed = 0;
        std::vector<std::string> failed_tests;
    };

    struct config_result {
        std::string configuration;
        std::string worker;
        std::optional<std::string> worker_id;
        bool ok = false;
        double seconds = 0;
        std::vector<step_result> steps;
        std::vector<diagnostic> diagnostics;
        std::optional<test_summary> tests;
        std::uint64_t files = 0;
        std::uint64_t blobs_sent = 0;
        std::uint64_t bytes_sent = 0;
        std::string snapshot;
        std::string error;
        std::string error_code;
        std::string workspace_id,project,worldline,version,operation,actor;
    };

    // Build, test and run jobs of the workspace, kept on core zero. As many run at once as the limits and the workers allow; waiting
    // jobs take turns by who asked. A job equal to one queued or running is not run again: its asker waits for the same result, and a
    // build equal to a finished one answers from the cache. A new request replaces the same asker's older one that has not started.
    // Each project and configuration has a few build trees on the workers; a job takes the free one whose last build shared the most
    // files with it, so a forked worldline builds on from where its origin left off.
    class backlog {
    public:
        struct entry {
            // Equal keys compute equal results: the files built and everything the worker is told.
            std::string key;
            std::string project, configuration, operation;
            // Who asked, as worlds::identity_key names them.
            std::string actor;
            // The trees a job may take: one set per project and configuration. `files` names the files built, as path and digest.
            std::string trees;
            std::vector<std::string> files;
            bool cacheable = false;
            // Run again even when a finished result is cached.
            bool fresh = false;
            std::string workspace_id,worldline,version;
            std::shared_ptr<execution_identity> executor;
        };

        struct view {
            std::uint64_t id = 0;
            std::string project, configuration, operation;
            // queued, running, done, cancelled or replaced.
            std::string state;
            std::optional<bool> ok;
            // Callers waiting for it, and the queued jobs before it.
            std::size_t waiting = 0, ahead = 0;
        };

        // A job's result: a build, test or run configuration's, or a preparation's.
        using outcome = std::variant<config_result, language::result>;

        // A caller's hold on a job: core zero writes it, the caller's core reads it.
        class ticket {
        public:
            explicit ticket(std::string actor) : actor_(std::move(actor)) {}
            auto done() const -> bool { return done_.load(std::memory_order_acquire); }
            // Valid once done.
            auto result() const -> const outcome& { return result_; }
            auto ahead() const -> std::size_t { return ahead_.load(std::memory_order_relaxed); }
            auto job() const -> std::uint64_t { return job_.load(std::memory_order_relaxed); }
            // The progress its job reported since `seen`, which moves past it.
            auto progress(std::size_t& seen) -> std::vector<jobs::progress> { return progress_ ? progress_->since(seen) : std::vector<jobs::progress>{}; }
            void leave() { left_.store(true, std::memory_order_release); }

        private:
            friend class backlog;
            std::string actor_;
            std::atomic<bool> done_{false}, left_{false};
            std::atomic<std::size_t> ahead_{0};
            std::atomic<std::uint64_t> job_{0};
            outcome result_;
            // The job's, set before the ticket is handed out; none for a cached result.
            std::shared_ptr<jobs::progress_log> progress_;
        };

        // Runs a job on core zero in the named tree, reporting into the job's log; nothing when every eligible worker is busy, so the job
        // waits for one.
        using runner = util::move_only_function<coroutine::task<std::optional<outcome>>(services&, jobs::progress_log&, std::string tree)>;

        backlog(cores::context<native::scheduler>& rt, services& zero) : rt_(rt), zero_(zero) {}

        auto submit(std::size_t here, entry e, runner r) -> coroutine::task<std::shared_ptr<ticket>>;
        auto list(std::size_t here) -> coroutine::task<std::vector<view>>;
        // Cancels a job the caller asked for alone, or any job for an owner. Says what happened.
        auto cancel(std::size_t here, std::uint64_t id, worlds::actor who) -> coroutine::task<std::string>;
        // Core zero's loop: starts what may start and lets go of jobs nobody waits for any more.
        auto serve(native::scheduler& io) -> coroutine::task<>;
        auto idle() const -> bool { return jobs_.empty() && running_ == 0; }

    private:
        // A build tree on the workers and what was last built in it.
        struct tree {
            std::string name;
            bool busy = false;
            std::unordered_set<std::string> files;
            std::uint64_t used = 0;
        };

        struct job {
            std::uint64_t id = 0;
            entry what;
            runner run;
            bool running = false;
            // After every eligible worker was busy, the job is not tried again before this.
            std::chrono::steady_clock::time_point retry_at{};
            std::vector<std::shared_ptr<ticket>> tickets;
            std::shared_ptr<jobs::progress_log> progress = std::make_shared<jobs::progress_log>();
            coroutine::stop_source stop;
        };

        auto enqueue(entry e, std::shared_ptr<runner> r) -> std::shared_ptr<ticket>;
        auto choose(const entry& e) -> tree&;
        auto views() const -> std::vector<view>;
        auto withdraw(std::uint64_t id, const worlds::actor& who) -> std::string;
        void step();
        auto next() -> std::shared_ptr<job>;
        auto launch(std::shared_ptr<job> j) -> coroutine::task<>;
        void finish(const std::shared_ptr<job>& j, outcome r, std::string state);
        static auto refused(const entry& e,std::string why) -> outcome;

        cores::context<native::scheduler>& rt_;
        services& zero_;
        std::uint64_t next_id_ = 1;
        std::list<std::shared_ptr<job>> jobs_;
        std::size_t running_ = 0;
        // Who asked, in the order their turns come.
        std::deque<std::string> turns_;
        std::list<std::pair<std::string, outcome>> cache_;
        std::map<std::string, std::deque<tree>> trees_;
        std::uint64_t clock_ = 0;
        std::deque<view> finished_;
    };

    struct results {
        bool ok = false;
        std::string snapshot;
        std::vector<config_result> results;
        std::string worldline;
        std::string revision;

        auto failed() const -> bool { return !ok; }
    };

    namespace detail {
        inline auto diagnostics(const jobs::build_report& r, const std::vector<jobs::step>& steps, std::size_t max = 100) -> std::vector<diagnostic> {
            static const std::regex line_re{R"(^(.+?):(\d+):(?:(\d+):)? (fatal error|error|warning|note): (.*)$)"};
            std::vector<diagnostic> out;
            std::filesystem::path tree{r.tree};
            for (std::size_t i = 0; i < r.steps.size() && out.size() < max; ++i) {
                auto cwd = i < steps.size() ? tree / steps[i].cwd : tree;
                std::istringstream text{r.steps[i].output};
                for (std::string line; std::getline(text, line) && out.size() < max;) {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    std::smatch m;
                    if (!std::regex_match(line, m, line_re))
                        continue;
                    std::filesystem::path file{m[1].str()};
                    auto abs = (file.is_absolute() ? file : cwd / file).lexically_normal();
                    auto rel = abs.lexically_relative(tree);
                    auto shown = !rel.empty() && *rel.begin() != ".." ? rel.generic_string() : m[1].str();
                    diagnostic d{.file = shown,
                                 .line = static_cast<std::uint32_t>(std::stoul(m[2].str())),
                                 .column = m[3].matched ? static_cast<std::uint32_t>(std::stoul(m[3].str())) : 0u,
                                 .severity = m[4].str() == "fatal error" ? "error" : m[4].str(),
                                 .message = m[5].str()};
                    if (!std::ranges::any_of(out,
                                             [&](auto& o) { return o.file == d.file && o.line == d.line && o.column == d.column && o.message == d.message; }))
                        out.push_back(std::move(d));
                }
            }
            return out;
        }

        inline auto ctest_summary(std::string_view output) -> std::optional<test_summary> {
            static const std::regex total_re{R"((\d+)% tests passed(?:, (\d+) tests? failed)? out of (\d+))"};
            static const std::regex failed_re{R"(^\s*\d+ - (.+?) \((.*)\)\s*$)"};
            std::string text{output};
            std::smatch m;
            if (!std::regex_search(text, m, total_re))
                return std::nullopt;
            test_summary t{.total = static_cast<std::uint32_t>(std::stoul(m[3].str())),
                           .failed = m[2].matched ? static_cast<std::uint32_t>(std::stoul(m[2].str())) : 0u};
            auto at = text.find("The following tests FAILED:");
            if (at != std::string::npos) {
                std::istringstream rest{text.substr(at)};
                std::string line;
                std::getline(rest, line);
                while (std::getline(rest, line) && std::regex_match(line, m, failed_re))
                    t.failed_tests.push_back(m[1].str());
            }
            return t;
        }

        inline auto operation(config::effective_config c, builds::request request) -> std::pair<config::effective_config, std::vector<jobs::step>> {
            if (!c.build.enabled)
                throw std::invalid_argument(std::format("build configuration '{}' is disabled", c.name));
            if (!c.build.operations.contains(request.operation))
                throw std::invalid_argument(std::format("build configuration '{}' offers no '{}' operation (it offers: {})", c.name, request.operation, [&] {
                    std::string names;
                    for (auto& [op, _] : c.build.operations)
                        names += (names.empty() ? "" : ", ") + op;
                    return names.empty() ? std::string{"none"} : names;
                }()));
            auto plan = builds::prepare(c.build, request);
            c.needs = plan.needs;
            c.env = std::move(plan.env);
            return {std::move(c), std::move(plan.steps)};
        }
    } // namespace detail

    using jobs::part_size;
    inline constexpr std::uint64_t push_batch = 8u << 20;
    static_assert(push_batch + part_size < transport::max_payload / 2);

    struct sync_stats {
        std::uint64_t blobs = 0;
        std::uint64_t bytes = 0;
    };

    inline constexpr std::chrono::seconds offer_deadline{60};
    inline constexpr std::chrono::seconds push_deadline{120};
    inline constexpr std::chrono::seconds build_overhead{300};

    inline auto no_answer(std::string_view what, std::chrono::seconds limit) -> std::runtime_error {
        return std::runtime_error("the worker did not answer " + std::string{what} + " within " + std::to_string(limit.count()) + " s");
    }

    template <class T>
    auto within(native::scheduler& io, std::chrono::seconds limit, std::string_view what, coroutine::task<transport::call_result<T>> call) -> coroutine::task<T> {
        auto r = co_await coroutine::deadline(io, limit, std::move(call));
        if (!r)
            throw no_answer(what, limit);
        if (!*r)
            transport::raise_call_error(std::move(r->error()));
        co_return std::move(**r);
    }

    struct execution_identity {
        std::mutex mutex;
        std::pair<std::string,std::optional<std::string>> value;
        bool began=false;
        void begin() {std::lock_guard held{mutex};began=true;}
        auto started() -> bool {std::lock_guard held{mutex};return began;}
        void select(std::pair<std::string,std::optional<std::string>> identity) {
            std::lock_guard held{mutex};
            value=std::move(identity);
        }
        template <class Result> void stamp(Result& result) {
            std::lock_guard held{mutex};
            result.worker=value.first;
            result.worker_id=value.second;
        }
    };

    inline auto backlog::refused(const entry& e,std::string why) -> outcome {
        auto code=why=="cancelled" ? "cancelled" : why.starts_with("replaced") ? "replaced" : "execution_failed";
        config_result result{.configuration=e.configuration,.snapshot=e.version,.error=std::move(why),.error_code=code,
            .workspace_id=e.workspace_id,.project=e.project,.worldline=e.worldline,.version=e.version,.operation=e.operation,.actor=e.actor};
        if (e.executor) e.executor->stamp(result);
        return result;
    }

    auto sync_and_run(native::scheduler& io, storage::registry& files, std::size_t core, transport::channel<native::socket>& worker, std::string project,
                      content::manifest tree, jobs::build_request request, jobs::progress_log* progress, std::string worldline,
                      std::string version, worlds::actor actor,std::shared_ptr<execution_identity> executor) -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
        auto report = [&](std::uint64_t done, std::uint64_t total, std::string stage) {
            if (progress)
                progress->push(jobs::progress{done, total, std::move(stage)});
        };
        sync_stats stats;

        std::unordered_map<content::hash, std::uint64_t> sizes;
        for (auto& e : tree.files)
            sizes.emplace(e.digest, e.size);
        auto expected_snapshot=tree.id();
        auto plan = co_await within(io, offer_deadline, "the offer", worker.call(jobs::sync_offer{project, std::move(tree)}));
        if (plan.snapshot!=expected_snapshot)
            throw tool_error("invalid_worker_reply","Worker returned a snapshot different from the offered source");

        struct piece {
            content::hash digest;
            std::uint64_t offset, total, length;
        };
        std::vector<std::vector<piece>> pushes(1);
        std::uint64_t batch = 0, total = 0;
        for (auto& h : plan.missing) {
            auto size = sizes.at(h);
            total += size;
            std::uint64_t at = 0;
            do {
                auto len = std::min(part_size, size - at);
                if (batch + len > push_batch && !pushes.back().empty()) {
                    pushes.emplace_back();
                    batch = 0;
                }
                pushes.back().push_back({h, at, size, len});
                batch += len;
                at += len;
            } while (at < size);
        }
        for (auto& push : pushes) {
            if (push.empty())
                continue;
            nodes::node_request wanted{.action = "content", .worldline = worldline, .revision = version};
            for (auto& p : push)
                wanted.parts.push_back({p.digest.str(), p.offset, p.length, {}});
            auto reply = co_await files.node(core, project, std::move(wanted), actor, true);
            if (reply.code != "ok")
                throw tool_error(reply.code, reply.reason);
            // A piece is sent compressed when that makes it smaller.
            std::vector<jobs::source_part> parts;
            for (auto& p : reply.parts) {
                auto bytes = lines::unhex(p.data);
                stats.bytes += bytes.size();
                auto packed = nodes::archive::pack(bytes);
                auto& sent = packed.size() < bytes.size() ? packed : bytes;
                parts.push_back({p.id, p.offset, p.total, std::vector<std::byte>{reinterpret_cast<const std::byte*>(sent.data()),
                                                                                 reinterpret_cast<const std::byte*>(sent.data()) + sent.size()},
                                 &sent == &packed});
            }
            co_await within(io, push_deadline, "a push", worker.call(jobs::source_bytes{std::move(parts)}));
            report(stats.bytes, total, "sync");
        }
        stats.blobs = plan.missing.size();

        report(0, 0, request.steps.empty() ? "run" : request.steps.front().name);
        request.snapshot = expected_snapshot;
        auto limit = std::chrono::seconds{request.timeout_s} + build_overhead;
        executor->begin();
        auto r = co_await within(io, limit, "the build", worker.call(request, progress));
        co_return std::pair{std::move(r), stats};
    }

    // A build admitted by the ledger; ended without a record, it is recorded as cancelled.
    struct admission {
        storage::registry* files;
        std::size_t core;
        std::uint64_t id;
        usage::build_event cancelled;
        std::chrono::steady_clock::time_point started;
        std::shared_ptr<execution_identity> executor=std::make_shared<execution_identity>();
        bool ended = false;
        ~admission() {
            if (ended)
                return;
            auto e = cancelled;
            executor->stamp(e);
            e.error = "cancelled";
            e.error_code="cancelled";
            e.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            files->post(core, [id = id, e](usage::ledger& ledger) { ledger.record(id, e); });
        }
    };

    // One configuration's job, run by the backlog on core zero: admitted by the ledger, sent to a worker, recorded. Nothing when every
    // eligible worker is busy; the admission is then given back and the job waits.
    auto dispatch(services& s, std::string project, config::effective_config config, std::vector<jobs::step> steps, content::manifest snapshot,
                  jobs::build_request request, std::string worldline, std::string version, worlds::actor actor, jobs::progress_log& progress,std::string operation,std::shared_ptr<execution_identity> executor)
        -> coroutine::task<std::optional<backlog::outcome>> {
        config_result r{.configuration = config.name, .snapshot = version,
            .workspace_id=s.snapshot()->server.auth.workspace,.project=project,.worldline=worldline,.version=version,
            .operation=operation,.actor=worlds::identity_key(actor)};
        auto started = std::chrono::steady_clock::now();
        std::optional<std::uint64_t> id;
        try {
            id = co_await s.files.with(s.core, project, [](storage::project_ctx& c) { return c.ledger.admit_build(c.settings.project, c.settings.limits); });
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        if (!id)
            co_return r;
        admission held{&s.files, s.core, *id, usage::build_event{.project=project,.target=config.name,.workspace_id=r.workspace_id,.worldline=worldline,
            .version=version,.snapshot=snapshot.id().str(),.operation=operation,.actor=r.actor}, started,std::move(executor)};

        auto& files = s.files;
        auto& rt = s.workers.io_context();
        auto& router = s.workers;
        std::optional<std::pair<jobs::build_report, sync_stats>> done;
        try {
            done = co_await s.workers.with_worker(
                s.core,
                routing::selection{.needs = config.needs.get() ? routing::requirements{.tools = config.needs.get()->tools,
                                                                                       .os = config.needs.get()->platform.os,
                                                                                       .arch = config.needs.get()->platform.arch}
                                                               : routing::requirements{},
                                   .tree_key = request.tree},
                [&files, &rt, &router, executor=held.executor, project, tree = snapshot, request, worldline, version, actor](auto& worker, jobs::progress_log* progress, std::size_t core) -> coroutine::task<std::pair<jobs::build_report,sync_stats>> {
                    auto identity=router.workers(core).identity(worker);
                    executor->select(identity);
                    auto result=co_await sync_and_run(rt.get_scheduler(core), files, core, worker, project, tree, request, progress, worldline, version, actor,executor);
                    result.first.worker=std::move(identity.first);
                    result.first.worker_id=std::move(identity.second);
                    co_return result;
                },
                &progress);
        } catch (const routing::no_worker& e) {
            if (e.busy) {
                held.ended = true;
                s.files.post(s.core, [id = *id](usage::ledger& ledger) { ledger.release(id); });
                co_return std::nullopt;
            }
            r.error = e.what();
            r.error_code="no_worker";
        } catch (const tool_error& e) {
            r.error=e.what();r.error_code=e.code;
        } catch (const transport::remote_error& e) {
            r.error=e.what();r.error_code=e.code.empty() ? "worker_failed" : e.code;
        } catch (const std::exception& e) {
            r.error = e.what();
            r.error_code=held.executor->started() ? "outcome_unknown" : "sync_failed";
        }

        held.executor->stamp(r);
        if (done) {
            auto& [rep, sync] = *done;
            r.worker = rep.worker;
            r.worker_id = rep.worker_id;
            r.ok = rep.ok();
            r.seconds = rep.seconds;
            r.diagnostics = detail::diagnostics(rep, steps);
            for (auto& st : rep.steps) {
                if (auto summary = detail::ctest_summary(st.output))
                    r.tests = std::move(summary);
                r.steps.push_back({st.name, st.exit_code, st.timed_out, st.seconds, std::move(st.output), st.truncated});
            }
            r.blobs_sent = sync.blobs;
            r.bytes_sent = sync.bytes;
        }
        auto last = r.steps.empty() ? nullptr : &r.steps.back();
        usage::build_event event{.project = project,
                                 .target = config.name,
                                 .worker = r.worker,
                                 .worker_id = r.worker_id,
                                 .ok = r.ok,
                                 .exit_code = last ? last->exit_code : -1,
                                 .timed_out = last && last->timed_out,
                                 .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                                 .blobs_sent = r.blobs_sent,
                                 .bytes_sent = r.bytes_sent,
                                 .error = r.error,.workspace_id=r.workspace_id,.worldline=worldline,.version=version,
                                 .snapshot=snapshot.id().str(),.operation=operation,.actor=r.actor,.error_code=r.error_code};
        // Posted, not awaited: a cancellation arriving now must not leave the admission held.
        held.ended = true;
        s.files.post(s.core, [id = *id, event](usage::ledger& ledger) { ledger.record(id, event); });
        co_return r;
    }

    // Waits here for a queued job and passes its progress on as it comes, with its place in the queue while it waits. A caller that goes
    // away leaves the job.
    template <class Pass> auto follow(services& s, std::shared_ptr<backlog::ticket> held, Pass pass) -> coroutine::task<backlog::outcome> {
        struct leaving {
            std::shared_ptr<backlog::ticket> held;
            ~leaving() { held->leave(); }
        } guard{held};
        auto io = s.workers.io_context().get_scheduler(s.core);
        std::size_t seen = 0;
        std::optional<std::size_t> told;
        while (!held->done()) {
            for (auto& p : held->progress(seen))
                pass(p);
            if (seen == 0 && told != held->ahead()) {
                told = held->ahead();
                pass({.stage = std::format("queued as job {}, {} ahead", held->job(), *told)});
            }
            co_await execution::schedule_after(io, std::chrono::milliseconds{50});
        }
        for (auto& p : held->progress(seen))
            pass(p);
        co_return held->result();
    }

    // What a worker reports for a preparation it ran, as the source keeps it.
    inline auto prepared(const language::plan& plan, const content::manifest& files, std::string version, jobs::build_report& report)
        -> language::result {
        language::result out{.status = report.ok() ? "prepared" : "failed", .revision = std::move(version), .snapshot = files.id(), .tree = report.tree,
                             .build_dirs = plan.build.build_dirs, .worker = report.worker, .worker_id = report.worker_id};
        for (auto& step : report.steps)
            out.steps.push_back({step.name, step.exit_code, step.timed_out, step.seconds, std::move(step.output), step.truncated});
        if (!report.ok())
            out.error = "analysis preparation command failed";
        auto outputs = report.measured.size() - std::min(report.measured.size(), plan.inputs.size());
        out.artifacts.assign(report.measured.begin(), report.measured.begin() + static_cast<std::ptrdiff_t>(outputs));
        out.inputs.assign(report.measured.begin() + static_cast<std::ptrdiff_t>(outputs), report.measured.end());
        return out;
    }

    // On the chosen worker: the source opens the receipt, then the worker gets the files and runs the preparation. Nothing runs when the
    // receipt was there already; `begun` holds what the source answered.
    auto prepare_on(native::scheduler io, storage::registry& registry, std::size_t core, transport::channel<native::socket>& worker, std::string project,
                    content::manifest files, jobs::build_request build, language::request request, jobs::progress_log* progress,
                    std::string line, std::string version, worlds::actor actor, std::optional<language::result>* begun,std::shared_ptr<execution_identity> executor)
        -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
        request.action = "begin";
        auto opened=co_await coroutine::under(coroutine::stop_token{},coroutine::deadline(io,offer_deadline,registry.analysis(core,project,std::move(request))));
        if (!opened || !*opened) throw std::runtime_error("preparation admission timed out");
        *begun=std::move(**opened);
        if ((*begun)->status != "preparing" || (*begun)->replayed)
            co_return {};
        co_return co_await sync_and_run(io, registry, core, worker, project, std::move(files), std::move(build), progress, std::move(line), std::move(version),
                                        std::move(actor),std::move(executor));
    }

    auto finish_preparation(services& s, const std::string& project, jobs::analysis_finish finished) -> coroutine::task<language::result> {
        auto saved=co_await coroutine::under(coroutine::stop_token{},coroutine::deadline(s.workers.io_context().get_scheduler(s.core),offer_deadline,
            s.files.analysis(s.core,project,std::move(finished))));
        if (!saved || !*saved) throw std::runtime_error("preparation receipt persistence timed out");
        co_return std::move(**saved);
    }

    // A preparation runs on a worker, in a build tree of the configuration beside its builds, when a worker can take it: the source
    // keeps the receipt (`begin`, then `analysis.finish`) and publishes the outline. When no worker can, the source runs it all.
    // Nothing while every worker that could is busy.
    auto run_preparation(services& zero, std::string project, language::request request, std::string tree, worlds::actor actor)
        -> coroutine::task<std::optional<backlog::outcome>> {
        auto plan = language::prepare(request.config);
        auto line = request.worldline.empty() ? std::string{"main"} : request.worldline;
        auto selected_tree = co_await selected(zero, project, line, request.revision, actor);
        content::manifest files;
        for (auto& f : lines::manifest(selected_tree))
            files.files.push_back({f.path, f.digest, f.size, f.executable});
        language::check(plan, files.files);
        auto needs = routing::requirements{.tools = plan.build.needs.tools, .os = plan.build.needs.platform.os, .arch = plan.build.needs.platform.arch};
        if (plan.server.argv.front().find('/') == std::string::npos && !std::ranges::contains(needs.tools, plan.server.argv.front()))
            needs.tools.push_back(plan.server.argv.front());
        jobs::build_request build{.project = project,
                                  .tree = std::move(tree),
                                  .steps = plan.build.steps,
                                  .timeout_s = language::timeout(request.config),
                                  .output_limit = language::output_limit(request.config),
                                  .env = plan.build.env};
        for (auto& step : plan.build.steps)
            std::ranges::copy(step.outputs, std::back_inserter(build.measure));
        std::ranges::copy(plan.inputs, std::back_inserter(build.measure));
        for (auto& input : plan.inputs)
            build.replaced.push_back(input.path);
        build.survey = jobs::survey{.provider = plan.server.provider,
                                    .argv = plan.server.argv,
                                    .initialization = plan.server.initialization,
                                    .env = plan.server.env,
                                    .seconds = language::timeout(request.config)};
        auto& registry = zero.files;
        auto& rt = zero.workers.io_context();
        auto& router = zero.workers;
        std::optional<language::result> begun;
        auto executor=std::make_shared<execution_identity>();
        std::optional<std::pair<jobs::build_report, sync_stats>> done;
        std::string failure,failure_code;
        bool stopped=false;
        auto token=co_await coroutine::current_token;
        try {
            done = co_await coroutine::under(token,zero.workers.with_worker(
                zero.core, routing::selection{.needs = needs, .tree_key = build.tree},
                [&registry, &rt, &router, &begun, executor, project, files, build, request, line, version = selected_tree.version, actor](auto& worker, jobs::progress_log* progress, std::size_t core) -> coroutine::task<std::pair<jobs::build_report,sync_stats>> {
                    auto identity=router.workers(core).identity(worker);
                    executor->select(identity);
                    auto result=co_await prepare_on(rt.get_scheduler(core), registry, core, worker, project, files, build, request, progress, line, version, actor, &begun,executor);
                    result.first.worker=std::move(identity.first);
                    result.first.worker_id=std::move(identity.second);
                    co_return result;
                },
                {}));
            stopped=!done;
        } catch (const routing::no_worker& e) {
            if (e.busy)
                co_return std::nullopt;
            begun.reset();
        } catch (const tool_error& e) {
            failure=e.what();failure_code=e.code;
        } catch (const transport::remote_error& e) {
            failure=e.what();failure_code=e.code.empty() ? "worker_failed" : e.code;
        } catch (const std::exception& e) {
            failure=e.what();failure_code=executor->started() ? "outcome_unknown" : "sync_failed";
        }
        if (stopped) {
            if (begun && begun->status=="preparing" && !begun->replayed) {
                language::result cancelled{.status="cancelled",.revision=selected_tree.version,.snapshot=files.id(),.error="cancelled"};
                executor->stamp(cancelled);
                co_await finish_preparation(zero,project,jobs::analysis_finish{.request=request,.result=std::move(cancelled)});
            }
            co_await coroutine::stop;
        }
        if (!begun && failure.empty())
            co_return co_await zero.files.analysis(zero.core,project,std::move(request));
        if (begun && (begun->status!="preparing" || begun->replayed)) co_return *begun;
        if (!done) {
            auto status=failure_code=="outcome_unknown" ? "outcome_unknown" : failure_code=="sync_failed" ? "sync_failed" : "failed";
            language::result failed{.status=status,.revision=selected_tree.version,.snapshot=files.id(),.error=failure};
            executor->stamp(failed);
            if (!begun) co_return failed;
            co_return co_await finish_preparation(zero,project,jobs::analysis_finish{.request=request,.result=std::move(failed)});
        }
        auto& report = done->first;
        jobs::analysis_finish finished{.request = request, .result = prepared(plan, files, selected_tree.version, report), .outline = std::move(report.outline),
                                       .surveyed = std::move(report.surveyed),
                                       .service = plan.server.provider,
                                       .survey_error = std::move(report.survey_error)};
        co_return co_await finish_preparation(zero, project, std::move(finished));
    }

    // Language analysis preparation takes its turn in the backlog with builds, since it uses the same machine. Receipts are read at once.
    auto prepare(services& s, std::string project, std::string configuration, language::request request, const worlds::actor& actor)
        -> coroutine::task<language::result> {
        if (request.action != "prepare")
            co_return co_await s.files.analysis(s.core, project, std::move(request));
        backlog::entry what{.key = content::hash_of(std::format("prepare\n{}\n{}", project, rfl::json::write(request))).str(), .project = project,
                            .configuration = configuration, .operation = "prepare", .actor = worlds::identity_key(actor),
                            .trees = std::string{content::hash_of(std::format("{}\n{}", project, rfl::json::write(request.config.build))).view().substr(0, 24)}};
        auto held = co_await s.queue->submit(s.core, std::move(what), [project, request, actor](services& zero, jobs::progress_log&, std::string tree) {
            return run_preparation(zero, project, request, std::move(tree), actor);
        });
        auto done = co_await follow(s, held, [](const jobs::progress&) {});
        // A build-shaped answer is the backlog's own: the preparation was let go, cancelled or replaced, before it ran.
        if (auto* refused = std::get_if<config_result>(&done))
            throw tool_error(nodes::code::invalid_record, refused->error);
        co_return std::get<language::result>(std::move(done));
    }

    // One configuration of a build, test or run: queued in the backlog, waited for here, its progress passed on as it comes.
    auto execute(services& s, std::string project, std::string operation, bool cacheable, bool fresh, config::effective_config config, std::vector<jobs::step> steps,
                 content::manifest snapshot, mcp::notifier& note, config_result& r, std::string worldline, std::string version, worlds::actor actor)
        -> coroutine::task<> {
        jobs::build_request request{.project = project,
                                    .steps = steps,
                                    .timeout_s = config.limits.timeout_s,
                                    .output_limit = config.limits.output_limit,
                                    .env = config.env,
                                    .max_memory_bytes = config.limits.max_memory_bytes,
                                    .max_cpu_seconds = config.limits.max_cpu_seconds};
        // Each queued or cached execution retains the source coordinate and caller that its audit record names.
        auto key = content::hash_of(std::format("{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}", s.snapshot()->server.auth.workspace,
            worldline,version,worlds::identity_key(actor),operation,snapshot.id().view(),rfl::json::write(request),rfl::json::write(config.needs))).str();
        auto executor=std::make_shared<execution_identity>();
        backlog::entry what{.key = std::move(key), .project = project, .configuration = config.name, .operation = operation,
                            .actor = worlds::identity_key(actor),
                            .trees = std::string{content::hash_of(std::format("{}\n{}", project, rfl::json::write(config))).view().substr(0, 24)},
                            .cacheable = cacheable, .fresh = fresh,.workspace_id=s.snapshot()->server.auth.workspace,.worldline=worldline,.version=version,.executor=executor};
        for (auto& f : snapshot.files)
            what.files.push_back(f.path + '\n' + f.digest.str());
        auto held = co_await s.queue->submit(
            s.core, std::move(what),
            [project, config, steps, snapshot, request, worldline, version, actor, operation,executor](services& zero, jobs::progress_log& progress, std::string tree) {
                auto placed = request;
                placed.tree = std::move(tree);
                return dispatch(zero, project, config, steps, snapshot, std::move(placed), worldline, version, actor, progress,operation,executor);
            });
        // A joined job or a cached result may come from an equal configuration of another name.
        r = std::get<config_result>(co_await follow(s, held, [&note, &config](const jobs::progress& p) {
            auto named = p;
            named.stage = std::format("{}: {}", config.name, p.stage);
            note(progress_note(named));
        }));
        r.configuration = config.name;
        r.snapshot=version;r.workspace_id=s.snapshot()->server.auth.workspace;r.project=project;r.worldline=worldline;
        r.version=version;r.operation=operation;r.actor=worlds::identity_key(actor);
    }

    // Builds always run one fixed version of a worldline, the current one of main when nothing is named.
    // Only the build tool's results are cached; `fresh` skips the cache.
    auto execute_all(services& s, std::string project, std::string operation, bool cacheable, bool fresh,
                     std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs, mcp::notifier& note, std::string worldline,
                     std::string version, const worlds::actor& actor) -> coroutine::task<results> {
        results out;
        out.results.resize(jobs.size());
        if (worldline.empty())
            worldline = "main";
        auto tree = co_await selected(s, project, worldline, version, actor);
        content::manifest snapshot;
        for (auto& f : lines::manifest(tree))
            snapshot.files.push_back({f.path, f.digest, f.size, f.executable});
        out.worldline = worldline;
        out.revision = tree.version;
        out.snapshot = tree.version;
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            for (std::size_t i = 0; i < jobs.size(); ++i)
                children.spawn(execute(s, project, operation, cacheable, fresh, jobs[i].first, jobs[i].second, snapshot, note, out.results[i], worldline, tree.version,
                                       actor));
            co_await children.join();
        });
        out.ok = std::ranges::all_of(out.results, &config_result::ok);
        co_return out;
    }

    struct configurations : tool<"configurations", "List build and run configurations and the operations each build configuration provides."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
        };
        struct config_view {
            std::string name;
            std::vector<std::string> adapters;
            std::vector<std::string> operations;
            bool enabled = true;
            bool is_default = false;
        };
        struct run_view {
            std::string name;
            std::string operation;
            std::vector<std::string> configurations;
        };
        struct output {
            std::vector<config_view> configurations;
            std::vector<run_view> run_configurations;
        };
        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto e = config::resolve(*s.snapshot(), project_of(in));
            output out;
            for (auto& [name, c] : e.configs) {
                config_view v{.name = name, .enabled = c.enabled, .is_default = std::ranges::contains(e.default_configurations, name)};
                for (auto& [_, b] : c.build.builders)
                    if (auto adapter = builds::adapter_of(b); !std::ranges::contains(v.adapters, adapter))
                        v.adapters.emplace_back(adapter);
                for (auto& [op, _] : c.build.operations)
                    v.operations.push_back(op);
                out.configurations.push_back(std::move(v));
            }
            for (auto& [name, r] : e.runs)
                out.run_configurations.push_back({name, r.spec.operation, r.configurations});
            co_return out;
        }
    };

    struct build : tool<"build", "Build a project with one or more configurations at once: sync its files "
                                 "to an eligible worker (matching each configuration's requirements), "
                                 "execute its build steps, and return each configuration's "
                                 "steps, output and compiler diagnostics in one result."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Configurations to build at once; the defaults if absent; [\"*\"] for all enabled", std::optional<std::vector<std::string>>>
                configurations;
            rfl::Description<"Targets accepted by the selected build operation; configured targets if absent", std::optional<std::vector<std::string>>> targets;
            rfl::Description<"Worldline to build; main when absent", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
            rfl::Description<"Run again even when an equal build finished before; builds answer from that result otherwise", std::optional<bool>> fresh;
        };
        using output = results;

        static auto run(services& s, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            auto targets = in.targets.value().value_or(std::vector<std::string>{});
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(in.configurations.value().value_or(std::vector<std::string>{}), "build"))
                jobs.push_back(detail::operation(*c, {.operation = "build", .parameters = {.targets = targets}}));
            co_return co_await execute_all(s, project, "build", true, in.fresh.value().value_or(false), std::move(jobs), note, in.worldline.value().value_or(""),
                                           in.version.value().value_or(""), actor);
        }
    };

    struct test : tool<"test", "Execute the test operation and its configured dependencies on one or more configurations; "
                               "returns steps, diagnostics and output, plus a test summary when CTest output is available."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Configurations to test on at once; the defaults if absent; [\"*\"] for all enabled", std::optional<std::vector<std::string>>>
                configurations;
            rfl::Description<"Test filter, if the selected operation accepts one", std::optional<std::string>> filter;
            rfl::Description<"Test labels, if the selected operation accepts them", std::optional<std::string>> labels;
            rfl::Description<"Worldline to build; main when absent", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
        };
        using output = results;

        static auto run(services& s, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(in.configurations.value().value_or(std::vector<std::string>{}), "test")) {
                jobs.push_back(detail::operation(*c, {.operation = "test", .parameters = {.filter = in.filter.value(), .labels = in.labels.value()}}));
            }
            co_return co_await execute_all(s, project, "test", false, false, std::move(jobs), note, in.worldline.value().value_or(""), in.version.value().value_or(""),
                                           actor);
        }
    };

    struct launch : tool<"run", "Invoke a saved run configuration's operation on its selected build configurations."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"The run configuration", std::string> configuration;
            rfl::Description<"Build configurations to run on; the run configuration's own if absent", std::optional<std::vector<std::string>>> configurations;
            rfl::Description<"Worldline to build; main when absent", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
        };
        using output = results;

        static auto run(services& s, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            auto& r = e.run(in.configuration.value());
            auto names = in.configurations.value().value_or(r.configurations);
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(names, r.spec.operation)) {
                auto build = *c;
                for (auto& [key, value] : r.spec.env)
                    build.build.env[key] = value;
                config::detail::apply(build.limits, r.spec.limits);
                jobs.push_back(detail::operation(std::move(build), {.operation = r.spec.operation, .parameters = r.spec.parameters}));
            }
            co_return co_await execute_all(s, project, r.spec.operation, false, false, std::move(jobs), note, in.worldline.value().value_or(""),
                                           in.version.value().value_or(""), actor);
        }
    };


    struct language_query : tool<"language", "Plan or prepare language analysis, retrieve a preparation receipt, or query the analysis of a worldline "
                                             "version: scopes, symbols, definitions, references or diagnostics. Queries answer from the newest analysis "
                                             "the version contains (stale when older); spans give a path and Unicode scalar offsets in the selected "
                                             "version, or no place when their characters are gone. Lock scopes with the reported environment."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            rfl::Description<"Worldline; main when absent", std::optional<std::string>> worldline;
            rfl::Description<"Version to query; the current one when absent", std::optional<std::string>> version;
            rfl::Description<"Analysis configuration; the project's only one when absent", std::optional<std::string>> configuration;
            rfl::Description<"plan, prepare, receipt, scopes, symbols, definitions, references or diagnostics", std::optional<std::string>> action;
            rfl::Description<"definitions/references: the symbol id", std::optional<std::string>> symbol;
            rfl::Description<"prepare/receipt: operation ID", std::optional<std::string>> operation_id;
        };
        struct output {
            std::string configuration;
            std::optional<std::string> environment, version;
            bool stale = false;
            bool analyzed = false;
            std::optional<std::vector<semantic::scope>> scopes;
            std::optional<std::vector<semantic::symbol>> symbols;
            std::optional<std::vector<semantic::reference>> references;
            std::optional<std::vector<semantic::relation>> relations;
            std::optional<std::vector<semantic::diagnostic>> diagnostics;
            std::optional<language::plan> plan;
            std::optional<language::result> preparation;
        };
        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto settings = config::resolve(*s.snapshot(), project);
            auto action = in.action.value().value_or("scopes");
            auto name = in.configuration.value().value_or("");
            if (name.empty()) {
                if (settings.analysis.size() != 1)
                    throw tool_error(nodes::code::invalid_record, "name the analysis configuration; this project has " + std::to_string(settings.analysis.size()));
                name = settings.analysis.begin()->first;
            }
            auto found = settings.analysis.find(name);
            if (found == settings.analysis.end() && action != "receipt")
                throw tool_error(nodes::code::invalid_record, "analysis configuration not found: " + name);
            output out{.configuration = name};
            if (action == "plan") {
                out.plan = language::prepare(found->second);
                co_return out;
            }
            auto line = in.worldline.value().value_or("main");
            if (action == "prepare" || action == "receipt") {
                out.preparation = co_await prepare(s, project, name,
                    {.action = action, .operation_id = in.operation_id.value().value_or(""), .worldline = line, .revision = in.version.value().value_or(""),
                     .config = found == settings.analysis.end() ? language::configuration{} : found->second}, actor);
                co_return out;
            }
            static const std::set<std::string> queries{"scopes", "symbols", "definitions", "references", "diagnostics"};
            if (!queries.contains(action))
                throw tool_error(nodes::code::invalid_record, "unknown language action " + action);
            out.environment = content::hash_of(config::write(found->second));
            auto r = co_await node_call(s, project, {.action = "semantic", .worldline = line, .revision = in.version.value().value_or(""), .environment = *out.environment},
                                        actor, true);
            out.version = r.version;
            out.stale = r.stale;
            if (!r.semantic)
                co_return out;
            out.analyzed = true;
            auto& g = *r.semantic;
            auto symbol = in.symbol.value();
            if (action == "scopes")
                out.scopes = std::move(g.scopes);
            else if (action == "symbols")
                out.symbols = std::move(g.symbols);
            else if (action == "definitions") {
                std::erase_if(g.symbols, [&](auto& x) { return symbol && x.id != *symbol; });
                out.symbols = std::move(g.symbols);
            } else if (action == "references") {
                std::erase_if(g.references, [&](auto& x) { return symbol && x.target != *symbol; });
                out.references = std::move(g.references);
                out.relations = std::move(g.relations);
            } else
                out.diagnostics = std::move(g.diagnostics);
            co_return out;
        }
    };

    struct generation_result {
        templates::plan preview;
        std::optional<mutation> receipt;
    };
    // Generated files are created in main in one batch; existing files are never overwritten. The caller holds "::".
    auto generate_files(services& s, const std::string& project, templates::parameters parameters, const worlds::actor& actor, std::string operation_id,
                        bool preview, bool for_this_batch = false) -> coroutine::task<generation_result> {
        generation_result out{templates::render(parameters)};
        if (preview)
            co_return out;
        if (operation_id.empty())
            throw tool_error(nodes::code::invalid_record, "generation names an operation_id");
        std::string input = content::hash_of(rfl::json::write(parameters)).str();
        out.receipt = co_await once<mutation>(s, project, actor, operation_id, input, [&]() -> coroutine::task<mutation> {
            auto observed = co_await main_tree(s, project, actor);
            std::vector<change> changes;
            for (const auto& file : out.preview.files) {
                if (std::ranges::contains(observed.files, file.path, &nodes::file_view::path))
                    throw tool_error(nodes::code::source_conflict, file.path + " already exists");
                changes.push_back({.kind = "create", .path = file.path, .text = file.text});
            }
            auto r = co_await main_apply(s, project, actor, observed, std::move(changes), operation_id, "generate " + parameters.template_id, rfl::json::write(record{input}),
                                         for_this_batch ? std::vector<std::string>{std::string{nodes::global_scope}} : std::vector<std::string>{});
            co_return mutation{r.version, r.receipt, operation_id, r.replayed};
        }, [&](const auto& done) { return mutation{done.first.version, done.first.receipt, operation_id, true}; });
        co_return out;
    }
    // Generation for a caller that does not hold the global scope itself, as the console does: its batch takes "::" for itself alone, so
    // no lock is left behind whatever happens to the request.
    auto generate_globally(services& s, const std::string& project, templates::parameters parameters, const worlds::actor& actor,
                           std::string operation_id) -> coroutine::task<generation_result> {
        co_return co_await generate_files(s, project, std::move(parameters), actor, std::move(operation_id), false, true);
    }
    struct generate : tool<"generate", "Preview or generate project templates and source classes/interfaces. List templates with action=catalog. Render with "
                                       "action=preview. Create the files in main with action=apply and an operation_id; this needs the \"::\" lock "
                                       "(worldline lock). Existing files are never overwritten."> {
        struct input {
            rfl::Description<"Project name", std::string> project;
            std::optional<std::string> action;
            std::optional<templates::arguments> parameters;
            std::optional<std::string> operation_id;
        };
        struct output {
            std::vector<templates::descriptor> catalog;
            std::optional<generation_result> generation;
        };
        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            if (in.action == "catalog")
                co_return output{templates::catalog()};
            if (in.action.value_or("preview") != "preview" && in.action != "apply")
                throw tool_error(nodes::code::invalid_record, "unknown generation action");
            if (!in.parameters)
                throw tool_error(nodes::code::invalid_record, "template parameters are required");
            co_return output{{}, co_await generate_files(s, project_of(in), templates::expand(*in.parameters), actor, in.operation_id.value_or(""),
                                                         in.action.value_or("preview") == "preview")};
        }
    };

    struct queue : tool<"jobs", "List the build, test and run jobs that are queued or running and the last ones that finished, or cancel one by id. "
                                "Jobs run as the workspace's limits and workers allow and take turns by who asked; an equal job runs once for "
                                "everyone who asks, and an equal finished build answers from its result. Cancel only a job no one else waits for, "
                                "unless you are an owner."> {
        struct input {
            rfl::Description<"list (default) or cancel", std::optional<std::string>> action;
            rfl::Description<"cancel: the job id", std::optional<std::uint64_t>> id;
        };
        struct output {
            std::vector<backlog::view> jobs;
            std::optional<std::string> cancelled;
        };
        static auto run(services& s, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto action = in.action.value().value_or("list");
            if (action == "cancel") {
                if (!in.id.value())
                    throw tool_error(nodes::code::invalid_record, "cancel names the job id");
                co_return output{.cancelled = co_await s.queue->cancel(s.core, *in.id.value(), actor)};
            }
            if (action != "list")
                throw tool_error(nodes::code::invalid_record, "action is list or cancel");
            co_return output{.jobs = co_await s.queue->list(s.core)};
        }
    };

    using standard =
        toolbox<read, write, edit, remove, move, list, glob, search, worldline, configurations, build, test, launch, language_query, generate, queue>;
} // namespace tools
