export module semantic;
import std;

// Language data refers to the immutable snapshot analyzed by the worker.
export namespace semantic {
    struct query {
        std::string action = "diagnostics";
        std::string path, symbol;
        std::optional<std::uint64_t> line;
    };
    // One-based lines and Unicode scalar columns in the snapshot read by the adapter.
    struct location {
        std::string path;
        std::uint64_t line = 0, column = 0, end_line = 0, end_column = 0;
        std::string symbol;
    };
    // Where characters are in one text: a path and Unicode scalar offsets [begin, end).
    struct place {
        std::string path;
        std::uint64_t begin = 0, end = 0;
    };
    // A provider reports a range in the snapshot it read; no source-side anchoring is needed.
    struct span {
        std::optional<semantic::place> place;
    };
    // Kinds are neutral: namespace, type, function, variable, module or other.
    struct scope {
        std::string id, parent, kind, symbol;
        span range;
    };
    struct symbol {
        std::string id, name, kind, scope;
        std::vector<span> declarations, definitions;
    };
    struct diagnostic {
        span range;
        std::string severity, message;
    };
    // What a language server reported for one analyzed snapshot.
    struct outline {
        bool complete = true;
        bool cacheable = true;
        std::vector<scope> scopes;
        std::vector<symbol> symbols;
        std::vector<diagnostic> diagnostics;
    };
} // namespace semantic
