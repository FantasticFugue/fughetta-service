export module lsp;
import std;
import native;
import coroutine;
import reflection;
import semantic;
import isolation;

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
    struct capabilities {
        std::optional<std::string> positionEncoding;
    };
    struct initialized {
        detail::capabilities capabilities;
    };
    struct configuration_items {
        std::vector<generic> items;
    };

    // LSP symbol kinds folded into the neutral kinds: namespace, type, function, variable, module and other.
    inline auto kind_name(int kind) -> std::string_view {
        switch (kind) {
        case 2: case 4: return "module";
        case 3: return "namespace";
        case 5: case 10: case 11: case 23: case 26: return "type";
        case 6: case 9: case 12: case 25: return "function";
        case 7: case 8: case 13: case 14: case 22: return "variable";
        default: return "other";
        }
    }
    // Kinds whose extent other declarations live in, and so make a scope.
    inline auto encloses(std::string_view kind) -> bool { return kind == "namespace" || kind == "type" || kind == "function" || kind == "module"; }

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

    inline constexpr std::size_t header_limit = 8 * 1024, body_limit = 64 * 1024 * 1024, depth_limit = 256;
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
    // An outline is converted only when its nesting and size are bounded, so parsing cannot run away.
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

// One adapter behind the neutral semantic model: it talks to a language server over its standard streams and
// reports outlines as semantic scopes and symbols. Spans name only a place, a path and Unicode scalar offsets, and
// the caller turns them into character identities of the version it analyzed. Server notifications, diagnostics
// among them, are not collected.
export namespace lsp {
    struct document {
        std::string path, language, text;
    };
    class session {
    public:
        session(native::scheduler io, io::command server) : io_(io) {
            server.interactive = true;
            child_.emplace(io::spawn_process(io, server));
        }

        // Starts the server on a workspace directory with its provider's initialization options and agrees on how positions are counted.
        auto start(const std::filesystem::path& root, detail::generic initialization) -> coroutine::task<> {
            using detail::object, detail::text;
            auto held = co_await lock_.lock();
            root_ = std::filesystem::absolute(root);
            auto uri = detail::uri(root_);
            auto reply = co_await call("initialize", object({
                {"processId", detail::null},
                {"rootUri", text(uri)},
                {"initializationOptions", std::move(initialization)},
                {"workspaceFolders", detail::generic::Array{object({{"uri", text(uri)}, {"name", text(root_.filename().string())}})}},
                {"capabilities", object({{"general", object({{"positionEncodings", detail::generic::Array{text("utf-32"), text("utf-16")}}})},
                                         {"textDocument", object({{"documentSymbol", object({{"hierarchicalDocumentSymbolSupport", detail::generic{true}}})}})}})}}));
            auto parsed = rfl::from_generic<detail::initialized, rfl::DefaultIfMissing>(reply);
            if (!parsed)
                throw std::runtime_error("language server answered initialize with an unreadable result");
            utf16_ = parsed->capabilities.positionEncoding.value_or("utf-16") != "utf-32";
            co_await notify("initialized", object({}));
        }

        auto describe(const document& d) -> coroutine::task<semantic::outline> {
            using detail::object, detail::text;
            auto held = co_await lock_.lock();
            auto uri = detail::uri(root_ / d.path);
            co_await notify("textDocument/didOpen", object({{"textDocument", object({{"uri", text(uri)}, {"languageId", text(d.language)},
                                                                                     {"version", detail::generic{std::int64_t{1}}}, {"text", text(d.text)}})}}));
            std::optional<detail::generic> reply;
            std::exception_ptr failed;
            try {
                reply = co_await call("textDocument/documentSymbol", object({{"textDocument", object({{"uri", text(uri)}})}}));
            } catch (...) {
                failed = std::current_exception();
            }
            co_await notify("textDocument/didClose", object({{"textDocument", object({{"uri", text(uri)}})}}));
            if (failed)
                std::rethrow_exception(failed);
            semantic::outline out;
            if (std::holds_alternative<std::nullopt_t>(reply->get()))
                co_return out;
            std::size_t seen = 0;
            if (!detail::bounded(*reply, 0, seen))
                throw std::runtime_error("language server outline is nested too deeply or too large");
            auto symbols = rfl::from_generic<std::vector<detail::symbol>>(*reply);
            if (!symbols)
                throw std::runtime_error("language server returned no hierarchical outline: " + std::string{symbols.error().what()});
            detail::lines lines{d.text, utf16_};
            // A symbol whose place is empty or outside the text read is left out with everything it contains.
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
            co_return out;
        }

        // A server that does not finish shutting down within five seconds is killed.
        // A session still busy with another request is ended at once: killing the server ends that request too.
        auto stop() -> coroutine::task<int> {
            auto held = lock_.try_lock();
            if (!held) {
                child_->kill();
                co_return -1;
            }
            std::optional<int> code;
            std::exception_ptr failed;
            try {
                if (auto finished = co_await coroutine::deadline(io_, std::chrono::seconds{5}, [](session& self) -> coroutine::task<int> {
                        (void)co_await self.call("shutdown", detail::null);
                        co_await self.notify("exit", detail::null);
                        self.child_->close_input();
                        co_return co_await io::async_wait(*self.child_);
                    }(*this)))
                    code = static_cast<int>(*finished);
            } catch (...) {
                failed = std::current_exception();
            }
            if (!code)
                child_->kill();
            if (failed)
                std::rethrow_exception(failed);
            co_return code.value_or(-1);
        }

