module;
#include <tree_sitter/api.h>

extern "C" {
    const TSLanguage* tree_sitter_cpp(void);
    const TSLanguage* tree_sitter_rust(void);
    const TSLanguage* tree_sitter_python(void);
    const TSLanguage* tree_sitter_javascript(void);
    const TSLanguage* tree_sitter_typescript(void);
    const TSLanguage* tree_sitter_tsx(void);
    const TSLanguage* tree_sitter_kotlin(void);
    const TSLanguage* tree_sitter_java(void);
    const TSLanguage* tree_sitter_c_sharp(void);
    const TSLanguage* tree_sitter_scala(void);
    const TSLanguage* tree_sitter_fsharp(void);
    const TSLanguage* tree_sitter_fsharp_signature(void);
}

export module syntax;
import std;

namespace syntax {
    namespace {
        struct scope_rule {
            std::string_view node_type, kind;
            bool function = false;
        };

        struct grammar {
            std::span<const std::string_view> extensions;
            const TSLanguage* (*language)();
            std::span<const scope_rule> scopes;
            std::span<const std::string_view> comments;
            std::string_view body_type{};
            std::string_view (*scope_name)(TSNode, const scope_rule&, std::string_view) = nullptr;
            TSNode (*function_body)(TSNode) = nullptr;
            std::span<const std::string_view> opaque_nodes{};
            std::uint32_t (*directive_end)(TSNode, std::string_view) = nullptr;
            bool (*reusable_comment)(std::string_view) = nullptr;
            std::string_view comment_sensitive_sequence{};
        };

        using tree_ptr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

        auto parse(const grammar& config, std::string_view text) -> tree_ptr {
            tree_ptr tree{nullptr, ts_tree_delete};
            if (!std::in_range<std::uint32_t>(text.size()))
                return tree;
            const std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser{ts_parser_new(), ts_parser_delete};
            if (parser && ts_parser_set_language(parser.get(), config.language()))
                tree.reset(ts_parser_parse_string(parser.get(), nullptr, text.empty() ? "" : text.data(), static_cast<std::uint32_t>(text.size())));
            return tree;
        }

        class cursor {
            TSTreeCursor position;
            bool active = true;

          public:
            explicit cursor(TSNode root) : position(ts_tree_cursor_new(root)) {}
            cursor(const cursor&) = delete;
            auto operator=(const cursor&) -> cursor& = delete;
            ~cursor() { ts_tree_cursor_delete(&position); }

            auto node() const -> TSNode { return ts_tree_cursor_current_node(&position); }
            explicit operator bool() const { return active; }

            void next(bool descend = true) {
                if (descend && ts_tree_cursor_goto_first_child(&position))
                    return;
                do {
                    if (ts_tree_cursor_goto_next_sibling(&position))
                        return;
                } while (ts_tree_cursor_goto_parent(&position));
                active = false;
            }
        };

        auto slice(std::string_view text, TSNode node) -> std::string_view {
            const auto begin = ts_node_start_byte(node);
            return text.substr(begin, ts_node_end_byte(node) - begin);
        }

        auto field(TSNode node, std::string_view name) -> TSNode {
            auto child = ts_node_child_by_field_name(node, name.data(), static_cast<std::uint32_t>(name.size()));
            if (ts_node_is_null(child) || !ts_node_is_extra(child))
                return child;

            // Inherited fields can resolve to extras in direct lookup. The cursor identifies the structural child.
            auto position = ts_tree_cursor_new(node);
            child = {};
            if (ts_tree_cursor_goto_first_child(&position)) {
                do {
                    const auto* label = ts_tree_cursor_current_field_name(&position);
                    auto candidate = ts_tree_cursor_current_node(&position);
                    if (label && name == label && !ts_node_is_extra(candidate)) {
                        child = candidate;
                        break;
                    }
                } while (ts_tree_cursor_goto_next_sibling(&position));
            }
            ts_tree_cursor_delete(&position);
            return child;
        }

        auto named_child(TSNode node, std::string_view type) -> TSNode {
            for (std::uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
                const auto child = ts_node_named_child(node, i);
                if (std::string_view{ts_node_type(child)} == type)
                    return child;
            }
            return {};
        }

        auto named_scope(TSNode name, std::string_view text) -> std::string_view {
            return ts_node_is_null(name) || ts_node_is_missing(name) ? "anonymous" : slice(text, name);
        }

