module;
#include <sodium.h>
export module tools;
import utility;
import constants;
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
    concept tool_for = requires(services& s, mcp::call_context context, typename T::input in, mcp::notifier& out, const worlds::actor& actor) {
        { T::name } -> std::convertible_to<std::string_view>;
        { T::description } -> std::convertible_to<std::string_view>;
        { T::run(s, context, std::move(in), out, actor) } -> std::same_as<coroutine::task<typename T::output>>;
    };

    template <class In>
    concept on_project = requires(const In& in) { in.project.value(); };

    template <on_project In> auto project_name(const In& in) -> std::string {
        if constexpr (requires { in.project.value().value_or(""); })
            return in.project.value().value_or("");
        else
            return in.project.value();
    }
    template <on_project In> auto project_of(const In& in) -> std::string {
        auto name = project_name(in);
        config::check_name("project", name);
        return name;
    }

    // A refusal from the node engine, kept with its code so MCP callers get { code, message }.
    struct tool_error : std::runtime_error, util::retained_error<tool_error> {
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

            auto call(std::string_view name, rfl::Generic args, mcp::notifier& out, const worlds::actor& who, mcp::call_context context = {})
                -> coroutine::task<mcp::tool_outcome> {
                std::optional<coroutine::task<mcp::tool_outcome>> run;
                (void)((name == Tools::name && (run.emplace(invoke<Tools>(*s_, context, std::move(args), out, who)), true)) || ...);
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
            static auto refused(refusal r) -> mcp::tool_outcome { return {.text = rfl::json::write(r), .structured = rfl::to_generic(r), .error = true}; }

            template <class T>
            auto invoke(services& s, mcp::call_context context, rfl::Generic args, mcp::notifier& note, const worlds::actor& who)
                -> coroutine::task<mcp::tool_outcome> {
                auto started = std::chrono::steady_clock::now();
                if (!who.run.empty()) {
                    if constexpr (T::name == "task")
                        co_return refused({nodes::code::permission_denied, "execution cannot administer tasks"});
                    auto object = args.to_object();
                    if (!object)
                        co_return refused({nodes::code::invalid_record, "arguments must be an object"});
                    if (auto project = object->get("project")) {
                        auto value = project->to_string();
                        if (!value || *value != who.project)
                            co_return refused({nodes::code::permission_denied, "project differs from task binding"});
                    } else
                        (*object)["project"] = who.project;
                    if constexpr (requires(typename T::input input) { input.worldline; }) {
                        if (auto line = object->get("worldline")) {
                            auto value = line->to_string();
                            if (!value || *value != who.worldline)
                                co_return refused({nodes::code::permission_denied, "worldline differs from task binding"});
                        } else
                            (*object)["worldline"] = who.worldline;
                    }
                    args = *object;
                }
                auto in = rfl::from_generic<typename T::input>(args);
                if (!in)
                    co_return refused({nodes::code::invalid_record, std::format("{}: bad arguments: {}", T::name, in.error().what())});
                std::optional<std::string> where;
                if constexpr (on_project<typename T::input>)
                    if (config::valid_name(project_name(*in)))
                        where = project_name(*in);
                auto& actor = who;

                std::optional<typename T::output> value;
                std::string error, code;
                try {
                    value.emplace(co_await T::run(s, context, std::move(*in), note, actor));
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
                    s.files.post(s.core, [e](usage::ledger& ledger) { ledger.record(e); });
                }
                co_return outcome;
            }

            services* s_;
        };
    };

    auto progress_note(const jobs::progress& p) -> mcp::notification {
        return {.method = "notifications/progress",
                .params = mcp::object({{"progress", static_cast<std::int64_t>(p.done)}, {"total", static_cast<std::int64_t>(p.total)}, {"message", p.stage}})};
    }

    namespace lines {
        auto base64(std::string_view bytes) -> std::string {
            static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((bytes.size() + 2) / 3 * 4);
            for (std::size_t i = 0; i < bytes.size(); i += 3) {
                std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
                if (i + 1 < bytes.size())
                    n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
                if (i + 2 < bytes.size())
                    n |= static_cast<unsigned char>(bytes[i + 2]);
                out += alphabet[(n >> 18) & 63];
                out += alphabet[(n >> 12) & 63];
                out += i + 1 < bytes.size() ? alphabet[(n >> 6) & 63] : '=';
                out += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
            }
            return out;
        }
        auto unbase64(std::string_view text) -> std::string {
            auto value = [](char c) -> int {
                if (c >= 'A' && c <= 'Z')
                    return c - 'A';
                if (c >= 'a' && c <= 'z')
                    return c - 'a' + 26;
                if (c >= '0' && c <= '9')
                    return c - '0' + 52;
                if (c == '+')
                    return 62;
                if (c == '/')
                    return 63;
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
                if (text[i + 2] != '=')
                    out += static_cast<char>((n >> 8) & 255);
                if (text[i + 3] != '=')
                    out += static_cast<char>(n & 255);
            }
            return out;
        }
        auto hex(std::string_view bytes) -> std::string {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out(bytes.size() * 2, '0');
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                auto c = static_cast<unsigned char>(bytes[i]);
                out[2 * i] = digits[c >> 4];
                out[2 * i + 1] = digits[c & 0x0f];
            }
            return out;
        }
        auto unhex(std::string_view text) -> std::string {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
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
        auto textual(std::string_view bytes) -> bool {
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
        auto bytes_of(const nodes::file_view& f) -> std::string { return f.encoding == "hex" ? unhex(f.text) : f.text; }
        // Caller-chosen operation IDs become the engine's 32-byte operation identities.
        auto operation_of(std::string_view id) -> std::string {
            if (id.empty() || id.size() > 256)
                throw tool_error(nodes::code::invalid_record, "an operation_id of 1 through 256 characters is required");
            return content::hash_of("operation:" + std::string{id}).str();
        }
        // The length of the longest prefix of `text` within `most` bytes that ends between characters.
        auto cut(std::string_view text, std::uint64_t most) -> std::size_t {
            if (text.size() <= most)
                return text.size();
            auto n = static_cast<std::size_t>(most);
            while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
                --n;
            return n;
        }
        auto scalars(std::string_view text) -> std::uint64_t {
            return static_cast<std::uint64_t>(std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        }

        struct entry {
            std::string path, digest, kind;
            std::uint64_t size = 0;
            bool executable = false;
        };
        auto manifest(const nodes::node_reply& tree) -> std::vector<entry> {
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
    auto execution_actor(services& s, std::string credential) -> coroutine::task<worlds::actor> {
        if (!credential.starts_with("fugrun."))
            throw tool_error("unauthenticated", "invalid execution credential");
        auto text = std::string_view{credential}.substr(7);
        auto dot = text.rfind('.');
        if (dot == std::string_view::npos)
            throw tool_error("unauthenticated", "invalid execution credential");
        auto project = std::string{text.substr(0, dot)}, token = std::string{text.substr(dot + 1)};
        config::check_name("project", project);
        if (token.size() != 64)
            throw tool_error("unauthenticated", "invalid execution credential");
        auto reply = co_await node_call(s, project, {.action = "task_lookup", .execution_token = token}, worlds::service_actor("execution"));
        if (!reply.execution)
            throw tool_error("unauthenticated", "execution not found");
        auto identity = rfl::json::read<worlds::actor>(reply.execution->identity);
        if (!identity)
            throw tool_error("unauthenticated", "invalid execution identity");
        auto who = *identity;
        who.run = reply.execution->run;
        who.task = reply.execution->task;
        who.project = project;
        who.worldline = reply.execution->worldline;
        who.execution_token = token;
        co_return who;
    }

    auto execution_line(const worlds::actor& actor, const std::optional<std::string>& selected) -> std::string {
        if (!actor.run.empty()) {
            if (selected && *selected != actor.worldline)
                throw tool_error(nodes::code::permission_denied, "worldline differs from task binding");
            return actor.worldline;
        }
        if (!selected || selected->empty())
            throw tool_error("context_required", "start a task execution or pass a worldline");
        return *selected;
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
    auto edits_of(const std::vector<change>& changes, const nodes::node_reply& observed, const config::effective_limits& limits) -> std::vector<nodes::edit> {
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
                    out.push_back({.kind = text ? "create" : "binary_create",
                                   .path = path,
                                   .text = text ? bytes : lines::hex(bytes),
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

    // A retried file tool answers from its receipt: recomputing the change on the moved worldline would be another input.
    // What a file tool records with its batch: the digest of the tool input it was asked for, and its count of replacements.
    struct record {
        std::string input;
        std::uint64_t replacements = 0;
    };
    auto recorded(services& s, const std::string& project, const worlds::actor& actor, const std::optional<std::string>& operation_id, const std::string& input,
                  std::string worldline = "main") -> coroutine::task<std::optional<std::pair<nodes::node_reply, record>>> {
        if (!operation_id)
            co_return std::nullopt;
        auto r = co_await s.files.node(s.core, project,
                                       {.action = "receipt",
                                        .worldline = std::move(worldline),
                                        .operation = lines::operation_of(*operation_id),
                                        .caller_operation = std::string{*operation_id}},
                                       actor, true);
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
    auto once(services& s, const std::string& project, const worlds::actor& actor, std::optional<std::string> operation_id, std::string input, Fresh fresh,
              Replay replay, std::string worldline = "main") -> coroutine::task<T> {
        if (auto done = co_await recorded(s, project, actor, operation_id, input, worldline))
            co_return replay(*done);
        std::exception_ptr failed;
        try {
            co_return co_await fresh();
        } catch (const tool_error&) {
            failed = util::current_exception();
        }
        // When even the receipt cannot be read, the caller hears the first answer, which may be outcome_unknown.
        std::optional<std::pair<nodes::node_reply, record>> done;
        try {
            done = co_await recorded(s, project, actor, operation_id, input, worldline);
        } catch (const std::exception&) {
        }
        if (done)
            co_return replay(*done);
        util::rethrow_exception(failed);
    }

    struct worldline
        : tool<"worldline", "Work in an explicit worldline: list, inspect, read, compare, diff, promote, fork, apply, receipt, history, reverse or redo. "
                            "Concurrent edits within a worldline converge; worldlines never merge. Writes name the observed version and "
                            "use Unicode scalar offsets. Repeat an operation_id with identical input to recover its recorded result."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"list, inspect, read, compare, diff, promote, fork, apply, receipt, history, contribution, reverse, redo, export or restore",
                             std::string>
                action;
            rfl::Description<"Worldline name; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"inspect/read/fork/export: selected version; diff/promote: source version (required for promote)", std::optional<std::string>>
                version;
            rfl::Description<"apply/reverse/redo/compare: observed version; diff/promote: target version (required for promote)", std::optional<std::string>>
                observed;
            rfl::Description<"read: first Unicode scalar to return in a text file", std::optional<std::uint64_t>> offset;
            rfl::Description<"read: Unicode scalars to return; bounded by the project read_max_bytes", std::optional<std::uint64_t>> count;
            rfl::Description<"read: omit content when this digest is unchanged", std::optional<std::string>> known_digest;
            rfl::Description<"apply/promote/reverse/redo: held scope lease tokens; an expired token refuses the write", std::optional<std::vector<std::string>>>
                leases;
            rfl::Description<"read: file path", std::optional<std::string>> path;
            rfl::Description<"fork/restore: name of the new worldline", std::optional<std::string>> name;
            rfl::Description<"fork/diff/promote: source worldline; diff/promote compare or copy into worldline", std::optional<std::string>> from;
            rfl::Description<"apply/promote/fork/reverse/redo/restore: caller-chosen ID of this operation", std::optional<std::string>> operation_id;
            rfl::Description<"apply/promote/fork/reverse/redo: what this change is for", std::optional<std::string>> intent;
            rfl::Description<"apply: one atomic batch", std::optional<std::vector<change>>> changes;
            rfl::Description<"export: replace a package of that name", std::optional<bool>> force;
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
            std::optional<std::string> worldline, version, written_version, base_version, path, digest, kind, data_base64, receipt, operation_id, text;
            std::optional<std::uint64_t> size, offset, count, total_scalars, next_offset;
            std::optional<bool> executable, replayed, truncated, unchanged;
            std::optional<std::vector<line_entry>> worldlines;
            std::optional<std::vector<lines::entry>> files;
            std::optional<std::vector<std::string>> conflicts;
            std::optional<std::vector<history_entry>> entries;
            std::optional<std::vector<file_change>> changes;
            // contribution: the worldlines whose current version contains the entry.
            std::optional<std::vector<std::string>> contained_in;
            // restore: the project configuration the package held, as JSON, for the caller to apply where it wants it.
            std::optional<std::string> configuration;
        };

        // Source text is valid UTF-8. Scan disjoint portions once, and only
        // copy/encode the selected byte window after applying the output limit.
        static auto render_read(const nodes::file_view& file, std::optional<std::uint64_t> offset_input, std::optional<std::uint64_t> count_input,
                                std::uint64_t max_bytes) -> output {
            std::string decoded;
            if (file.encoding == "hex")
                decoded = lines::unhex(file.text);
            const std::string_view bytes = file.encoding == "hex" ? decoded : std::string_view{file.text};
            output out{.path = file.path, .digest = file.digest, .kind = file.kind, .size = bytes.size(), .executable = file.executable};
            auto selected = bytes;
            if (offset_input || count_input) {
                if (file.kind != "text")
                    throw tool_error(nodes::code::invalid_record, "source scalar ranges require a text file");
                const auto offset = offset_input.value_or(0);
                const auto wanted = count_input.value_or(std::numeric_limits<std::uint64_t>::max());
                std::uint64_t total = 0, returned = 0;
                std::size_t at = 0, start = bytes.size(), end = bytes.size();
                // Count leading bytes in blocks so the prefix scan can be
                // vectorized. A block has at most this many scalars, so it
                // cannot overshoot the requested offset. Finish a character
                // crossing the block boundary before starting the next one.
                constexpr std::size_t block = 256;
                while (offset - total >= block && bytes.size() - at >= block) {
                    total += lines::scalars(bytes.substr(at, block));
                    at += block;
                    while (at < bytes.size() && (static_cast<unsigned char>(bytes[at]) & 0xc0) == 0x80)
                        ++at;
                }
                while (at < bytes.size()) {
                    if (total == offset)
                        start = end = at;
                    auto next = at + 1;
                    while (next < bytes.size() && (static_cast<unsigned char>(bytes[next]) & 0xc0) == 0x80)
                        ++next;
                    if (total >= offset) {
                        if (returned == wanted || next - start > max_bytes)
                            break;
                        end = next;
                        ++returned;
                    }
                    at = next;
                    ++total;
                }
                // Only the unvisited suffix is counted; neither endpoint is
                // searched from the beginning again. No offset+count overflow.
                total += lines::scalars(bytes.substr(at));
                if (offset > total)
                    throw tool_error(nodes::code::invalid_record, "read offset is beyond the text");
                const auto requested = std::min(wanted, total - offset);
                selected = bytes.substr(start, end - start);
                out.offset = offset;
                out.count = returned;
                out.total_scalars = total;
                out.next_offset = offset + returned;
                out.truncated = returned < requested;
            }
            if (file.kind == "text")
                out.text = std::string{selected};
            out.data_base64 = lines::base64(selected);
            return out;
        }

        static auto delta(services& s, const std::string& project, const std::string& line, const std::string& before, const std::string& after,
                          const worlds::actor& actor, std::string from = {}) -> coroutine::task<output> {
            auto base = co_await node_call(s, project, {.action = "head", .worldline = from.empty() ? line : from, .revision = before}, actor, true);
            auto diff = co_await node_call(
                s, project,
                {.action = from.empty() ? "compare" : "diff", .worldline = line, .revision = after, .expected = base.frontier, .target = from, .max_bytes = 0},
                actor, true);
            output out{.worldline = line, .version = diff.version, .base_version = diff.base_version};
            out.changes.emplace();
            out.files.emplace();
            out.conflicts = diff.conflicts;
            auto side = [](const auto& file) {
                return file_side{.path = file.path, .digest = file.digest, .kind = file.kind, .size = file.size, .executable = file.executable};
            };
            for (const auto& change : diff.changes) {
                out.changes->push_back({change.type, change.before ? std::optional{side(*change.before)} : std::nullopt,
                                        change.after ? std::optional{side(*change.after)} : std::nullopt});
                if (change.after) {
                    const auto& f = *change.after;
                    out.files->push_back({f.path, f.digest, f.kind, f.size, f.executable});
                }
            }
            co_return out;
        }

        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto& action = in.action.value();
            auto selection = in.worldline.value();
            if (!selection && action == "fork")
                selection = in.from.value();
            if (!selection && action == "restore")
                selection = in.name.value();
            auto line = action == "list" && actor.run.empty() && !selection ? std::string{} : execution_line(actor, selection);
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
                std::string selected_version = in.version.value().value_or("");
                if (in.known_digest.value()) {
                    auto tree = co_await node_call(s, project, {.action = "stat", .worldline = line, .revision = selected_version, .target = *in.path.value()},
                                                   actor, true);
                    auto found = std::ranges::find(tree.files, *in.path.value(), &nodes::file_view::path);
                    if (found == tree.files.end())
                        throw tool_error(nodes::code::missing_objects, "file not in this version");
                    if (found->digest == *in.known_digest.value())
                        co_return output{.worldline = line,
                                         .version = tree.version,
                                         .path = found->path,
                                         .digest = found->digest,
                                         .kind = found->kind,
                                         .size = found->size,
                                         .executable = found->executable,
                                         .unchanged = true};
                    selected_version = tree.version;
                }
                auto r = co_await node_call(s, project, {.action = "read", .worldline = line, .revision = selected_version, .target = *in.path.value()}, actor,
                                            true);
                auto file = std::ranges::find(r.files, *in.path.value(), &nodes::file_view::path);
                if (file == r.files.end())
                    throw tool_error(nodes::code::missing_objects, *in.path.value() + " is not in this version");
                out = render_read(*file, in.offset.value(), in.count.value(),
                                  (in.offset.value() || in.count.value()) ? config::resolve(*s.snapshot(), project).limits.read_max_bytes : 0);
                out.worldline = line;
                out.version = r.version;
            } else if (action == "compare") {
                if (!in.observed.value())
                    throw tool_error(nodes::code::invalid_record, "compare requires observed");
                co_return co_await delta(s, project, line, *in.observed.value(), in.version.value().value_or(""), actor);
            } else if (action == "diff") {
                if (!in.from.value())
                    throw tool_error(nodes::code::invalid_record, "diff requires from");
                co_return co_await delta(s, project, line, in.version.value().value_or(""), in.observed.value().value_or(""), actor, *in.from.value());
            } else if (action == "promote") {
                if (!in.from.value() || !in.version.value() || !in.observed.value() || !in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "promote requires from, version, observed and operation_id");
                auto source = co_await node_call(s, project, {.action = "head", .worldline = *in.from.value(), .revision = *in.version.value()}, actor, true);
                auto base = co_await node_call(s, project, {.action = "head", .worldline = line, .revision = *in.observed.value()}, actor, true);
                auto r = co_await node_call(s, project,
                                            {.action = "promote",
                                             .worldline = line,
                                             .leases = in.leases.value().value_or(std::vector<std::string>{}),
                                             .operation = lines::operation_of(*in.operation_id.value()),
                                             .caller_operation = *in.operation_id.value(),
                                             .expected = base.frontier,
                                             .source = source.frontier,
                                             .target = *in.from.value(),
                                             .intent = in.intent.value().value_or("")},
                                            actor);
                out = co_await delta(s, project, line, r.base_version, r.version, actor);
                out.receipt = r.receipt;
                out.operation_id = in.operation_id.value();
                out.replayed = r.replayed;
                out.written_version = r.written_version;
            } else if (action == "fork") {
                if (!in.name.value())
                    throw tool_error(nodes::code::invalid_record, "fork names the new worldline");
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "fork names an operation_id");
                // A retry answers from the receipt: the source worldline may have moved on since the original resolved its version.
                auto name = *in.name.value();
                auto id = *in.operation_id.value();
                std::string input = content::hash_of(rfl::json::write(std::array{name, in.from.value().value_or(line), in.version.value().value_or(""),
                                                                                 in.intent.value().value_or("")}))
                                        .str();
                auto apply_once = [&]() -> coroutine::task<nodes::node_reply> {
                    auto base = co_await selected(s, project, in.from.value().value_or(line), in.version.value().value_or(""), actor);
                    co_return co_await node_call(s, project,
                                                 {.action = "fork",
                                                  .worldline = name,
                                                  .operation = lines::operation_of(id),
                                                  .caller_operation = id,
                                                  .source = base.frontier,
                                                  .intent = in.intent.value().value_or(""),
                                                  .result = rfl::json::write(record{input})},
                                                 actor);
                };
                auto r = co_await once<nodes::node_reply>(
                    s, project, actor, id, input, std::move(apply_once),
                    [](const auto& done) {
                        auto r = done.first;
                        r.replayed = true;
                        return r;
                    },
                    name);
                out = {.worldline = name, .version = r.version, .receipt = r.receipt, .operation_id = id, .replayed = r.replayed};
            } else if (action == "apply" || action == "reverse" || action == "redo") {
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, action + " names an operation_id");
                if (!in.observed.value())
                    throw tool_error(nodes::code::invalid_record, action + " names the version it observed");
                auto observed = co_await node_call(s, project, {.action = "head", .worldline = line, .revision = *in.observed.value()}, actor, true);
                std::set<std::string> loaded;
                for (const auto& change : in.changes.value().value_or(std::vector<change>{}))
                    if ((change.kind.value() == "edit" || change.kind.value() == "write") && loaded.insert(change.path.value()).second) {
                        auto file = co_await node_call(
                            s, project, {.action = "read", .worldline = line, .revision = observed.version, .target = change.path.value()}, actor, true);
                        observed.files.insert(observed.files.end(), file.files.begin(), file.files.end());
                    }
                nodes::node_request request{.action = action,
                                            .worldline = line,
                                            .leases = in.leases.value().value_or(std::vector<std::string>{}),
                                            .operation = lines::operation_of(in.operation_id.value().value_or("")),
                                            .caller_operation = std::string{in.operation_id.value().value_or("")},
                                            .expected = observed.frontier,
                                            .target = in.target.value().value_or(""),
                                            .intent = in.intent.value().value_or("")};
                if (action == "apply") {
                    auto limits = config::resolve(*s.snapshot(), project).limits;
                    request.edits = edits_of(in.changes.value().value_or(std::vector<change>{}), observed, limits);
                    if (request.edits.empty())
                        throw tool_error(nodes::code::invalid_record, "apply needs at least one change");
                }
                auto r = co_await node_call(s, project, std::move(request), actor);
                out = co_await delta(s, project, line, r.base_version, r.version, actor);
                out.receipt = r.receipt;
                out.operation_id = in.operation_id.value();
                out.replayed = r.replayed;
                out.written_version = r.written_version;
            } else if (action == "receipt") {
                auto r = co_await node_call(s, project,
                                            {.action = "receipt",
                                             .worldline = line,
                                             .operation = lines::operation_of(in.operation_id.value().value_or("")),
                                             .caller_operation = std::string{in.operation_id.value().value_or("")}},
                                            actor, true);
                out = co_await delta(s, project, r.worldline, r.base_version, r.version, actor);
                out.receipt = r.receipt;
                out.operation_id = in.operation_id.value();
                out.replayed = true;
                out.written_version = r.written_version;
            } else if (action == "history") {
                auto limit = std::min<std::uint64_t>(in.limit.value().value_or(100), config::resolve(*s.snapshot(), project).limits.list_max_entries);
                auto r = co_await node_call(s, project, {.action = "history", .worldline = line, .target = in.cursor.value().value_or(""), .limit = limit},
                                            actor, true);
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
                auto r = co_await node_call(s, project,
                                            {.action = "export",
                                             .worldline = line,
                                             .revision = in.version.value().value_or(""),
                                             .target = *in.package.value(),
                                             .configuration = settings ? config::write(*settings) : std::string{},
                                             .force = in.force.value().value_or(false)},
                                            actor);
                out.version = r.version;
                out.files = lines::manifest(r);
            } else if (action == "restore") {
                if (!in.package.value() || !in.name.value() || !in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "a restore names its package, the new worldline and an operation_id");
                auto r = co_await node_call(s, project,
                                            {.action = "restore",
                                             .worldline = *in.name.value(),
                                             .operation = lines::operation_of(*in.operation_id.value()),
                                             .caller_operation = *in.operation_id.value(),
                                             .target = *in.package.value()},
                                            actor);
                auto tree = co_await selected(s, project, *in.name.value(), r.version, actor);
                out = {
                    .worldline = *in.name.value(), .version = r.version, .receipt = r.receipt, .operation_id = in.operation_id.value(), .replayed = r.replayed};
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
                    changes.push_back(
                        {c.type, c.before ? std::optional{side(*c.before)} : std::nullopt, c.after ? std::optional{side(*c.after)} : std::nullopt});
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

    // File operations read a snapshot and submit edits against that observed version.
    auto main_tree(services& s, const std::string& project, const worlds::actor& actor, bool text = false) -> coroutine::task<nodes::node_reply> {
        co_return co_await node_call(s, project, {.action = text ? "inspect" : "tree", .worldline = "main"}, actor, true);
    }
    auto main_apply(services& s, const std::string& project, const worlds::actor& actor, const nodes::node_reply& observed, std::vector<change> changes,
                    std::string operation_id, std::string intent, std::string result = {}) -> coroutine::task<nodes::node_reply> {
        auto limits = config::resolve(*s.snapshot(), project).limits;
        co_return co_await node_call(s, project,
                                     {.action = "apply",
                                      .worldline = "main",
                                      .operation = lines::operation_of(operation_id),
                                      .caller_operation = std::string{operation_id},
                                      .expected = observed.frontier,
                                      .edits = edits_of(changes, observed, limits),
                                      .intent = std::move(intent),
                                      .result = std::move(result)},
                                     actor);
    }

    struct mutation {
        std::string version, receipt, operation_id;
        bool replayed = false;
    };

    struct step_result {
        std::string name;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::string output;
        bool truncated = false;
        std::optional<builds::exported_file> log;
        bool log_truncated = false;
        std::string log_error;
        std::string cache_error;
    };

    using diagnostic = builds::finding;
    using test_summary = builds::tally;

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
        std::vector<builds::exported_file> exports;
        std::string export_error;
        std::vector<std::string> export_skipped;
        std::string dependency_cache;
        builds::timings timings;
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
            std::string workspace_id, worldline, version;
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
            auto reuse() const -> const std::string& { return reuse_; }
            // The progress its job reported since `seen`, which moves past it.
            auto progress(std::size_t& seen) -> std::vector<jobs::progress> { return progress_ ? progress_->since(seen) : std::vector<jobs::progress>{}; }
            void leave() { left_.store(true, std::memory_order_release); }

        private:
            friend class backlog;
            std::string actor_;
            std::string reuse_ = "executed";
            std::chrono::steady_clock::time_point submitted_ = std::chrono::steady_clock::now();
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
        auto forget(std::size_t here, std::string key) -> coroutine::task<>;
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
            std::chrono::steady_clock::time_point retry_at{}, started_at{};
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
        static auto refused(const entry& e, std::string why) -> outcome;

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
        // What the steps' reports say, read on the worker: their diagnostics, at most `max` and each once, and the tests of every
        // step that ran some.
        auto read(jobs::build_report& r, std::size_t max = 100) -> std::pair<std::vector<diagnostic>, std::optional<test_summary>> {
            std::vector<diagnostic> out;
            std::optional<test_summary> tests;
            for (auto& step : r.steps) {
                if (step.tests) {
                    if (!tests)
                        tests.emplace();
                    tests->total += step.tests->total;
                    tests->failed += step.tests->failed;
                    std::ranges::move(step.tests->failed_tests, std::back_inserter(tests->failed_tests));
                }
                for (auto& d : step.diagnostics) {
                    if (out.size() >= max)
                        break;
                    if (!std::ranges::any_of(out,
                                             [&](auto& o) { return o.file == d.file && o.line == d.line && o.column == d.column && o.message == d.message; }))
                        out.push_back(std::move(d));
                }
            }
            return {std::move(out), std::move(tests)};
        }

        auto operation(config::effective_config c, builds::request request) -> std::pair<config::effective_config, std::vector<jobs::step>> {
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
        double seconds = 0;
    };

    inline constexpr std::chrono::seconds offer_deadline{60};
    inline constexpr std::chrono::seconds push_deadline{120};
    inline constexpr std::chrono::seconds build_overhead{300};

    auto no_answer(std::string_view what, std::chrono::seconds limit) -> std::runtime_error {
        return std::runtime_error("the worker did not answer " + std::string{what} + " within " + std::to_string(limit.count()) + " s");
    }

    template <class T>
    auto within(native::scheduler& io, std::chrono::seconds limit, std::string_view what, coroutine::task<transport::call_result<T>> call)
        -> coroutine::task<T> {
        auto r = co_await coroutine::deadline(io, limit, std::move(call));
        if (!r)
            throw no_answer(what, limit);
        if (!*r)
            transport::raise_call_error(std::move(r->error()));
        co_return std::move(**r);
    }

    struct execution_identity {
        std::mutex mutex;
        std::pair<std::string, std::optional<std::string>> value;
        bool began = false;
        builds::timings timings;
        std::optional<std::chrono::steady_clock::time_point> syncing;
        void sync_begin() {
            std::lock_guard held{mutex};
            syncing = std::chrono::steady_clock::now();
        }
        void sync_end() {
            std::lock_guard held{mutex};
            timings.sync_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - *syncing).count();
            syncing.reset();
        }
        void begin() {
            std::lock_guard held{mutex};
            began = true;
        }
        auto started() -> bool {
            std::lock_guard held{mutex};
            return began;
        }
        void select(std::pair<std::string, std::optional<std::string>> identity) {
            std::lock_guard held{mutex};
            value = std::move(identity);
        }
        template <class Result> void stamp(Result& result) {
            std::lock_guard held{mutex};
            result.worker = value.first;
            result.worker_id = value.second;
            if constexpr (requires { result.timings; }) {
                result.timings = timings;
                if (syncing)
                    result.timings.sync_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - *syncing).count();
            }
        }
    };

    auto backlog::refused(const entry& e, std::string why) -> outcome {
        auto code = why == "cancelled" ? "cancelled" : why.starts_with("replaced") ? "replaced" : "execution_failed";
        config_result result{.configuration = e.configuration, .snapshot = e.version, .error = std::move(why), .error_code = code};
        if (e.executor)
            e.executor->stamp(result);
        return result;
    }

    auto sync_and_run(native::scheduler& io, storage::registry& files, std::size_t core, transport::channel<native::socket>& worker, std::string project,
                      content::manifest tree, jobs::build_request request, jobs::progress_log* progress, std::string worldline, std::string version,
                      worlds::actor actor, std::shared_ptr<execution_identity> executor) -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
        auto report = [&](std::uint64_t done, std::uint64_t total, std::string stage) {
            if (progress)
                progress->push(jobs::progress{done, total, std::move(stage)});
        };
        sync_stats stats;
        const auto sync_started = std::chrono::steady_clock::now();
        executor->sync_begin();

        std::unordered_map<content::hash, std::uint64_t> sizes;
        for (auto& e : tree.files)
            sizes.emplace(e.digest, e.size);
        auto expected_snapshot = tree.id();
        auto plan = co_await within(io, offer_deadline, "the offer", worker.call(jobs::sync_offer{project, std::move(tree)}));
        if (plan.snapshot != expected_snapshot)
            throw tool_error("invalid_worker_reply", "Worker returned a snapshot different from the offered source");

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
                parts.push_back(
                    {p.id, p.offset, p.total,
                     std::vector<std::byte>{reinterpret_cast<const std::byte*>(sent.data()), reinterpret_cast<const std::byte*>(sent.data()) + sent.size()},
                     &sent == &packed});
            }
            co_await within(io, push_deadline, "a push", worker.call(jobs::source_bytes{std::move(parts)}));
            report(stats.bytes, total, "sync");
        }
        executor->sync_end();
        stats.blobs = plan.missing.size();
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - sync_started).count();

        report(0, 0, request.steps.empty() ? "run" : request.steps.front().name);
        request.snapshot = expected_snapshot;
        auto limit = std::chrono::seconds{request.timeout_s} + build_overhead;
        executor->begin();
        auto r = co_await within(io, limit, "the build", worker.call(request, progress));
        std::uint64_t exported = 0, logged = 0;
        if (r.exports.size() > constants::export_files)
            throw tool_error("invalid_worker_reply", "too many exported files");
        std::vector<builds::exported_file*> transfer;
        for (auto& file : r.exports)
            transfer.push_back(&file);
        for (const auto& file : r.exports) {
            content::checked_name(file.name);
            content::checked_path(file.path);
            if (!request.exports.contains(file.name) || !content::is_hash(file.digest) || file.size > constants::export_bytes - exported)
                throw tool_error("invalid_worker_reply", "invalid exported file");
            exported += file.size;
        }
        for (std::size_t index = 0; index < r.steps.size(); ++index)
            if (r.steps[index].log) {
                const auto& log = *r.steps[index].log;
                if (index >= request.steps.size() || log.name != "step-" + std::to_string(index) + ".log" || log.path != std::to_string(index) + ".log" ||
                    !content::is_hash(log.digest) || log.size > constants::log_bytes - logged)
                    throw tool_error("invalid_worker_reply", "invalid execution log");
                logged += log.size;
                transfer.push_back(&*r.steps[index].log);
            }
        for (auto* target : transfer) {
            auto& file = *target;
            try {
                if (co_await files.has_export(core, file.digest, file.size))
                    continue;
                std::uint64_t offset = 0;
                do {
                    const auto length = std::min(part_size, file.size - offset);
                    auto bytes = co_await within(io, push_deadline, "an export", worker.call(jobs::export_read{file.digest, offset, length}));
                    if (bytes.parts.size() != 1)
                        throw tool_error("invalid_worker_reply", "invalid export part count");
                    auto& part = bytes.parts.front();
                    if (part.digest != file.digest || part.offset != offset || part.total != file.size || part.packed || part.bytes.size() != length)
                        throw tool_error("invalid_worker_reply", "invalid export part");
                    co_await files.receive_export(core, std::move(part));
                    offset += length;
                } while (offset < file.size);
            } catch (const std::exception& error) {
                file.delivery_error = std::string{error.what()}.substr(0, 512);
            }
        }
        co_return std::pair{std::move(r), stats};
    }

    struct invocation {
        storage::registry& files;
        std::size_t core;
        usage::call_event event;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        ~invocation() {
            event.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            files.post(core, [value = event](usage::ledger& ledger) { ledger.record(value); });
        }
    };

    // A build admitted by the ledger; ended without a record, it is recorded as cancelled.
    struct admission {
        storage::registry* files;
        std::size_t core;
        std::uint64_t id;
        usage::build_event cancelled;
        std::chrono::steady_clock::time_point started;
        std::shared_ptr<execution_identity> executor = std::make_shared<execution_identity>();
        bool ended = false;
        ~admission() {
            if (ended)
                return;
            auto e = cancelled;
            executor->stamp(e);
            e.error = "cancelled";
            e.error_code = "cancelled";
            e.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            files->post(core, [id = id, e](usage::ledger& ledger) { ledger.record(id, e); });
        }
    };

    // One configuration's job, run by the backlog on core zero: admitted by the ledger, sent to a worker, recorded. Nothing when every
    // eligible worker is busy; the admission is then given back and the job waits.
    auto dispatch(services& s, std::string project, config::effective_config config, std::vector<jobs::step> steps, content::manifest snapshot,
                  jobs::build_request request, std::string worldline, std::string version, worlds::actor actor, jobs::progress_log& progress,
                  std::string operation, std::shared_ptr<execution_identity> executor, std::string toolchain)
        -> coroutine::task<std::optional<backlog::outcome>> {
        if (!actor.run.empty())
            co_await selected(s, project, worldline, version, actor);
        config_result r{.configuration = config.name, .snapshot = version};
        auto workspace = s.snapshot()->server.auth.workspace;
        auto who = worlds::execution_key(actor);
        auto started = std::chrono::steady_clock::now();
        std::optional<std::uint64_t> id;
        try {
            id = co_await s.files.with(s.core, project, [](storage::project_ctx& c) { return c.ledger.admit_build(c.settings.project, c.settings.limits); });
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        if (!id)
            co_return r;
        admission held{&s.files,
                       s.core,
                       *id,
                       usage::build_event{.project = project,
                                          .target = config.name,
                                          .workspace_id = workspace,
                                          .worldline = worldline,
                                          .version = version,
                                          .snapshot = snapshot.id().str(),
                                          .operation = operation,
                                          .actor = who},
                       started,
                       std::move(executor)};

        auto& files = s.files;
        auto& rt = s.workers.io_context();
        auto& router = s.workers;
        std::optional<std::pair<jobs::build_report, sync_stats>> done;
        try {
            auto run_on_worker = [&files, &rt, &router, executor = held.executor, project, tree = snapshot, request, worldline, version,
                                  actor](auto& worker, jobs::progress_log* progress,
                                         std::size_t core) -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
                auto identity = router.workers(core).identity(worker);
                executor->select(identity);
                auto result =
                    co_await sync_and_run(rt.get_scheduler(core), files, core, worker, project, tree, request, progress, worldline, version, actor, executor);
                result.first.worker = std::move(identity.first);
                result.first.worker_id = std::move(identity.second);
                co_return result;
            };
            done =
                co_await s.workers.with_worker(s.core,
                                               routing::selection{.needs = config.needs.get() ? routing::requirements{.tools = config.needs.get()->tools,
                                                                                                                      .os = config.needs.get()->platform.os,
                                                                                                                      .arch = config.needs.get()->platform.arch,
                                                                                                                      .toolchain_id = toolchain}
                                                                                              : routing::requirements{.toolchain_id = toolchain},
                                                                  .tree_key = request.tree},
                                               std::move(run_on_worker), &progress);
        } catch (const routing::no_worker& e) {
            if (e.busy) {
                held.ended = true;
                s.files.post(s.core, [id = *id](usage::ledger& ledger) { ledger.release(id); });
                co_return std::nullopt;
            }
            r.error = e.what();
            r.error_code = "no_worker";
        } catch (const tool_error& e) {
            r.error = e.what();
            r.error_code = e.code;
        } catch (const transport::remote_error& e) {
            r.error = e.what();
            r.error_code = e.code.empty() ? "worker_failed" : e.code;
        } catch (const std::exception& e) {
            r.error = e.what();
            r.error_code = held.executor->started() ? "outcome_unknown" : "sync_failed";
        }

        held.executor->stamp(r);
        if (done) {
            auto& [rep, sync] = *done;
            r.worker = rep.worker;
            r.worker_id = rep.worker_id;
            r.ok = rep.ok();
            r.seconds = rep.seconds;
            r.timings.sync_seconds = sync.seconds;
            r.timings.worker_seconds = rep.seconds;
            r.timings.preparation_seconds = rep.preparation_seconds;
            r.timings.execution_seconds = rep.execution_seconds;
            std::tie(r.diagnostics, r.tests) = detail::read(rep);
            for (auto& st : rep.steps)
                r.steps.push_back({st.name, st.exit_code, st.timed_out, st.seconds, std::move(st.output), st.truncated, std::move(st.log), st.log_truncated,
                                   std::move(st.log_error), std::move(st.cache_error)});
            r.blobs_sent = sync.blobs;
            r.bytes_sent = sync.bytes;
            r.exports = std::move(rep.exports);
            r.export_error = std::move(rep.export_error);
            r.export_skipped = std::move(rep.export_skipped);
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
                                 .error = r.error,
                                 .workspace_id = workspace,
                                 .worldline = worldline,
                                 .version = version,
                                 .snapshot = snapshot.id().str(),
                                 .operation = operation,
                                 .actor = who,
                                 .error_code = r.error_code};
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
                pass({.stage = std::format("queued, {} ahead", *told)});
            }
            co_await execution::schedule_after(io, std::chrono::milliseconds{50});
        }
        for (auto& p : held->progress(seen))
            pass(p);
        co_return held->result();
    }

    // A compact result of worker preparation and analysis.
    auto prepared(const language::plan& plan, const content::manifest& files, std::string version, jobs::build_report& report) -> language::result {
        language::result out{.status = report.ok() ? "prepared" : "failed",
                             .revision = std::move(version),
                             .snapshot = files.id(),
                             .tree = report.tree,
                             .build_dirs = plan.build.build_dirs,
                             .worker = report.worker,
                             .worker_id = report.worker_id};
        for (auto& step : report.steps)
            out.steps.push_back({step.name, step.exit_code, step.timed_out, step.seconds, std::move(step.output), step.truncated});
        if (!report.ok()) {
            out.error = "analysis preparation command failed";
            for (const auto& step : out.steps) {
                if (step.exit_code == 0 && !step.timed_out)
                    continue;
                constexpr std::size_t limit = 4096;
                auto start = step.output.size() > limit ? step.output.size() - limit : 0;
                while (start < step.output.size() && (static_cast<unsigned char>(step.output[start]) & 0xC0) == 0x80)
                    ++start;
                out.error = std::format("analysis preparation step '{}': {}; truncated={}\n{}", step.name,
                                        step.timed_out ? "timed out" : std::format("exit code {}", step.exit_code), step.truncated || start != 0,
                                        step.output.substr(start));
                break;
            }
        }
        out.artifacts = report.measured;
        for (const auto& input : plan.inputs)
            if (auto measured = std::ranges::find(report.measured, input.name, &builds::artifact_result::name); measured != report.measured.end())
                out.inputs.push_back(*measured);
        return out;
    }

    auto hot_key(const std::string& project, const std::string& worldline, const std::string& configuration) -> std::string {
        return std::string{content::hash_of(rfl::json::write(std::vector<std::string>{project, worldline, configuration})).view().substr(0, 24)};
    }

    // Select an actual runtime before computing a content key. Busy workers still name
    // valid runtimes: a cache hit needs no slot, and a miss can wait in the backlog.
    auto select_toolchain(services& s, routing::requirements needs, const std::string& trees) -> coroutine::task<std::string> {
        auto workers = co_await s.workers.describe(s.core);
        std::optional<std::tuple<bool, bool, std::string>> selected;
        for (const auto& worker : workers) {
            if (!worker.online || worker.dependency_publisher != needs.dependency_publisher || (needs.os && worker.platform.os != *needs.os) ||
                (needs.arch && worker.platform.arch != *needs.arch) ||
                !std::ranges::all_of(needs.tools, [&](const auto& name) { return std::ranges::contains(worker.tools, name); }))
                continue;
            if (worker.toolchain_id.empty())
                continue;
            bool cold = !std::ranges::any_of(worker.trees, [&](const auto& tree) { return tree.starts_with(trees + "-"); });
            const bool publisher = std::ranges::any_of(workers, [&](const auto& candidate) {
                return candidate.online && candidate.dependency_publisher && candidate.toolchain_id == worker.toolchain_id;
            });
            auto candidate = std::tuple{cold, !publisher, worker.toolchain_id};
            if (!selected || candidate < *selected)
                selected = std::move(candidate);
        }
        if (!selected)
            throw tool_error("no_worker", "no worker with the required tools and a reported toolchain fingerprint is online");
        co_return std::get<2>(*selected);
    }

    // Run fills before admitting the consumer job: no nested backlog slot is held while waiting.
    auto fill_dependencies(services& s, const std::string& project, const std::vector<jobs::step>& steps, const content::manifest& snapshot,
                           const std::string& toolchain, const worlds::actor& actor, const std::string& worldline, const std::string& version,
                           std::chrono::steady_clock::time_point deadline, mcp::notifier* note) -> coroutine::task<std::string> {
        if (!std::ranges::any_of(steps, [](const auto& step) { return step.dependencies.has_value(); }))
            co_return "";
        const auto workers = co_await s.workers.describe(s.core);
        if (!std::ranges::any_of(workers, [&](const auto& worker) { return worker.online && worker.dependency_publisher && worker.toolchain_id == toolchain; }))
            co_return "unavailable: no matching dependency publisher";
        if (std::ranges::any_of(snapshot.files, [](const auto& file) { return std::filesystem::path{file.path}.filename() == "vcpkg-configuration.json"; }))
            co_return "skipped: custom registries";
        std::string status;
        std::set<std::string> filled;
        bool omitted = false;
        for (const auto& step : steps) {
            if (!step.dependencies)
                continue;
            auto record_status = [&](std::string value) {
                if (omitted)
                    return;
                constexpr std::string_view marker = "additional dependency results omitted";
                const auto entry = step.name + ": " + value;
                if (status.size() + entry.size() + 2 <= constants::dependency_status_bytes - marker.size() - 2) {
                    if (!status.empty())
                        status += "; ";
                    status += entry;
                } else {
                    if (!status.empty())
                        status += "; ";
                    status += marker;
                    omitted = true;
                }
            };
            const auto& input = *step.dependencies;
            const auto path = (std::filesystem::path{input.manifest_dir} / "vcpkg.json").lexically_normal().generic_string();
            const auto found = std::ranges::find(snapshot.files, path, &content::entry::path);
            if (found == snapshot.files.end() || found->size > constants::dependency_manifest_bytes) {
                record_status("skipped: manifest absent or too large");
                continue;
            }
            try {
                nodes::node_request wanted{.action = "content", .worldline = worldline, .revision = version};
                wanted.parts.push_back({found->digest.str(), 0, found->size, {}});
                auto reply = co_await s.files.node(s.core, project, std::move(wanted), actor, true);
                if (reply.code != "ok" || reply.parts.size() != 1)
                    throw std::runtime_error("dependency manifest is unavailable");
                const auto text = lines::unhex(reply.parts.front().data);
                if (text.size() != found->size || content::hash_of(text) != found->digest)
                    throw std::runtime_error("dependency manifest digest mismatch");
                jobs::dependency_fill request{.manifest = builds::dependency_manifest(text),
                                              .triplet = input.triplet,
                                              .host_triplet = input.host_triplet,
                                              .features = input.features,
                                              .default_features = input.default_features};
                const auto key = content::hash_of(rfl::json::write(std::tuple{std::string{"dependency.fill"}, toolchain, request})).str();
                if (!filled.insert(key).second) {
                    record_status("same dependencies as an earlier step");
                    continue;
                }
                backlog::entry entry{.key = key,
                                     .project = project,
                                     .configuration = "dependencies",
                                     .operation = "dependencies",
                                     .actor = worlds::execution_key(actor),
                                     .cacheable = false};
                auto run_operation = [request, toolchain, project, actor, worldline, version, snapshot_id = snapshot.id().str()](
                                         services& zero, jobs::progress_log& progress, std::string) -> coroutine::task<std::optional<backlog::outcome>> {
                    config_result out{.configuration = "dependencies"};
                    std::uint64_t admission_id;
                    try {
                        admission_id = co_await zero.files.with(zero.core, project, [](storage::project_ctx& context) {
                            return context.ledger.admit_build(context.settings.project, context.settings.limits);
                        });
                    } catch (const std::exception& error) {
                        out.error = std::string{error.what()}.substr(0, 512);
                        co_return out;
                    }
                    admission held{&zero.files, zero.core, admission_id,
                                   usage::build_event{.project = project,
                                                      .target = "dependencies",
                                                      .workspace_id = zero.snapshot()->server.auth.workspace,
                                                      .worldline = worldline,
                                                      .version = version,
                                                      .snapshot = snapshot_id,
                                                      .operation = "dependencies",
                                                      .actor = worlds::execution_key(actor),
                                                      .error_code = "cancelled"},
                                   std::chrono::steady_clock::now()};
                    std::int32_t exit_code = -1;
                    bool timed_out = false;
                    try {
                        auto run_on_worker = [request, &zero, identity = held.executor](auto& worker, jobs::progress_log* progress,
                                                                                        std::size_t core) -> coroutine::task<jobs::build_report> {
                            identity->select(zero.workers.workers(core).identity(worker));
                            identity->begin();
                            co_return co_await within(zero.workers.io_context().get_scheduler(core), std::chrono::seconds{request.timeout_s} + build_overhead,
                                                      "dependency fill", worker.call(request, progress));
                        };
                        auto report =
                            co_await zero.workers.with_worker(zero.core, routing::selection{.needs = {.toolchain_id = toolchain, .dependency_publisher = true}},
                                                              std::move(run_on_worker), &progress);
                        out.worker = report.worker;
                        out.ok = !report.steps.empty() && report.ok() &&
                                 std::ranges::all_of(report.steps, [](const auto& step) { return step.cache_error.empty(); });
                        if (!report.steps.empty()) {
                            exit_code = report.steps.back().exit_code;
                            timed_out = report.steps.back().timed_out;
                        }
                        if (!out.ok)
                            out.error = "trusted dependency fill failed; private build remains available";
                    } catch (const routing::no_worker& error) {
                        if (error.busy) {
                            held.ended = true;
                            zero.files.post(zero.core, [admission_id](usage::ledger& ledger) { ledger.release(admission_id); });
                            co_return std::nullopt;
                        }
                        out.error = error.what();
                    } catch (const std::exception& error) {
                        out.error = std::string{error.what()}.substr(0, 512);
                    }
                    held.executor->stamp(out);
                    auto event = held.cancelled;
                    held.executor->stamp(event);
                    event.ok = out.ok;
                    event.exit_code = exit_code;
                    event.timed_out = timed_out;
                    event.error_code = out.ok ? "" : "dependency_fill_failed";
                    event.error = out.error;
                    event.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - held.started).count();
                    held.ended = true;
                    zero.files.post(zero.core, [admission_id, event](usage::ledger& ledger) { ledger.record(admission_id, event); });
                    co_return out;
                };
                auto ticket = co_await s.queue->submit(s.core, std::move(entry), std::move(run_operation));
                const auto remaining = std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now());
                auto done = co_await coroutine::deadline(s.workers.io_context().get_scheduler(s.core), remaining,
                                                         follow(s, ticket, [note](const jobs::progress& progress) {
                                                             if (note)
                                                                 (*note)(progress_note(progress));
                                                         }));
                if (!done) {
                    record_status("timeout: waiting for shared dependencies");
                    co_return status;
                }
                const auto& result = std::get<config_result>(*done);
                record_status(result.ok ? "filled" : "failed: " + result.error);
            } catch (const std::exception& error) {
                record_status("skipped: " + std::string{error.what()}.substr(0, 256));
            }
        }
        co_return status;
    }

    // Preparation uses the bound build configuration's worker and hot directory. Source only supplies the pinned files.
    auto run_preparation(services& zero, std::string project, language::request request, std::string tree, worlds::actor actor, std::string toolchain,
                         jobs::progress_log* progress = nullptr) -> coroutine::task<std::optional<backlog::outcome>> {
        language::plan plan;
        try {
            plan = language::prepare(request.config, request.build);
        } catch (const std::invalid_argument& error) {
            throw tool_error(nodes::code::invalid_record, error.what());
        }
        auto line = execution_line(actor, std::optional{request.worldline});
        auto selected_tree = co_await selected(zero, project, line, request.revision, actor);
        content::manifest files;
        for (auto& f : lines::manifest(selected_tree))
            files.files.push_back({f.path, f.digest, f.size, f.executable});
        try {
            language::check(plan, files.files);
        } catch (const std::invalid_argument& error) {
            throw tool_error(nodes::code::invalid_record, error.what());
        }
        auto needs = routing::requirements{.tools = plan.build.needs.tools, .os = plan.build.needs.platform.os, .arch = plan.build.needs.platform.arch};
        if (plan.server.argv.front().find('/') == std::string::npos && !std::ranges::contains(needs.tools, plan.server.argv.front()))
            needs.tools.push_back(plan.server.argv.front());
        needs.toolchain_id = toolchain;
        jobs::build_request build{.project = project,
                                  .tree = std::move(tree),
                                  .steps = plan.build.steps,
                                  .timeout_s = language::timeout(request.build),
                                  .output_limit = language::output_limit(request.build),
                                  .env = plan.build.env,
                                  .max_memory_bytes = request.build.limits.max_memory_bytes,
                                  .max_cpu_seconds = request.build.limits.max_cpu_seconds};
        for (auto& step : plan.build.steps)
            std::ranges::copy(step.outputs, std::back_inserter(build.measure));
        build.survey = jobs::survey{.provider = plan.server.provider,
                                    .argv = plan.server.argv,
                                    .initialization = plan.server.initialization,
                                    .env = plan.server.env,
                                    .seconds = language::timeout(request.build),
                                    .query = request.query};
        auto& registry = zero.files;
        auto& rt = zero.workers.io_context();
        auto& router = zero.workers;
        auto executor = std::make_shared<execution_identity>();
        auto started = std::chrono::steady_clock::now();
        std::uint64_t admission_id;
        try {
            admission_id = co_await zero.files.with(zero.core, project,
                                                    [](storage::project_ctx& c) { return c.ledger.admit_build(c.settings.project, c.settings.limits); });
        } catch (const usage::quota_exceeded& error) {
            throw tool_error(nodes::code::resource_limit, error.what());
        }
        admission held{&zero.files,
                       zero.core,
                       admission_id,
                       usage::build_event{.project = project,
                                          .target = request.config.build_configuration,
                                          .workspace_id = zero.snapshot()->server.auth.workspace,
                                          .worldline = line,
                                          .version = selected_tree.version,
                                          .snapshot = files.id().str(),
                                          .operation = "prepare",
                                          .actor = worlds::execution_key(actor)},
                       started,
                       executor};
        auto record = [&](const language::result& result, const sync_stats& sync) {
            auto event = held.cancelled;
            executor->stamp(event);
            event.ok = result.status == "prepared" && result.analysis == "analyzed";
            event.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            event.blobs_sent = sync.blobs;
            event.bytes_sent = sync.bytes;
            event.error = result.analysis_error.value_or(result.error);
            event.error_code = event.ok || result.status == "failed" ? "" : result.status == "prepared" ? result.analysis_error_code : result.status;
            if (!result.steps.empty()) {
                event.exit_code = result.steps.back().exit_code;
                event.timed_out = result.steps.back().timed_out;
            }
            held.ended = true;
            zero.files.post(zero.core, [id = admission_id, event](usage::ledger& ledger) { ledger.record(id, event); });
        };
        std::optional<std::pair<jobs::build_report, sync_stats>> done;
        std::string failure, failure_code;
        bool stopped = false;
        auto token = co_await coroutine::current_token;
        try {
            auto run_on_worker = [&registry, &rt, &router, executor, project, files, build, line, version = selected_tree.version,
                                  actor](auto& worker, jobs::progress_log* progress,
                                         std::size_t core) -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
                auto identity = router.workers(core).identity(worker);
                executor->select(identity);
                auto result =
                    co_await sync_and_run(rt.get_scheduler(core), registry, core, worker, project, files, build, progress, line, version, actor, executor);
                result.first.worker = std::move(identity.first);
                result.first.worker_id = std::move(identity.second);
                co_return result;
            };
            done = co_await coroutine::under(
                token, zero.workers.with_worker(zero.core, routing::selection{.needs = needs, .tree_key = build.tree}, std::move(run_on_worker), progress));
            stopped = !done;
        } catch (const routing::no_worker& e) {
            if (e.busy) {
                held.ended = true;
                zero.files.post(zero.core, [id = admission_id](usage::ledger& ledger) { ledger.release(id); });
                co_return std::nullopt;
            }
            failure = e.what();
            failure_code = "no_worker";
        } catch (const tool_error& e) {
            failure = e.what();
            failure_code = e.code;
        } catch (const transport::remote_error& e) {
            failure = e.what();
            failure_code = e.code.empty() ? "worker_failed" : e.code;
        } catch (const std::exception& e) {
            failure = e.what();
            failure_code = executor->started() ? "outcome_unknown" : "sync_failed";
        }
        if (stopped)
            co_await coroutine::stop;
        if (!done) {
            language::result failed{.status = failure_code, .revision = selected_tree.version, .snapshot = files.id(), .error = failure};
            executor->stamp(failed);
            record(failed, {});
            co_return failed;
        }
        auto& report = done->first;
        auto result = prepared(plan, files, selected_tree.version, report);
        result.worldline = line;
        result.timings.sync_seconds = done->second.seconds;
        result.timings.worker_seconds = report.seconds;
        result.timings.preparation_seconds = report.preparation_seconds;
        result.timings.execution_seconds = report.execution_seconds;
        if (report.survey_error) {
            result.analysis = "failed";
            result.analysis_error = report.survey_error;
            result.analysis_error_code = report.survey_error_code;
        } else if (report.ok() && report.outline) {
            result.analysis = "analyzed";
            result.outline = std::move(report.outline);
            result.locations = std::move(report.locations);
        } else {
            result.analysis = "failed";
            result.analysis_error = result.error.empty() ? "language worker produced no result" : result.error;
        }
        record(result, done->second);
        co_return result;
    }

    // Each caller owns a waiting ticket; preparation has no durable receipt or separate user operation.
    auto prepare(services& s, mcp::call_context context, std::string project, std::string configuration, language::request request, const worlds::actor& actor,
                 mcp::notifier* note = nullptr, semantic::query requested = {}) -> coroutine::task<language::result> {
        if (request.worldline.empty())
            throw tool_error("context_required", "worldline is required");
        auto source = co_await selected(s, project, request.worldline, request.revision, actor);
        request.revision = source.version;
        auto plan = language::prepare(request.config, request.build);
        auto needs = routing::requirements{.tools = plan.build.needs.tools, .os = plan.build.needs.platform.os, .arch = plan.build.needs.platform.arch};
        if (plan.server.argv.front().find('/') == std::string::npos && !std::ranges::contains(needs.tools, plan.server.argv.front()))
            needs.tools.push_back(plan.server.argv.front());
        auto trees = hot_key(project, request.worldline, request.config.build_configuration);
        content::manifest manifest;
        for (const auto& file : source.files)
            manifest.files.push_back({file.path, file.digest, file.size, file.executable});
        invocation observed{s.files,
                            s.core,
                            {.project = project,
                             .configuration = configuration,
                             .operation = requested.action,
                             .actor = worlds::execution_key(actor),
                             .worldline = request.worldline,
                             .version = source.version,
                             .snapshot = manifest.id().str(),
                             .reuse = "none",
                             .parameters = rfl::json::write(requested)}};
        observed.event.timings.client_sync_seconds = context.client_sync_seconds;
        std::string toolchain;
        try {
            toolchain = co_await select_toolchain(s, needs, trees);
        } catch (const tool_error&) {
            observed.event.state = "failed";
            throw;
        }
        auto normalized = request;
        normalized.worldline.clear();
        normalized.revision.clear();
        normalized.config.build_configuration.clear();
        auto key = content::hash_of(rfl::json::write(std::tuple{std::string{"language"}, manifest.id().str(), toolchain, normalized})).str();
        const auto dependency_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{language::timeout(request.build)};
        const auto dependency_started = std::chrono::steady_clock::now();
        co_await fill_dependencies(s, project, plan.build.steps, manifest, toolchain, actor, request.worldline, source.version, dependency_deadline, note);
        const auto dependency_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - dependency_started).count();
        observed.event.timings.dependency_seconds = dependency_seconds;
        const auto left = std::chrono::ceil<std::chrono::seconds>(dependency_deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            throw tool_error("timeout", "execution deadline exceeded while filling dependencies");
        request.build.limits.timeout_s = static_cast<std::uint32_t>(left);
        backlog::entry what{.key = key,
                            .project = project,
                            .configuration = configuration,
                            .operation = "prepare",
                            .actor = worlds::execution_key(actor),
                            .trees = trees,
                            .cacheable = true,
                            .worldline = request.worldline};
        for (auto& f : source.files)
            what.files.push_back(f.path + '\n' + f.digest);
        auto held = co_await s.queue->submit(s.core, std::move(what),
                                             [project, request, actor, toolchain](services& zero, jobs::progress_log& progress, std::string tree) {
                                                 return run_preparation(zero, project, request, std::move(tree), actor, toolchain, &progress);
                                             });
        observed.event.calculation = key;
        observed.event.reuse = held->reuse();
        auto done = co_await follow(s, held, [note](const jobs::progress& progress) {
            if (note)
                (*note)(progress_note(progress));
        });
        if (auto* refused = std::get_if<config_result>(&done)) {
            auto timings = refused->timings;
            timings.client_sync_seconds = context.client_sync_seconds;
            observed.event.timings = timings;
            observed.event.timings.dependency_seconds = dependency_seconds;
            observed.event.state = "failed";
            throw tool_error(refused->error_code, refused->error);
        }
        auto result = std::get<language::result>(std::move(done));
        result.timings.dependency_seconds = dependency_seconds;
        result.timings.client_sync_seconds = context.client_sync_seconds;
        observed.event.timings = result.timings;
        observed.event.state = result.status == "prepared" && result.analysis == "analyzed" ? "succeeded" : "failed";
        result.worldline = request.worldline;
        result.revision = source.version;
        result.snapshot = manifest.id();
        co_return result;
    }

    // One configuration of a build, test or run: queued in the backlog, waited for here, its progress passed on as it comes.
    auto execute(services& s, mcp::call_context context, std::string project, std::string operation, bool cacheable, bool fresh,
                 config::effective_config config, std::vector<jobs::step> steps, content::manifest snapshot, mcp::notifier& note, config_result& r,
                 std::string worldline, std::string version, worlds::actor actor) -> coroutine::task<> {
        r.timings.client_sync_seconds = context.client_sync_seconds;
        jobs::build_request request{.project = project,
                                    .steps = steps,
                                    .timeout_s = config.limits.timeout_s,
                                    .output_limit = config.limits.output_limit,
                                    .env = config.env,
                                    .max_memory_bytes = config.limits.max_memory_bytes,
                                    .max_cpu_seconds = config.limits.max_cpu_seconds,
                                    .fresh = fresh};
        request.exports = config.build.operations.at(operation).exports;
        auto trees = hot_key(project, worldline, config.name);
        auto required = config.needs.get().value_or(config::requirements{});
        auto needs = routing::requirements{.tools = required.tools, .os = required.platform.os, .arch = required.platform.arch};
        invocation observed{s.files,
                            s.core,
                            {.project = project,
                             .configuration = config.name,
                             .operation = operation,
                             .actor = worlds::execution_key(actor),
                             .worldline = worldline,
                             .version = version,
                             .snapshot = snapshot.id().str(),
                             .reuse = "none"}};
        observed.event.timings.client_sync_seconds = context.client_sync_seconds;
        std::string toolchain;
        try {
            toolchain = co_await select_toolchain(s, needs, trees);
        } catch (const tool_error& error) {
            observed.event.state = "failed";
            r.configuration = config.name;
            r.snapshot = version;
            r.error = error.what();
            r.error_code = error.code;
            co_return;
        }
        auto normalized = request;
        normalized.project.clear();
        normalized.tree.clear();
        normalized.fresh = false;
        auto key = content::hash_of(rfl::json::write(std::tuple{operation, snapshot.id().str(), toolchain, normalized, config.needs})).str();
        // Test and run operations are effects: every call gets its own execution.
        static std::atomic<std::uint64_t> calls{0};
        if (!cacheable)
            key += "/call/" + std::to_string(calls.fetch_add(1));
        const auto dependency_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{request.timeout_s};
        const auto dependency_started = std::chrono::steady_clock::now();
        const auto dependency_status =
            co_await fill_dependencies(s, project, steps, snapshot, toolchain, actor, worldline, version, dependency_deadline, &note);
        const auto dependency_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - dependency_started).count();
        observed.event.timings.dependency_seconds = dependency_seconds;
        const auto left = std::chrono::ceil<std::chrono::seconds>(dependency_deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            r.timings.dependency_seconds = dependency_seconds;
            r.configuration = config.name;
            r.snapshot = version;
            r.dependency_cache = dependency_status;
            r.error = "execution deadline exceeded while filling dependencies";
            r.error_code = "timeout";
            observed.event.state = "failed";
            co_return;
        }
        request.timeout_s = static_cast<std::uint32_t>(left);
        auto executor = std::make_shared<execution_identity>();
        backlog::entry what{.key = key,
                            .project = project,
                            .configuration = config.name,
                            .operation = operation,
                            .actor = worlds::execution_key(actor),
                            .trees = trees,
                            .cacheable = cacheable,
                            .fresh = fresh,
                            .workspace_id = s.snapshot()->server.auth.workspace,
                            .worldline = worldline,
                            .version = version,
                            .executor = executor};
        for (auto& f : snapshot.files)
            what.files.push_back(f.path + '\n' + f.digest.str());
        auto held = co_await s.queue->submit(s.core, std::move(what),
                                             [project, config, steps, snapshot, request, worldline, version, actor, operation, executor,
                                              toolchain](services& zero, jobs::progress_log& progress, std::string tree) {
                                                 auto placed = request;
                                                 placed.tree = std::move(tree);
                                                 return dispatch(zero, project, config, steps, snapshot, std::move(placed), worldline, version, actor, progress,
                                                                 operation, executor, toolchain);
                                             });
        observed.event.calculation = key;
        observed.event.reuse = held->reuse();
        // A joined job or a cached result may come from an equal configuration of another name.
        r = std::get<config_result>(co_await follow(s, held, [&note, &config](const jobs::progress& p) {
            auto named = p;
            named.stage = std::format("{}: {}", config.name, p.stage);
            note(progress_note(named));
        }));
        r.dependency_cache = dependency_status;
        r.timings.dependency_seconds = dependency_seconds;
        r.timings.client_sync_seconds = context.client_sync_seconds;
        observed.event.timings = r.timings;
        observed.event.state = r.ok ? "succeeded" : "failed";
        r.configuration = config.name;
        r.snapshot = version;
        auto downloads = r.exports;
        for (const auto& step : r.steps)
            if (step.log)
                downloads.push_back(*step.log);
        auto granted = co_await s.files.grant_exports(s.core, project, std::move(downloads));
        std::size_t index = 0;
        for (auto& file : r.exports)
            file = std::move(granted.at(index++));
        for (auto& step : r.steps)
            if (step.log)
                *step.log = std::move(granted.at(index++));
        if (std::ranges::any_of(r.exports, [](const auto& file) { return !file.delivery_error.empty(); }) ||
            std::ranges::any_of(r.steps, [](const auto& step) { return step.log && !step.log->delivery_error.empty(); }))
            co_await s.queue->forget(s.core, key);
    }

    // Builds run one fixed version of the selected task worldline.
    // Only the build tool's results are cached; `fresh` rebuilds from a clean tree.
    auto execute_all(services& s, mcp::call_context context, std::string project, std::string operation, bool cacheable, bool fresh,
                     std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs, mcp::notifier& note, std::string worldline,
                     std::string version, const worlds::actor& actor) -> coroutine::task<results> {
        results out;
        out.results.resize(jobs.size());
        if (worldline.empty())
            throw tool_error("context_required", "worldline is required");
        auto tree = co_await selected(s, project, worldline, version, actor);
        content::manifest snapshot;
        for (auto& f : lines::manifest(tree))
            snapshot.files.push_back({f.path, f.digest, f.size, f.executable});
        out.worldline = worldline;
        out.revision = tree.version;
        out.snapshot = tree.version;
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            for (std::size_t i = 0; i < jobs.size(); ++i)
                children.spawn(execute(s, context, project, operation, cacheable, fresh, jobs[i].first, jobs[i].second, snapshot, note, out.results[i],
                                       worldline, tree.version, actor));
            co_await children.join();
        };
        co_await coroutine::with_scope(std::move(run_children));
        out.ok = std::ranges::all_of(out.results, &config_result::ok);
        co_return out;
    }

    struct configurations : tool<"configurations", "List build and run configurations and the operations each build configuration provides."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
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
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
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

    struct build : tool<"build", "Build a fixed server-side worldline version with one or more configurations: send its files "
                                 "to an eligible worker (matching each configuration's requirements), "
                                 "execute its build steps, and return each configuration's "
                                 "steps, output and diagnostics in one result."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"Configurations to build at once; the defaults if absent; [\"*\"] for all enabled", std::optional<std::vector<std::string>>>
                configurations;
            rfl::Description<"Targets accepted by the selected build operation; configured targets if absent", std::optional<std::vector<std::string>>> targets;
            rfl::Description<"Worldline to build; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
            rfl::Description<"Rebuild from a clean working tree, bypassing cached and in-flight build results", std::optional<bool>> fresh;
        };
        using output = results;

        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            auto targets = in.targets.value().value_or(std::vector<std::string>{});
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(in.configurations.value().value_or(std::vector<std::string>{}), "build"))
                jobs.push_back(detail::operation(*c, {.operation = "build", .parameters = {.targets = targets}}));
            co_return co_await execute_all(s, context, project, "build", true, in.fresh.value().value_or(false), std::move(jobs), note,
                                           execution_line(actor, in.worldline.value()), in.version.value().value_or(""), actor);
        }
    };

    struct test : tool<"test", "Execute the test operation and its configured dependencies on one or more configurations; "
                               "returns steps, diagnostics and output, plus a test summary when the test runner is one the "
                               "service reads (CTest, Bazel, pytest)."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"Configurations to test on at once; the defaults if absent; [\"*\"] for all enabled", std::optional<std::vector<std::string>>>
                configurations;
            rfl::Description<"Test filter, if the selected operation accepts one", std::optional<std::string>> filter;
            rfl::Description<"Test labels, if the selected operation accepts them", std::optional<std::string>> labels;
            rfl::Description<"Worldline to build; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
        };
        using output = results;

        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto e = config::resolve(*s.snapshot(), project);
            std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs;
            for (auto* c : e.pick(in.configurations.value().value_or(std::vector<std::string>{}), "test")) {
                jobs.push_back(detail::operation(*c, {.operation = "test", .parameters = {.filter = in.filter.value(), .labels = in.labels.value()}}));
            }
            co_return co_await execute_all(s, context, project, "test", false, false, std::move(jobs), note, execution_line(actor, in.worldline.value()),
                                           in.version.value().value_or(""), actor);
        }
    };

    struct launch : tool<"run", "Invoke a saved run configuration's operation on its selected build configurations."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"The run configuration", std::string> configuration;
            rfl::Description<"Build configurations to run on; the run configuration's own if absent", std::optional<std::vector<std::string>>> configurations;
            rfl::Description<"Worldline to build; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"Version of the worldline to build; its current one when absent", std::optional<std::string>> version;
        };
        using output = results;

        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
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
            co_return co_await execute_all(s, context, project, r.spec.operation, false, false, std::move(jobs), note,
                                           execution_line(actor, in.worldline.value()), in.version.value().value_or(""), actor);
        }
    };

    struct language_query : tool<"language", "Query scopes, symbols, definitions, references or diagnostics for a worldline version. "
                                             "Analysis is prepared automatically within the call when missing or stale."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"Worldline; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            rfl::Description<"Version to query; the current one when absent", std::optional<std::string>> version;
            rfl::Description<"Analysis configuration; the project's only one when absent", std::optional<std::string>> configuration;
            rfl::Description<"scopes, symbols, definitions, references or diagnostics (default)", std::optional<std::string>> action;
            rfl::Description<"File path; required for definitions and references", std::optional<std::string>> path;
            rfl::Description<"Symbol name; a prefix for symbols, an exact name for definitions/references", std::optional<std::string>> symbol;
            rfl::Description<"One-based source line to disambiguate a symbol", std::optional<std::uint64_t>> line;
        };
        struct output {
            builds::timings timings;
            std::string configuration, worldline;
            std::optional<std::string> environment, version;
            bool stale = false;
            bool analyzed = false;
            std::optional<std::vector<semantic::scope>> scopes;
            std::optional<std::vector<semantic::symbol>> symbols;
            std::optional<std::vector<semantic::location>> definitions, references;
            std::optional<std::vector<semantic::diagnostic>> diagnostics;
        };
        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in);
            auto settings = config::resolve(*s.snapshot(), project);
            auto action = in.action.value().value_or("diagnostics");
            auto name = in.configuration.value().value_or("");
            if (name.empty()) {
                if (settings.analysis.size() != 1)
                    throw tool_error(nodes::code::invalid_record,
                                     "name the analysis configuration; this project has " + std::to_string(settings.analysis.size()));
                name = settings.analysis.begin()->first;
            }
            auto found = settings.analysis.find(name);
            if (found == settings.analysis.end())
                throw tool_error(nodes::code::invalid_record, "analysis configuration not found: " + name);
            output out{.configuration = name};
            auto line = execution_line(actor, in.worldline.value());
            out.worldline = line;
            static const std::set<std::string> queries{"scopes", "symbols", "definitions", "references", "diagnostics"};
            if (!queries.contains(action))
                throw tool_error(nodes::code::invalid_record, "unknown language action " + action);
            auto bound = settings.config(found->second.build_configuration).build;
            out.environment = language::fingerprint(found->second, bound).str();
            semantic::query query{.action = action, .path = in.path.value().value_or(""), .symbol = in.symbol.value().value_or(""), .line = in.line.value()};
            if (!query.path.empty())
                content::checked_path(query.path);
            if ((action == "definitions" || action == "references") && (query.path.empty() || query.symbol.empty()))
                throw tool_error(nodes::code::invalid_record, "definitions and references require path and symbol");
            if (query.line && !*query.line)
                throw tool_error(nodes::code::invalid_record, "line must be positive");
            auto calculation = action == "scopes" || action == "symbols" || action == "diagnostics" ? semantic::query{} : query;
            auto prepared = co_await prepare(
                s, context, project, name,
                {.worldline = line, .revision = in.version.value().value_or(""), .config = found->second, .build = bound, .query = calculation}, actor, &note,
                query);
            if (prepared.status != "prepared" || prepared.analysis != "analyzed" || !prepared.outline)
                throw tool_error(prepared.status == "prepared" || prepared.status == "failed" ? prepared.analysis_error_code : prepared.status,
                                 prepared.analysis_error.value_or(prepared.error));
            out.timings = prepared.timings;
            out.version = prepared.revision;
            out.analyzed = true;
            auto& report = *prepared.outline;
            out.stale = !report.complete;
            if (action == "scopes") {
                if (out.stale)
                    throw tool_error("scope_stale", "scope references require complete analysis of the selected version");
                std::erase_if(report.scopes,
                              [&](const auto& scope) { return !query.path.empty() && (!scope.range.place || scope.range.place->path != query.path); });
                nodes::node_request register_scopes{.action = "scope_register", .worldline = line, .revision = prepared.revision};
                register_scopes.configuration = name;
                register_scopes.environment = *out.environment;
                register_scopes.analyzed_scopes = std::move(report.scopes);
                auto registered = co_await node_call(s, project, std::move(register_scopes), actor);
                out.scopes = std::move(registered.scopes);
            } else if (action == "symbols") {
                std::erase_if(report.symbols, [&](const auto& value) {
                    if (!query.symbol.empty() && !value.name.starts_with(query.symbol))
                        return true;
                    return !query.path.empty() &&
                           !std::ranges::any_of(value.definitions, [&](const auto& span) { return span.place && span.place->path == query.path; });
                });
                out.symbols = std::move(report.symbols);
            } else if (action == "definitions")
                out.definitions = std::move(prepared.locations);
            else if (action == "references")
                out.references = std::move(prepared.locations);
            else {
                if (!query.path.empty())
                    std::erase_if(report.diagnostics, [&](const auto& value) { return !value.range.place || value.range.place->path != query.path; });
                out.diagnostics = std::move(report.diagnostics);
            }
            co_return out;
        }
    };

    struct generation_result {
        templates::plan preview;
        std::optional<mutation> receipt;
    };
    // Generated files are created in main in one observed batch; existing files are never overwritten.
    auto generate_files(services& s, const std::string& project, templates::parameters parameters, const worlds::actor& actor, std::string operation_id,
                        bool preview) -> coroutine::task<generation_result> {
        generation_result out{templates::render(parameters)};
        if (preview)
            co_return out;
        if (operation_id.empty())
            throw tool_error(nodes::code::invalid_record, "generation names an operation_id");
        std::string input = content::hash_of(rfl::json::write(parameters)).str();
        auto apply_once = [&]() -> coroutine::task<mutation> {
            auto observed = co_await main_tree(s, project, actor);
            std::vector<change> changes;
            for (const auto& file : out.preview.files) {
                if (std::ranges::contains(observed.files, file.path, &nodes::file_view::path))
                    throw tool_error(nodes::code::source_conflict, file.path + " already exists");
                changes.push_back({.kind = "create", .path = file.path, .text = file.text});
            }
            auto r = co_await main_apply(s, project, actor, observed, std::move(changes), operation_id, "generate " + parameters.template_id,
                                         rfl::json::write(record{input}));
            co_return mutation{r.version, r.receipt, operation_id, r.replayed};
        };
        out.receipt = co_await once<mutation>(s, project, actor, operation_id, input, std::move(apply_once),
                                              [&](const auto& done) { return mutation{done.first.version, done.first.receipt, operation_id, true}; });
        co_return out;
    }
    // Console generation publishes one observed batch. There is purposely no MCP
    // `generate` tool: agents write files with the file tools, and the console's
    // generation flow goes through this REST helper only.
    auto generate_globally(services& s, const std::string& project, templates::parameters parameters, const worlds::actor& actor, std::string operation_id)
        -> coroutine::task<generation_result> {
        co_return co_await generate_files(s, project, std::move(parameters), actor, std::move(operation_id), false);
    }

    struct scope : tool<"scope", "Acquire an atomic set of exclusive scope leases using references returned by language(scopes). Pass tokens to "
                                 "worldline(apply); renew or release each token. Conflicts return immediately."> {
        struct input {
            rfl::Description<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            rfl::Description<"acquire, renew, release or list", std::string> action;
            rfl::Description<"Task worldline; explicit selection required without execution context", std::optional<std::string>> worldline;
            rfl::Description<"acquire: scope references returned by language", std::optional<std::vector<std::string>>> scopes;
            rfl::Description<"acquire: whole files, including empty and binary files", std::optional<std::vector<std::string>>> files;
            rfl::Description<"acquire: path entries for create/remove/move", std::optional<std::vector<std::string>>> entries;
            rfl::Description<"acquire with files/entries: observed version", std::optional<std::string>> version;
            rfl::Description<"acquire: unique operation ID", std::optional<std::string>> operation_id;
            rfl::Description<"renew/release: lease token", std::optional<std::string>> token;
            rfl::Description<"Lease duration, 1 through 600 seconds; default 120", std::optional<std::uint64_t>> seconds;
        };
        struct output {
            std::string worldline, version;
            std::vector<nodes::scope_lease> leases;
        };
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            auto project = project_of(in), action = in.action.value();
            if (action != "acquire" && action != "renew" && action != "release" && action != "list")
                throw tool_error(nodes::code::invalid_record, "unknown scope action");
            auto line = execution_line(actor, in.worldline.value());
            nodes::node_request request{.action = "scope_" + action, .worldline = line};
            request.lease_token = in.token.value().value_or("");
            request.lease_seconds = in.seconds.value().value_or(constants::scope_lease_seconds);
            if (action == "acquire") {
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "acquire requires operation_id");
                request.scope_refs = in.scopes.value().value_or(std::vector<std::string>{});
                request.scope_files = in.files.value().value_or(std::vector<std::string>{});
                request.scope_entries = in.entries.value().value_or(std::vector<std::string>{});
                request.revision = in.version.value().value_or("");
                request.operation = lines::operation_of(*in.operation_id.value());
            }
            auto result = co_await node_call(s, project, std::move(request), actor, action == "list");
            co_return output{line, result.version, std::move(result.leases)};
        }
    };

    struct artifact : tool<"artifact", "Retrieve a build/test/run export or execution log by its project and digest. "
                                       "Use the export metadata returned by execution tools. Read bounded base64 chunks, "
                                       "decode them in offset order into a local file, and preserve its executable flag. "
                                       "Continue at next_offset until eof; no source synchronization is required."> {
        struct input {
            rfl::Description<"Project that produced the artifact; supplied by execution context when absent", std::optional<std::string>> project;
            rfl::Description<"Digest returned in exports or a step log", std::string> digest;
            rfl::Description<"Byte offset; defaults to zero", std::optional<std::uint64_t>> offset;
            rfl::Description<"Bytes to retrieve, 1 through 65536; defaults to 65536", std::optional<std::uint64_t>> length;
        };
        struct output {
            std::string digest;
            std::uint64_t offset, total, next_offset;
            bool eof;
            std::string data_base64;
        };
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor&) -> coroutine::task<output> {
            auto project = project_of(in);
            auto length = in.length.value().value_or(constants::mcp_artifact_bytes);
            if (!length || length > constants::mcp_artifact_bytes)
                throw tool_error(nodes::code::invalid_record, "artifact length must be 1 through 65536 bytes");
            auto bytes = co_await s.files.read_export(s.core, project, {in.digest.value(), in.offset.value().value_or(0), length});
            const auto& part = bytes.parts.at(0);
            const auto next = part.offset + part.bytes.size();
            co_return output{part.digest.str(),  part.offset,
                             part.total,         next,
                             next == part.total, lines::base64({reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()})};
        }
    };

    struct task : tool<"task", "Create a task on an explicit worldline, fork a task worldline, start an execution, stop a run or close a task. Only the "
                               "orchestrator's account credential can administer tasks."> {
        struct input {
            rfl::Description<"Project", std::string> project;
            rfl::Description<"create, fork, start, stop or close", std::string> action;
            rfl::Description<"Task identifier", std::string> task;
            rfl::Description<"create/fork: task worldline", std::optional<std::string>> worldline;
            rfl::Description<"fork: source worldline", std::optional<std::string>> from;
            rfl::Description<"fork: source version", std::optional<std::string>> version;
            rfl::Description<"create/fork/start: unique operation ID", std::optional<std::string>> operation_id;
            rfl::Description<"stop: execution identifier", std::optional<std::string>> run;
            rfl::Description<"start: permit source writes; default true", std::optional<bool>> writable;
        };
        struct output {
            std::string task, worldline;
            std::optional<std::string> run, credential;
            bool active = true;
        };
        static auto run(services& s, mcp::call_context, input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<output> {
            if (!actor.run.empty())
                throw tool_error(nodes::code::permission_denied, "execution credentials cannot administer tasks");
            auto project = project_of(in), action = in.action.value();
            config::check_name("task", in.task.value());
            if (action != "create" && action != "fork" && action != "start" && action != "stop" && action != "close")
                throw tool_error(nodes::code::invalid_record, "unknown task action");
            nodes::node_request request{.action = "task_" + action};
            request.task_id = in.task.value();
            request.run_id = in.run.value().value_or("");
            request.execution_writable = in.writable.value().value_or(true);
            if (action == "create" || action == "fork" || action == "start") {
                if (!in.operation_id.value())
                    throw tool_error(nodes::code::invalid_record, "operation_id is required");
                request.operation = lines::operation_of(*in.operation_id.value());
            }
            if (action == "create" || action == "fork") {
                if (!in.worldline.value() || in.worldline.value()->empty())
                    throw tool_error("context_required", "pass a task worldline");
                request.worldline = *in.worldline.value();
            }
            if (action == "fork") {
                if (!in.from.value() || !in.version.value())
                    throw tool_error(nodes::code::invalid_record, "fork requires from and version");
                auto base = co_await selected(s, project, *in.from.value(), *in.version.value(), actor);
                request.action = "fork";
                request.source = base.frontier;
                request.result = rfl::json::write(std::tuple{in.task.value(), *in.from.value(), *in.version.value()});
            }
            auto result = co_await node_call(s, project, std::move(request), actor);
            if (!result.execution)
                co_return output{in.task.value(), result.worldline};
            const auto& e = *result.execution;
            co_return output{e.task, e.worldline, e.run.empty() ? std::nullopt : std::optional{e.run},
                             e.token.empty() ? std::nullopt : std::optional{"fugrun." + project + "." + e.token}, e.active};
        }
    };

    using standard = toolbox<task, configurations, worldline, scope, build, test, launch, language_query, artifact>;
} // namespace tools
