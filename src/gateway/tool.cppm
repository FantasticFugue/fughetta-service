export module tool;
import std;
import meta;

export namespace tools {
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

    // A refusal from the node engine, kept with its code so MCP callers get { code, message }.
    struct tool_error : std::runtime_error {
        tool_error(std::string_view code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
        std::string code;
    };

} // namespace tools
