export module environment;
import std;
import host;

export namespace environment {
    inline auto trim(std::string_view s) -> std::string_view {
        constexpr std::string_view spaces = " \t\r";
        auto begin = s.find_first_not_of(spaces);
        if (begin == s.npos)
            return {};
        return s.substr(begin, s.find_last_not_of(spaces) - begin + 1);
    }

    inline auto parse(std::istream& in) -> std::vector<std::pair<std::string, std::string>> {
        std::vector<std::pair<std::string, std::string>> entries;
        std::string line;
        std::size_t number = 0;
        while (std::getline(in, line)) {
            ++number;
            auto fail = [&] { throw std::invalid_argument("invalid .env assignment at line " + std::to_string(number)); };
            auto s = trim(line);
            if (number == 1 && s.starts_with("\xef\xbb\xbf"))
                s = trim(s.substr(3));
            if (s.empty() || s.front() == '#')
                continue;
            if (s.starts_with("export ") || s.starts_with("export\t"))
                s = trim(s.substr(6));
            auto eq = s.find('=');
            if (eq == s.npos)
                fail();
            auto name = trim(s.substr(0, eq));
            auto first = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
            if (name.empty() || !first(name.front()) || !std::ranges::all_of(name, [&](char c) { return first(c) || (c >= '0' && c <= '9'); }))
                fail();
            auto raw = trim(s.substr(eq + 1));
            std::string value;
            if (!raw.empty() && (raw.front() == '\'' || raw.front() == '"')) {
                char quote = raw.front();
                std::size_t i = 1;
                for (; i < raw.size() && raw[i] != quote; ++i) {
                    if (quote == '"' && raw[i] == '\\') {
                        if (++i == raw.size())
                            fail();
                        switch (raw[i]) {
                        case 'n':
                            value += '\n';
                            break;
                        case 'r':
                            value += '\r';
                            break;
                        case 't':
                            value += '\t';
                            break;
                        case '"':
                        case '\\':
                        case '$':
                            value += raw[i];
                            break;
                        default:
                            value += '\\';
                            value += raw[i];
                            break;
                        }
                    } else
                        value += raw[i];
                }
                if (i == raw.size())
                    fail();
                auto tail = trim(raw.substr(i + 1));
                if (!tail.empty() && tail.front() != '#')
                    fail();
            } else {
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '#' && (i == 0 || raw[i - 1] == ' ' || raw[i - 1] == '\t')) {
                        raw = trim(raw.substr(0, i));
                        break;
                    }
                }
                value = raw;
            }
            entries.emplace_back(name, std::move(value));
        }
        if (in.bad())
            throw std::runtime_error("cannot read .env");
        return entries;
    }

    inline auto load(const std::filesystem::path& path) -> bool {
        std::ifstream in{path, std::ios::binary};
        if (!in) {
            if (!std::filesystem::exists(path))
                return false;
            throw std::runtime_error("cannot open .env");
        }
        auto entries = parse(in);
        for (auto& [name, value] : entries) {
            if (std::getenv(name.c_str()))
                continue;
            host::set_environment_if_absent(name, value);
        }
        return true;
    }
} // namespace environment
