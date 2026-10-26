export module semantic;
import std;

// Language data that every analysis provider publishes and every consumer reads. Positions are the
// identities of source characters, so they follow edits and do not depend on how a provider counts text.
export namespace semantic {
    // One source character: the edit run that inserted it and its index inside that run.
    struct anchor {
        std::string run;
        std::uint64_t offset = 0;
        auto operator<=>(const anchor&) const = default;
    };
    // Where characters are in one text: a path and Unicode scalar offsets [begin, end).
    struct place {
        std::string path;
        std::uint64_t begin = 0, end = 0;
    };
    // The characters first..last of one document. A provider reports only where they are in the text it read;
    // a query reports where they are in the selected source, or nothing when they are gone.
    struct span {
        std::string document;
        anchor first, last;
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
    struct reference {
        span range;
        std::string target, kind;
    };
    struct relation {
        std::string source, target, kind;
    };
    struct diagnostic {
        span range;
        std::string severity, message;
    };
    struct coverage {
        std::string document, digest;
    };
    // Everything one provider reported for one source version in one environment.
    struct generation {
        std::string version, environment, service, service_version;
        std::vector<coverage> covered;
        std::vector<scope> scopes;
        std::vector<symbol> symbols;
        std::vector<reference> references;
        std::vector<relation> relations;
        std::vector<diagnostic> diagnostics;
    };
    // What a language server reported before its places are anchored in a source version.
    struct outline {
        std::vector<scope> scopes;
        std::vector<symbol> symbols;
        std::vector<diagnostic> diagnostics;
    };
} // namespace semantic
