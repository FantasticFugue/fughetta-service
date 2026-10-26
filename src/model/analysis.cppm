module language;
import reflection;

namespace language {
    auto timeout(const configuration& c) -> std::uint32_t { return c.build.limits.timeout_s.value_or(60); }
    auto output_limit(const configuration& c) -> std::uint64_t { return c.build.limits.output_limit.value_or(1u << 20); }
    auto provider_of(const provider& p) -> std::string_view {
        return p.visit([](const auto& w) { return std::remove_cvref_t<decltype(w)>::name; });
    }
    namespace {
        void settings(const configuration& c) {
            if (!timeout(c) || timeout(c) > 90)
                throw std::invalid_argument("analysis timeout must be between 1 and 90 seconds");
            if (output_limit(c) > (1u << 20))
                throw std::invalid_argument("analysis output limit exceeds 1 MiB");
            if (c.build.limits.max_memory_bytes || c.build.limits.max_cpu_seconds)
                throw std::invalid_argument("Source preparation does not provide process resource isolation");
            if (rfl::json::write(c).find("\\u0000") != std::string::npos)
                throw std::invalid_argument("analysis configuration contains a null character");
        }
    } // namespace
    auto prepare(const configuration& c) -> plan {
        settings(c);
        // A server that reads no build output needs no build.
        auto build = c.build.operations.empty() ? builds::plan{} : builds::prepare(c.build, {.operation = c.operation});
        std::vector<builds::context> inputs;
        std::set<std::string> seen;
        for (auto& name : c.inputs) {
            if (!seen.insert(name).second)
                throw std::invalid_argument("duplicate language service input: " + name);
            auto output = std::ranges::find(build.contexts, name, &builds::context::name);
            if (output == build.contexts.end())
                throw std::invalid_argument("preparation does not provide output: " + name);
            inputs.push_back(*output);
        }
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
        auto recipe = content::hash_of(rfl::json::write(build));
        return {.build = std::move(build),
                .server = std::move(server),
                .inputs = std::move(inputs),
                .fingerprint = content::hash_of(rfl::json::write(c)),
                .recipe = std::move(recipe)};
    }
    void validate(const configuration& c) {
        if (!c.build.builders.empty() || !c.build.tasks.empty() || !c.build.operations.empty() || !c.build.outputs.empty() ||
            !c.inputs.empty() || c.operation != "prepare")
            prepare(c);
        else
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