    private:
        auto send(const detail::generic& body) -> coroutine::task<> {
            auto text = rfl::json::write(body);
            auto frame = std::format("Content-Length: {}\r\n\r\n{}", text.size(), text);
            co_await io::async_write(*child_, std::as_bytes(std::span{frame}));
        }
        auto notify(std::string method, detail::generic params) -> coroutine::task<> {
            co_await send(detail::object({{"jsonrpc", detail::text("2.0")}, {"method", detail::text(std::move(method))}, {"params", std::move(params)}}));
        }
        // Server requests are answered as a client without workspace settings would; this adapter implements no other method.
        auto answer(const detail::message& request) -> coroutine::task<> {
            using detail::object, detail::text;
            auto& method = *request.method;
            detail::generic reply;
            if (method == "workspace/configuration") {
                std::size_t count = 0;
                if (request.params)
                    if (auto items = rfl::from_generic<detail::configuration_items, rfl::DefaultIfMissing>(*request.params))
                        count = items->items.size();
                reply = object({{"jsonrpc", text("2.0")}, {"id", *request.id}, {"result", detail::generic::Array(count, detail::null)}});
            } else if (method == "window/workDoneProgress/create" || method == "client/registerCapability" || method == "client/unregisterCapability")
                reply = object({{"jsonrpc", text("2.0")}, {"id", *request.id}, {"result", detail::null}});
            else
                reply = object({{"jsonrpc", text("2.0")}, {"id", *request.id},
                                {"error", object({{"code", detail::generic{std::int64_t{-32601}}}, {"message", text("method not supported")}})}});
            co_await send(reply);
        }
        auto call(std::string method, detail::generic params) -> coroutine::task<detail::generic> {
            auto id = ++next_;
            co_await send(detail::object({{"jsonrpc", detail::text("2.0")}, {"id", detail::generic{id}}, {"method", detail::text(method)}, {"params", std::move(params)}}));
            for (;;) {
                auto incoming = co_await receive();
                if (incoming.method) {
                    if (incoming.id)
                        co_await answer(incoming);
                    continue;
                }
                if (!incoming.id || !detail::same_id(*incoming.id, id))
                    continue;
                if (incoming.error)
                    throw std::runtime_error(std::format("language server refused {}: {}", method, incoming.error->message));
                co_return incoming.result.value_or(detail::null);
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
                        buffer_.erase(0, body + *length);
                        if (!parsed)
                            throw std::runtime_error("language server sent an unreadable message");
                        co_return std::move(*parsed);
                    }
                }
                auto count = co_await io::async_read_some(*child_, net::buffer(chunk_));
                if (count == 0)
                    throw std::runtime_error("language server closed its output");
                buffer_.append(reinterpret_cast<const char*>(chunk_.data()), count);
            }
        }

        native::scheduler io_;
        std::optional<native::process> child_;
        coroutine::mutex lock_;
        std::filesystem::path root_;
        std::string buffer_;
        std::array<std::byte, 65536> chunk_;
        std::int64_t next_ = 0;
        bool utf16_ = true;
    };

    // What one survey may take: documents and their bytes are checked before any is read, records as the server reports them.
    struct limits {
        std::size_t documents = 2000;
        std::uint64_t document_bytes = 4u << 20, bytes = 64u << 20;
        std::size_t records = 200000;
        std::chrono::seconds per_document{30}, total{600};
    };

    // Asks the server for each document's outline; a document the server cannot describe in time ends the survey with an error.
    auto survey(native::scheduler io, io::command server, rfl::Generic initialization, std::filesystem::path tree, std::vector<document> documents,
                limits bounds = {}) -> coroutine::task<semantic::outline> {
        if (documents.size() > bounds.documents)
            throw std::runtime_error(std::format("the survey covers at most {} documents", bounds.documents));
        session session{io, std::move(server)};
        semantic::outline out;
        std::exception_ptr failed;
        auto until = std::chrono::steady_clock::now() + bounds.total;
        auto left = [&] { return std::max(std::chrono::seconds{1}, std::chrono::duration_cast<std::chrono::seconds>(until - std::chrono::steady_clock::now())); };
        try {
            if (!co_await coroutine::deadline(io, std::min(bounds.per_document, left()), session.start(tree, std::move(initialization))))
                throw std::runtime_error("the language server did not start in time");
            std::size_t records = 0;
            for (auto& d : documents) {
                auto described = co_await coroutine::deadline(io, std::min(bounds.per_document, left()),
                                                              session.describe(d));
                if (!described)
                    throw std::runtime_error("the language server did not describe " + d.path + " in time");
                records += described->scopes.size() + described->symbols.size();
                if (records > bounds.records)
                    throw std::runtime_error(std::format("the language server reported more than {} scopes and symbols", bounds.records));
                std::ranges::move(described->scopes, std::back_inserter(out.scopes));
                std::ranges::move(described->symbols, std::back_inserter(out.symbols));
            }
        } catch (...) {
            failed = std::current_exception();
        }
        (void)co_await session.stop();
        if (failed)
            std::rethrow_exception(failed);
        co_return out;
    }

    auto bounded_survey(native::scheduler io, io::command server, rfl::Generic initialization, std::filesystem::path tree,
                        std::vector<document> documents, limits bounds, isolation::pool& pool) -> coroutine::task<semantic::outline> {
        auto resource = pool.start({}, {});
        resource->concurrency(1);
        server.isolation = resource->target();
        auto token = co_await coroutine::current_token;
        std::optional<semantic::outline> result;
        std::exception_ptr error;
        try {
            result = co_await coroutine::under(token, survey(io, std::move(server), std::move(initialization), std::move(tree), std::move(documents), bounds));
        } catch (...) { error = std::current_exception(); }
        resource->close();
        if (error) std::rethrow_exception(error);
        if (!result) co_await coroutine::stop;
        co_return std::move(*result);
    }

} // namespace lsp