        auto cpp_scope_name(TSNode node, const scope_rule& rule, std::string_view text) -> std::string_view {
            TSNode name = ts_node_child_by_field_name(node, "name", 4);
            if (rule.function) {
                name = ts_node_child_by_field_name(node, "declarator", 10);
                while (!ts_node_is_null(name)) {
                    auto inner = ts_node_child_by_field_name(name, "declarator", 10);
                    if (ts_node_is_null(inner) && std::string_view{ts_node_type(name)} == "parenthesized_declarator")
                        inner = ts_node_named_child(name, 0);
                    if (ts_node_is_null(inner))
                        break;
                    name = inner;
                }
            }
            return named_scope(name, text);
        }

        auto cpp_function_body(TSNode node) -> TSNode {
            auto body = ts_node_child_by_field_name(node, "body", 4);
            if (ts_node_is_null(body)) {
                // Constructor function-try-blocks have an unlabelled try statement.
                for (std::uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
                    auto child = ts_node_named_child(node, i);
                    if (std::string_view{ts_node_type(child)} == "try_statement") {
                        body = child;
                        break;
                    }
                }
            }
            if (!ts_node_is_null(body) && std::string_view{ts_node_type(body)} == "try_statement")
                body = ts_node_child_by_field_name(body, "body", 4);
            if (!ts_node_is_null(body) && std::string_view{ts_node_type(body)} == "compound_statement")
                return body;
            return {};
        }

        auto cpp_directive_end(TSNode node, std::string_view type) -> std::uint32_t {
            if (type == "preproc_def" || type == "preproc_function_def" || type == "preproc_include" || type == "preproc_call" ||
                type == "#else" || type == "#endif")
                return ts_node_end_byte(node);

            TSNode argument{};
            if (type == "preproc_if" || type == "preproc_elif" || type == "preproc_if_in_attribute_list")
                argument = ts_node_child_by_field_name(node, "condition", 9);
            else if (type == "preproc_ifdef" || type == "preproc_elifdef")
                argument = ts_node_child_by_field_name(node, "name", 4);
            else
                return 0;
            // The conditional node also contains its branches. Only the opening directive counts here.
            const auto end = ts_node_is_null(argument) || ts_node_is_missing(argument) ? ts_node_child(node, 0) : argument;
            return ts_node_is_null(end) ? 0 : ts_node_end_byte(end);
        }

        auto kotlin_scope_name(TSNode node, const scope_rule& rule, std::string_view text) -> std::string_view {
            return named_scope(named_child(node, rule.function ? "simple_identifier" : "type_identifier"), text);
        }

        auto kotlin_function_body(TSNode node) -> TSNode {
            return named_child(node, "function_body");
        }

        auto fsharp_scope_name(TSNode node, const scope_rule&, std::string_view text) -> std::string_view {
            auto name = field(node, "name");
            if (!ts_node_is_null(name))
                return named_scope(name, text);

            for (const auto type : {"function_declaration_left", "value_declaration_left", "type_name"}) {
                const auto left = named_child(node, type);
                if (ts_node_is_null(left))
                    continue;
                for (const auto identifier : {"identifier", "op_identifier", "active_pattern", "long_identifier"}) {
                    name = named_child(left, identifier);
                    if (!ts_node_is_null(name))
                        return named_scope(name, text);
                }
            }
            return named_scope(named_child(node, "identifier"), text);
        }

        auto fsharp_function_body(TSNode node) -> TSNode {
            if (std::string_view{ts_node_type(node)} == "function_or_value_defn" &&
                ts_node_is_null(named_child(node, "function_declaration_left")))
                return {};
            return field(node, "body");
        }

        auto csharp_directive_end(TSNode node, std::string_view type) -> std::uint32_t {
            constexpr std::string_view single_directives[]{
                "preproc_define", "preproc_undef", "preproc_line", "preproc_nullable", "preproc_pragma",
                "preproc_error", "preproc_warning", "preproc_region", "preproc_endregion", "ignored_directive"};
            if (std::ranges::contains(single_directives, type))
                return ts_node_end_byte(node);
            return cpp_directive_end(node, type);
        }

        auto fsharp_directive_end(TSNode node, std::string_view type) -> std::uint32_t {
            if (type == "compiler_directive_decl" || type == "fsi_directive_decl" || type == "preproc_line")
                return ts_node_end_byte(node);
            if (type == "preproc_if") {
                for (auto i = ts_node_child_count(node); i > 0; --i) {
                    const auto* field_name = ts_node_field_name_for_child(node, i - 1);
                    if (field_name && std::string_view{field_name} == "condition")
                        return ts_node_end_byte(ts_node_child(node, i - 1));
                }
            }
            return cpp_directive_end(node, type);
        }

