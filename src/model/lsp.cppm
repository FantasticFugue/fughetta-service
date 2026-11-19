module;
#include <ada.h>
export module lsp;
import utility;
import std;
import host;
import http;
import native;
import coroutine;
import json;
import records;
import provider;
import evaluation;
import toolchain;

// The Language Server Protocol is spoken only inside this module.
namespace lsp::detail {
    inline auto object(std::initializer_list<std::pair<std::string, json::value>> fields) -> json::value {
        return json::object(fields);
    }
    inline auto text(std::string value) -> json::value { return json::value{std::move(value)}; }
    inline const json::value null{std::nullopt};

    struct position {
        std::uint64_t line = 0, character = 0;
    };
    struct range {
        position start, end;
    };
    struct symbol {
        std::string name;
        int kind = 0;
        detail::range range;
        detail::range selectionRange;
        std::optional<std::vector<symbol>> children;
    };
    struct location {
        std::string uri;
        detail::range range;
    };
    struct location_link {
        std::string targetUri;
        detail::range targetRange, targetSelectionRange;
    };
    struct diagnostic {
        detail::range range;
        std::optional<json::value> severity, code;
        std::optional<std::string> message;
    };
    struct published {
        std::string uri;
        std::vector<diagnostic> diagnostics;
        std::optional<std::int64_t> version;
    };
    struct report_of {
        std::string kind;
        std::optional<std::vector<diagnostic>> items;
    };
    struct failure {
        int code = 0;
        std::string message;
    };
    struct message {
        std::optional<json::value> id;
        std::optional<std::string> method;
        std::optional<json::value> params;
        std::optional<json::value> result;
        std::optional<failure> error;
    };
    // What the server's answer to `initialize` says it offers. What it does not name, it does not offer.
    struct capabilities {
        std::optional<std::string> positionEncoding;
        std::optional<json::value> documentSymbolProvider, diagnosticProvider, definitionProvider, referencesProvider;
    };
    // One request at a time is admitted by the session lock. Its reader may finish the
    // response before the caller starts waiting, or after cancellation has removed it.
    struct response {
        std::optional<message> value;
        std::exception_ptr error;
        bool cancelled = false;
        std::coroutine_handle<> waiter;
        struct on_stop {
            response* self;
            void operator()() const noexcept {
                self->cancelled = true;
                self->wake();
            }
        };
        std::optional<execution::stop_callback_for_t<coroutine::stop_token, on_stop>> watch;
        auto await_ready() const noexcept -> bool { return value.has_value() || error || cancelled; }
        template <class P> auto await_suspend(std::coroutine_handle<P> frame) noexcept -> std::coroutine_handle<> {
            auto token = execution::get_stop_token(execution::get_env(frame.promise()));
            if (token.stop_requested()) {
                cancelled = true;
                return frame;
            }
            waiter = frame;
            if (token.stop_possible())
                watch.emplace(token, on_stop{this});
            return std::noop_coroutine();
        }
        auto await_resume() -> std::optional<message> {
            watch.reset();
            if (cancelled)
                return std::nullopt;
            if (error)
                std::rethrow_exception(error);
            return std::move(value);
        }
        void wake() {
            if (auto frame = std::exchange(waiter, {}))
                frame.resume();
        }
    };
    struct initialized {
        detail::capabilities capabilities;
    };
    struct configuration_items {
        std::vector<json::value> items;
    };

    // A capability is offered when it is present and neither `false` nor `null`.
    inline auto offered(const std::optional<json::value>& capability) -> bool {
        if (!capability || std::holds_alternative<std::monostate>(capability->get()))
            return false;
        if (auto* flag = std::get_if<bool>(&capability->get()))
            return *flag;
        return true;
    }
    inline auto number(const std::optional<json::value>& value) -> std::optional<std::int64_t> {
        if (!value)
            return std::nullopt;
        if (auto* integer = std::get_if<std::int64_t>(&value->get()))
            return *integer;
        if (auto* real = std::get_if<double>(&value->get()))
            return static_cast<std::int64_t>(*real);
        return std::nullopt;
    }
    inline auto field_of(const json::object& fields, std::string_view name) -> const json::value* {
        auto found = fields.find(name);
        return found == fields.end() ? nullptr : &found->second;
    }

    // LSP symbol kinds folded into the neutral kinds: namespace, type, function, variable, module and other.
    inline auto kind_name(int kind) -> std::string_view {
        switch (kind) {
        case 2:
        case 4:
            return "module";
        case 3:
            return "namespace";
        case 5:
        case 10:
        case 11:
        case 23:
        case 26:
            return "type";
        case 6:
        case 9:
        case 12:
        case 25:
            return "function";
        case 7:
        case 8:
        case 13:
        case 14:
        case 22:
            return "variable";
        default:
            return "other";
        }
    }
    // Kinds whose extent other declarations live in, and so make a scope.
    inline auto encloses(std::string_view kind) -> bool { return kind == "namespace" || kind == "type" || kind == "function" || kind == "module"; }
    // LSP severities (1 error to 4 hint) by their number; a diagnostic without one is an error, as the specification says.
    inline auto severity_name(const std::optional<json::value>& severity) -> std::string {
        switch (number(severity).value_or(1)) {
        case 2:
            return "warning";
        case 3:
            return "information";
        case 4:
            return "hint";
        default:
            return "error";
        }
    }
    // A diagnostic's message, with the code the server gave it when that code is text.
    inline auto message_text(const diagnostic& d) -> std::string {
        std::string out = d.message.value_or("");
        if (d.code)
            if (auto* code = std::get_if<std::string>(&d.code->get()); code && !code->empty())
                out = out.empty() ? "[" + *code + "]" : std::format("[{}] {}", *code, out);
        return out;
    }

