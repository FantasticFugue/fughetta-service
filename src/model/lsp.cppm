export module lsp;
import utility;
import std;
import host;
import native;
import coroutine;
import reflection;
import semantic;

// The Language Server Protocol is spoken only inside this module.
namespace lsp::detail {
    using generic = rfl::Generic;
    inline auto object(std::initializer_list<std::pair<std::string, generic>> fields) -> generic {
        generic::Object out;
        for (auto& [k, v] : fields)
            out.insert(k, v);
        return out;
    }
    inline auto text(std::string value) -> generic { return generic{std::move(value)}; }
    inline const generic null{std::nullopt};

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
        std::optional<generic> severity, code;
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
        std::optional<generic> id;
        std::optional<std::string> method;
        std::optional<generic> params;
        std::optional<generic> result;
        std::optional<failure> error;
    };
    // What the server's answer to `initialize` says it offers. What it does not name, it does not offer.
    struct capabilities {
        std::optional<std::string> positionEncoding;
        std::optional<generic> documentSymbolProvider, diagnosticProvider, definitionProvider, referencesProvider;
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
                util::rethrow_exception(error);
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
        std::vector<generic> items;
    };

    // A capability is offered when it is present and neither `false` nor `null`.
    inline auto offered(const std::optional<generic>& capability) -> bool {
        if (!capability || std::holds_alternative<std::nullopt_t>(capability->get()))
            return false;
        if (auto* flag = std::get_if<bool>(&capability->get()))
            return *flag;
        return true;
    }
    inline auto number(const std::optional<generic>& value) -> std::optional<std::int64_t> {
        if (!value)
            return std::nullopt;
        if (auto* integer = std::get_if<std::int64_t>(&value->get()))
            return *integer;
        if (auto* real = std::get_if<double>(&value->get()))
            return static_cast<std::int64_t>(*real);
        return std::nullopt;
    }
    inline auto field_of(const generic::Object& fields, std::string_view name) -> const generic* {
        auto found = std::ranges::find(fields, name, &generic::Object::value_type::first);
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
    inline auto severity_name(const std::optional<generic>& severity) -> std::string {
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
        auto text = path.generic_string();
        std::string out = "file:";
        if (text.starts_with("//"))
            text.erase(0, 2), out += "//";
        else
            out += text.starts_with('/') ? "//" : "///";
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':')
                out += static_cast<char>(c);
            else
                out += std::format("%{:02X}", c);
        }
        return out;
    }
    // The absolute path a `file:` URI names, the inverse of `uri`; nothing for another scheme or an undecodable escape.
    inline auto path_of(std::string_view text) -> std::optional<std::filesystem::path> {
        if (!text.starts_with("file:"))
            return std::nullopt;
        auto encoded = text.substr(5);
        if (encoded.starts_with("///"))
            encoded.remove_prefix(2);
        std::string decoded;
        decoded.reserve(encoded.size());
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '%') {
                unsigned byte = 0;
                if (i + 3 > encoded.size())
                    return std::nullopt;
                auto* end = encoded.data() + i + 3;
                auto [last, error] = std::from_chars(encoded.data() + i + 1, end, byte, 16);
                if (error != std::errc{} || last != end)
                    return std::nullopt;
                decoded += static_cast<char>(byte);
                i += 2;
                continue;
            }
            decoded += encoded[i];
        }
        // The authority is empty here, so the local path follows its slashes: 'file:///x' names '/x'.
        if (decoded.empty() || decoded.front() != '/')
            return std::nullopt;
        // On Windows 'file:///C:/x' names 'C:/x'.
        if (host::os == "windows" && decoded.size() >= 4 && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':' && decoded[3] == '/')
            decoded.erase(0, 1);
        return std::filesystem::path{decoded};
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
    inline auto bounded(const generic& value, std::size_t depth, std::size_t& seen) -> bool {
        if (depth > 4 * depth_limit || ++seen > 1'000'000)
            return false;
        if (auto* list = std::get_if<generic::Array>(&value.get()))
            return std::ranges::all_of(*list, [&](auto& v) { return bounded(v, depth + 1, seen); });
        if (auto* fields = std::get_if<generic::Object>(&value.get()))
            return std::ranges::all_of(*fields, [&](auto& field) { return bounded(field.second, depth + 1, seen); });
        return true;
    }
    inline auto same_id(const generic& id, std::int64_t expected) -> bool {
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
    struct query_error : std::invalid_argument, util::retained_error<query_error> {
        using std::invalid_argument::invalid_argument;
    };

    struct document {
        std::string path, language, text;
    };
    class session {
    public:
        session(native::scheduler io, io::command server) : io_(io) {
            server.interactive = true;
            child_.emplace(io::spawn_process(io, server));
        }

        // Starts the server on a workspace directory with its provider's initialization options, offering the capabilities this
        // client uses, and agrees on how positions are counted. The `settings` of the options also answer the server's
        // workspace/configuration requests, as editors do.
        auto start(const std::filesystem::path& root, detail::generic initialization) -> coroutine::task<> {
            using detail::object, detail::text;
            auto held = co_await lock_.lock();
            if (started_)
                throw std::logic_error("language server session was started twice");
            root_ = std::filesystem::absolute(root);
            if (auto* fields = std::get_if<detail::generic::Object>(&initialization.get()))
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
                        {"workspaceFolders", detail::generic::Array{object({{"uri", text(uri)}, {"name", text(root_.filename().string())}})}},
                        {"capabilities",
                         object({{"general", object({{"positionEncodings", detail::generic::Array{text("utf-32"), text("utf-16")}}})},
                                 {"workspace", object({{"configuration", detail::generic{true}}, {"workspaceFolders", detail::generic{true}}})},
                                 // Diagnostics are pulled where the server offers it, and pushed otherwise; the pull is announced, since servers
                                 // such as ty and ruff pull only for a client that says it can.
                                 {"textDocument", object({{"synchronization", object({{"dynamicRegistration", detail::generic{false}}})},
                                                          {"documentSymbol", object({{"hierarchicalDocumentSymbolSupport", detail::generic{true}}})},
                                                          {"diagnostic", object({{"dynamicRegistration", detail::generic{false}}})},
                                                          {"publishDiagnostics", object({})}})}})}}));
            auto parsed = rfl::from_generic<detail::initialized, rfl::DefaultIfMissing>(reply);
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

        // Reports what the server offers about one document.
        auto describe(const document& d) -> coroutine::task<semantic::outline> {
            auto held = co_await lock_.lock();
            co_return co_await reported(d);
        }

        auto lookup(const semantic::query& query) -> coroutine::task<std::vector<semantic::location>> {
            auto held = co_await lock_.lock();
            using detail::object, detail::text;
            if (query.action != "definitions" && query.action != "references")
                throw query_error("a location query names definitions or references");
            if (query.path.empty() || query.symbol.empty() || (query.line && !*query.line))
                throw query_error("definitions and references require path, symbol and an optional positive line");
            if (!(query.action == "definitions" ? definitions_ : references_))
                throw std::runtime_error("language server does not offer " + query.action);
            auto source = documents_.find(query.path);
            if (source == documents_.end())
                throw query_error("language query path is not an analyzed source file: " + query.path);
            auto identifier = object({{"textDocument", object({{"uri", text(detail::uri(root_ / query.path))}})}});
            std::vector<detail::position> candidates;
            if (symbols_) {
                auto answer = co_await call("textDocument/documentSymbol", identifier);
                std::size_t seen = 0;
                if (!detail::bounded(answer, 0, seen))
                    throw std::runtime_error("language symbol response exceeds the record limit");
                if (auto symbols = rfl::from_generic<std::vector<detail::symbol>>(answer)) {
                    auto visit = [&](this auto&& self, const detail::symbol& symbol, std::string prefix) -> void {
                        auto qualified = prefix.empty() ? symbol.name : prefix + "::" + symbol.name;
                        if ((query.symbol == symbol.name || query.symbol == qualified) && (!query.line || symbol.selectionRange.start.line + 1 == *query.line))
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
                for (std::size_t at = 0; (at = input.find(query.symbol, at)) != std::string::npos; at += query.symbol.size()) {
                    auto end = at + query.symbol.size();
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
                    if (!query.line || position.line + 1 == *query.line)
                        candidates.push_back(position);
                }
            }
            if (candidates.size() != 1)
                throw query_error(candidates.empty() ? "symbol was not found in the selected file/line"
                                                     : "symbol is ambiguous; select its occurrence with line");
            auto position = candidates.front();
            auto params = object({{"textDocument", object({{"uri", text(detail::uri(root_ / query.path))}})},
                                  {"position", object({{"line", detail::generic{static_cast<std::int64_t>(position.line)}},
                                                       {"character", detail::generic{static_cast<std::int64_t>(position.character)}}})}});
            if (query.action == "references")
                params = object({{"textDocument", object({{"uri", text(detail::uri(root_ / query.path))}})},
                                 {"position", object({{"line", detail::generic{static_cast<std::int64_t>(position.line)}},
                                                      {"character", detail::generic{static_cast<std::int64_t>(position.character)}}})},
                                 {"context", object({{"includeDeclaration", detail::generic{true}}})}});
            auto answer = co_await call(query.action == "definitions" ? "textDocument/definition" : "textDocument/references", std::move(params));
            std::size_t seen = 0;
            if (!detail::bounded(answer, 0, seen))
                throw std::runtime_error("language locations exceed the response limit");
            std::vector<detail::location> locations;
            if (std::holds_alternative<std::nullopt_t>(answer.get()))
                co_return std::vector<semantic::location>{};
            if (auto list = rfl::from_generic<std::vector<detail::location>>(answer))
                locations = std::move(*list);
            else if (auto links = rfl::from_generic<std::vector<detail::location_link>, rfl::DefaultIfMissing>(answer)) {
                for (const auto& link : *links)
                    locations.push_back({link.targetUri, link.targetSelectionRange});
            } else if (auto one = rfl::from_generic<detail::location>(answer))
                locations.push_back(std::move(*one));
            else
                throw std::runtime_error("language server returned unreadable locations");
            std::vector<semantic::location> result;
            for (const auto& location : locations) {
                auto path = detail::path_of(location.uri);
                if (!path)
                    throw std::runtime_error("language server returned a non-file location");
                auto relative = path->lexically_relative(root_).generic_string();
                auto found = documents_.find(relative);
                std::string external;
                if (found == documents_.end()) {
                    std::error_code error;
                    auto bytes = std::filesystem::file_size(*path, error);
                    if (error || bytes > (4u << 20))
                        throw std::runtime_error("cannot read the language server's target file within the document limit: " + path->string());
                    std::ifstream input{*path, std::ios::binary};
                    external.assign(std::istreambuf_iterator<char>{input}, {});
                    if (external.size() != bytes || external.contains('\0'))
                        throw std::runtime_error("language target is not readable text: " + path->string());
                }
                detail::lines lines{found == documents_.end() ? std::string_view{external} : std::string_view{found->second.value.text}, utf16_};
                auto first = lines.offset(location.range.start), last = lines.offset(location.range.end);
                auto first_line = lines.offset({location.range.start.line, 0}), last_line = lines.offset({location.range.end.line, 0});
                if (!first || !last || !first_line || !last_line || *last < *first)
                    throw std::runtime_error("language server returned a range outside its document");
                std::string context;
                if (auto outline = outlines_.find(relative); outline != outlines_.end()) {
                    std::uint64_t width = std::numeric_limits<std::uint64_t>::max();
                    for (const auto& scope : outline->second.scopes)
                        if (scope.range.place && scope.range.place->begin <= *first && scope.range.place->end >= *last &&
                            scope.range.place->end - scope.range.place->begin < width) {
                            width = scope.range.place->end - scope.range.place->begin;
                            context = scope.symbol;
                        }
                }
                result.push_back({relative.starts_with("../") || relative == ".." ? path->generic_string() : relative, location.range.start.line + 1,
                                  *first - *first_line + 1, location.range.end.line + 1, *last - *last_line + 1, std::move(context)});
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
                outlines_.erase(it->first);
                it = documents_.erase(it);
            }
            for (const auto& d : documents)
                co_await update(d);
            detail::generic::Array events;
            for (const auto& path : changed)
                events.push_back(object({{"uri", text(detail::uri(root_ / path))}, {"type", detail::generic{std::int64_t{2}}}}));
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
                failed = util::current_exception();
            }
            co_await abort();
            if (failed)
                util::rethrow_exception(failed);
            co_return code.value_or(-1);
        }

    private:
        auto send(const detail::generic& body) -> coroutine::task<> {
            auto text = rfl::json::write(body);
            auto frame = std::format("Content-Length: {}\r\n\r\n{}", text.size(), text);
            auto turn = co_await writing_.lock();
            co_await io::async_write(*child_, std::as_bytes(std::span{frame}));
        }
        auto notify(std::string method, detail::generic params) -> coroutine::task<> {
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
                ended_ = util::make_exception_ptr(std::runtime_error("language reader stopped"));
            } catch (...) {
                ended_ = util::current_exception();
            }
            if (pending_) {
                pending_->error = ended_;
                pending_->wake();
            }
        }

        auto call(std::string method, detail::generic params) -> coroutine::task<detail::generic> {
            if (ended_)
                util::rethrow_exception(ended_);
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
                {{"jsonrpc", detail::text("2.0")}, {"id", detail::generic{id}}, {"method", detail::text(method)}, {"params", std::move(params)}}));
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
            detail::generic reply = object({{"jsonrpc", text("2.0")}, {"id", *request.id}, {"result", detail::null}});
            if (method == "workspace/configuration") {
                std::size_t count = 0;
                if (request.params)
                    if (auto items = rfl::from_generic<detail::configuration_items, rfl::DefaultIfMissing>(*request.params))
                        count = items->items.size();
                reply = object({{"jsonrpc", text("2.0")}, {"id", *request.id}, {"result", detail::generic::Array(count, settings_)}});
            } else if (method != "client/registerCapability" && method != "client/unregisterCapability" && method != "window/workDoneProgress/create")
                reply = object({{"jsonrpc", text("2.0")},
                                {"id", *request.id},
                                {"error", object({{"code", detail::generic{std::int64_t{-32601}}}, {"message", text("method not supported")}})}});
            co_await send(reply);
        }
        // Ignore delayed diagnostics from an earlier document version.
        void pushed(const detail::generic& params) {
            auto parsed = rfl::from_generic<detail::published, rfl::DefaultIfMissing>(params);
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
                        auto parsed = rfl::json::read<detail::message, rfl::DefaultIfMissing>(text);
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

        // Point diagnostics anchor to the adjacent character so source edits can track them.
        void located(const document& d, const std::vector<detail::diagnostic>& items, std::vector<semantic::diagnostic>& out) const {
            detail::lines lines{d.text, utf16_};
            auto text = static_cast<std::uint64_t>(std::ranges::count_if(d.text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
            for (auto& item : items) {
                if (out.size() >= detail::document_records)
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
                out.push_back({.range = semantic::span{.place = semantic::place{d.path, first, last}},
                               .severity = detail::severity_name(item.severity),
                               .message = detail::message_text(item)});
            }
        }
        // A full report must include diagnostics when the request has no previous result ID.
        void converted(const detail::generic& answer, const document& d, std::vector<semantic::diagnostic>& out) const {
            std::size_t seen = 0;
            if (!detail::bounded(answer, 0, seen))
                throw std::runtime_error("language server diagnostic report is nested too deeply or too large");
            auto parsed = rfl::from_generic<detail::report_of, rfl::DefaultIfMissing>(answer);
            if (!parsed)
                throw std::runtime_error("language server answered an unreadable diagnostic report: " + std::string{parsed.error().what()});
            if (parsed->kind != "full")
                throw std::runtime_error("language server answered a " + parsed->kind + " diagnostic report, which holds no diagnostics");
            located(d, parsed->items.value_or(std::vector<detail::diagnostic>{}), out);
        }
        // Turns one document symbol answer into scopes and symbols of the document; a symbol whose place is empty or outside
        // the text read is left out with everything it contains.
        void described(const detail::generic& answer, const document& d, semantic::outline& out) const {
            if (std::holds_alternative<std::nullopt_t>(answer.get()))
                return;
            std::size_t seen = 0;
            if (!detail::bounded(answer, 0, seen))
                throw std::runtime_error("language server outline is nested too deeply or too large");
            auto symbols = rfl::from_generic<std::vector<detail::symbol>>(answer);
            if (!symbols)
                throw std::runtime_error("language server returned no hierarchical outline: " + std::string{symbols.error().what()});
            detail::lines lines{d.text, utf16_};
            auto span = [&](const detail::range& r) -> std::optional<semantic::span> {
                auto begin = lines.offset(r.start), end = lines.offset(r.end);
                if (!begin || !end || *begin >= *end)
                    return std::nullopt;
                return semantic::span{.place = semantic::place{d.path, *begin, *end}};
            };
            // A symbol's identity is its document and qualified name, so it survives edits elsewhere in the file; same-named
            // siblings (overloads) are told apart by their order among themselves.
            std::map<std::string, std::size_t> named;
            auto visit = [&](this auto& self, const detail::symbol& s, const std::string& scope, const std::string& qualified, std::size_t depth) -> void {
                auto range = span(s.range);
                auto name = span(s.selectionRange);
                if (!range || !name || depth > detail::depth_limit)
                    return;
                auto path = qualified.empty() ? s.name : qualified + "::" + s.name;
                auto nth = named[path]++;
                auto id = d.path + "#" + path + (nth ? "~" + std::to_string(nth) : std::string{});
                std::string kind{detail::kind_name(s.kind)};
                out.symbols.push_back({.id = id, .name = s.name, .kind = kind, .scope = scope, .definitions = {*name}});
                auto inner = scope;
                if (detail::encloses(kind)) {
                    out.scopes.push_back({.id = id, .parent = scope, .kind = kind, .symbol = id, .range = *range});
                    inner = id;
                }
                if (s.children)
                    for (auto& child : *s.children)
                        self(child, inner, path, depth + 1);
            };
            for (auto& s : *symbols)
                visit(s, "", "", 0);
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
                             object({{"uri", uri}, {"languageId", text(d.language)}, {"version", detail::generic{version}}, {"text", text(d.text)}})}}));
            } else if (found->second.value.text != d.text) {
                auto version = ++version_;
                found->second = opened{d, version};
                outlines_.erase(d.path);
                co_await notify("textDocument/didChange", object({{"textDocument", object({{"uri", uri}, {"version", detail::generic{version}}})},
                                                                  {"contentChanges", detail::generic::Array{object({{"text", text(d.text)}})}}}));
            }
        }

        auto reported(const document& d) -> coroutine::task<semantic::outline> {
            if (!symbols_ && !pull_)
                throw std::runtime_error(
                    std::format("the language server declares neither documentSymbolProvider nor diagnosticProvider, so {} cannot be described", d.path));
            using detail::object, detail::text;
            co_await update(d);
            auto identifier = object({{"textDocument", object({{"uri", text(detail::uri(root_ / d.path))}})}});
            semantic::outline out;
            if (symbols_)
                described(co_await call("textDocument/documentSymbol", identifier), d, out);
            if (pull_)
                converted(co_await call("textDocument/diagnostic", identifier), d, out.diagnostics);
            else {
                const auto& latest = documents_.at(d.path);
                located(d, latest.diagnostics, out.diagnostics);
                out.complete = latest.current_diagnostics;
                out.cacheable = false; // A document version cannot certify cross-file dependencies of pushed diagnostics.
            }
            outlines_[d.path] = out;
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
        std::map<std::string, semantic::outline> outlines_;
        detail::generic settings_ = detail::null;
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

    // Asks the server about each document; a document the server cannot describe in time ends the survey with an error.
    auto survey(native::scheduler io, io::command server, rfl::Generic initialization, std::filesystem::path tree, std::vector<document> documents,
                limits bounds = {}) -> coroutine::task<semantic::outline> {
        if (documents.size() > bounds.documents)
            throw std::runtime_error(std::format("the survey covers at most {} documents", bounds.documents));
        session session{io, std::move(server)};
        semantic::outline out;
        std::exception_ptr failed;
        auto until = std::chrono::steady_clock::now() + bounds.total;
        auto left = [&] {
            return std::max(std::chrono::seconds{1}, std::chrono::duration_cast<std::chrono::seconds>(until - std::chrono::steady_clock::now()));
        };
        auto token = co_await coroutine::current_token;
        bool finished = false;
        auto work = [&]() -> coroutine::task<> {
            if (!co_await coroutine::deadline(io, std::min(bounds.per_document, left()), session.start(tree, std::move(initialization))))
                throw std::runtime_error("the language server did not start in time");
            std::size_t records = 0;
            for (auto& d : documents) {
                auto described = co_await coroutine::deadline(io, std::min(bounds.per_document, left()), session.describe(d));
                if (!described)
                    throw std::runtime_error("the language server did not describe " + d.path + " in time");
                records += described->scopes.size() + described->symbols.size() + described->diagnostics.size();
                if (records > bounds.records)
                    throw std::runtime_error(std::format("the language server reported more than {} scopes, symbols and diagnostics", bounds.records));
                std::ranges::move(described->scopes, std::back_inserter(out.scopes));
                std::ranges::move(described->symbols, std::back_inserter(out.symbols));
                std::ranges::move(described->diagnostics, std::back_inserter(out.diagnostics));
            }
        };
        try {
            finished = co_await coroutine::under(token, work());
        } catch (...) {
            failed = util::current_exception();
        }
        co_await coroutine::under(coroutine::stop_token{}, session.abort());
        if (!finished && !failed)
            co_await coroutine::stop;
        if (failed)
            util::rethrow_exception(failed);
        co_return out;
    }

} // namespace lsp