        auto rust_scope_name(TSNode node, const scope_rule&, std::string_view text) -> std::string_view {
            const auto name = field(node, "name");
            return named_scope(ts_node_is_null(name) ? field(node, "type") : name, text);
        }

        auto javascript_scope_name(TSNode node, const scope_rule&, std::string_view text) -> std::string_view {
            auto name = field(node, "name");
            if (ts_node_is_null(name)) {
                const auto parent = ts_node_parent(node);
                if (!ts_node_is_null(parent) && std::string_view{ts_node_type(parent)} == "variable_declarator")
                    name = field(parent, "name");
            }
            return named_scope(name, text);
        }

        auto rust_reusable_comment(std::string_view text) -> bool {
            return !text.starts_with("///") && !text.starts_with("//!") && !text.starts_with("/**") && !text.starts_with("/*!");
        }

        auto python_reusable_comment(std::string_view text) -> bool {
            text.remove_prefix(1);
            const auto begin = text.find_first_not_of(" \t");
            if (begin == std::string_view::npos)
                return true;
            const auto payload = text.substr(begin);
            // Encoding declarations and type, lint and formatter comments carry source information.
            return !payload.starts_with('!') && !payload.starts_with("noqa") && !payload.starts_with("nosec") &&
                   !payload.starts_with("pragma") && !payload.contains("coding") && !payload.contains(':') && !payload.contains('=');
        }

        auto c_style_reusable_comment(std::string_view text) -> bool {
            if ((!text.starts_with("//") && !text.starts_with("/*")) || text.starts_with("///") ||
                text.starts_with("/**") || text.starts_with("//>"))
                return false;
            // Compiler directives and tool annotations can live in comments.
            return !text.contains('@') && !text.contains('#') && !text.contains(':');
        }

        auto fsharp_reusable_comment(std::string_view text) -> bool {
            return text.starts_with("(*") ? !text.contains('@') && !text.contains('#') && !text.contains(':') : c_style_reusable_comment(text);
        }

