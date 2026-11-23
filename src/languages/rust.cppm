export module rust;
import lsp;
import std;
import meta;
import json;
import content;
import coroutine;
import records;
import evaluation;
import native;
export import provider;

namespace {
    auto identifier_byte(unsigned char c) -> bool { return std::isalnum(c) || c == '_' || c >= 0x80; }
    auto continuation(unsigned char c) -> bool { return (c & 0xc0) == 0x80; }

    // Keep literals and comments intact so their contents cannot become attributes or item boundaries.
    auto source_tokens(std::string_view text) -> std::vector<std::string_view> {
        std::vector<std::string_view> out;
        while (!text.empty()) {
            std::size_t end = 1;
            const auto first = static_cast<unsigned char>(text.front());
            if (std::isspace(first)) {
                text.remove_prefix(1);
                continue;
            }
            if (text.starts_with("//")) {
                text.remove_prefix(std::min(text.find('\n'), text.size()));
                continue;
            }
            if (text.starts_with("/*")) {
                std::size_t depth = 1;
                end = 2;
                while (depth && end + 1 < text.size()) {
                    if (text.substr(end, 2) == "/*") {
                        ++depth;
                        end += 2;
                    } else if (text.substr(end, 2) == "*/") {
                        --depth;
                        end += 2;
                    } else
                        ++end;
                }
                text.remove_prefix(depth ? text.size() : end);
                continue;
            }
            const std::size_t raw = text.starts_with("br") || text.starts_with("cr") ? 2 : text.starts_with('r') ? 1 : 0;
            if (raw) {
                const auto quote = text.find_first_not_of('#', raw);
                if (quote != text.npos && text[quote] == '"') {
                    const auto close = '"' + std::string{text.substr(raw, quote - raw)};
                    const auto at = text.find(close, quote + 1);
                    end = at == text.npos ? text.size() : at + close.size();
                    out.push_back(text.substr(0, end));
                    text.remove_prefix(end);
                    continue;
                }
            }
            if (first == '"') {
                while (end < text.size()) {
                    const char c = text[end++];
                    if (c == '"')
                        break;
                    if (c == '\\' && end < text.size())
                        ++end;
                }
            } else if (first == '\'' && end < text.size()) {
                // A character literal closes after its character or escape. A lifetime has no closing apostrophe.
                auto close = end + 1;
                if (text[end] == '\\') {
                    const auto at = text.find('\'', end + 2);
                    if (at != text.npos && at - end <= 12 && text.substr(end, at - end).find_first_of("\r\n") == text.npos)
                        close = at;
                } else {
                    while (close < text.size() && continuation(static_cast<unsigned char>(text[close])))
                        ++close;
                }
                if (close < text.size() && text[close] == '\'')
                    end = close + 1;
            } else if (identifier_byte(first)) {
                while (end < text.size() && identifier_byte(static_cast<unsigned char>(text[end])))
                    ++end;
            }
            out.push_back(text.substr(0, end));
            text.remove_prefix(end);
        }
        return out;
    }

    auto string_literal(std::string_view literal) -> std::optional<std::string> {
        if (literal.starts_with('r')) {
            const auto quote = literal.find('"');
            if (quote != literal.npos && literal.size() > 2 * quote && literal[literal.size() - quote] == '"')
                return std::string{literal.substr(quote + 1, literal.size() - 2 * quote - 1)};
        }
        if (!literal.starts_with('"') || !literal.ends_with('"') || literal.size() < 2)
            return {};
        literal.remove_prefix(1);
        literal.remove_suffix(1);
        std::string out;
        while (!literal.empty()) {
            const auto escape = literal.find('\\');
            out += literal.substr(0, escape);
            if (escape == literal.npos)
                break;
            literal.remove_prefix(escape + 1);
            if (literal.empty())
                return {};
            const auto code = literal.front();
            literal.remove_prefix(1);
            switch (code) {
            case '\\':
            case '"':
            case '\'':
                out += code;
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case '\r':
            case '\n':
                literal.remove_prefix(std::min(literal.find_first_not_of(" \t\r\n"), literal.size()));
                break;
            case 'x':
            case 'u': {
                auto width = std::size_t{2};
                if (code == 'u') {
                    if (!literal.starts_with('{'))
                        return {};
                    literal.remove_prefix(1);
                    width = literal.find('}');
                }
                if (width > literal.size())
                    return {};
                const auto digits = literal.substr(0, width) | std::views::filter([](char c) { return c != '_'; }) | std::ranges::to<std::string>();
                std::uint32_t scalar = 0;
                const auto last = digits.data() + digits.size();
                const auto parsed = std::from_chars(digits.data(), last, scalar, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != last || !scalar || (code == 'x' && scalar >= 0x80) || scalar > 0x10ffff ||
                    (scalar >= 0xd800 && scalar <= 0xdfff))
                    return {};
                literal.remove_prefix(width + (code == 'u'));
                if (scalar < 0x80) {
                    out += static_cast<char>(scalar);
                    break;
                }
                const auto trailing = scalar < 0x800 ? 1u : scalar < 0x10000 ? 2u : 3u;
                constexpr std::array lead{0, 0xc0, 0xe0, 0xf0};
                out += static_cast<char>(lead[trailing] | (scalar >> (6 * trailing)));
                for (auto i = trailing; i > 0; --i)
                    out += static_cast<char>(0x80 | ((scalar >> (6 * (i - 1))) & 0x3f));
                break;
            }
            default:
                return {};
            }
        }
        return out;
    }

