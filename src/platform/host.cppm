export module host;
import std;

export namespace host {
    extern const std::string_view default_data_root;
    extern const std::string_view os;
    extern const std::string_view arch;

    auto environment() -> std::map<std::string, std::string>;
    void set_environment_if_absent(const std::string& name, const std::string& value);
    auto detect_tools(const std::map<std::string, std::string>& mapped = {}) -> std::vector<std::string>;
    auto executable() -> std::filesystem::path;
    auto name() -> std::string;
} // namespace host