    // A file URI for an absolute path, with every byte outside the unreserved set percent-encoded.
    inline auto uri(const std::filesystem::path& path) -> std::string {
        static constexpr auto encode = [] {
            std::array<std::uint8_t, 32> chars{};
            for (unsigned i = 0; i < 256; ++i)
                if (!((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9') ||
                      i == '-' || i == '.' || i == '_' || i == '~' || i == '/' || i == ':'))
                    chars[i / 8] |= std::uint8_t{1} << (i % 8);
            return chars;
        }();
        const auto utf8 = path.generic_u8string();
        std::string_view text{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        auto prefix = text.starts_with('/') ? "file://" : "file:///";
        if (text.starts_with("//"))
            text.remove_prefix(2);
        return prefix + ada::unicode::percent_encode(text, encode.data());
    }
    // Interpret the URL as a local filesystem path, including UNC authority on Windows.
    inline auto path_of(std::string_view text) -> std::optional<std::filesystem::path> {
        auto url = ada::parse(text);
        if (!url || url->get_protocol() != "file:" || url->has_search() || url->has_hash())
            return std::nullopt;
        std::string decoded;
        try {
            const auto host = url->get_hostname();
            if (!host.empty())
                decoded = "//" + std::string{host};
            decoded += web::target::unescape(url->get_pathname());
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
        if (decoded.empty() || decoded.front() != '/' || decoded.contains('\0'))
            return std::nullopt;
        if (host::os == "windows" && decoded.size() >= 4 && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':' && decoded[3] == '/')
            decoded.erase(0, 1);
        return std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(decoded.data()), decoded.size()}};
    }

    // Line starts of a UTF-8 text and the Unicode scalar count before each, so a position costs one line scan.
    class lines {
    public:
        lines(std::string_view text, bool utf16) : text_(text), utf16_(utf16) {
            std::uint64_t scalars = 0;
            starts_.push_back({0, 0});
            for (std::size_t i = 0; i < text.size();) {
                if (text[i] == '\r' || text[i] == '\n') {
                    auto next = i + (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n' ? 2 : 1);
                    scalars += next - i;
                    ends_.push_back(i);
                    starts_.push_back({next, scalars});
                    i = next;
                    continue;
                }
                ++scalars;
                i += width(i);
            }
            ends_.push_back(text.size());
        }
        // Nothing for a position outside the text, which means the server read another text.
        auto offset(const position& at) const -> std::optional<std::uint64_t> {
            if (at.line >= starts_.size())
                return std::nullopt;
            auto [i, scalars] = starts_[at.line];
            auto end = ends_[at.line];
            std::uint64_t units = 0;
            for (; i < end && units < at.character; ++scalars) {
                auto step = width(i);
                units += utf16_ && step == 4 ? 2 : 1;
                i += step;
            }
            if (units < at.character)
                return std::nullopt;
            return scalars;
        }

        auto position_of(std::uint64_t offset) const -> std::optional<position> {
            auto found = std::ranges::upper_bound(starts_, offset, {}, [](const auto& start) { return start.second; });
            if (found == starts_.begin())
                return {};
            --found;
            const auto line = static_cast<std::uint64_t>(found - starts_.begin());
            auto [byte, scalar] = *found;
            std::uint64_t character = 0;
            while (byte < ends_[line] && scalar < offset) {
                const auto step = width(byte);
                character += utf16_ && step == 4 ? 2 : 1;
                byte += step;
                ++scalar;
            }
            if (scalar != offset)
                return {};
            return position{line, character};
        }

    private:
        auto width(std::size_t i) const -> std::size_t {
            auto lead = static_cast<unsigned char>(text_[i]);
            std::size_t n = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            return std::min(n, text_.size() - i);
        }
        std::string_view text_;
        bool utf16_;
        std::vector<std::pair<std::size_t, std::uint64_t>> starts_;
        std::vector<std::size_t> ends_;
    };

    inline constexpr std::size_t header_limit = 8 * 1024, body_limit = 64 * 1024 * 1024, depth_limit = 256, document_records = 20000;
    inline auto field(std::string_view line, std::string_view name) -> bool {
        return line.size() > name.size() && line[name.size()] == ':' &&
               std::ranges::equal(line.substr(0, name.size()), name, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
    }
    // A message is parsed only when its JSON nesting and element count are bounded, so parsing cannot run away.
    inline auto shallow(std::string_view text) -> bool {
        std::size_t depth = 0, elements = 0;
        bool quoted = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto c = text[i];
            if (quoted) {
                if (c == '\\')
                    ++i;
                else if (c == '"')
                    quoted = false;
                continue;
            }
            if (c == '"')
                quoted = true;
            else if (c == '[' || c == '{') {
                if (++depth > 4 * depth_limit || ++elements > 1'000'000)
                    return false;
            } else if (c == ']' || c == '}')
                depth -= depth > 0;
            else if (c == ',' && ++elements > 1'000'000)
                return false;
        }
        return true;
    }
    // An answer is converted only when its nesting and size are bounded, so parsing cannot run away.
    inline auto bounded(const json::value& value, std::size_t depth, std::size_t& seen) -> bool {
        if (depth > 4 * depth_limit || ++seen > 1'000'000)
            return false;
        if (auto* list = std::get_if<json::array>(&value.get()))
            return std::ranges::all_of(*list, [&](auto& v) { return bounded(v, depth + 1, seen); });
        if (auto* fields = std::get_if<json::object>(&value.get()))
            return std::ranges::all_of(*fields, [&](auto& field) { return bounded(field.second, depth + 1, seen); });
        return true;
    }
    inline auto same_id(const json::value& id, std::int64_t expected) -> bool {
        if (auto* n = std::get_if<std::int64_t>(&id.get()))
            return *n == expected;
        if (auto* d = std::get_if<double>(&id.get()))
            return *d == static_cast<double>(expected);
        return false;
    }
} // namespace lsp::detail

// One adapter behind the neutral semantic model: it talks to a language server over its standard streams and reports its document
// symbols as semantic scopes and symbols, and its diagnostics. The server's answer to `initialize` says which of the two it
// offers (documentSymbolProvider, diagnosticProvider), and only those are requested; a server that offers neither is an error,
// never an empty answer. Spans name only a place, a path and Unicode scalar offsets, and the caller turns them into character
// identities of the version it analyzed. No call here waits by itself: the caller bounds each one with a deadline.
export namespace lsp {
    using document = language::document;
    class session {
    public:
        session(native::scheduler io, io::command server) : io_(io) {
            server.interactive = true;
            child_.emplace(io::spawn_process(io, server));
        }

        // Starts the server on a workspace directory with its provider's initialization options, offering the capabilities this
        // client uses, and agrees on how positions are counted. The `settings` of the options also answer the server's
        // workspace/configuration requests, as editors do.
        auto start(const std::filesystem::path& root, json::value initialization) -> coroutine::task<> {
            using detail::object, detail::text;
            auto held = co_await lock_.lock();
            if (started_)
                throw std::logic_error("language server session was started twice");
            root_ = std::filesystem::absolute(root);
            if (auto* fields = std::get_if<json::object>(&initialization.get()))
                if (auto found = detail::field_of(*fields, "settings"))
                    settings_ = *found;
            auto uri = detail::uri(root_);
            auto reply = co_await call(
                "initialize",
                object({// No process ID: a server that watches it outlive the client would leave, and is killed with its session.
                        {"processId", detail::null},
                        {"clientInfo", object({{"name", text("fughetta-service")}})},
                        {"rootUri", text(uri)},
                        {"initializationOptions", std::move(initialization)},
                        {"workspaceFolders", json::array{object({{"uri", text(uri)}, {"name", text(root_.filename().string())}})}},
                        {"capabilities",
                         object({{"general", object({{"positionEncodings", json::array{text("utf-32"), text("utf-16")}}})},
                                 {"workspace", object({{"configuration", json::value{true}}, {"workspaceFolders", json::value{true}}})},
                                 // Diagnostics are pulled where the server offers it, and pushed otherwise; the pull is announced, since servers
                                 // such as ty and ruff pull only for a client that says it can.
                                 {"textDocument", object({{"synchronization", object({{"dynamicRegistration", json::value{false}}})},
                                                          {"documentSymbol", object({{"hierarchicalDocumentSymbolSupport", json::value{true}}})},
                                                          {"diagnostic", object({{"dynamicRegistration", json::value{false}}})},
                                                          {"publishDiagnostics", object({})}})}})}}));
            auto parsed = json::from_value<detail::initialized, json::defaults>(reply);
            if (!parsed)
                throw std::runtime_error("language server answered initialize with an unreadable result: " + std::string{parsed.error().what()});
            auto& capabilities = parsed->capabilities;
            // LSP uses UTF-16 when positionEncoding is omitted.
            auto encoding = capabilities.positionEncoding.value_or("utf-16");
            if (encoding != "utf-32" && encoding != "utf-16")
                throw std::runtime_error("language server answered position encoding " + encoding + ", which it was not offered");
            utf16_ = encoding == "utf-16";
            symbols_ = detail::offered(capabilities.documentSymbolProvider);
            pull_ = detail::offered(capabilities.diagnosticProvider);
            definitions_ = detail::offered(capabilities.definitionProvider);
            references_ = detail::offered(capabilities.referencesProvider);
            started_ = true;
            co_await notify("initialized", object({}));
            reading_ = true;
            reader_.spawn(pump());
        }

        auto workspace_request(std::string method) -> coroutine::task<json::value> {
            auto held = co_await lock_.lock();
            co_return co_await call(std::move(method), detail::object({{"uri", detail::text(detail::uri(root_))}}));
        }

        auto document_request(std::string method, const document& d) -> coroutine::task<json::value> {
            auto held = co_await lock_.lock();
            co_await update(d);
            co_return co_await call(std::move(method), detail::object({{"uri", detail::text(detail::uri(root_ / d.path))}}));
        }

        // Reports what the server offers about one document.
        auto describe(const document& d, semantic::unit unit, std::vector<semantic::fact_kind> facts = {}) -> coroutine::task<semantic::shard> {
            auto held = co_await lock_.lock();
            co_return co_await reported(d, std::move(unit), facts);
        }

        struct lookup_result {
            std::vector<semantic::locator> targets;
        };
        auto definitions() const -> bool { return definitions_; }
        auto lookup(const semantic::request& query) -> coroutine::task<lookup_result> {
            auto held = co_await lock_.lock();
            using detail::object, detail::text;
            if (query.action != "definitions" && query.action != "references")
                throw language::resolution_error("a location query names definitions or references");
            const auto* selected = query.target ? &query.target->name : query.selector && query.selector->name ? &*query.selector->name : nullptr;
            if (!selected || !selected->path || selected->text.empty())
                throw language::resolution_error("the selected entity has no retained source declaration");
            const auto& path_name = *selected->path;
            const auto& symbol_name = selected->text;
            if (!(query.action == "definitions" ? definitions_ : references_))
                throw std::runtime_error("language server does not offer " + query.action.str());
            auto source = documents_.find(path_name);
            if (source == documents_.end())
                throw language::resolution_error("language query path is not an analyzed source file: " + path_name);
            auto identifier = object({{"textDocument", object({{"uri", text(detail::uri(root_ / path_name))}})}});
            std::vector<detail::position> candidates;
            if (query.target && query.target->occurrence) {
                const auto& at = *query.target->occurrence;
                auto position = detail::lines{source->second.value.text, utf16_}.position_of(at.begin);
                if (at.path != path_name || !position)
                    throw language::resolution_error("selected declaration is outside the installed source");
                candidates.push_back(*position);
            }
            if (candidates.empty() && symbols_) {
                auto answer = co_await call("textDocument/documentSymbol", identifier);
                std::size_t seen = 0;
                if (!detail::bounded(answer, 0, seen))
                    throw std::runtime_error("language symbol response exceeds the record limit");
                if (auto symbols = json::from_value<std::vector<detail::symbol>>(answer)) {
                    auto visit = [&](this auto&& self, const detail::symbol& symbol, std::string prefix) -> void {
                        auto qualified = prefix.empty() ? symbol.name : prefix + "::" + symbol.name;
                        if (symbol_name == symbol.name || symbol_name == qualified)
                            candidates.push_back(symbol.selectionRange.start);
                        if (symbol.children)
                            for (const auto& child : *symbol.children)
                                self(child, qualified);
                    };
                    for (const auto& symbol : *symbols)
                        visit(symbol, "");
                }
            }
            // Providers without document symbols can still resolve a name at a selected occurrence.
            if (candidates.empty()) {
                const auto& input = source->second.value.text;
                auto identifier_byte = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
                for (std::size_t at = 0; (at = input.find(symbol_name, at)) != std::string::npos; at += symbol_name.size()) {
                    auto end = at + symbol_name.size();
                    if ((at && identifier_byte(input[at - 1])) || (end < input.size() && identifier_byte(input[end])))
                        continue;
                    detail::position position;
                    for (std::size_t i = 0; i < at;) {
                        auto c = static_cast<unsigned char>(input[i]);
                        if (c == '\r' || c == '\n') {
                            i += c == '\r' && i + 1 < at && input[i + 1] == '\n' ? 2 : 1;
                            ++position.line;
                            position.character = 0;
                        } else {
                            auto width = c < 0x80 ? 1u : c >= 0xF0 ? 4u : c >= 0xE0 ? 3u : 2u;
                            position.character += utf16_ && width == 4 ? 2 : 1;
                            i += width;
                        }
                    }
                    candidates.push_back(position);
                }
            }
            if (candidates.size() != 1)
                throw language::resolution_error(candidates.empty() ? "symbol was not found in the selected file"
                                                                    : "symbol name is ambiguous in the selected file");
            auto position = candidates.front();
            auto params = object({{"textDocument", object({{"uri", text(detail::uri(root_ / path_name))}})},
                                  {"position", object({{"line", json::value{static_cast<std::int64_t>(position.line)}},
                                                       {"character", json::value{static_cast<std::int64_t>(position.character)}}})}});
            if (query.action == "references")
                params = object({{"textDocument", object({{"uri", text(detail::uri(root_ / path_name))}})},
                                 {"position", object({{"line", json::value{static_cast<std::int64_t>(position.line)}},
                                                      {"character", json::value{static_cast<std::int64_t>(position.character)}}})},
                                 {"context", object({{"includeDeclaration", json::value{true}}})}});
            auto answer = co_await call(query.action == "definitions" ? "textDocument/definition" : "textDocument/references", std::move(params));
            std::size_t seen = 0;
            if (!detail::bounded(answer, 0, seen))
                throw std::runtime_error("language locations exceed the response limit");
            lookup_result result;
            std::vector<detail::location> locations;
            if (std::holds_alternative<std::monostate>(answer.get()))
                co_return result;
            if (auto list = json::from_value<std::vector<detail::location>>(answer))
                locations = std::move(*list);
            else if (auto links = json::from_value<std::vector<detail::location_link>, json::defaults>(answer)) {
                for (const auto& link : *links)
                    locations.push_back({link.targetUri, link.targetSelectionRange});
            } else if (auto one = json::from_value<detail::location>(answer))
                locations.push_back(std::move(*one));
            else
                throw std::runtime_error("language server returned unreadable locations");
            std::map<std::string, std::string> external_texts;
            std::map<std::string, detail::lines> indexes;
            for (const auto& location : locations) {
                auto path = detail::path_of(location.uri);
                if (!path)
                    throw std::runtime_error("language server returned a non-file location");
                auto relative = path->lexically_relative(root_).generic_string();
                auto found = documents_.find(relative);
                auto& external = external_texts[relative];
                if (found == documents_.end() && !indexes.contains(relative)) {
                    std::error_code error;
                    auto bytes = std::filesystem::file_size(*path, error);
                    if (error || bytes > (4u << 20))
                        throw std::runtime_error("cannot read the language server's target file within the document limit: " + path->string());
                    std::ifstream input{*path, std::ios::binary};
                    external.assign(std::istreambuf_iterator<char>{input}, {});
                    if (external.size() != bytes || external.contains('\0'))
                        throw std::runtime_error("language target is not readable text: " + path->string());
                }
                const auto& lines =
                    indexes.try_emplace(relative, found == documents_.end() ? std::string_view{external} : std::string_view{found->second.value.text}, utf16_)
                        .first->second;
                auto first = lines.offset(location.range.start), last = lines.offset(location.range.end);
                auto first_line = lines.offset({location.range.start.line, 0}), last_line = lines.offset({location.range.end.line, 0});
                if (!first || !last || !first_line || !last_line || *last < *first)
                    throw std::runtime_error("language server returned a range outside its document");
                semantic::locator at;
                if (found != documents_.end())
                    at.source = semantic::place{relative, *first, *last};
                else
                    at.artifact = semantic::artifact_location{path->generic_string(), path->parent_path().generic_string(), symbol_name, *first};
                at.position =
                    semantic::coordinates{location.range.start.line + 1, *first - *first_line + 1, location.range.end.line + 1, *last - *last_line + 1};
                result.targets.push_back(std::move(at));
            }
            co_return result;
        }

        // Apply a complete source snapshot before querying any document. Watch notifications also
        // cover generated inputs, such as a compilation database, that are not open text documents.
        auto synchronize(const std::vector<document>& documents, const std::vector<std::string>& changed = {}) -> coroutine::task<> {
            auto held = co_await lock_.lock();
            using detail::object, detail::text;
            std::set<std::string> present;
            for (const auto& d : documents)
                present.insert(d.path);
            for (auto it = documents_.begin(); it != documents_.end();) {
                if (present.contains(it->first)) {
                    ++it;
                    continue;
                }
                co_await notify("textDocument/didClose", object({{"textDocument", object({{"uri", text(detail::uri(root_ / it->first))}})}}));
                it = documents_.erase(it);
            }
            for (const auto& d : documents)
                co_await update(d);
            json::array events;
            for (const auto& path : changed)
                events.push_back(object({{"uri", text(detail::uri(root_ / path))}, {"type", json::value{std::int64_t{2}}}}));
            if (!events.empty())
                co_await notify("workspace/didChangeWatchedFiles", object({{"changes", std::move(events)}}));
        }

        // Retire a failed or cancelled context without a shutdown round trip.
        auto abort() -> coroutine::task<> {
            if (child_)
                child_->kill();
            reader_.request_stop();
            co_await coroutine::under(coroutine::stop_token{}, reader_.join());
            reading_ = false;
        }

        // A server that does not finish shutting down within five seconds is killed.
        // A session still busy with another request is ended at once: killing the server ends that request too.
        auto stop() -> coroutine::task<int> {
            auto held = lock_.try_lock();
            if (!held) {
                co_await abort();
                auto finished = co_await lock_.lock();
                co_return -1;
            }
            std::optional<int> code;
            std::exception_ptr failed;
            try {
                static constexpr auto run_until_deadline = [](session& self) -> coroutine::task<int> {
                    (void)co_await self.call("shutdown", detail::null);
                    co_await self.notify("exit", detail::null);
                    self.child_->close_input();
                    co_return co_await io::async_wait(*self.child_);
                };
                if (auto finished = co_await coroutine::deadline(io_, std::chrono::seconds{5}, run_until_deadline(*this)))
                    code = static_cast<int>(*finished);
            } catch (...) {
                failed = util::capture_current_exception();
            }
            co_await abort();
            if (failed)
                std::rethrow_exception(failed);
            co_return code.value_or(-1);
        }

    private:
        auto send(const json::value& body) -> coroutine::task<> {
            auto text = json::write(body);
            auto frame = std::format("Content-Length: {}\r\n\r\n{}", text.size(), text);
            auto turn = co_await writing_.lock();
            co_await io::async_write(*child_, std::as_bytes(std::span{frame}));
        }
        auto notify(std::string method, json::value params) -> coroutine::task<> {
            co_await send(detail::object({{"jsonrpc", detail::text("2.0")}, {"method", detail::text(std::move(method))}, {"params", std::move(params)}}));
        }
        auto pump() -> coroutine::task<> {
            try {
                for (;;) {
                    auto incoming = co_await coroutine::stop_point(receive());
                    if (!incoming)
                        break;
                    if (incoming->method) {
                        if (incoming->id)
                            co_await answer(*incoming);
                        else if (incoming->params && *incoming->method == "textDocument/publishDiagnostics")
                            pushed(*incoming->params);
                    } else if (pending_ && incoming->id && detail::same_id(*incoming->id, waiting_id_)) {
                        pending_->value = std::move(*incoming);
                        pending_->wake();
                    }
                }
                ended_ = std::make_exception_ptr(std::runtime_error("language reader stopped"));
            } catch (...) {
                ended_ = util::capture_current_exception();
            }
            if (pending_) {
                pending_->error = ended_;
                pending_->wake();
            }
        }

        auto call(std::string method, json::value params) -> coroutine::task<json::value> {
            if (ended_)
                std::rethrow_exception(ended_);
            auto id = ++next_;
            detail::response response;
            struct reset {
                detail::response*& slot;
                ~reset() { slot = nullptr; }
            } guard{pending_};
            if (reading_) {
                waiting_id_ = id;
                pending_ = &response;
            }
            co_await send(detail::object(
                {{"jsonrpc", detail::text("2.0")}, {"id", json::value{id}}, {"method", detail::text(method)}, {"params", std::move(params)}}));
            for (;;) {
                detail::message incoming;
                if (reading_) {
                    auto result = co_await response;
                    if (!result)
                        co_await coroutine::stop;
                    incoming = std::move(*result);
                } else {
                    incoming = co_await receive();
                    if (incoming.method) {
                        if (incoming.id)
                            co_await answer(incoming);
                        else if (incoming.params && *incoming.method == "textDocument/publishDiagnostics")
                            pushed(*incoming.params);
                        continue;
                    }
                    if (!incoming.id || !detail::same_id(*incoming.id, id))
                        continue;
                }
                if (incoming.error)
                    throw std::runtime_error(std::format("language server refused {}: {}", method, incoming.error->message));
                co_return incoming.result.value_or(detail::null);
            }
        }
        // A server that asks for settings is answered with those it was started with; registrations and progress are granted,
        // and anything else is a method this client does not have.
        auto answer(const detail::message& request) -> coroutine::task<> {
            using detail::object, detail::text;
            auto& method = *request.method;
            json::value reply = object({{"jsonrpc", text("2.0")}, {"id", *request.id}, {"result", detail::null}});
            if (method == "workspace/configuration") {
                std::size_t count = 0;
                if (request.params)
                    if (auto items = json::from_value<detail::configuration_items, json::defaults>(*request.params))
                        count = items->items.size();
                reply = object({{"jsonrpc", text("2.0")}, {"id", *request.id}, {"result", json::array(count, settings_)}});
            } else if (method != "client/registerCapability" && method != "client/unregisterCapability" && method != "window/workDoneProgress/create")
                reply = object({{"jsonrpc", text("2.0")},
                                {"id", *request.id},
                                {"error", object({{"code", json::value{std::int64_t{-32601}}}, {"message", text("method not supported")}})}});
            co_await send(reply);
        }
        // Ignore delayed diagnostics from an earlier document version.
        void pushed(const json::value& params) {
            auto parsed = json::from_value<detail::published, json::defaults>(params);
            if (!parsed)
                return;
            auto path = detail::path_of(parsed->uri);
            if (!path)
                return;
            auto relative = path->lexically_relative(root_).generic_string();
            auto found = documents_.find(relative);
            if (found != documents_.end() && (!parsed->version || *parsed->version == found->second.version)) {
                found->second.diagnostics = std::move(parsed->diagnostics);
                found->second.current_diagnostics = parsed->version.has_value();
            }
        }
        auto receive() -> coroutine::task<detail::message> {
            for (;;) {
                auto header = buffer_.find("\r\n\r\n");
                if (header == std::string::npos && buffer_.size() > detail::header_limit)
                    throw std::runtime_error("language server frame header exceeds 8 KiB");
                if (header != std::string::npos) {
                    if (header > detail::header_limit)
                        throw std::runtime_error("language server frame header exceeds 8 KiB");
                    std::optional<std::size_t> length;
                    for (std::size_t at = 0; at < header;) {
                        auto end = std::min(buffer_.find("\r\n", at), header);
                        std::string_view line{buffer_.data() + at, end - at};
                        if (detail::field(line, "content-length")) {
                            auto value = line.substr(line.find(':') + 1);
                            while (!value.empty() && value.front() == ' ')
                                value.remove_prefix(1);
                            std::size_t parsed = 0;
                            auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                            if (error != std::errc{} || last != value.data() + value.size() || parsed > detail::body_limit)
                                throw std::runtime_error("language server frame has an invalid Content-Length");
                            length = parsed;
                        }
                        at = end + 2;
                    }
                    if (!length)
                        throw std::runtime_error("language server frame has no Content-Length");
                    auto body = header + 4;
                    if (buffer_.size() - body >= *length) {
                        auto text = std::string_view{buffer_}.substr(body, *length);
                        if (!detail::shallow(text))
                            throw std::runtime_error("language server message is nested too deeply or too large");
                        auto parsed = json::read<detail::message, json::defaults>(text);
                        if (!parsed)
                            throw std::runtime_error("language server sent an unreadable message");
                        buffer_.erase(0, body + *length);
                        co_return std::move(*parsed);
                    }
                }
                auto count = co_await io::async_read_some(*child_, net::buffer(chunk_));
                if (count == 0)
                    throw std::runtime_error("language server closed its output");
                buffer_.append(reinterpret_cast<const char*>(chunk_.data()), count);
            }
        }

        static auto ownership(const semantic::shard& shard) -> semantic::ownership {
            return {.owner_unit = shard.unit.id,
                    .context = shard.unit.context,
                    .origin = {.provider = shard.unit.recipe.unit.provider, .kind = std::string{"source"}}};
        }
        void located(const document& d, const std::vector<detail::diagnostic>& items, semantic::shard& out) const {
            detail::lines lines{d.text, utf16_};
            const auto text = static_cast<std::uint64_t>(std::ranges::count_if(d.text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
            for (const auto& item : items) {
                if (out.diagnostics.size() >= detail::document_records)
                    throw std::runtime_error("language server diagnostic count exceeds the document limit");
                auto begin = lines.offset(item.range.start), end = lines.offset(item.range.end);
                if (!begin || !end || *begin > *end || *end > text)
                    throw std::runtime_error("language server diagnostic range is outside the document");
                auto first = *begin, last = *end;
                if (first == last && text) {
                    if (last < text)
                        ++last;
                    else
                        --first;
                }
                std::string code;
                if (item.code)
                    code = std::holds_alternative<std::string>(item.code->get()) ? std::get<std::string>(item.code->get()) : json::write(*item.code);
                out.diagnostics.push_back({.id = out.unit.id + ":diagnostic:" + std::to_string(out.diagnostics.size()),
                                           .producer = out.unit.recipe.unit.provider,
                                           .code = std::move(code),
                                           .severity = detail::severity_name(item.severity),
                                           .message = detail::message_text(item),
                                           .ownership = ownership(out),
                                           .location = {.source = semantic::place{d.path, first, last}}});
            }
        }
        void converted(const json::value& answer, const document& d, semantic::shard& out) const {
            std::size_t seen = 0;
            if (!detail::bounded(answer, 0, seen))
                throw std::runtime_error("language server diagnostic report is nested too deeply or too large");
            auto parsed = json::from_value<detail::report_of, json::defaults>(answer);
            if (!parsed)
                throw std::runtime_error("language server answered an unreadable diagnostic report: " + std::string{parsed.error().what()});
            if (parsed->kind != "full")
                throw std::runtime_error("language server answered a " + parsed->kind + " diagnostic report, which holds no diagnostics");
            located(d, parsed->items.value_or(std::vector<detail::diagnostic>{}), out);
        }
        void described(const json::value& answer, const document& d, semantic::shard& out) const {
            if (std::holds_alternative<std::monostate>(answer.get()))
                return;
            std::size_t seen = 0;
            if (!detail::bounded(answer, 0, seen))
                throw std::runtime_error("language server symbols are nested too deeply or too large");
            auto symbols = json::from_value<std::vector<detail::symbol>>(answer);
            if (!symbols)
                throw std::runtime_error("language server returned unreadable document symbols: " + std::string{symbols.error().what()});
            detail::lines lines{d.text, utf16_};
            auto span = [&](const detail::range& r) -> std::optional<semantic::place> {
                auto begin = lines.offset(r.start), end = lines.offset(r.end);
                if (!begin || !end || *begin >= *end)
                    return std::nullopt;
                return semantic::place{d.path, *begin, *end};
            };
            std::map<std::string, std::size_t> named, scopes;
            auto visit = [&](this auto& self, const detail::symbol& row, std::optional<std::string> scope, std::optional<std::string> parent,
                             const std::string& qualified, std::size_t depth) -> void {
                auto range = span(row.range), name = span(row.selectionRange);
                if (!range || !name || depth > detail::depth_limit)
                    return;
                auto path = qualified.empty() ? row.name : qualified + "::" + row.name;
                const auto nth = named[path]++;
                auto id = d.path + "#" + path + (nth ? "~" + std::to_string(nth) : std::string{});
                std::string kind{detail::kind_name(row.kind)};
                semantic::anchored_identity identity{
                    out.unit.recipe.unit.project, out.unit.recipe.unit.project, out.unit.recipe.unit.provider, {.file = d.path, .declaration = id}};
                out.identities.push_back({identity, id});
                out.symbols.push_back({.id = id,
                                       .ownership = ownership(out),
                                       .language = d.language,
                                       .domain = "source",
                                       .module = out.unit.recipe.unit.project,
                                       .name = row.name,
                                       .qualified_name = path,
                                       .kind = kind,
                                       .owner = parent,
                                       .declaring_scope = scope,
                                       .native_key = identity});
                out.occurrences.push_back({.id = id + ":definition",
                                           .ownership = ownership(out),
                                           .symbol = id,
                                           .location = {.source = *name},
                                           .roles = {std::string{"definition"}},
                                           .selection = *name,
                                           .body = *range});
                if (scope)
                    out.scopes.at(scopes.at(*scope)).declared_members.push_back(id);
                auto inner = scope;
                if (detail::encloses(kind)) {
                    inner = "scope:" + id;
                    scopes.emplace(*inner, out.scopes.size());
                    out.scopes.push_back({.id = *inner,
                                          .ownership = ownership(out),
                                          .lexical_parent = scope,
                                          .semantic_owner = id,
                                          .location = {.source = *range},
                                          .kind = kind});
                }
                if (row.children)
                    for (const auto& child : *row.children)
                        self(child, inner, id, path, depth + 1);
            };
            for (const auto& row : *symbols)
                visit(row, {}, {}, "", 0);
        }
        struct opened {
            document value;
            std::int64_t version = 1;
            std::vector<detail::diagnostic> diagnostics;
            bool current_diagnostics = false;
        };

        auto update(const document& d) -> coroutine::task<> {
            if (!started_)
                throw std::logic_error("language server session was not started");
            using detail::object, detail::text;
            auto uri = text(detail::uri(root_ / d.path));
            auto found = documents_.find(d.path);
            if (found == documents_.end()) {
                auto version = ++version_;
                documents_.emplace(d.path, opened{d, version});
                co_await notify(
                    "textDocument/didOpen",
                    object({{"textDocument",
                             object({{"uri", uri}, {"languageId", text(d.language)}, {"version", json::value{version}}, {"text", text(d.text)}})}}));
            } else if (found->second.value.text != d.text) {
                auto version = ++version_;
                found->second = opened{d, version};
                co_await notify("textDocument/didChange", object({{"textDocument", object({{"uri", uri}, {"version", json::value{version}}})},
                                                                  {"contentChanges", json::array{object({{"text", text(d.text)}})}}}));
            }
        }

        auto reported(const document& d, semantic::unit unit, const std::vector<semantic::fact_kind>& facts) -> coroutine::task<semantic::shard> {
            if (!symbols_ && !pull_ && !definitions_ && !references_)
                throw std::runtime_error("the language server declares no supported analysis request for " + d.path);
            using detail::object, detail::text;
            co_await update(d);
            auto identifier = object({{"textDocument", object({{"uri", text(detail::uri(root_ / d.path))}})}});
            semantic::shard out{.unit = std::move(unit)};
            out.unit.contributions.files = {d.path};
            out.unit.coverage.requested = {.files = {d.path},
                                           .contexts = {out.unit.context},
                                           .facts = facts.empty() ? std::vector<semantic::fact_kind>{std::string{"symbols"}, std::string{"definitions"},
                                                                                                     std::string{"scopes"}, std::string{"diagnostics"}}
                                                                  : facts};
            auto available = out.unit.coverage.requested;
            available.facts.clear();
            const auto wants = [&](const std::string& name) { return facts.empty() || std::ranges::contains(facts, name); };
            if (symbols_ && (wants("symbols") || wants("definitions") || wants("scopes"))) {
                described(co_await call("textDocument/documentSymbol", identifier), d, out);
                available.facts = {std::string{"symbols"}, std::string{"definitions"}, std::string{"scopes"}};
            }
            if (wants("diagnostics")) {
                bool current = true;
                if (pull_)
                    converted(co_await call("textDocument/diagnostic", identifier), d, out);
                else {
                    const auto& latest = documents_.at(d.path);
                    located(d, latest.diagnostics, out);
                    current = latest.current_diagnostics;
                    out.unit.cacheable = false;
                }
                if (current)
                    available.facts.push_back(std::string{"diagnostics"});
            }
            out.unit.coverage.available.push_back(available);
            for (const auto& name : out.unit.coverage.requested.facts)
                if (!std::ranges::contains(available.facts, name)) {
                    auto missing = out.unit.coverage.requested;
                    missing.facts = {name};
                    out.unit.coverage.missing.push_back(
                        {.scope = std::move(missing), .reason = "the language server has no current " + name.str() + " records"});
                }
            co_return out;
        }

        native::scheduler io_;
        std::optional<native::process> child_;
        coroutine::mutex lock_, writing_;
        coroutine::scope reader_;
        detail::response* pending_ = nullptr;
        std::exception_ptr ended_;
        std::int64_t waiting_id_ = 0, version_ = 0;
        bool reading_ = false;
        std::string buffer_;
        std::array<std::byte, 16 * 1024> chunk_{};
        std::filesystem::path root_;
        std::map<std::string, opened> documents_;
        json::value settings_ = detail::null;
        std::int64_t next_ = 0;
        bool started_ = false, utf16_ = true, symbols_ = false, pull_ = false, definitions_ = false, references_ = false;
    };

    // What one survey may take: documents and their bytes are checked before any is read, records as the server reports them.
    struct limits {
        std::size_t documents = 2000;
        std::uint64_t document_bytes = 4u << 20, bytes = 64u << 20;
        std::size_t records = 200000;
        std::chrono::seconds per_document{30}, total{600};
    };

} // namespace lsp

export namespace language {
    class lsp_adapter : public adapter {
    public:
        static constexpr adapter_kind transport = adapter_kind::lsp;
        lsp_adapter(native::scheduler io, io::command command, session_state identity, std::map<std::string, std::vector<std::string>> commands = {},
                    toolchain::identity runtime = {}, document_loader loader = {})
            : adapter(io, command, std::move(identity), std::move(commands), std::move(runtime), std::move(loader)), server_(io, std::move(command)) {}

    protected:
        auto do_open(json::value initialization) -> coroutine::task<> override { co_await server_.start(root_, std::move(initialization)); }
        auto do_update(language::installation input, const std::vector<std::string>& changed) -> coroutine::task<> override {
            install(std::move(input));
            const auto& documents = installed_.documents;
            co_await server_.synchronize(documents, changed);
        }
        auto do_units(const semantic::extent& demand) -> coroutine::task<std::vector<semantic::unit_key>> override {
            std::vector<semantic::unit_key> result;
            for (const auto& document : installed_.documents)
                if (demand.files.empty() || std::ranges::contains(demand.files, document.path))
                    result.push_back({state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::file, document.path});
            co_return result;
        }
        auto do_evaluate(const evaluation_request& request) -> coroutine::task<evaluated_unit> override {
            const auto found = std::ranges::find(installed_.documents, request.unit.name, &document::path);
            if (found == installed_.documents.end())
                throw std::invalid_argument("LSP document is not installed");
            auto unit = make_unit(request.unit);
            evaluated_unit result;
            auto facts = request.demand.facts;
            std::erase(facts, semantic::fact_kind::make<"references">());
            result.shards.push_back(co_await server_.describe(*found, std::move(unit), std::move(facts)));
            co_return result;
        }
        auto resolves() const -> bool override { return true; }
        auto make_unit(const semantic::unit_key& key) const -> semantic::unit {
            semantic::recipe recipe{key, state_.options};
            auto unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
            unit.direct_inputs = state_.inputs;
            unit.cacheable = !runtime_.execution.empty();
            return unit;
        }
        auto do_resolve(const evaluation_request& request, const semantic::request& query) -> coroutine::task<evaluated_unit> override;
        auto do_close() -> coroutine::task<> override { co_await server_.abort(); }
        void install(installation input) {
            texts_.clear();
            for (const auto& document : input.documents)
                texts_.emplace(document.path, document.text);
            installed_ = std::move(input);
        }
        installation installed_;
        std::map<std::string, std::string> texts_;
        lsp::session server_;
    };
    auto lsp_adapter::do_resolve(const evaluation_request& request, const semantic::request& query) -> coroutine::task<evaluated_unit> {
        if (!query.selector || (!query.target && (!query.selector->name || !query.selector->name->path)))
            throw resolution_error("the selected entity has no retained source declaration");
        const auto& selected = query.target ? query.target->name : *query.selector->name;
        semantic::shard shard{.unit = make_unit(request.unit)};
        shard.unit.resolution = semantic::unit_resolution{query.action.str(), *query.selector, {}};
        auto own = [&] { return semantic::ownership{shard.unit.id, shard.unit.context, {.provider = state_.provider, .kind = std::string{"source"}}}; };
        std::set<std::string> documents;
        auto load = [&](const std::string& path) -> coroutine::task<> {
            if (!documents.insert(path).second)
                co_return;
            auto found = std::ranges::find(installed_.documents, path, &document::path);
            if (found == installed_.documents.end())
                throw resolution_error("LSP target document is not installed: " + path);
            auto rows = co_await server_.describe(*found, shard.unit, {std::string{"symbols"}, std::string{"definitions"}, std::string{"scopes"}});
            auto append = [](auto& into, auto& from) { std::ranges::move(from, std::back_inserter(into)); };
            append(shard.identities, rows.identities);
            append(shard.symbols, rows.symbols);
            append(shard.occurrences, rows.occurrences);
            append(shard.scopes, rows.scopes);
            shard.unit.contributions.files.push_back(path);
        };
        co_await load(*selected.path);
        auto definitions = query;
        definitions.action = "definitions";
        lsp::session::lookup_result declarations;
        if (server_.definitions())
            declarations = co_await server_.lookup(definitions);
        auto uses = query.action == "references" ? co_await server_.lookup(query) : declarations;
        for (const auto& target : declarations.targets)
            if (target.source)
                co_await load(target.source->path);
        for (const auto& target : uses.targets)
            if (target.source)
                co_await load(target.source->path);
        auto enclosing = [&](const semantic::place& at) -> const semantic::records::scope* {
            const semantic::records::scope* result = nullptr;
            std::uint64_t width = std::numeric_limits<std::uint64_t>::max();
            for (const auto& scope : shard.scopes)
                if (scope.location.source) {
                    const auto& range = *scope.location.source;
                    if (range.path == at.path && range.begin <= at.begin && range.end >= at.end && range.end - range.begin < width) {
                        result = &scope;
                        width = range.end - range.begin;
                    }
                }
            return result;
        };
        auto add_target = [&](const std::string& id) {
            auto& targets = shard.unit.resolution->targets;
            if (!std::ranges::any_of(targets, [&](const auto& target) { return target.symbol_id == id; }))
                targets.push_back({id, shard.unit.context});
        };
        std::set<std::pair<std::string, std::string>> external_reads;
        auto normalize = [&](semantic::locator& at) {
            if (!at.artifact)
                return true;
            const auto member = toolchain::locate(runtime_.inputs, at.artifact->artifact);
            if (!member) {
                shard.unit.cacheable = false;
                shard.unit.coverage.missing.push_back(
                    {.scope = {.facts = {query.action.str()}, .selector = query.selector}, .reason = "the external location has no declared SDK input"});
                return false;
            }
            at.artifact->artifact = member->input->object;
            at.artifact->module = member->input->name;
            at.artifact->member = member->path;
            if (external_reads.emplace(member->input->name, member->path).second)
                shard.unit.observed_dependencies.push_back({.kind = semantic::dependency_kind::artifact,
                                                            .project = state_.project,
                                                            .key = member->path,
                                                            .value = member->input->object,
                                                            .scope = member->input->name});
            return true;
        };
        for (auto at : declarations.targets) {
            if (!normalize(at))
                continue;
            std::optional<std::string> existing;
            std::uint64_t width = std::numeric_limits<std::uint64_t>::max();
            if (at.source)
                for (const auto& occurrence : shard.occurrences)
                    if (occurrence.selection && occurrence.selection->path == at.source->path && occurrence.selection->begin <= at.source->begin &&
                        occurrence.selection->end >= at.source->end && occurrence.selection->end - occurrence.selection->begin < width) {
                        existing = occurrence.symbol;
                        width = occurrence.selection->end - occurrence.selection->begin;
                    }
            if (existing) {
                add_target(*existing);
                continue;
            }
            const auto id = shard.unit.id + ":target:" + std::to_string(shard.identities.size());
            std::optional<std::string> parent, scope;
            std::optional<semantic::declaration_identity> identity;
            std::string language, module = state_.project;
            if (at.source) {
                const auto document = std::ranges::find(installed_.documents, at.source->path, &document::path);
                language = document->language;
                if (at.source->begin == at.source->end) {
                    const auto length = std::ranges::count_if(document->text, [](unsigned char c) { return (c & 0xC0) != 0x80; });
                    if (at.source->end < static_cast<std::uint64_t>(length))
                        ++at.source->end;
                    else if (at.source->begin)
                        --at.source->begin;
                    else
                        throw resolution_error("LSP returned a declaration in an empty document");
                }
                if (const auto* owner = enclosing(*at.source)) {
                    parent = owner->semantic_owner;
                    scope = owner->id;
                }
                identity = semantic::anchored_identity{state_.project, module, state_.provider, {.file = at.source->path, .declaration = id}};
            } else if (at.artifact) {
                module = at.artifact->module;
            } else
                continue;
            if (identity)
                shard.identities.push_back({*identity, id});
            shard.symbols.push_back({.id = id,
                                     .ownership = own(),
                                     .language = language,
                                     .domain = "source",
                                     .module = module,
                                     .name = selected.text,
                                     .qualified_name = selected.text,
                                     .kind = std::string{"other"},
                                     .owner = parent,
                                     .declaring_scope = scope,
                                     .native_key = identity});
            shard.occurrences.push_back(
                {.id = id + ":definition", .ownership = own(), .symbol = id, .location = std::move(at), .roles = {std::string{"definition"}}});
            add_target(id);
        }
        if (!server_.definitions())
            for (const auto& symbol : shard.symbols)
                if (symbol.name == selected.text || symbol.qualified_name == selected.text)
                    add_target(symbol.id);
        if (query.action == "references")
            for (auto at : uses.targets) {
                if (!normalize(at))
                    continue;
                semantic::records::reference row{.id = shard.unit.id + ":reference:" + std::to_string(shard.references.size()),
                                                 .ownership = own(),
                                                 .location = std::move(at),
                                                 .roles = {std::string{"reference"}},
                                                 .resolution = std::string{"unresolved"}};
                if (row.location.source)
                    if (const auto* owner = enclosing(*row.location.source))
                        row.from = owner->semantic_owner;
                const auto& targets = shard.unit.resolution->targets;
                if (targets.size() == 1) {
                    row.target = targets.front().symbol_id;
                    row.resolution = "resolved";
                } else if (!targets.empty()) {
                    for (const auto& target : targets)
                        row.candidates.push_back(target.symbol_id);
                    row.resolution = "ambiguous";
                }
                shard.references.push_back(std::move(row));
            }
        shard.unit.coverage.requested = request.demand;
        shard.unit.coverage.requested.selector = query.selector;
        shard.unit.coverage.available.push_back({.contexts = {shard.unit.context},
                                                 .facts = {std::string{"symbols"}, std::string{"definitions"}, std::string{"scopes"}, query.action.str()},
                                                 .selector = query.selector});
        evaluated_unit result;
        result.shards.push_back(std::move(shard));
        co_return result;
    }

} // namespace language