        auto grammar_for(std::string_view path) -> const grammar* {
            static constexpr std::string_view cpp_extensions[]{
                ".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx",
                ".h++", ".ipp", ".tpp", ".inl", ".cppm", ".ixx", ".mpp", ".cu"};
            static constexpr scope_rule cpp_scopes[]{
                {"function_definition", "function", true},
                {"class_specifier", "class"},
                {"struct_specifier", "struct"},
                {"enum_specifier", "enum"},
                {"namespace_definition", "namespace"}};
            static constexpr std::string_view rust_extensions[]{".rs"};
            static constexpr scope_rule rust_scopes[]{
                {"function_item", "function", true},
                {"impl_item", "impl"},
                {"trait_item", "trait"},
                {"struct_item", "struct"},
                {"enum_item", "enum"},
                {"mod_item", "module"}};
            static constexpr std::string_view line_and_block_comments[]{"line_comment", "block_comment"};

            static constexpr std::string_view python_extensions[]{".py", ".pyi"};
            static constexpr scope_rule python_scopes[]{
                {"function_definition", "function", true},
                {"class_definition", "class"}};

            static constexpr std::string_view javascript_extensions[]{".js", ".jsx", ".mjs", ".cjs"};
            static constexpr std::string_view typescript_extensions[]{".ts", ".mts", ".cts"};
            static constexpr std::string_view tsx_extensions[]{".tsx"};
            static constexpr scope_rule javascript_scopes[]{
                {"function_declaration", "function", true},
                {"generator_function_declaration", "function", true},
                {"function_expression", "function", true},
                {"generator_function", "function", true},
                {"arrow_function", "function", true},
                {"method_definition", "function", true},
                {"class", "class"},
                {"class_declaration", "class"},
                {"abstract_class_declaration", "class"},
                {"interface_declaration", "interface"},
                {"enum_declaration", "enum"},
                {"internal_module", "module"}};

            static constexpr std::string_view kotlin_extensions[]{".kt", ".kts"};
            static constexpr scope_rule kotlin_scopes[]{
                {"function_declaration", "function", true},
                {"class_declaration", "class"},
                {"object_declaration", "object"},
                {"companion_object", "object"}};
            static constexpr std::string_view kotlin_comments[]{"line_comment", "multiline_comment"};

            static constexpr std::string_view java_extensions[]{".java"};
            static constexpr scope_rule java_scopes[]{
                {"method_declaration", "function", true},
                {"constructor_declaration", "constructor", true},
                {"compact_constructor_declaration", "constructor", true},
                {"class_declaration", "class"},
                {"interface_declaration", "interface"},
                {"annotation_type_declaration", "interface"},
                {"enum_declaration", "enum"},
                {"record_declaration", "record"}};

            static constexpr std::string_view csharp_extensions[]{".cs", ".csx"};
            static constexpr scope_rule csharp_scopes[]{
                {"method_declaration", "function", true},
                {"local_function_statement", "function", true},
                {"constructor_declaration", "constructor", true},
                {"class_declaration", "class"},
                {"interface_declaration", "interface"},
                {"struct_declaration", "struct"},
                {"enum_declaration", "enum"},
                {"record_declaration", "record"},
                {"namespace_declaration", "namespace"}};

            static constexpr std::string_view scala_extensions[]{".scala", ".sc"};
            static constexpr scope_rule scala_scopes[]{
                {"function_definition", "function", true},
                {"function_declaration", "function"},
                {"class_definition", "class"},
                {"trait_definition", "trait"},
                {"object_definition", "object"},
                {"enum_definition", "enum"}};
            static constexpr std::string_view scala_comments[]{"comment", "block_comment"};

            static constexpr std::string_view fsharp_extensions[]{".fs", ".fsx", ".fsscript"};
            static constexpr std::string_view fsharp_signature_extensions[]{".fsi"};
            static constexpr scope_rule fsharp_scopes[]{
                {"function_or_value_defn", "binding", true},
                {"method_or_prop_defn", "member", true},
                {"member_defn", "member"},
                {"anon_type_defn", "type"},
                {"record_type_defn", "record"},
                {"union_type_defn", "union"},
                {"enum_type_defn", "enum"},
                {"interface_type_defn", "interface"},
                {"type_abbrev_defn", "type"},
                {"named_module", "module"},
                {"module_defn", "module"},
                {"namespace", "namespace"}};

            static constexpr std::string_view generic_comments[]{"comment"};
            static constexpr std::string_view cpp_macro_definitions[]{"preproc_def", "preproc_function_def"};
            static constexpr grammar grammars[]{
                {cpp_extensions, tree_sitter_cpp, cpp_scopes, generic_comments, "compound_statement",
                 cpp_scope_name, cpp_function_body, cpp_macro_definitions, cpp_directive_end},
                {.extensions = rust_extensions, .language = tree_sitter_rust, .scopes = rust_scopes, .comments = line_and_block_comments, .body_type = "block",
                 .scope_name = rust_scope_name, .reusable_comment = rust_reusable_comment},
                {.extensions = python_extensions, .language = tree_sitter_python, .scopes = python_scopes, .comments = generic_comments, .body_type = "block",
                 .reusable_comment = python_reusable_comment},
                {.extensions = javascript_extensions, .language = tree_sitter_javascript, .scopes = javascript_scopes, .comments = generic_comments, .body_type = "statement_block",
                 .scope_name = javascript_scope_name, .reusable_comment = c_style_reusable_comment},
                {.extensions = typescript_extensions, .language = tree_sitter_typescript, .scopes = javascript_scopes, .comments = generic_comments, .body_type = "statement_block",
                 .scope_name = javascript_scope_name, .reusable_comment = c_style_reusable_comment},
                {.extensions = tsx_extensions, .language = tree_sitter_tsx, .scopes = javascript_scopes, .comments = generic_comments, .body_type = "statement_block",
                 .scope_name = javascript_scope_name, .reusable_comment = c_style_reusable_comment},
                {.extensions = kotlin_extensions, .language = tree_sitter_kotlin, .scopes = kotlin_scopes, .comments = kotlin_comments,
                 .body_type = "function_body", .scope_name = kotlin_scope_name, .function_body = kotlin_function_body, .reusable_comment = c_style_reusable_comment},
                {.extensions = java_extensions, .language = tree_sitter_java, .scopes = java_scopes, .comments = line_and_block_comments,
                 .reusable_comment = c_style_reusable_comment, .comment_sensitive_sequence = "\\u"},
                {.extensions = csharp_extensions, .language = tree_sitter_c_sharp, .scopes = csharp_scopes, .comments = generic_comments,
                 .directive_end = csharp_directive_end, .reusable_comment = c_style_reusable_comment},
                {.extensions = scala_extensions, .language = tree_sitter_scala, .scopes = scala_scopes, .comments = scala_comments,
                 .reusable_comment = c_style_reusable_comment},
                {.extensions = fsharp_extensions, .language = tree_sitter_fsharp, .scopes = fsharp_scopes, .comments = line_and_block_comments,
                 .scope_name = fsharp_scope_name, .function_body = fsharp_function_body, .directive_end = fsharp_directive_end,
                 .reusable_comment = fsharp_reusable_comment},
                {.extensions = fsharp_signature_extensions, .language = tree_sitter_fsharp_signature, .scopes = fsharp_scopes, .comments = line_and_block_comments,
                 .scope_name = fsharp_scope_name, .directive_end = fsharp_directive_end, .reusable_comment = fsharp_reusable_comment}};

            const auto name = path.substr(path.find_last_of('/') + 1);
            const auto dot = name.find_last_of('.');
            if (dot == std::string_view::npos)
                return nullptr;
            const auto extension = name.substr(dot);
            const auto found = std::ranges::find_if(grammars, [&](const auto& entry) {
                return std::ranges::contains(entry.extensions, extension);
            });
            return found == std::end(grammars) ? nullptr : found;
        }

