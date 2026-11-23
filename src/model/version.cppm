export module version;
import std;

namespace toolchain {
    auto digit(char c) -> bool { return c >= '0' && c <= '9'; }

    auto identifier(char c) -> bool {
        return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
    }

    auto trim(std::string_view text) -> std::string_view {
        const auto first = text.find_first_not_of(" \t\r\n");
        return first == text.npos ? std::string_view{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    auto numeric(std::string_view text) -> bool { return !text.empty() && std::ranges::all_of(text, digit); }

    auto compare_identifier(std::string_view a, std::string_view b) -> std::strong_ordering {
        const bool an = numeric(a), bn = numeric(b);
        if (an != bn)
            return an ? std::strong_ordering::less : std::strong_ordering::greater;
        if (an && a.size() != b.size())
            return a.size() <=> b.size();
        return a <=> b;
    }
}

export namespace toolchain {
    struct semver {
        std::array<std::uint64_t, 3> release{};
        std::vector<std::string> prerelease;

        friend auto operator<=>(const semver& a, const semver& b) -> std::strong_ordering {
            if (auto order = a.release <=> b.release; order != 0)
                return order;
            if (a.prerelease.empty() != b.prerelease.empty())
                return a.prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
            return std::lexicographical_compare_three_way(a.prerelease.begin(), a.prerelease.end(), b.prerelease.begin(), b.prerelease.end(),
                                                         compare_identifier);
        }
        friend auto operator==(const semver&, const semver&) -> bool = default;

        auto str() const -> std::string {
            auto text = std::format("{}.{}.{}", release[0], release[1], release[2]);
            for (std::size_t i = 0; i < prerelease.size(); ++i)
                text += (i == 0 ? "-" : ".") + prerelease[i];
            return text;
        }
    };
}

namespace toolchain {
    struct version_pattern {
        semver value;
        std::size_t precision = 0;
        bool wildcard = false;
    };

    auto parse_pattern(std::string_view text, bool allow_wildcard) -> std::optional<version_pattern> {
        version_pattern out;
        const auto build = text.find('+');
        if (build != text.npos) {
            for (auto part : std::views::split(text.substr(build + 1), '.'))
                if (part.empty() || !std::ranges::all_of(part, identifier))
                    return std::nullopt;
            if (build + 1 == text.size())
                return std::nullopt;
            text = text.substr(0, build);
        }
        if (const auto dash = text.find('-'); dash != text.npos) {
            const auto suffix = text.substr(dash + 1);
            if (suffix.empty())
                return std::nullopt;
            for (auto part : std::views::split(suffix, '.')) {
                const std::string_view token{part};
                if (token.empty() || !std::ranges::all_of(token, identifier) || (numeric(token) && token.size() > 1 && token.front() == '0'))
                    return std::nullopt;
                out.value.prerelease.emplace_back(token);
            }
            text = text.substr(0, dash);
        }
        std::size_t components = 0;
        for (auto part : std::views::split(text, '.')) {
            const std::string_view token{part};
            if (++components > out.value.release.size())
                return std::nullopt;
            if (allow_wildcard && (token == "x" || token == "X" || token == "*")) {
                out.wildcard = true;
                continue;
            }
            if (out.wildcard || !numeric(token) || (token.size() > 1 && token.front() == '0'))
                return std::nullopt;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), out.value.release[out.precision++]);
            if (error != std::errc{} || end != token.data() + token.size())
                return std::nullopt;
        }
        if (!components || ((build != text.npos || !out.value.prerelease.empty()) && out.precision != out.value.release.size()))
            return std::nullopt;
        return out;
    }
}

export namespace toolchain {
    auto parse_version(std::string_view text) -> std::optional<semver> {
        if (auto pattern = parse_pattern(text, false))
            return std::move(pattern->value);
        return std::nullopt;
    }

    struct version_constraint {
        enum class operation { exact, less, at_most, greater, at_least, prefix, bounded };
        struct clause {
            operation op;
            semver value;
            std::size_t prefix = 0;
        };
        std::vector<clause> all;

        auto matches(const std::optional<semver>& version) const -> bool {
            if (all.empty())
                return true;
            if (!version)
                return false;
            if (!version->prerelease.empty() && !std::ranges::any_of(all, [&](const auto& part) {
                    return !part.value.prerelease.empty() && part.value.release == version->release;
                }))
                return false;
            return std::ranges::all_of(all, [&](const auto& part) {
                const auto order = *version <=> part.value;
                const auto same_prefix = [&] {
                    return std::equal(part.value.release.begin(), part.value.release.begin() + part.prefix, version->release.begin());
                };
                switch (part.op) {
                case operation::exact: return order == 0;
                case operation::less: return order < 0;
                case operation::at_most: return order <= 0;
                case operation::greater: return order > 0;
                case operation::at_least: return order >= 0;
                case operation::prefix: return same_prefix();
                case operation::bounded: return order >= 0 && same_prefix();
                }
                return false;
            });
        }

        static auto parse(std::string_view expression) -> version_constraint {
            using enum operation;
            version_constraint out;
            if (expression.empty())
                return out;
            for (auto part : std::views::split(expression, ',')) {
                auto text = trim(std::string_view{part});
                const auto op_end = text.find_first_not_of("<>=^~");
                const auto symbol = text.substr(0, op_end);
                auto value = op_end == text.npos ? std::nullopt : parse_pattern(trim(text.substr(op_end)), symbol.empty() || symbol == "=");
                if (!value)
                    throw std::invalid_argument("invalid version constraint: " + std::string{expression});
                clause term{.op = exact, .value = std::move(value->value)};
                if (symbol.empty() || symbol == "=") {
                    if (value->precision < term.value.release.size()) {
                        term.op = prefix;
                        term.prefix = value->precision;
                    }
                } else if (symbol == "^" || symbol == "~") {
                    term.op = bounded;
                    term.prefix = 1;
                    if (symbol == "~") {
                        term.prefix = std::min<std::size_t>(2, value->precision);
                    } else {
                        while (term.prefix < value->precision && term.value.release[term.prefix - 1] == 0)
                            ++term.prefix;
                    }
                } else if (symbol == "<") {
                    term.op = less;
                } else if (symbol == "<=") {
                    term.op = at_most;
                } else if (symbol == ">") {
                    term.op = greater;
                } else if (symbol == ">=") {
                    term.op = at_least;
                } else {
                    throw std::invalid_argument("invalid version constraint: " + std::string{expression});
                }
                out.all.push_back(std::move(term));
            }
            return out;
        }
    };
}
