export module tools:worldlines;
import utility;
import constants;
import std;
import knowledge;
import json;
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
export import publication;
import native;

export import :common;

export namespace tools {
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
        auto reply = co_await node_call(s, project, {.action = "task_lookup", .execution_token = token}, worlds::service_actor("execution"), true);
        if (!reply.execution)
            throw tool_error("unauthenticated", "execution not found");
        auto identity = json::read<worlds::actor>(reply.execution->identity);
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
        if (actor.dependency) {
            if (selected && *selected != actor.dependency->worldline)
                throw tool_error(nodes::code::permission_denied, "worldline differs from dependency read grant");
            return actor.dependency->worldline;
        }
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

    using change_kind = json::literal<"edit", "replace", "write", "create", "remove", "move", "mode">;
    struct change {
        json::described<"edit, replace, write, create, remove, move or mode", change_kind> kind;
        json::described<"Path relative to the project root", std::string> path;
        json::described<"edit: first Unicode scalar to replace, in the observed version; replace: first scalar of the search window",
                         std::optional<std::uint64_t>>
            offset;
        json::described<"edit: number of Unicode scalars to replace (0 inserts); replace: length of the search window", std::optional<std::uint64_t>> count;
        json::described<"replace: text that occurs exactly once in the window", std::optional<std::string>> old_text;
        json::described<"replace: its replacement", std::optional<std::string>> new_text;
        json::described<"replace: a scope reference from language; the window is that structure in the observed version", std::optional<std::string>> scope;
        json::described<"edit/write/create: new text", std::optional<std::string>> text;
        json::described<"write/create: new bytes, base64", std::optional<std::string>> data_base64;
        json::described<"create/mode: executable flag", std::optional<bool>> executable;
        json::described<"move: new path", std::optional<std::string>> destination;
    };

