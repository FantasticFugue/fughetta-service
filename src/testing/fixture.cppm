export module fixture;
import std;
import meta;
import coroutine;
import codec;
import mcp;
import protocol;
import tools;
export import reflection;
export import configuration;

export namespace fixture {
    template <meta::fixed_string Name> auto options(builds::builder& b) -> builds::config<Name>& { return rfl::get<builds::with<Name>>(b.variant()).options; }
    template <meta::fixed_string Name> auto options(const builds::builder& b) -> const builds::config<Name>& {
        return rfl::get<builds::with<Name>>(b.variant()).options;
    }
    auto needs(builds::builder& b) -> builds::requirements& {
        return b.visit([](auto& w) -> builds::requirements& { return w.needs.get(); });
    }
    auto env(builds::builder& b) -> builds::environment& {
        return b.visit([](auto& w) -> builds::environment& { return w.env; });
    }
    // The clice server of an analysis, started as `executable serve args...`.
    auto clice(std::string executable, std::vector<std::string> args = {}) -> language::provider {
        return language::with<"clice">{.options = {.executable = std::move(executable), .args = std::move(args)}};
    }
    void command(builds::configuration& build, std::string operation, std::vector<std::string> argv) {
        options<"command">(build.builders.at("main")).operations.at(operation).argv = std::move(argv);
    }
    auto commands(std::vector<std::string> build, std::vector<std::string> prepare = {}, std::vector<std::string> test = {}, std::string cwd = "",
                  std::vector<std::string> run = {}) -> builds::configuration {
        builds::config<"command"> c;
        builds::configuration out;
        c.operations["build"] = {.argv = std::move(build), .working_dir = cwd};
        out.tasks["build"] = {.builder = "main", .operation = "build"};
        out.operations["build"] = {.tasks = {"build"}};
        if (!prepare.empty()) {
            c.operations["prepare"] = {.argv = std::move(prepare), .working_dir = cwd};
            out.tasks["prepare"] = {.builder = "main", .operation = "prepare"};
            out.tasks["build"].after = {"prepare"};
            out.operations["prepare"] = {.tasks = {"prepare"}};
        }
        if (!test.empty()) {
            c.operations["test"] = {.argv = std::move(test), .working_dir = cwd};
            out.tasks["test"] = {.builder = "main", .operation = "test", .after = {"build"}};
            out.operations["test"] = {.tasks = {"test"}};
        }
        if (!run.empty()) {
            c.operations["run"] = {.argv = std::move(run), .working_dir = cwd, .append_args = true};
            out.tasks["run"] = {.builder = "main", .operation = "run", .after = {"build"}};
            out.operations["run"] = {.tasks = {"run"}};
        }
        out.builders["main"] = builds::with<"command">{.options = std::move(c)};
        return out;
    }
    auto cmake(std::string dir, std::string compiler = "") -> builds::configuration {
        builds::configuration out;
        out.builders["main"] = builds::with<"cmake">{.options = {.build_dir = std::move(dir), .c_compiler = std::move(compiler)}};
        out.tasks = {{"configure", {.builder = "main", .operation = "configure"}},
                     {"build", {.builder = "main", .operation = "build", .after = {"configure"}}},
                     {"test", {.builder = "main", .operation = "test", .after = {"build"}}}};
        out.operations = {{"prepare", {.tasks = {"configure"}}}, {"build", {.tasks = {"build"}}}, {"test", {.tasks = {"test"}}}};
        return out;
    }
    void launcher(builds::configuration& build, std::string executable) {
        build.builders["launcher"] = builds::with<"command">{
            .options = {
                .operations = {{"run", {.argv = {"${input:binary_dir}/" + executable}, .inputs = {{"binary_dir", "directory"}}, .append_args = true}}}}};
        build.tasks["run"] = {.builder = "launcher", .operation = "run", .inputs = {{"binary_dir", {"build", "build_tree"}}}};
        build.operations["run"] = {.tasks = {"run"}};
    }
    // Plain inputs and outputs for the file helpers below. The helpers run the same node operations the retired
    // gateway file tools did, through tools::once and tools::main_apply, so operation IDs, limits and refusals
    // reach the tests unchanged; nothing here is an MCP tool or holds a schema.
    struct write_input {
        std::string project, path, content, operation_id;
    };
    struct edit_input {
        std::string project, path, old_string, new_string;
        std::optional<bool> replace_all;
        std::string operation_id;
    };
    struct edit_output {
        std::string version, receipt, operation_id;
        std::uint64_t replacements = 0;
        bool replayed = false;
    };
    struct remove_input {
        std::string project, path, operation_id;
    };
    struct move_input {
        std::string project, from, to, operation_id;
    };
    struct read_input {
        std::string project, path;
    };
    struct read_output {
        std::string content, version;
    };
    struct list_input {
        std::string project;
    };
    struct listed_file {
        std::string path;
        std::uint64_t size = 0;
    };
    struct list_output {
        std::vector<listed_file> files;
        std::string version;
    };
    struct search_input {
        std::string project, query;
    };
    struct search_match {
        std::string path;
        std::uint64_t line = 0;
        std::string text;
    };
    struct search_output {
        std::vector<search_match> matches;
        std::string version;
    };