    struct function_header {
        std::optional<std::string> export_name;
        bool no_mangle = false;
        bool conditional_no_mangle = false;
        bool extern_abi = false;
    };

    auto header(std::span<const std::string_view> tokens) -> function_header {
        function_header out;
        auto take = [&](std::string_view token) {
            if (tokens.empty() || tokens.back() != token)
                return false;
            tokens = tokens.first(tokens.size() - 1);
            return true;
        };
        auto group = [&](std::string_view open, std::string_view close) -> std::optional<std::span<const std::string_view>> {
            if (!take(close))
                return {};
            const auto contents = tokens;
            std::size_t depth = 1;
            while (!tokens.empty()) {
                if (take(open)) {
                    if (!--depth)
                        return contents.subspan(tokens.size() + 1);
                } else if (take(close)) {
                    ++depth;
                } else {
                    tokens = tokens.first(tokens.size() - 1);
                }
            }
            return {};
        };
        if (!take("fn"))
            return out;
        while (!tokens.empty()) {
            if (take("pub") || take("const") || take("async") || take("unsafe") || take("safe") || take("default"))
                continue;
            if (take("extern")) {
                out.extern_abi = true;
                continue;
            }
            if (const auto token = tokens.back(); token.contains('"')) {
                take(token);
                if (!take("extern"))
                    return out;
                const auto abi = string_literal(token);
                out.extern_abi = abi == "C" || abi == "system" || abi == "C-unwind";
                continue;
            }
            if (tokens.back() == ")") {
                if (group("(", ")") && take("pub"))
                    continue;
                return out;
            }
            break;
        }
        // Walk only adjacent outer attributes. A previous item ends the walk at its ';' or '}'.
        while (!tokens.empty() && tokens.back() == "]") {
            auto attribute = group("[", "]");
            if (!attribute || !take("#"))
                break;
            auto record = [&](auto&& self, std::span<const std::string_view> value, bool conditional) -> void {
                if (value.size() >= 3 && value[1] == "(" && value.back() == ")") {
                    auto arguments = value.subspan(2, value.size() - 3);
                    if (value[0] == "unsafe") {
                        self(self, arguments, conditional);
                    } else if (value[0] == "cfg_attr") {
                        std::size_t start = 0, depth = 0;
                        for (std::size_t i = 0; i <= arguments.size(); ++i) {
                            if (i == arguments.size() || (depth == 0 && arguments[i] == ",")) {
                                if (start != 0)
                                    self(self, arguments.subspan(start, i - start), true);
                                start = i + 1;
                            } else if (arguments[i] == "(" || arguments[i] == "[" || arguments[i] == "{") {
                                ++depth;
                            } else if (depth && (arguments[i] == ")" || arguments[i] == "]" || arguments[i] == "}")) {
                                --depth;
                            }
                        }
                    }
                    return;
                }
                if (value.size() == 1 && value[0] == "no_mangle") {
                    if (conditional)
                        out.conditional_no_mangle = true;
                    else
                        out.no_mangle = true;
                }
                if (!conditional && value.size() == 3 && value[0] == "export_name" && value[1] == "=") {
                    if (auto name = string_literal(value[2]); name && !name->empty())
                        out.export_name = std::move(name);
                }
            };
            record(record, *attribute, false);
        }
        return out;
    }

