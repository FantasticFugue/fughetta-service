export module host;
import std;

export namespace host {
    extern const std::string_view default_data_root;
    extern const std::string_view os;
    extern const std::string_view arch;

    auto environment() -> std::map<std::string, std::string>;
    void set_environment_if_absent(const std::string& name, const std::string& value);
    // PATH discovery selects executables. Explicit mappings may also name
    // readable tool resources launched through an interpreter.
    auto resolve_tools(const std::map<std::string, std::string>& mapped = {}, const std::vector<std::string>& wanted = {})
        -> std::map<std::string, std::string>;
    auto detect_tools(const std::map<std::string, std::string>& mapped = {}) -> std::vector<std::string>;
    auto executable() -> std::filesystem::path;
    auto name() -> std::string;
} // namespace host