        auto next_reusable_comment(const grammar& config, cursor& walk, std::string_view text) -> TSNode {
            while (walk) {
                const auto node = walk.node();
                const bool comment = std::ranges::contains(config.comments, std::string_view{ts_node_type(node)});
                walk.next(!comment);
                if (comment && config.reusable_comment(slice(text, node)))
                    return node;
            }
            return {};
        }

        auto next_line_break(std::string_view& text) -> std::string_view {
            for (auto at = text.find_first_of("\r\n\xe2"); at != std::string_view::npos; at = text.find_first_of("\r\n\xe2")) {
                text.remove_prefix(at);
                if (text.front() == '\r' || text.front() == '\n') {
                    const auto length = text.starts_with("\r\n") ? 2u : 1u;
                    const auto result = text.substr(0, length);
                    text.remove_prefix(length);
                    return result;
                }
                if (text.starts_with("\xe2\x80\xa8") || text.starts_with("\xe2\x80\xa9")) {
                    const auto result = text.substr(0, 3);
                    text.remove_prefix(3);
                    return result;
                }
                text.remove_prefix(1);
            }
            return {};
        }

        auto same_line_breaks(std::string_view before, std::string_view after) -> bool {
            for (;;) {
                const auto line = next_line_break(before);
                if (line != next_line_break(after))
                    return false;
                if (line.empty())
                    return true;
            }
        }

        auto same_outside_comments(const grammar& config, TSNode before_root, std::string_view before,
                                   TSNode after_root, std::string_view after) -> bool {
            cursor left{before_root}, right{after_root};
            std::uint32_t left_end = 0, right_end = 0;
            for (;;) {
                const auto a = next_reusable_comment(config, left, before);
                const auto b = next_reusable_comment(config, right, after);
                if (ts_node_is_null(a) || ts_node_is_null(b))
                    return ts_node_is_null(a) && ts_node_is_null(b) && before.substr(left_end) == after.substr(right_end);
                const auto a_begin = ts_node_start_byte(a), b_begin = ts_node_start_byte(b);
                if (before.substr(left_end, a_begin - left_end) != after.substr(right_end, b_begin - right_end) ||
                    ts_node_symbol(a) != ts_node_symbol(b) ||
                    !same_line_breaks(slice(before, a), slice(after, b)))
                    return false;
                left_end = ts_node_end_byte(a);
                right_end = ts_node_end_byte(b);
            }
        }

        using token = std::pair<TSSymbol, std::string_view>;

        auto next_token(const grammar& config, cursor& walk, std::string_view text) -> std::optional<token> {
            while (walk) {
                const auto node = walk.node();
                const std::string_view type = ts_node_type(node);
                if (std::ranges::contains(config.comments, type)) {
                    walk.next(false);
                    continue;
                }
                // Macro spelling includes whitespace with preprocessing meaning.
                if (ts_node_child_count(node) == 0 || std::ranges::contains(config.opaque_nodes, type)) {
                    walk.next(false);
                    if (auto spelling = slice(text, node); !spelling.empty())
                        return token{ts_node_symbol(node), spelling};
                    continue;
                }
                walk.next();
            }
            return std::nullopt;
        }
    }