    void linkage(semantic::shard& shard, const language::document& document) {
        std::unordered_map<semantic::symbol_id, const semantic::records::occurrence*> definitions;
        for (const auto& occurrence : shard.occurrences)
            if (occurrence.body && occurrence.location.source && occurrence.body->path == document.path && occurrence.location.source->path == document.path &&
                std::ranges::contains(occurrence.roles, std::string{"definition"}))
                definitions.try_emplace(occurrence.symbol, &occurrence);
        if (definitions.empty())
            return;

        const std::string_view text = document.text;
        // Each entry records the UTF-8 continuation bytes skipped before a scalar offset.
        std::vector<std::pair<std::uint64_t, std::size_t>> offsets;
        std::size_t extra = 0;
        for (std::size_t byte = 0; byte < text.size(); ++byte)
            if (continuation(static_cast<unsigned char>(text[byte]))) {
                ++extra;
                const auto scalar = byte + 1 - extra;
                if (offsets.empty() || offsets.back().first != scalar)
                    offsets.emplace_back(scalar, extra);
                else
                    offsets.back().second = extra;
            }
        auto byte_at = [&](std::uint64_t scalar) -> std::size_t {
            auto at = std::ranges::upper_bound(offsets, scalar, {}, &std::pair<std::uint64_t, std::size_t>::first);
            const auto skipped = at == offsets.begin() ? 0 : std::prev(at)->second;
            return scalar > text.size() - skipped ? text.size() : static_cast<std::size_t>(scalar) + skipped;
        };

        const auto tokens = source_tokens(text);
        auto token_at = [&](std::size_t byte) {
            return std::ranges::lower_bound(tokens, byte, {}, [&](std::string_view token) { return static_cast<std::size_t>(token.data() - text.data()); });
        };
        for (auto& symbol : shard.symbols) {
            if (symbol.language != "rust" || symbol.domain != "source" || symbol.kind != semantic::symbol_kind::make<"function">())
                continue;
            const auto found = definitions.find(symbol.id);
            if (found == definitions.end())
                continue;
            const auto& body = *found->second->body;
            const auto& name = *found->second->location.source;
            if (name.begin < body.begin || name.end > body.end)
                continue;
            const auto selection = token_at(byte_at(name.begin));
            const auto exported = header(std::span{tokens.begin(), selection});
            auto add = [&](std::string_view modifier) {
                if (!std::ranges::contains(symbol.modifiers, modifier))
                    symbol.modifiers.emplace_back(modifier);
            };
            if (exported.export_name) {
                symbol.extensions["native.linkage_name"] = *exported.export_name;
                add("export_name");
            } else if (exported.no_mangle) {
                symbol.extensions["native.linkage_name"] = symbol.name;
                add("no_mangle");
            }
            if (exported.conditional_no_mangle)
                add("cfg_attr(no_mangle)");
            if (exported.export_name || exported.no_mangle || exported.extern_abi)
                add("extern");
        }
    }
} // namespace

export namespace language {
    class rust_adapter : public lsp_adapter {
    public:
        using lsp_adapter::lsp_adapter;

    protected:
        auto do_evaluate(const evaluation_request& request) -> coroutine::task<evaluated_unit> override {
            auto result = co_await lsp_adapter::do_evaluate(request);
            const auto found = std::ranges::find(installed_.documents, request.unit.name, &document::path);
            if (found != installed_.documents.end())
                for (auto& shard : result.shards)
                    linkage(shard, *found);
            co_return result;
        }
    };
} // namespace language

