#pragma once

import launch;
import helper;
import runner;
import host;
import constants;

namespace managed_test {
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    using json::array;
    using json::object;
    using json::value;
    using test::require;

    struct workspace {
        fs::path root;
        workspace(const fs::path& work, std::string_view name) {
            fs::create_directories(work);
            const auto parent = fs::canonical(work);
            do {
                root = parent / std::format("{}-{}", name, std::random_device{}());
            } while (!fs::create_directory(root));
        }
        workspace(const workspace&) = delete;
        ~workspace() {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    };

    inline auto parse(std::string_view text) -> value {
        auto parsed = json::read<value>(text);
        if (!parsed)
            throw std::runtime_error(parsed.error().what());
        return std::move(*parsed);
    }

    inline auto fields(auto& row) -> auto& { return std::get<object>(row.data); }

    inline auto field(const value& row, std::string_view key) -> const value& {
        const auto& properties = std::get<object>(row.data);
        const auto found = properties.find(key);
        if (found == properties.end())
            throw std::runtime_error("missing field " + std::string{key});
        return found->second;
    }

    inline auto optional_field(const value& row, std::string_view key) -> value {
        auto found = std::get<object>(row.data).get(key);
        return found ? std::move(*found) : value{};
    }

    inline auto text(const value& row, std::string_view key) -> const std::string& { return std::get<std::string>(field(row, key).data); }
    inline auto number(const value& row, std::string_view key) -> std::int64_t { return field(row, key).to_int64().value(); }
    inline auto items(const value& row) -> const array& { return std::get<array>(row.data); }
    inline auto items(const value& row, std::string_view key) -> const array& { return items(field(row, key)); }

    template <class Predicate>
    auto select(const array& rows, Predicate matches, std::string_view name) -> const value& {
        auto found = std::ranges::find_if(rows, matches);
        require(found != rows.end() && std::find_if(std::next(found), rows.end(), matches) == rows.end(), name);
        return *found;
    }

    inline auto by(const array& rows, std::string_view key, std::string_view name) -> const value& {
        return select(rows, [&](const value& row) { return text(row, key) == name; }, name);
    }

    inline auto only(const array& rows) -> const value& {
        require(rows.size() == 1, "expected a single record");
        return rows.front();
    }

    inline auto replaced(std::string source, std::string_view from, std::string_view to) -> std::string {
        const auto at = source.find(from);
        require(at != source.npos, from);
        source.replace(at, from.size(), to);
        return source;
    }

    inline auto no_errors(const value& shard) -> bool {
        return std::ranges::none_of(items(shard, "diagnostics"), [](const auto& row) { return text(row, "severity") == "error"; });
    }

    inline auto document(std::string_view path, std::string_view language, std::string_view contents) -> object {
        return {{"path", path}, {"language", language}, {"text", contents}};
    }

    inline auto demand(array facts, array files = {}) -> object {
        return {{"files", std::move(files)}, {"modules", array{}}, {"contexts", array{}}, {"projects", array{}}, {"facts", std::move(facts)}};
    }

    inline auto session(std::string_view provider) -> object {
        return {{"id", "session"}, {"slot", "slot"}, {"project", "app"}, {"provider", provider},
                {"stage", "source"}, {"options", "options"}, {"epoch", 0}};
    }

    class driver {
    public:
        native::scheduler io;
        std::map<std::string, std::string> env = host::environment();

        explicit driver(native::scheduler scheduler) : io(std::move(scheduler)) {}

        void configure(const fs::path& root, std::string_view sdk_name, std::string_view sdk_id, const fs::path& sdk) {
            env["TMPDIR"] = root.string();
            env["FUGHETTA_STAGING"] = (root / "objects").string();
            env["FUGHETTA_SDKS"] = json::write(array{object{{"name", sdk_name}, {"object", sdk_id}, {"roots", array{sdk.string()}}}});
        }

        auto run(std::vector<std::string> argv, fs::path cwd) -> coroutine::task<std::string> {
            const auto output = cwd / ".driver-stdout";
            io::command command{.argv = std::move(argv), .cwd = std::move(cwd), .env = env, .standard_output = output};
            const auto name = command.argv.front();
            auto result = co_await runner::run_step(io, std::move(command), 600s, 1u << 20, name);
            auto stdout_text = test::slurp(output);
            if (result.exit_code || result.timed_out)
                throw std::runtime_error(std::format("{} exited with code {}{}\n{}{}", name, result.exit_code,
                                                     result.timed_out ? " after timeout" : "", stdout_text, result.output));
            co_return stdout_text;
        }