    export struct scope {
        std::string id, kind, name;
        std::uint64_t begin = 0, end = 0;
    };

    export struct outline {
        std::vector<scope> scopes;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> bodies, directives;

        auto touches_directive(std::uint64_t begin, std::uint64_t end) const -> bool {
            return std::ranges::any_of(directives, [&](const auto& directive) {
                return begin < directive.second && directive.first < end;
            });
        }

        auto inside_body(std::uint64_t begin, std::uint64_t end) const -> bool {
            return std::ranges::any_of(bodies, [&](const auto& body) { return body.first < begin && end < body.second; });
        }
    };

    export auto supports(std::string_view path) -> bool { return grammar_for(path) != nullptr; }

    export auto outline_provider_family(std::string_view path) -> std::string_view {
        const auto* config = grammar_for(path);
        if (!config)
            return {};
        if (config->language == tree_sitter_python)
            return "python";
        if (config->language == tree_sitter_javascript || config->language == tree_sitter_typescript || config->language == tree_sitter_tsx)
            return "typescript";
        return {};
    }

    export auto tracks_directives(std::string_view path) -> bool {
        const auto* config = grammar_for(path);
        return config && config->directive_end;
    }

    export class parsed_file {
        std::string_view text;
        const grammar* config;
        tree_ptr tree;

      public:
        // The source text must outlive this snapshot.
        parsed_file(std::string_view path, std::string_view text)
            : text(text), config(grammar_for(path)), tree(config ? parse(*config, text) : tree_ptr{nullptr, ts_tree_delete}) {}

        auto analyze(bool include_scopes) const -> outline {
            outline result;
            if (!tree || (!include_scopes && !config->directive_end))
                return result;

            std::vector<std::uint32_t> continuation_bytes;
            for (std::uint32_t i = 0; i < text.size(); ++i)
                if ((static_cast<unsigned char>(text[i]) & 0xc0) == 0x80)
                    continuation_bytes.push_back(i);
            auto scalar_offset = [&](std::uint32_t byte) -> std::uint64_t {
                return byte - (std::ranges::lower_bound(continuation_bytes, byte) - continuation_bytes.begin());
            };

            for (cursor walk{ts_tree_root_node(tree.get())}; walk; walk.next()) {
                const auto node = walk.node();
                const std::string_view type = ts_node_type(node);
                if (config->directive_end) {
                    const auto begin = ts_node_start_byte(node);
                    const auto end = config->directive_end(node, type);
                    if (begin < end)
                        result.directives.emplace_back(scalar_offset(begin), scalar_offset(end));
                }
                if (!include_scopes)
                    continue;
                const auto rule = std::ranges::find(config->scopes, type, &scope_rule::node_type);
                if (rule == config->scopes.end() || ts_node_has_error(node))
                    continue;
                const auto name = config->scope_name ? config->scope_name(node, *rule, text) : named_scope(field(node, "name"), text);
                const auto begin = scalar_offset(ts_node_start_byte(node));
                const auto end = scalar_offset(ts_node_end_byte(node));
                result.scopes.push_back({std::format("syntax:{}:{}", rule->kind, name), std::string{rule->kind}, std::string{name}, begin, end});
                if (rule->function) {
                    const auto body = config->function_body ? config->function_body(node) : field(node, "body");
                    if (!ts_node_is_null(body) && (config->body_type.empty() || std::string_view{ts_node_type(body)} == config->body_type))
                        result.bodies.emplace_back(scalar_offset(ts_node_start_byte(body)), scalar_offset(ts_node_end_byte(body)));
                }
            }
            return result;
        }

        auto comment_or_whitespace_only(const parsed_file& after) const -> bool {
            if (!tree || !after.tree || config != after.config)
                return false;
            const auto before_root = ts_tree_root_node(tree.get());
            const auto after_root = ts_tree_root_node(after.tree.get());
            if (ts_node_has_error(before_root) || ts_node_has_error(after_root))
                return false;

            if (config->reusable_comment) {
                const auto sensitive = config->comment_sensitive_sequence;
                if (!sensitive.empty() && (text.contains(sensitive) || after.text.contains(sensitive)))
                    return false;
                return same_outside_comments(*config, before_root, text, after_root, after.text);
            }

            cursor left{before_root}, right{after_root};
            for (;;) {
                const auto a = next_token(*config, left, text);
                const auto b = next_token(*config, right, after.text);
                if (a != b)
                    return false;
                if (!a)
                    return true;
            }
        }
    };
}