    auto write_file(tools::services& s, write_input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<tools::mutation> {
        config::check_name("project", in.project);
        std::string input = content::hash_of(rfl::json::write(std::tuple{std::string{"write"}, in.path, in.content})).str();
        auto apply_once = [&]() -> coroutine::task<tools::mutation> {
            auto observed = co_await tools::main_tree(s, in.project, actor);
            bool exists = std::ranges::contains(observed.files, in.path, &nodes::file_view::path);
            auto r = co_await tools::main_apply(s, in.project, actor, observed,
                                                {tools::change{.kind = exists ? "write" : "create", .path = in.path, .text = in.content}}, in.operation_id,
                                                "write " + in.path, rfl::json::write(tools::record{input}));
            co_return tools::mutation{r.version, r.receipt, in.operation_id, r.replayed};
        };
        co_return co_await tools::once<tools::mutation>(
            s, in.project, actor, std::optional<std::string>{in.operation_id}, input, std::move(apply_once),
            [&](const auto& done) { return tools::mutation{done.first.version, done.first.receipt, in.operation_id, true}; });
    }

    auto edit_text(tools::services& s, edit_input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<edit_output> {
        config::check_name("project", in.project);
        std::string input =
            content::hash_of(rfl::json::write(std::tuple{std::string{"edit"}, in.path, in.old_string, in.new_string, in.replace_all.value_or(false)})).str();
        auto apply_once = [&]() -> coroutine::task<edit_output> {
            auto r = co_await tools::node_call(s, in.project, {.action = "read", .worldline = "main", .target = in.path}, actor, true);
            auto file = std::ranges::find(r.files, in.path, &nodes::file_view::path);
            if (file == r.files.end() || file->kind != "text")
                throw tools::tool_error(nodes::code::missing_objects, in.path + " is not a text file of the project");
            if (in.old_string.empty())
                throw tools::tool_error(nodes::code::invalid_record, "old_string is empty");
            std::vector<std::size_t> at;
            for (auto i = file->text.find(in.old_string); i != std::string::npos; i = file->text.find(in.old_string, i + in.old_string.size()))
                at.push_back(i);
            if (at.empty())
                throw tools::tool_error(nodes::code::invalid_record, "old_string does not occur in " + in.path);
            if (at.size() > 1 && !in.replace_all.value_or(false))
                throw tools::tool_error(nodes::code::invalid_record,
                                        std::format("old_string occurs {} times; set replace_all or give more context", at.size()));
            // One edit per occurrence would edit the path twice in one batch, so the edit spans from the first to the last occurrence.
            std::string replaced;
            std::size_t last = at.front();
            for (auto i : at) {
                replaced += file->text.substr(last, i - last);
                replaced += in.new_string;
                last = i + in.old_string.size();
            }
            auto begin = tools::lines::scalars(std::string_view{file->text}.substr(0, at.front()));
            auto count = tools::lines::scalars(std::string_view{file->text}.substr(at.front(), last - at.front()));
            auto tree = co_await tools::selected(s, in.project, "main", r.version, actor);
            auto done = co_await tools::main_apply(s, in.project, actor, tree,
                                                   {tools::change{.kind = "edit", .path = in.path, .offset = begin, .count = count, .text = replaced}},
                                                   in.operation_id, "edit " + in.path, rfl::json::write(tools::record{input, at.size()}));
            co_return edit_output{done.version, done.receipt, in.operation_id, at.size(), done.replayed};
        };
        co_return co_await tools::once<edit_output>(
            s, in.project, actor, std::optional<std::string>{in.operation_id}, input, std::move(apply_once),
            [&](const auto& done) { return edit_output{done.first.version, done.first.receipt, in.operation_id, done.second.replacements, true}; });
    }

    auto remove_file(tools::services& s, remove_input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<tools::mutation> {
        config::check_name("project", in.project);
        std::string input = content::hash_of(rfl::json::write(std::tuple{std::string{"remove"}, in.path})).str();
        auto apply_once = [&]() -> coroutine::task<tools::mutation> {
            auto observed = co_await tools::main_tree(s, in.project, actor);
            auto r = co_await tools::main_apply(s, in.project, actor, observed, {tools::change{.kind = "remove", .path = in.path}}, in.operation_id,
                                                "remove " + in.path, rfl::json::write(tools::record{input}));
            co_return tools::mutation{r.version, r.receipt, in.operation_id, r.replayed};
        };
        co_return co_await tools::once<tools::mutation>(
            s, in.project, actor, std::optional<std::string>{in.operation_id}, input, std::move(apply_once),
            [&](const auto& done) { return tools::mutation{done.first.version, done.first.receipt, in.operation_id, true}; });
    }

    auto move_file(tools::services& s, move_input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<tools::mutation> {
        config::check_name("project", in.project);
        std::string input = content::hash_of(rfl::json::write(std::tuple{std::string{"move"}, in.from, in.to})).str();
        auto apply_once = [&]() -> coroutine::task<tools::mutation> {
            auto observed = co_await tools::main_tree(s, in.project, actor);
            auto r = co_await tools::main_apply(s, in.project, actor, observed, {tools::change{.kind = "move", .path = in.from, .destination = in.to}},
                                                in.operation_id, "move " + in.from, rfl::json::write(tools::record{input}));
            co_return tools::mutation{r.version, r.receipt, in.operation_id, r.replayed};
        };
        co_return co_await tools::once<tools::mutation>(
            s, in.project, actor, std::optional<std::string>{in.operation_id}, input, std::move(apply_once),
            [&](const auto& done) { return tools::mutation{done.first.version, done.first.receipt, in.operation_id, true}; });
    }

    auto read_file(tools::services& s, read_input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<read_output> {
        config::check_name("project", in.project);
        auto r = co_await tools::node_call(s, in.project, {.action = "read", .worldline = "main", .target = in.path}, actor, true);
        auto file = std::ranges::find(r.files, in.path, &nodes::file_view::path);
        if (file == r.files.end())
            throw tools::tool_error(nodes::code::missing_objects, in.path + " is not in the project");
        if (file->kind != "text")
            throw tools::tool_error(nodes::code::invalid_record, in.path + " is a binary file");
        co_return read_output{file->text, r.version};
    }

    auto list_files(tools::services& s, list_input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<list_output> {
        config::check_name("project", in.project);
        auto tree = co_await tools::main_tree(s, in.project, actor);
        list_output out{.version = tree.version};
        for (auto& e : tools::lines::manifest(tree))
            out.files.push_back({e.path, e.size});
        co_return out;
    }

    auto search_files(tools::services& s, search_input in, mcp::notifier&, const worlds::actor& actor) -> coroutine::task<search_output> {
        config::check_name("project", in.project);
        auto tree = co_await tools::main_tree(s, in.project, actor, true);
        search_output out{.version = tree.version};
        std::ranges::sort(tree.files, {}, &nodes::file_view::path);
        for (auto& f : tree.files) {
            if (f.kind != "text")
                continue;
            std::istringstream text{f.text};
            std::uint64_t number = 0;
            for (std::string line; std::getline(text, line);) {
                ++number;
                if (line.find(in.query) != std::string::npos)
                    out.matches.push_back({f.path, number, line});
            }
        }
        co_return out;
    }
} // namespace fixture
