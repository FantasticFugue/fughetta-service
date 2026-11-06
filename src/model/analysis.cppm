module language;
import reflection;

namespace language {
    auto timeout(const builds::configuration& c) -> std::uint32_t { return c.limits.timeout_s.value_or(1800); }
    auto output_limit(const builds::configuration& c) -> std::uint64_t { return c.limits.output_limit.value_or(1u << 20); }
    auto provider_of(const provider& p) -> std::string_view {
        return p.visit([](const auto& w) { return std::remove_cvref_t<decltype(w)>::name; });
    }
    auto fingerprint(const configuration& c, const builds::configuration& build) -> content::hash {
        return content::hash_of(rfl::json::write(c) + "\n" + rfl::json::write(build));
    }
    auto prepare(const configuration& c, const builds::configuration& bound) -> plan {
        validate(c);
        auto types = c.server.visit([](const auto& w) {
            using implementation = typename std::remove_cvref_t<decltype(w)>::implementation;
            if constexpr (requires { implementation::required_inputs(w.options); })
                return implementation::required_inputs(w.options);
            std::vector<std::string> out;
            for (auto type : implementation::inputs)
                out.emplace_back(type);
            return out;
        });
        auto paths = c.server.visit([](const auto& w) -> std::map<std::string, std::string> {
            using implementation = typename std::remove_cvref_t<decltype(w)>::implementation;
            if constexpr (requires { implementation::input_paths(w.options); })
                return implementation::input_paths(w.options);
            return {};
        });
        auto build = builds::prepare_inputs(bound, types, paths);
        auto inputs = build.contexts;
        auto server = c.server.visit([&](const auto& w) {
            using implementation = typename std::remove_cvref_t<decltype(w)>::implementation;
            implementation::validate(w.options);
            for (auto& input : inputs)
                if (!std::ranges::contains(implementation::inputs, std::string_view{input.type}))
                    throw std::invalid_argument("the language provider does not read " + input.type + " output " + input.name);
            auto out = implementation::launch(w.options, inputs);
            out.provider = std::string{std::remove_cvref_t<decltype(w)>::name};
            return out;
        });
        builds::validate_environment(server.env);
        return {.build = std::move(build), .server = std::move(server), .inputs = std::move(inputs), .fingerprint = fingerprint(c, bound)};
    }
    auto locate(program value, const std::filesystem::path& tree, const builds::toolset& tools, std::uint32_t jobs) -> program {
        if (value.argv.empty())
            throw std::invalid_argument("language server executable is required");
        if (!std::filesystem::path(value.argv.front()).is_absolute()) {
            builds::tools({value.argv.front()});
            value.argv.front() = builds::tool(value.argv.front());
        }
        auto bound = builds::locate({.argv = std::move(value.argv), .env = std::move(value.env)}, tree, tools, jobs);
        value.argv = std::move(bound.argv);
        value.env = std::move(bound.env);
        return value;
    }
    void validate(const configuration& c) {
        if (c.build_configuration.empty())
            throw std::invalid_argument("analysis build_configuration is required");
        if (rfl::json::write(c).find("\\u0000") != std::string::npos)
            throw std::invalid_argument("analysis configuration contains a null character");
        c.server.visit([](const auto& w) { std::remove_cvref_t<decltype(w)>::implementation::validate(w.options); });
    }
    void check(const plan& p, const std::vector<content::entry>& files) {
        for (auto& entry : files) {
            for (auto& dir : p.build.build_dirs)
                if (entry.path == dir || entry.path.starts_with(dir + "/"))
                    throw std::invalid_argument("analysis build directory overlaps tracked source files");
            for (auto& step : p.build.steps)
                for (auto& output : step.outputs)
                    if (entry.path == output.path || entry.path.starts_with(output.path + "/"))
                        throw std::invalid_argument("analysis output overlaps tracked source files");
        }
    }
} // namespace language