    // The byte where a text's scalar at `scalar` begins, or its size past the end.
    auto byte_of(std::string_view text, std::uint64_t scalar) -> std::size_t {
        std::size_t at = 0;
        for (; at < text.size(); ++at)
            if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && scalar-- == 0)
                return at;
        return text.size();
    }

    // Engine edits for one batch. A whole new content is checked against the file limit here; the node checks the state it publishes.
    // `located` holds the observed range of each scope a replace names.
    auto edits_of(const std::vector<change>& changes, const nodes::node_reply& observed, const config::effective_limits& limits,
                  const std::map<std::string, semantic::place>& located = {}) -> std::vector<nodes::edit> {
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
            if (kind == change_kind::make<"edit">()) {
                if (!c.offset.value() || !c.text.value())
                    throw tool_error(nodes::code::invalid_record, "an edit names offset and text");
                out.push_back(
                    {.kind = nodes::edit_kind::edit, .path = path, .text = *c.text.value(), .offset = *c.offset.value(), .count = c.count.value().value_or(0)});
            } else if (kind == change_kind::make<"replace">()) {
                if (!c.old_text.value() || c.old_text.value()->empty() || !c.new_text.value())
                    throw tool_error(nodes::code::invalid_record, "a replace names a non-empty old_text and new_text");
                if (c.scope.value() && (c.offset.value() || c.count.value()))
                    throw tool_error(nodes::code::invalid_record, "a replace window is a scope or offset and count, not both");
                if (found == files.end() || found->second->kind != "text")
                    throw tool_error(nodes::code::invalid_record, path + " is not a text file in the observed version");
                const std::string_view text = found->second->text;
                std::uint64_t first = c.offset.value().value_or(0), last = std::numeric_limits<std::uint64_t>::max();
                if (c.count.value())
                    last = first + std::min(*c.count.value(), last - first);
                if (c.scope.value()) {
                    auto range = located.find(*c.scope.value());
                    if (range == located.end())
                        throw tool_error(nodes::code::invalid_record, "replace scopes are resolved by worldline apply");
                    if (range->second.path != path)
                        throw tool_error(nodes::code::invalid_record, "scope " + *c.scope.value() + " is in " + range->second.path + ", not " + path);
                    first = range->second.begin;
                    last = range->second.end;
                }
                const auto begin = byte_of(text, first), end = std::max(begin, byte_of(text, last));
                const std::string_view window = text.substr(begin, end - begin), needle = *c.old_text.value();
                auto at = window.find(needle);
                if (at == std::string_view::npos)
                    throw tool_error(nodes::code::invalid_record, "old_text does not occur in the window of " + path);
                if (window.find(needle, at + 1) != std::string_view::npos)
                    throw tool_error(nodes::code::invalid_record,
                                     "old_text occurs more than once in the window of " + path + "; narrow it with scope or offset");
                out.push_back({.kind = nodes::edit_kind::edit,
                               .path = path,
                               .text = *c.new_text.value(),
                               .offset = lines::scalars(text.substr(0, begin + at)),
                               .count = lines::scalars(needle)});
            } else if (kind == change_kind::make<"write">() || kind == change_kind::make<"create">()) {
                auto bytes = content();
                bool text = lines::textual(bytes);
                if (kind == change_kind::make<"create">())
                    out.push_back({.kind = text ? nodes::edit_kind::create : nodes::edit_kind::binary_create,
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
                    out.push_back({.kind = text ? nodes::edit_kind::write : nodes::edit_kind::binary, .path = path, .text = text ? bytes : lines::hex(bytes)});
                }
            } else if (kind == change_kind::make<"remove">())
                out.push_back({.kind = nodes::edit_kind::remove, .path = path});
            else if (kind == change_kind::make<"move">()) {
                if (!c.destination.value())
                    throw tool_error(nodes::code::invalid_record, "a move names its destination");
                out.push_back({.kind = nodes::edit_kind::move, .path = path, .target = *c.destination.value()});
            } else if (kind == change_kind::make<"mode">()) {
                if (!c.executable.value())
                    throw tool_error(nodes::code::invalid_record, "a mode change names executable");
                out.push_back({.kind = nodes::edit_kind::mode, .path = path, .executable = *c.executable.value()});
            }
        }
        return out;
    }

    // A retried file tool answers from its receipt: recomputing the change on the moved worldline would be another input.
    // What a file tool records with its batch: the digest of the tool input it was asked for, and its count of replacements.
    struct record {
        std::string input;
        std::uint64_t replacements = 0;
        std::optional<std::string> observed;
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
        auto kept = json::read<record>(r.result);
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
            failed = util::capture_current_exception();
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
        : tool<"worldline", "Work in an explicit worldline: list, inspect, read, compare, diff, promote, fork, apply, receipt, history, reverse or redo. "
                            "Concurrent edits within a worldline converge; worldlines never merge. Writes name the observed version; "
                            "a replace change swaps old_text, unique within the file, an offset window or a language scope reference, "
                            "for new_text without offsets. An apply without leases locks the nearest enclosing registered or syntax scope, "
                            "falling back to the edited text. Reach is body_only for registered function bodies, syntax_body_only for syntax "
                            "estimates, ffi_boundary_touched for changes to a registered FFI boundary, or declaration_touched. "
                            "For syntax_body_only, use semantic analysis to check callers because body edits can change inferred types or callable behavior. "
                            "Repeat an operation_id with identical input to recover its recorded result."> {
        struct input {
            json::described<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            json::described<"list, inspect, read, compare, diff, promote, fork, apply, receipt, history, contribution, reference, references, "
                             "reference_history, reverse, redo, export or restore",
                             std::string>
                action;
            json::described<"Worldline name; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            json::described<"Selected version or @tag/channel; diff/promote: source version (required for promote)", std::optional<std::string>> version;
            json::described<"apply/reverse/redo/compare: observed version; diff/promote: target version (required for promote)", std::optional<std::string>>
                observed;
            json::described<"read: first Unicode scalar to return in a text file", std::optional<std::uint64_t>> offset;
            json::described<"read: Unicode scalars to return; bounded by the project read_max_bytes", std::optional<std::uint64_t>> count;
            json::described<"read: omit content when this digest is unchanged", std::optional<std::string>> known_digest;
            json::described<"apply/promote/reverse/redo: explicit scope lease tokens that must cover the edits; omit them to lock implicitly",
                             std::optional<std::vector<std::string>>>
                leases;
            json::described<"read: file path", std::optional<std::string>> path;
            json::described<"fork/restore: name of the new worldline", std::optional<std::string>> name;
            json::described<"fork/diff/promote: source worldline; diff/promote compare or copy into worldline", std::optional<std::string>> from;
            json::described<"apply/promote/fork/reverse/redo/restore: caller-chosen ID of this operation", std::optional<std::string>> operation_id;
            json::described<"apply/promote/fork/reverse/redo: what this change is for", std::optional<std::string>> intent;
            json::described<"apply: one atomic batch", std::optional<std::vector<change>>> changes;
            json::described<"export: replace a package of that name", std::optional<bool>> force;
            json::described<"export/restore: package name, shared by the workspace's projects", std::optional<std::string>> package;
            json::described<"reverse/redo/contribution: the history entry to undo, restore or describe", std::optional<std::string>> target;
            json::described<"history: page size", std::optional<std::uint64_t>> limit;
            json::described<"history: the last entry of the previous page", std::optional<std::string>> cursor;
            json::described<"reference: name, tag/channel kind, expected prior receipt and build/test/review attestations",
                             std::optional<nodes::reference_input>>
                reference;
            json::described<"references/reference_history: last sequence of the previous page", std::optional<std::uint64_t>> after;
        };
        struct line_entry {
            std::string name, version;
            std::optional<std::string> base_version, created_by;
        };
        struct history_entry {
            std::string id, operation_id, actor, intent, tool;
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
            // Apply reach uses the observed revision. syntax_body_only leaves caller impact open.
            std::optional<std::string> reach;
            std::optional<std::uint64_t> size, offset, count, total_scalars, next_offset;
            std::optional<bool> executable, replayed, truncated, unchanged;
            std::optional<std::vector<line_entry>> worldlines;
            std::optional<std::vector<lines::entry>> files;
            std::optional<std::vector<std::string>> conflicts;
            std::optional<std::vector<history_entry>> entries;
            std::optional<std::vector<nodes::reference_record>> references;
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
            } else if (action == "reference" || action == "references" || action == "reference_history") {
                if (action == "reference" && (!in.reference.value() || !in.version.value() || !in.operation_id.value()))
                    throw tool_error(nodes::code::invalid_record, "reference requires reference, version and operation_id");
                nodes::node_request request{.action = action,
                                            .reference = in.reference.value(),
                                            .worldline = line,
                                            .revision = in.version.value().value_or(""),
                                            .intent = in.intent.value().value_or("")};
                request.after = in.after.value().value_or(0);
                request.limit = std::min<std::uint64_t>(in.limit.value().value_or(100), config::resolve(*s.snapshot(), project).limits.list_max_entries);
                if (action == "reference") {
                    request.operation = lines::operation_of(*in.operation_id.value());
                    request.caller_operation = *in.operation_id.value();
                }
                auto r = co_await node_call(s, project, std::move(request), actor, action != "reference");
                out.references = r.references;
                out.truncated = r.truncated;
                if (action == "reference") {
                    out.version = r.version;
                    out.receipt = r.receipt;
                    out.operation_id = in.operation_id.value();
                    out.replayed = r.replayed;
                }
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
                std::string input = content::hash_of(json::write(std::array{name, in.from.value().value_or(line), in.version.value().value_or(""),
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
                                                  .result = json::write(record{input})},
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
                std::set<std::string> loaded, scopes;
                for (const auto& change : in.changes.value().value_or(std::vector<change>{})) {
                    const auto& kind = change.kind.value();
                    if ((kind == change_kind::make<"edit">() || kind == change_kind::make<"replace">() || kind == change_kind::make<"write">()) && loaded.insert(change.path.value()).second) {
                        auto file = co_await node_call(
                            s, project, {.action = "read", .worldline = line, .revision = observed.version, .target = change.path.value()}, actor, true);
                        observed.files.insert(observed.files.end(), file.files.begin(), file.files.end());
                    }
                    if (kind == change_kind::make<"replace">() && change.scope.value())
                        scopes.insert(*change.scope.value());
                }
                std::map<std::string, semantic::place> located;
                if (!scopes.empty())
                    located = (co_await node_call(s, project,
                                                  {.action = "scope_locate",
                                                   .worldline = line,
                                                   .revision = observed.version,
                                                   .scope_refs = std::vector<std::string>{scopes.begin(), scopes.end()}},
                                                  actor, true))
                                  .located;
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
                    request.edits = edits_of(in.changes.value().value_or(std::vector<change>{}), observed, limits, located);
                    if (request.edits.empty())
                        throw tool_error(nodes::code::invalid_record, "apply needs at least one change");
                }
                auto r = co_await node_call(s, project, std::move(request), actor);
                out = co_await delta(s, project, line, r.base_version, r.version, actor);
                out.receipt = r.receipt;
                out.operation_id = in.operation_id.value();
                out.replayed = r.replayed;
                out.written_version = r.written_version;
                if (!r.reach.empty())
                    out.reach = r.reach;
            } else if (action == "receipt") {
                auto r = co_await node_call(s, project,
                                            {.action = "receipt",
                                             .worldline = line,
                                             .operation = lines::operation_of(in.operation_id.value().value_or("")),
                                             .caller_operation = std::string{in.operation_id.value().value_or("")}},
                                            actor, true);
                if (!r.references.empty()) {
                    out.worldline = r.worldline;
                    out.version = r.version;
                    out.references = r.references;
                } else
                    out = co_await delta(s, project, r.worldline, r.base_version, r.version, actor);
                out.receipt = r.receipt;
                out.operation_id = in.operation_id.value();
                out.replayed = true;
                out.written_version = r.written_version;
                if (!r.reach.empty())
                    out.reach = r.reach;
            } else if (action == "history") {
                auto limit = std::min<std::uint64_t>(in.limit.value().value_or(100), config::resolve(*s.snapshot(), project).limits.list_max_entries);
                auto r = co_await node_call(s, project,
                                            {.action = "history",
                                             .worldline = line,
                                             .revision = in.version.value().value_or(""),
                                             .target = in.cursor.value().value_or(""),
                                             .limit = limit},
                                            actor, true);
                std::vector<history_entry> entries;
                for (auto& h : r.history)
                    entries.push_back({h.event, h.operation, h.actor, h.intent, h.tool, h.parents});
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
                    entries.push_back({h.event, h.operation, h.actor, h.intent, h.tool, h.parents});
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

} // namespace tools
