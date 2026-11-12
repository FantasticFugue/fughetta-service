import std;
import boost.ut;
import coroutine;
import native;
import semantic;
import lsp;
import reflection;
import support;

namespace {
    using namespace boost::ut;

    // A language server written for this suite. It answers as its scenario says, and logs every method it receives, and the result of
    // every response, so that a test can tell what a session asked and answered.
    static constexpr auto server = R"python(
import json, os, sys, pathlib

here = os.path.dirname(os.path.abspath(sys.argv[0]))
scenario = json.load(open(os.path.join(here, "scenario.json")))
log = open(os.path.join(here, "sent.log"), "w", encoding="utf-8", newline="\n")

def write(message):
    body = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()

def read():
    length = 0
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if not line.strip():
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(sys.stdin.buffer.read(length))

def items(specs):
    return [{"range": {"start": {"line": line, "character": character}, "end": {"line": line, "character": character + width}},
             "severity": severity, "code": code, "message": message}
            for (line, character, width, severity, code, message) in specs]

uri = None
while True:
    message = read()
    if message is None:
        break
    method = message.get("method")
    log.write((method or "response " + json.dumps(message.get("result"))) + "\n")
    if method in ("textDocument/didOpen", "textDocument/didChange", "textDocument/didClose", "workspace/didChangeWatchedFiles"):
        log.write(json.dumps(message["params"]) + "\n")
    log.flush()
    if method == "initialize":
        write({"jsonrpc": "2.0", "id": message["id"], "result": {"capabilities": scenario["capabilities"]}})
    elif method == "initialized" and scenario.get("configure"):
        write({"jsonrpc": "2.0", "id": 900, "method": "workspace/configuration", "params": {"items": [{"section": "mock"}, {"section": "mock"}]}})
    elif method == "textDocument/didOpen":
        uri = message["params"]["textDocument"]["uri"]
        if scenario.get("idle"):
            write({"jsonrpc":"2.0", "id":901, "method":"workspace/configuration", "params":{"items":[{}]}})
            write({"jsonrpc":"2.0", "method":"textDocument/publishDiagnostics", "params":{"uri":uri,"version":1,"diagnostics":items([[0,0,1,2,"idle","idle diagnostic"]])}})
    elif method == "textDocument/didChange" and scenario.get("idle"):
        for version, message in [(2,"current diagnostic"),(1,"stale diagnostic")]:
            write({"jsonrpc":"2.0", "method":"textDocument/publishDiagnostics", "params":{"uri":uri,"version":version,"diagnostics":items([[0,0,1,2,"idle",message]])}})
    elif method == "textDocument/documentSymbol":
        if scenario.get("push"):
            write({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {"uri": uri, "diagnostics": items(scenario["push"])}})
        write({"jsonrpc": "2.0", "id": message["id"], "result": scenario.get("symbols", [])})
    elif method == "textDocument/diagnostic":
        if scenario.get("hang"):
            continue
        if scenario.get("fail"):
            write({"jsonrpc": "2.0", "id": message["id"], "error": {"code": -32803, "message": scenario["fail"]}})
        else:
            write({"jsonrpc": "2.0", "id": message["id"], "result": {"kind": "full", "items": items(scenario.get("pull", []))}})
    elif method in ("textDocument/definition", "textDocument/references"):
        log.write(json.dumps(message["params"]) + "\n")
        log.flush()
        results = []
        for spec in scenario.get("locations", []):
            results.append({"uri": pathlib.Path(here, spec["path"]).as_uri(), "range": spec["range"]})
        write({"jsonrpc": "2.0", "id": message["id"], "result": results})
    elif method == "shutdown":
        write({"jsonrpc": "2.0", "id": message["id"], "result": None})
    elif method == "exit":
        sys.exit(0)
)python";

    // The mock server and its scenario in a scratch tree.
    struct mock {
        test::scratch tmp;
        std::vector<std::string> argv;
        mock(std::string_view name, std::string_view scenario) : tmp(name) {
            test::spit(tmp.dir / "server.py", server);
            test::spit(tmp.dir / "scenario.json", scenario);
            argv = {std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "python3", (tmp.dir / "server.py").string()};
        }
        auto sent() const -> std::string { return test::slurp(tmp.dir / "sent.log"); }
    };

    struct outcome {
        std::optional<semantic::outline> report;
        std::string error;
        int code = -1;
    };

    // Starts a server on `tree`, describes one document, and stops the server. A document that fails is an error of the outcome.
    auto describe(const std::vector<std::string>& argv, const std::filesystem::path& tree, std::string_view settings, const lsp::document& document)
        -> outcome {
        native::context context;
        auto run_operation = [&]() -> coroutine::task<outcome> {
            lsp::session session{context.get_scheduler(), {.argv = argv, .cwd = tree}};
            outcome out;
            co_await session.start(tree, *rfl::json::read<rfl::Generic>(settings));
            try {
                out.report = co_await session.describe(document);
            } catch (const std::exception& error) {
                out.error = error.what();
            }
            out.code = co_await session.stop();
            co_return out;
        };
        auto ran = cores::run(context, run_operation());
        expect(ran.has_value() >> fatal);
        return std::move(*ran);
    }

    auto is(const std::optional<semantic::place>& place, std::string_view path, std::uint64_t begin, std::uint64_t end) -> bool {
        return place && place->path == path && place->begin == begin && place->end == end;
    }
    auto has(const semantic::outline& report, std::string_view text) -> bool {
        return std::ranges::any_of(report.diagnostics, [&](auto& d) { return d.message.contains(text); });
    }

    static constexpr auto lsp_suite = [] {
        "a server that offers document symbols is asked for them and for no diagnostics"_test = [] {
            mock m{"lsp-symbols", R"({
                "capabilities": {"positionEncoding": "utf-32", "documentSymbolProvider": true},
                "symbols": [{"name": "demo", "kind": 3,
                             "range": {"start": {"line": 0, "character": 0}, "end": {"line": 2, "character": 1}},
                             "selectionRange": {"start": {"line": 0, "character": 10}, "end": {"line": 0, "character": 14}},
                             "children": [{"name": "twice", "kind": 12,
                                           "range": {"start": {"line": 1, "character": 0}, "end": {"line": 1, "character": 30}},
                                           "selectionRange": {"start": {"line": 1, "character": 14}, "end": {"line": 1, "character": 19}}}]}]})"};
            auto done =
                describe(m.argv, m.tmp.dir, "{}", {.path = "a.cpp", .language = "cpp", .text = "namespace demo {\nint twice(int x) { return x; }\n}\n"});
            expect((done.report.has_value()) >> fatal) << done.error;
            expect(done.code == 0) << "the server exits after shutdown";
            auto& outline = *done.report;
            auto find = [&](std::string_view name) { return std::ranges::find(outline.symbols, name, &semantic::symbol::name); };
            expect((find("demo") != outline.symbols.end() && find("twice") != outline.symbols.end()) >> fatal);
            expect(find("twice")->scope == find("demo")->id) << "members nest under their namespace";
            expect(find("demo")->id == "a.cpp#demo" && find("twice")->id == "a.cpp#demo::twice");
            expect(std::ranges::any_of(outline.scopes, [&](auto& s) { return s.id == find("demo")->id && s.kind == "namespace"; }));
            expect(is(find("demo")->definitions.at(0).place, "a.cpp", 10, 14)) << "a place counts Unicode scalars";
            expect(done.report->diagnostics.empty());
            expect(m.sent().contains("textDocument/documentSymbol") && !m.sent().contains("textDocument/diagnostic"));
        };

        "a server that only pulls diagnostics reports them without inventing symbols, and gets its settings"_test = [] {
            mock m{"lsp-pull", R"({
                "capabilities": {"diagnosticProvider": {"identifier": "mock", "interFileDependencies": false, "workspaceDiagnostics": false}},
                "configure": true,
                "pull": [[0, 14, 1, 1, "invalid-syntax", "Expected an expression"]]})"};
            // The emoji is two UTF-16 units and one scalar, so the second `=` is at 14 for the server and at 13 for a place.
            auto done = describe(m.argv, m.tmp.dir, R"({"settings": {"diagnosticMode": "workspace"}})",
                                 {.path = "s.py", .language = "python", .text = "s = '😀'; x = = 1\n"});
            expect((done.report.has_value()) >> fatal) << done.error;
            expect(done.report->symbols.empty() && done.report->scopes.empty());
            expect((done.report->diagnostics.size() == 1u) >> fatal);
            auto& found = done.report->diagnostics.front();
            expect(found.severity == "error" && found.message == "[invalid-syntax] Expected an expression");
            expect(is(found.range.place, "s.py", 13, 14));
            auto sent = m.sent();
            expect(sent.contains("textDocument/diagnostic") && !sent.contains("textDocument/documentSymbol"));
            expect(sent.contains(R"(response [{"diagnosticMode": "workspace"}, {"diagnosticMode": "workspace"}])"))
                << "settings answer workspace/configuration";
        };

        "diagnostics a server pushes while its symbols are awaited are kept"_test = [] {
            mock m{"lsp-push", R"({
                "capabilities": {"documentSymbolProvider": true},
                "push": [[1, 0, 3, 2, "W1", "unused"]]})"};
            auto done = describe(m.argv, m.tmp.dir, "{}", {.path = "p.py", .language = "python", .text = "x = 1\ny = 2\n"});
            expect((done.report.has_value()) >> fatal) << done.error;
            expect((done.report->diagnostics.size() == 1u) >> fatal);
            expect(done.report->diagnostics.front().severity == "warning" && done.report->diagnostics.front().message == "[W1] unused");
            expect(is(done.report->diagnostics.front().range.place, "p.py", 6, 9));
            expect(!m.sent().contains("textDocument/diagnostic"));
            expect(!done.report->complete) << "unversioned pushes are not safe content-cache entries";
        };

        "a server that declares nothing to ask fails the document and is not shown it"_test = [] {
            mock m{"lsp-none", R"({"capabilities": {"textDocumentSync": 1}})"};
            auto done = describe(m.argv, m.tmp.dir, "{}", {.path = "x.py", .language = "python", .text = "x = 1\n"});
            expect(!done.report && done.error.contains("neither documentSymbolProvider nor diagnosticProvider")) << done.error;
            expect(!m.sent().contains("textDocument/didOpen"));
        };

        "a refused request fails the document with the server's reason"_test = [] {
            mock m{"lsp-refused", R"({
                "capabilities": {"diagnosticProvider": {"interFileDependencies": false, "workspaceDiagnostics": false}},
                "fail": "analysis crashed"})"};
            auto done = describe(m.argv, m.tmp.dir, "{}", {.path = "x.py", .language = "python", .text = "x = 1\n"});
            expect(!done.report && done.error.contains("refused textDocument/diagnostic") && done.error.contains("analysis crashed")) << done.error;
        };

        "a cancelled query releases its response waiter and stops the background reader"_test = [] {
            mock m{"lsp-cancel", R"({"capabilities":{"diagnosticProvider":true},"hang":true})"};
            native::context context;
            auto run_operation = [&]() -> coroutine::task<bool> {
                auto io = context.get_scheduler();
                lsp::session session{io, {.argv = m.argv, .cwd = m.tmp.dir}};
                co_await session.start(m.tmp.dir, *rfl::json::read<rfl::Generic>("{}"));
                lsp::document document{"a.py", "python", "x=1"};
                auto result = co_await coroutine::deadline(io, std::chrono::milliseconds{30}, session.describe(document));
                co_await session.abort();
                co_return !result;
            };
            auto ran = cores::run(context, run_operation());
            expect(ran && *ran);
        };

        "an idle session answers server requests and keeps only current diagnostics"_test = [] {
            mock m{"lsp-idle", R"({"capabilities":{"documentSymbolProvider":true},"idle":true})"};
            native::context context;
            auto run_operation = [&]() -> coroutine::task<int> {
                auto io = context.get_scheduler();
                lsp::session session{io, {.argv = m.argv, .cwd = m.tmp.dir}};
                co_await session.start(m.tmp.dir, *rfl::json::read<rfl::Generic>(R"({"settings":{"mode":"test"}})"));
                std::vector<lsp::document> documents{{"a.py", "python", "x = 1\n"}};
                co_await session.synchronize(documents);
                for (int i = 0; i < 100 && !m.sent().contains("response [{"); ++i)
                    co_await execution::schedule_after(io, std::chrono::milliseconds{5});
                expect(m.sent().contains(R"(response [{"mode": "test"}])")) << "no client query was needed to answer the server";
                auto first = co_await session.describe(documents.front());
                expect((first.diagnostics.size() == 1u) >> fatal);
                expect(first.diagnostics.front().message.contains("idle diagnostic"));
                documents.front().text = "x = 2\n";
                co_await session.synchronize(documents);
                auto current = co_await session.describe(documents.front());
                expect((current.diagnostics.size() == 1u) >> fatal);
                expect(current.diagnostics.front().message.contains("current diagnostic"));
                expect(current.complete) << "version-matched pushes describe this snapshot";
                expect(!current.cacheable) << "pushes cannot certify changes to dependencies in other files";
                co_return co_await session.stop();
            };
            auto ran = cores::run(context, run_operation());
            expect(ran && *ran == 0);
        };

        "definition and reference queries select names and return Unicode locations"_test = [] {
            mock m{"lsp-locations", R"({"capabilities":{"definitionProvider":true,"referencesProvider":true},
                "locations":[{"path":"b.py","range":{"start":{"line":0,"character":10},"end":{"line":0,"character":13}}}]})"};
            native::context context;
            auto run_operation = [&]() -> coroutine::task<int> {
                lsp::session session{context.get_scheduler(), {.argv = m.argv, .cwd = m.tmp.dir}};
                co_await session.start(m.tmp.dir, *rfl::json::read<rfl::Generic>("{}"));
                std::vector<lsp::document> documents{{"a.py", "python", "foo()\nfoo()\n"}, {"b.py", "python", "x = '😀'; foo()\n"}};
                co_await session.synchronize(documents);
                bool ambiguous = false;
                try {
                    (void)co_await session.lookup({.action = "definitions", .path = "a.py", .symbol = "foo"});
                } catch (const std::invalid_argument&) {
                    ambiguous = true;
                }
                expect(ambiguous);
                auto definitions = co_await session.lookup({.action = "definitions", .path = "a.py", .symbol = "foo", .line = 2});
                expect((definitions.size() == 1u) >> fatal);
                expect(definitions.front().path == "b.py" && definitions.front().line == 1u && definitions.front().column == 10u &&
                       definitions.front().end_column == 13u);
                auto references = co_await session.lookup({.action = "references", .path = "b.py", .symbol = "foo"});
                expect(references.size() == 1u);
                co_return co_await session.stop();
            };
            auto ran = cores::run(context, run_operation());
            expect(ran && *ran == 0);
            expect(m.sent().contains("\"character\": 10") && m.sent().contains("\"includeDeclaration\": true"));
        };

        "a session updates open files without restarting and closes removed files"_test = [] {
            mock m{"lsp-incremental", R"({"capabilities":{"diagnosticProvider":true}, "pull":[]})"};
            native::context context;
            auto run_operation = [&]() -> coroutine::task<int> {
                lsp::session session{context.get_scheduler(), {.argv = m.argv, .cwd = m.tmp.dir}};
                co_await session.start(m.tmp.dir, *rfl::json::read<rfl::Generic>("{}"));
                std::vector<lsp::document> documents{{"a.py", "python", "x = '😀'\n"}, {"b.py", "python", "y = 1\n"}};
                co_await session.synchronize(documents);
                (void)co_await session.describe(documents.front());
                documents.front().text = "x = '😀'; z = 2\n";
                documents.pop_back();
                std::vector<std::string> generated{"compile_commands.json"};
                co_await session.synchronize(documents, generated);
                (void)co_await session.describe(documents.front());
                co_await session.synchronize(documents);
                co_return co_await session.stop();
            };
            auto ran = cores::run(context, run_operation());
            expect(ran && *ran == 0);
            auto log = m.sent();
            auto occurrences = [&](std::string_view value) {
                std::size_t count = 0, at = 0;
                while ((at = log.find(value, at)) != std::string::npos) {
                    ++count;
                    at += value.size();
                }
                return count;
            };
            expect(occurrences("initialize\n") == 1u);
            expect(occurrences("textDocument/didOpen\n") == 2u);
            expect(occurrences("textDocument/didChange\n") == 1u);
            expect(occurrences("textDocument/didClose\n") == 1u);
            expect(log.contains("\"version\": 2") && log.contains("compile_commands.json"));
        };

        "a survey collects diagnostics from every visited document"_test = [] {
            mock m{"lsp-survey", R"({
                "capabilities": {"diagnosticProvider": {"interFileDependencies": false, "workspaceDiagnostics": false}},
                "pull": [[0, 0, 1, 1, "E3", "bad"]]})"};
            native::context context;
            auto run_operation = [&]() -> coroutine::task<semantic::outline> {
                co_return co_await lsp::survey(
                    context.get_scheduler(), {.argv = m.argv, .cwd = m.tmp.dir}, *rfl::json::read<rfl::Generic>("{}"), m.tmp.dir,
                    {{.path = "a.py", .language = "python", .text = "x = 1\n"}, {.path = "b.py", .language = "python", .text = "y = 2\n"}});
            };
            auto ran = cores::run(context, run_operation());
            expect((ran.has_value()) >> fatal);
            expect((ran->diagnostics.size() == 2u) >> fatal);
            expect(ran->diagnostics.at(0).range.place->path == "a.py" && ran->diagnostics.at(1).range.place->path == "b.py");
        };

        "a clice outline becomes semantic scopes and symbols"_test = [] {
            auto clice = std::getenv("TEST_CLICE");
            if (!clice) {
                std::println("skipped: TEST_CLICE names no clice executable");
                return;
            }
            test::scratch tmp{"lsp-clice"};
            std::string text =
                "namespace demo {\r\n// é😀\r\nstruct shape {\n    int area() const { return 1; }\n};\n/*é😀*/ int twice(int x) { return x * 2; }\n}\n";
            test::spit(tmp.dir / "a b#1.cpp", text);
            test::spit(tmp.dir / "compile_commands.json",
                       std::format(R"([{{"directory":"{}","file":"a b#1.cpp","arguments":["clang++","-std=c++23","-c","a b#1.cpp"]}}])", tmp.dir.string()));
            auto done = describe({clice, "serve", "--log-level", "off"}, tmp.dir,
                                 R"({"rules":[{"compile_commands":["compile_commands.json"]}],"project":{"cache_dir":"clice"}})",
                                 {.path = "a b#1.cpp", .language = "cpp", .text = text});
            expect((done.report.has_value()) >> fatal) << done.error;
            expect(done.code == 0) << "the server exits after shutdown";
            auto& outline = *done.report;
            auto find = [&](std::string_view name) { return std::ranges::find(outline.symbols, name, &semantic::symbol::name); };
            expect((find("demo") != outline.symbols.end() && find("shape") != outline.symbols.end() && find("twice") != outline.symbols.end()) >> fatal);
            auto area = find("area");
            expect((area != outline.symbols.end()) >> fatal);
            expect(area->scope == find("shape")->id) << "members nest under their class";
            auto& definition = *find("twice")->definitions.at(0).place;
            auto scalars = [](std::string_view s) { return std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }); };
            auto expected = scalars(std::string_view{text}.substr(0, text.find("twice")));
            expect(definition.path == "a b#1.cpp" && definition.begin == static_cast<std::uint64_t>(expected) && definition.end == definition.begin + 5)
                << definition.begin << " " << expected;
            auto scope = std::ranges::find(outline.scopes, find("demo")->id, &semantic::scope::id);
            expect((scope != outline.scopes.end()) >> fatal);
            expect(scope->range.place->begin == 0u && scope->kind == "namespace");
            expect(find("area")->kind == "function" && find("shape")->kind == "type");
            expect(find("demo")->id == "a b#1.cpp#demo" && area->id == "a b#1.cpp#demo::shape::area");
        };

        "ty reports a type error of a document it was shown"_test = [] {
            auto ty = std::getenv("TEST_TY");
            if (!ty) {
                std::println("skipped: TEST_TY names no ty executable");
                return;
            }
            test::scratch tmp{"lsp-ty"};
            std::string text = "x: int = \"a\"\n";
            test::spit(tmp.dir / "app.py", text);
            auto done = describe({ty, "server"}, tmp.dir, R"({"settings": {}})", {.path = "app.py", .language = "python", .text = text});
            expect((done.report.has_value()) >> fatal) << done.error;
            expect(has(*done.report, "invalid-assignment")) << "ty rejects a str for an int";
            expect(std::ranges::all_of(done.report->diagnostics, [](auto& d) { return d.range.place && d.range.place->path == "app.py"; }));
            auto ignored = describe({ty, "server"}, tmp.dir, R"({"settings":{"configuration":{"rules":{"invalid-assignment":"ignore"}}}})",
                                    {.path = "app.py", .language = "python", .text = text});
            expect((ignored.report.has_value()) >> fatal) << ignored.error;
            expect(!has(*ignored.report, "invalid-assignment")) << "the configured rule changes the real server's diagnostics";
        };

        "ruff reports an unused import of a document it was shown"_test = [] {
            auto ruff = std::getenv("TEST_RUFF");
            if (!ruff) {
                std::println("skipped: TEST_RUFF names no ruff executable");
                return;
            }
            test::scratch tmp{"lsp-ruff"};
            std::string text = "import os\n";
            test::spit(tmp.dir / "tool.py", text);
            auto done = describe({ruff, "server"}, tmp.dir, R"({"settings": {}})", {.path = "tool.py", .language = "python", .text = text});
            expect((done.report.has_value()) >> fatal) << done.error;
            expect(has(*done.report, "F401")) << "ruff's default rules report an import that is not used";
            auto ignored =
                describe({ruff, "server"}, tmp.dir, R"({"settings":{"lint":{"ignore":["F401"]}}})", {.path = "tool.py", .language = "python", .text = text});
            expect((ignored.report.has_value()) >> fatal) << ignored.error;
            expect(!has(*ignored.report, "F401")) << "the configured rule changes the real server's diagnostics";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"lsp">{lsp_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