namespace language {
    template <> struct server<"rust"> {
        static auto conventions() -> std::vector<semantic::language_conventions> {
            return {{.language = "rust",
                     .visibility = {"public", "crate", "super", "private", "local"},
                     .value_wrappers = {"core::result::Result",
                                        "std::result::Result",
                                        "Result",
                                        "core::option::Option",
                                        "std::option::Option",
                                        "Option",
                                        "alloc::boxed::Box",
                                        "std::boxed::Box",
                                        "Box",
                                        "alloc::sync::Arc",
                                        "std::sync::Arc",
                                        "Arc",
                                        "alloc::rc::Rc",
                                        "std::rc::Rc",
                                        "Rc",
                                        "alloc::borrow::Cow",
                                        "std::borrow::Cow",
                                        "Cow",
                                        "std::sync::Mutex",
                                        "std::sync::RwLock",
                                        "core::pin::Pin",
                                        "std::pin::Pin",
                                        "Pin",
                                        "core::task::Poll",
                                        "std::task::Poll",
                                        "Poll"},
                     .wrapper_arguments = "<",
                     .scope_edges = {"base", "import"},
                     .escape_types = {{"core::any::Any", "std::any::Any", "dyn Any", "*const core::ffi::c_void", "*mut core::ffi::c_void",
                                       "*const std::os::raw::c_void", "*mut std::os::raw::c_void"}},
                     .unsafe_casts = {{"core::mem::transmute", "std::mem::transmute", "core::mem::transmute_copy", "std::mem::transmute_copy"}}}};
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            auto out = source_channels();
            out.push_back(linkage_channel());
            return out;
        }
        using adapter = rust_adapter;
        struct options {
            std::string executable = "rust-analyzer";
            builds::environment env;
            std::vector<std::string> linked_projects;
            std::vector<std::string> features;
            std::optional<bool> all_features;
            std::optional<bool> no_default_features;
            std::optional<std::string> target;
            std::optional<bool> cargo_build_scripts;
            std::optional<bool> proc_macros;
            std::map<std::string, json::value> settings;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 1> documents{{{"rs", "rust"}}};
        static constexpr std::array<std::string_view, 0> inputs{};
        static constexpr std::array<std::string_view, 5> facts{"symbols", "definitions", "references", "scopes", "diagnostics"};
        static constexpr auto unit_kind = semantic::unit_kind::file;
        using emits = std::tuple<domain<"native">>;
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable}); }
        // Cargo and rustup can select executables from the installed project before indexing.
        static auto executes_project_code(const options&) -> bool { return true; }
        static void validate(const options& o);
        static auto launch(const options& o, const std::vector<builds::context>&) -> program;
    };
    static_assert(frontend<server<"rust">>);

    void server<"rust">::validate(const options& o) {
        if (o.executable.empty() || o.executable.contains('\0') || o.executable.contains("${"))
            throw std::invalid_argument("rust-analyzer executable is required");
        requirements(o);
        if (builds::in_project({o.executable}))
            content::checked_path(o.executable);
        builds::validate_environment(o.env);
        for (const auto& project : o.linked_projects)
            builds::relative(project);
        auto word = [](std::string_view value) {
            if (value.empty() || value.starts_with('-') || value.contains("${") || value.contains('\0') || value.find_first_of("\r\n") != value.npos)
                throw std::invalid_argument("rust-analyzer features and target must be single option values");
        };
        for (const auto& feature : o.features)
            word(feature);
        if (o.target)
            word(*o.target);
        for (const auto& [key, value] : o.settings)
            if (key.empty() || std::holds_alternative<std::monostate>(value.get()))
                throw std::invalid_argument("rust-analyzer settings require nonempty keys and non-null values");
    }

    auto server<"rust">::launch(const options& o, const std::vector<builds::context>&) -> program {
        program out{.argv = {o.executable}, .env = o.env};
        json::object init(o.settings.begin(), o.settings.end());
        auto object = [](json::value& value) -> json::object& {
            if (!std::holds_alternative<json::object>(value.get()))
                value = json::object{};
            return std::get<json::object>(value.get());
        };
        auto& cargo = object(init["cargo"]);
        object(cargo["buildScripts"])["enable"] = o.cargo_build_scripts.value_or(false);
        if (!o.features.empty())
            cargo["features"] = json::to_value(o.features);
        if (o.all_features) {
            cargo["allFeatures"] = *o.all_features;
            if (*o.all_features)
                cargo["features"] = "all";
            else if (const auto features = cargo.find("features"); features != cargo.end() && features->second == json::value{"all"})
                features->second = json::array{};
        }
        if (o.no_default_features)
            cargo["noDefaultFeatures"] = *o.no_default_features;
        if (o.target)
            cargo["target"] = *o.target;
        object(init["procMacro"])["enable"] = o.proc_macros.value_or(false);
        if (!o.linked_projects.empty())
            init["linkedProjects"] = json::to_value(o.linked_projects);
        // rust-analyzer can also run Cargo checks when it finishes indexing.
        if (!o.cargo_build_scripts.value_or(false))
            init["checkOnSave"] = false;
        auto settings = init;
        init["settings"] = std::move(settings);
        out.initialization = std::move(init);
        return out;
    }
} // namespace language