        auto export_model(fs::path root, std::string kind, std::string tool, std::string exporter,
                          std::vector<std::string> projects = {}) -> coroutine::task<value> {
            fs::remove_all(env.at("FUGHETTA_STAGING"));
            const auto output = (root / "model/model.json").string();
            std::vector<std::string> args;
            if (kind == "gradle") {
                const auto receipt = (root / "model/dependencies.json").string();
                args = {tool, "--no-daemon", "--console=plain", "--no-configuration-cache", "--max-workers=2", "--init-script", exporter,
                        "-Dfughetta.root=" + root.string(), "-Dfughetta.prepare.output=" + receipt, "-Dfughetta.model.output=" + output,
                        "-Dfughetta.dependencies=" + receipt, "fughettaPrepare", "fughettaModel"};
            } else {
                const auto receipt = (root / "model/assets.json").string();
                args = {tool, exporter, "--tool", tool, "--root", root.string(), "--project-dir", root.string(), "--jobs", "2"};
                for (const auto& project : projects)
                    args.insert(args.end(), {"--project", project});
                args.insert(args.end(), {"--restore-output", receipt, "--model-output", output, "--assets", receipt, "--operations", "restore", "model"});
            }
            co_await run(std::move(args), root);
            co_return parse(test::slurp(output));
        }
    };

    class conversation {
    public:
        explicit conversation(driver& runtime, std::ostream* transcript = nullptr) : runtime_(runtime), transcript_(transcript) {}
        void start(std::vector<std::string> argv, fs::path cwd) {
            child_.emplace(io::spawn_process(runtime_.io, {.argv = std::move(argv), .cwd = std::move(cwd), .env = runtime_.env,
                                                           .stdio = io::process_stdio::blocking}));
        }

        auto call(std::string operation, object params) -> coroutine::task<value> {
            const language::helper::message<object> packet{++sequence_, operation, std::move(params)};
            auto outgoing = json::write(packet) + "\n";
            require(outgoing.size() <= constants::semantic_object_bytes, "helper request exceeds the semantic object limit");
            co_await io::async_write(*child_, std::as_bytes(std::span{outgoing}));
            std::size_t end;
            while ((end = pending_.find('\n')) == std::string::npos) {
                std::array<std::byte, 16384> chunk;
                const auto count = co_await io::async_read_some(*child_, net::buffer(chunk));
                require(count != 0, "helper closed stdout during " + operation);
                require(pending_.size() + count <= constants::semantic_object_bytes, "helper response exceeds the semantic object limit");
                pending_.append(reinterpret_cast<const char*>(chunk.data()), count);
            }
            auto reply = parse(std::string_view{pending_}.substr(0, end));
            pending_.erase(0, end + 1);
            if (transcript_)
                *transcript_ << json::write(object{{"message", json::to_value(packet)}, {"response", reply}}) << '\n';
            require(number(reply, "request_id") == static_cast<std::int64_t>(sequence_), "unexpected request ID in helper reply");
            const auto& response = std::get<object>(reply.data);
            if (const auto error = response.find("error"); error != response.end())
                throw std::runtime_error(operation + ": " + json::write(error->second));
            co_return field(reply, "result");
        }

        auto finish() -> coroutine::task<> {
            co_await call("close", {});
            auto exit = co_await coroutine::deadline(runtime_.io, 15s, io::async_wait(*child_));
            require(exit && *exit == 0, "helper did not exit cleanly after close");
            child_.reset();
            pending_.clear();
        }

    private:
        driver& runtime_;
        std::ostream* transcript_;
        std::optional<native::process> child_;
        std::string pending_;
        std::uint64_t sequence_ = 0;
    };

    inline auto opening(const object& state, const fs::path& root, std::string_view sdk_name, const fs::path& sdk) -> object {
        return {{"session", state}, {"initialization", object{}},
                {"host", object{{"objects", (root / "objects").string()}, {"sdks", object{{std::string{sdk_name}, sdk.string()}}}}}};
    }

    inline auto installation(const object& state, array documents, std::string_view model, object sources, std::string_view changed) -> object {
        return {{"session", state}, {"documents", std::move(documents)}, {"artifacts", object{{"model/model.json", model}}},
                {"sources", std::move(sources)}, {"generated", object{}}, {"changed", array{changed}}, {"facts", object{}}};
    }
}
