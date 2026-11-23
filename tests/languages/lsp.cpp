import std;
import toolchain;
import boost.ut;
import coroutine;
import native;
import semantic;
import knowledge;
import lsp;
import rust;
import evaluation;
import reuse;
import helper;
import json;
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

    auto unit_for(const lsp::document& document) -> semantic::unit {
        semantic::recipe recipe{{"app", "language", "test", "evaluate", semantic::unit_kind::file, document.path}, "options"};
        return semantic::make_unit(recipe, semantic::context_identifier(recipe));
    }
    auto source_definition(const semantic::shard& shard, const std::string& id) -> const semantic::place& {
        return *std::ranges::find(shard.occurrences, id, &semantic::records::occurrence::symbol)->location.source;
    }
    struct outcome {
        std::optional<semantic::shard> report;
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
            co_await session.start(tree, *json::read<json::value>(settings));
            try {
                out.report = co_await session.describe(document, unit_for(document));
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
            auto& records = *done.report;
            auto find = [&](std::string_view name) { return std::ranges::find(records.symbols, name, &semantic::records::symbol::name); };
            expect((find("demo") != records.symbols.end() && find("twice") != records.symbols.end()) >> fatal);
            expect(find("twice")->owner == find("demo")->id) << "members nest under their namespace";
            expect(find("demo")->id == "a.cpp#demo" && find("twice")->id == "a.cpp#demo::twice");
            expect(std::ranges::any_of(records.scopes, [&](auto& s) { return s.semantic_owner == find("demo")->id && s.kind == "namespace"; }));
            expect(is(std::optional{source_definition(records, find("demo")->id)}, "a.cpp", 10, 14)) << "a place counts Unicode scalars";
            expect(done.report->diagnostics.empty());
            expect(m.sent().contains("textDocument/documentSymbol") && !m.sent().contains("textDocument/diagnostic"));
        };

        "Rust LSP evaluation retains the exports attached to each function"_test = [] {
            mock m{"lsp-rust-exports", R"({
                "capabilities":{"documentSymbolProvider":true},
                "symbols":[
                    {"name":"java_add","kind":12,"range":{"start":{"line":2,"character":0},"end":{"line":2,"character":29}},
                     "selectionRange":{"start":{"line":2,"character":16},"end":{"line":2,"character":24}}},
                    {"name":"internal_add","kind":12,"range":{"start":{"line":3,"character":0},"end":{"line":4,"character":56}},
                     "selectionRange":{"start":{"line":4,"character":39},"end":{"line":4,"character":51}}},
                    {"name":"private_add","kind":12,"range":{"start":{"line":6,"character":0},"end":{"line":6,"character":30}},
                     "selectionRange":{"start":{"line":6,"character":14},"end":{"line":6,"character":25}}}
                ]})"};
            native::context context;
            auto evaluate = [&]() -> coroutine::task<language::evaluated_unit> {
                language::rust_adapter session{context.get_scheduler(), {.argv = m.argv, .cwd = m.tmp.dir},
                                               {.slot = "rust", .project = "app", .provider = "rust", .options = "rust", .stage = "rust"}};
                const auto opened = co_await session.open(language::server<"rust">::launch({}, {}).initialization);
                const std::vector<language::document> documents{{"lib.rs", "rust", R"rust(// 😀
#[no_mangle]
extern "C" fn   java_add() {}
#[unsafe(export_name = "add\x5fnumbers")]
pub(crate) unsafe extern "C-unwind" fn internal_add() {}
// #[no_mangle]
extern "C" fn private_add() {}
)rust"}};
                const auto installed = co_await session.update(
                    opened.epoch, {.snapshot = "rust-source", .documents = documents, .source_files = {{"lib.rs", "source"}}});
                const auto units = co_await session.units(installed.epoch, {});
                auto result = co_await session.evaluate({.execution = {"request", "computation", "rust"}, .slot_id = installed.slot, .unit = units.at(0),
                                                         .epoch = installed.epoch, .inputs = installed.inputs, .demand = {.facts = {std::string{"symbols"}}}});
                co_await session.close();
                co_return result;
            };
            const auto result = cores::run(context, evaluate());
            expect((result.has_value() && result->shards.size() == 1_u && result->shards.front().symbols.size() == 3_u) >> fatal);
            const auto& rows = result->shards.front().symbols;
            expect(rows[0].extensions.at("native.linkage_name") == json::value{"java_add"} && rows[0].modifiers == std::vector<std::string>{"no_mangle", "extern"});
            expect(rows[1].extensions.at("native.linkage_name") == json::value{"add_numbers"} &&
                   rows[1].modifiers == std::vector<std::string>{"export_name", "extern"});
            expect(!rows[2].extensions.contains("native.linkage_name") && rows[2].modifiers == std::vector<std::string>{"extern"});
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
            expect(done.report->unit.coverage.available.front().facts == std::vector<semantic::fact_kind>{std::string{"diagnostics"}});
            expect((done.report->diagnostics.size() == 1u) >> fatal);
            auto& found = done.report->diagnostics.front();
            expect(found.severity == "error" && found.message == "[invalid-syntax] Expected an expression");
            expect(is(found.location.source, "s.py", 13, 14));
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
            expect(is(done.report->diagnostics.front().location.source, "p.py", 6, 9));
            expect(!m.sent().contains("textDocument/diagnostic"));
            expect(!done.report->unit.coverage.missing.empty()) << "unversioned pushes are not safe content-cache entries";
        };

        "a server that declares nothing to ask fails the document and is not shown it"_test = [] {
            mock m{"lsp-none", R"({"capabilities": {"textDocumentSync": 1}})"};
            auto done = describe(m.argv, m.tmp.dir, "{}", {.path = "x.py", .language = "python", .text = "x = 1\n"});
            expect(!done.report && done.error.contains("no supported analysis request")) << done.error;
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
                co_await session.start(m.tmp.dir, *json::read<json::value>("{}"));
                lsp::document document{"a.py", "python", "x=1"};
                auto result = co_await coroutine::deadline(io, std::chrono::milliseconds{30}, session.describe(document, unit_for(document)));
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
                co_await session.start(m.tmp.dir, *json::read<json::value>(R"({"settings":{"mode":"test"}})"));
                std::vector<lsp::document> documents{{"a.py", "python", "x = 1\n"}};
                co_await session.synchronize(documents);
                for (int i = 0; i < 100 && !m.sent().contains("response [{"); ++i)
                    co_await execution::schedule_after(io, std::chrono::milliseconds{5});
                expect(m.sent().contains(R"(response [{"mode": "test"}])")) << "no client query was needed to answer the server";
                auto first = co_await session.describe(documents.front(), unit_for(documents.front()));
                expect((first.diagnostics.size() == 1u) >> fatal);
                expect(first.diagnostics.front().message.contains("idle diagnostic"));
                documents.front().text = "x = 2\n";
                co_await session.synchronize(documents);
                auto current = co_await session.describe(documents.front(), unit_for(documents.front()));
                expect((current.diagnostics.size() == 1u) >> fatal);
                expect(current.diagnostics.front().message.contains("current diagnostic"));
                expect(current.unit.coverage.missing.empty()) << "version-matched pushes describe this snapshot";
                expect(!current.unit.cacheable) << "pushes cannot certify changes to dependencies in other files";
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
                co_await session.start(m.tmp.dir, *json::read<json::value>("{}"));
                std::vector<lsp::document> documents{{"a.py", "python", "foo()\nfoo()\nbar()\n"}, {"b.py", "python", "x = '😀'; foo()\n"}};
                co_await session.synchronize(documents);
                bool ambiguous = false;
                try {
                    (void)co_await session.lookup(
                        {.action = std::string{"definitions"}, .selector = semantic::selection{.name = semantic::named{.text = "foo", .path = "a.py"}}});
                } catch (const std::invalid_argument&) {
                    ambiguous = true;
                }
                expect(ambiguous);
                auto definitions = co_await session.lookup(
                    {.action = std::string{"definitions"}, .selector = semantic::selection{.name = semantic::named{.text = "bar", .path = "a.py"}}});
                expect((definitions.targets.size() == 1u) >> fatal);
                expect(definitions.targets.front().source->path == "b.py" && definitions.targets.front().position->line == 1u &&
                       definitions.targets.front().position->column == 10u && definitions.targets.front().position->end_column == 13u);
                auto references = co_await session.lookup(
                    {.action = std::string{"references"},
                     .selector = semantic::selection{.entity = semantic::entity{"selected-symbol"}},
                     .target = semantic::resolution_target{.name = {.text = "foo", .path = "b.py"}, .occurrence = semantic::place{"b.py", 9, 12}}});
                expect(references.targets.size() == 1u);
                co_return co_await session.stop();
            };
            auto ran = cores::run(context, run_operation());
            expect(ran && *ran == 0);
            expect(m.sent().contains("\"character\": 10") && m.sent().contains("\"includeDeclaration\": true"));
        };

        "a stdio helper evaluates logical module units through the adapter lifecycle"_test = [] {
            test::scratch scratch{"module-helper"};
            semantic::unit_key key{"app", "modules", "test-helper", "evaluate", semantic::unit_kind::module, "library:main"};
            language::evaluated_unit produced;
            auto& shard = produced.shards.emplace_back();
            shard.unit = semantic::make_unit({key, "options"}, "module-context");
            shard.unit.direct_inputs = {{semantic::input_kind::file, "src/main.kt", "source-one"}};
            test::spit(scratch.dir / "result.json", json::write(produced));
            test::spit(scratch.dir / "helper.py", R"python(
import json, sys
with open('result.json') as stream:
    result = json.load(stream)
for line in sys.stdin:
    request = json.loads(line)
    operation = request['operation']
    answer = True
    if operation == 'units':
        answer = [result['shards'][0]['unit']['recipe']['unit']]
    elif operation == 'evaluate':
        answer = result
    elif operation == 'update':
        for objects in request['params']['facts'].values():
            for reference in objects:
                with open(reference['path']) as stream:
                    assert json.load(stream)['id'] == reference['id']
    print(json.dumps({'request_id': request['request_id'], 'result': answer}), flush=True)
)python");
            std::filesystem::create_directories(scratch.dir / "objects");
            native::context context;
            auto operation = [&]() -> coroutine::task<> {
                language::helper_adapter helper{
                    context.get_scheduler(),
                    {.argv = {std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "python3", (scratch.dir / "helper.py").string()},
                     .cwd = scratch.dir},
                    {.slot = "slot", .project = "app", .provider = "test-helper", .options = "options", .stage = "modules"}};
                helper.objects({.root = scratch.dir / "objects"});
                const auto opened = co_await helper.open(json::value{true});
                const auto installed = co_await helper.update(
                    opened.epoch, {.snapshot = "snapshot", .documents = {}, .source_files = {{"src/main.kt", "source-one"}}, .model = {}});
                const auto units = co_await helper.units(installed.epoch, {});
                expect(units == std::vector{key});
                const auto evaluated = co_await helper.evaluate(
                    {.execution = {"request", "computation", "modules"}, .slot_id = "slot", .unit = key, .epoch = installed.epoch, .inputs = installed.inputs});
                expect(evaluated.session.id == installed.id && evaluated.request.unit == key && evaluated.shards.size() == 1u);
                expect(evaluated.shards.front().unit.recipe.unit == key);
                semantic::shard upstream{.id = "upstream-object"};
                upstream.unit = semantic::make_unit({{"app", "source", "test-helper", "evaluate", semantic::unit_kind::file, "src/main.kt"}, "source-options"},
                                                    "source-context");
                auto object = std::make_shared<semantic::shard_object>();
                object->data = std::make_shared<const semantic::shard>(std::move(upstream));
                object->path = scratch.dir / "upstream.json";
                test::spit(object->path, json::write(*object->data));
                const auto updated = co_await helper.update(installed.epoch, {.snapshot = installed.snapshot,
                                                                              .documents = {},
                                                                              .source_files = {{"src/main.kt", "source-one"}},
                                                                              .model = {},
                                                                              .generated = {},
                                                                              .source_facts = {{"source", {object}}}});
                expect(updated.epoch == installed.epoch + 1) << "upstream records advance the installed epoch";
                co_await helper.close();
                expect(helper.state().phase == "closed");
            };
            expect(cores::run(context, operation()));
        };

        "a unit records the tools and environment used by its language process"_test = [] {
            mock m{"lsp-observations", R"({"capabilities":{"diagnosticProvider":true}, "pull":[]})"};
            native::context context;
            const auto executable = toolchain::tool_path(m.argv.front());
            const semantic::source_vector sources{{"app", {{"a.py", "source-file"}}}};
            const reuse::channel_values channels;
            std::vector<semantic::candidate> candidates;
            auto evaluate = [&](toolchain::identity installation, std::string mode, std::string ambient)
                -> coroutine::task<std::pair<semantic::unit, bool>> {
                language::lsp_adapter session{context.get_scheduler(),
                                              {.argv = m.argv, .cwd = m.tmp.dir, .env = builds::environment{{"MODE", std::move(mode)}}},
                                              {.slot = "slot", .project = "app", .provider = "ty", .options = "python", .stage = "python"},
                                              {}, std::move(installation)};
                const auto opened = co_await session.open(json::value{});
                const auto installed = co_await session.update(opened.epoch, {.snapshot = "snapshot", .documents = {{"a.py", "python", "x = 1"}},
                                                                             .source_files = sources.at("app")});
                const semantic::extent demand{.facts = {std::string{"diagnostics"}}};
                const auto units = co_await session.units(installed.epoch, demand);
                const auto unit = units.at(0);
                auto inputs = co_await session.current_inputs(candidates);
                inputs.at("app").push_back({semantic::input_kind::environment, "AMBIENT", std::move(ambient)});
                auto processes = session.current_processes(units, demand);
                const reuse::environment current{sources, inputs, channels, "app", {}, {}, std::move(processes)};
                const bool accepted = !candidates.empty() && reuse::accept(candidates.front(), installed, demand, current);
                if (accepted) {
                    auto incomplete = candidates.front();
                    std::erase_if(incomplete.units.front().direct_inputs, [](const auto& input) { return input.kind == semantic::input_kind::tool; });
                    expect(!reuse::accept(incomplete, installed, demand, current));
                }
                auto result = co_await session.evaluate({.execution = {"request", "computation", "python"}, .slot_id = installed.slot, .unit = unit,
                                                        .epoch = installed.epoch, .inputs = installed.inputs, .demand = demand});
                co_await session.close();
                co_return std::pair{std::move(result.shards.at(0).unit), accepted};
            };
            auto run_operation = [&]() -> coroutine::task<> {
                toolchain::identity installation{.executables = {{executable, "server-one"}, {"/tools/unused", "unused-one"}}};
                auto [first, reused] = co_await evaluate(installation, "one", "ambient-one");
                expect(first.cacheable && !reused);
                candidates = {{.id = "previous", .recipe = first.recipe, .units = {first}}};

                installation.executables["/tools/unused"] = "unused-two";
                installation.executables["/tools/added"] = "added";
                auto [other_installation, accepted] = co_await evaluate(installation, "one", "ambient-one");
                expect(accepted && std::ranges::equal(other_installation.direct_inputs, first.direct_inputs));

                auto [other_ambient, ambient_accepted] = co_await evaluate(installation, "one", "ambient-two");
                expect(ambient_accepted && std::ranges::equal(other_ambient.direct_inputs, first.direct_inputs));

                auto [other_mode, mode_accepted] = co_await evaluate(installation, "two", "ambient-one");
                expect(!mode_accepted && !std::ranges::equal(other_mode.direct_inputs, first.direct_inputs));

                installation.executables[executable] = "server-two";
                auto [other_server, tool_accepted] = co_await evaluate(installation, "one", "ambient-one");
                expect(!tool_accepted && !std::ranges::equal(other_server.direct_inputs, first.direct_inputs));
            };
            expect(cores::run(context, run_operation()));
        };

        "a provider session advances installed epochs while retaining its language process"_test = [] {
            mock m{"lsp-incremental", R"({"capabilities":{"diagnosticProvider":true,"documentSymbolProvider":true}, "symbols":[], "pull":[]})"};
            native::context context;
            auto run_operation = [&]() -> coroutine::task<int> {
                language::lsp_adapter session{context.get_scheduler(),
                                              {.argv = m.argv, .cwd = m.tmp.dir},
                                              {.slot = "python", .project = "app", .provider = "ty", .options = "python", .stage = "python"},
                                              {},
                                              {.executables = {{toolchain::tool_path(m.argv.front()), "mock-tool"}}}};
                auto opened = co_await session.open(*json::read<json::value>("{}"));
                std::vector<language::document> documents{{"a.py", "python", "x = '😀'\n"}, {"b.py", "python", "y = 1\n"}};
                std::map<std::string, std::string> sources{{"a.py", "a-1"}, {"b.py", "b-1"}, {"config.json", "config-1"}};
                auto first = co_await session.update(opened.epoch, {.snapshot = "source-a", .documents = documents, .source_files = sources, .model = {}});
                language::evaluation_request request{.execution = {"request-a", "shared-computation", "python"},
                                                     .slot_id = first.slot,
                                                     .unit = {"app", "python", "ty", "evaluate", semantic::unit_kind::file, "a.py"},
                                                     .epoch = first.epoch,
                                                     .inputs = first.inputs,
                                                     .demand = {.facts = {std::string{"diagnostics"}}}};
                request.slot_id = "other-slot";
                bool wrong_slot = false;
                try {
                    (void)co_await session.evaluate(request);
                } catch (const std::invalid_argument&) {
                    wrong_slot = true;
                }
                expect(wrong_slot && session.state().phase == "ready");
                request.slot_id = first.slot;
                auto initial = co_await session.evaluate(request);
                expect(initial.request.execution == request.execution &&
                       initial.shards.front().unit.coverage.available.front().facts == std::vector<semantic::fact_kind>{std::string{"diagnostics"}});
                expect(!m.sent().contains("textDocument/documentSymbol")) << "a diagnostic demand skips declaration work";
                expect(initial.session.id == opened.id && initial.session.snapshot == "source-a" && initial.session.epoch == 1u);
                semantic::store stored{m.tmp.dir / "semantic"};
                semantic::view selected{
                    .snapshots = {{"app", "main", "v1", first.snapshot}},
                    .configuration = "python-profile",
                    .effective_profile = first.inputs,
                    .coverage = {.requested = {.files = {"a.py"}, .facts = initial.shards.front().unit.coverage.available.front().facts},
                                 .available = {{.files = {"a.py"}, .facts = initial.shards.front().unit.coverage.available.front().facts}}}};
                auto saved = stored.publish_records(std::move(selected), initial.shards, {});
                stored.remember(saved, request.unit);
                auto candidates = stored.candidates("app", "python", {{"app", sources}});
                expect(candidates.size() == 1u >> fatal) << "an empty document still publishes its completed unit";
                candidates.front().retained = true;
                auto validated = [&]() -> coroutine::task<std::vector<semantic::candidate>> {
                    auto inputs = co_await session.current_inputs(candidates);
                    semantic::source_vector selected{{"app", sources}};
                    reuse::channel_values channels;
                    auto processes = session.current_processes({request.unit}, request.demand);
                    reuse::environment current{selected, inputs, channels, "app", {}, {}, std::move(processes)};
                    auto accepted = candidates;
                    std::erase_if(accepted, [&](const auto& candidate) { return !reuse::accept(candidate, session.state(), request.demand, current); });
                    co_return accepted;
                };
                request.retained_only = true;
                auto approved = co_await validated();
                auto reused = co_await session.evaluate(request, approved);
                expect(reused.reused == candidates.front().id && !reused.deferred && reused.shards.empty());
                documents.front().text = "x = '😀'; z = 2\n";
                documents.pop_back();
                std::map<std::string, std::string> generated{{"compile_commands.json", "model-1"}};
                sources = {{"a.py", "a-2"}, {"config.json", "config-1"}};
                auto second =
                    co_await session.update(first.epoch, {.snapshot = "source-b", .documents = documents, .source_files = sources, .model = generated});
                bool stale = false;
                try {
                    (void)co_await session.evaluate(request);
                } catch (const std::invalid_argument&) {
                    stale = true;
                }
                expect(stale) << "work for the previous input epoch never reaches the provider";
                request.execution.request_id = "request-b";
                request.epoch = second.epoch;
                request.inputs = second.inputs;
                request.demand.facts = {std::string{"symbols"}};
                auto remaining = co_await validated();
                auto deferred = co_await session.evaluate(request, remaining);
                expect(deferred.deferred && !deferred.reused) << "a retained contribution must still match the installed inputs and requested data";
                request.retained_only = false;
                auto evaluated = co_await session.evaluate(request);
                expect(std::ranges::equal(evaluated.request.inputs,
                                          std::vector<semantic::input_object>{{semantic::input_kind::source, "app", "source-b"},
                                                                              {semantic::input_kind::artifact, "compile_commands.json", "model-1"}}));
                expect(evaluated.shards.front().unit.coverage.available.front().facts ==
                           std::vector<semantic::fact_kind>{std::string{"symbols"}, std::string{"definitions"}, std::string{"scopes"}} &&
                       evaluated.shards.front().diagnostics.empty());
                expect(evaluated.session.epoch == 2u && evaluated.session.snapshot == "source-b" && evaluated.request.unit == request.unit);
                sources["config.json"] = "config-2";
                auto third =
                    co_await session.update(second.epoch, {.snapshot = "source-c", .documents = documents, .source_files = sources, .model = generated});
                expect(third.id == opened.id && third.epoch == 3u);
                auto repeated =
                    co_await session.update(third.epoch, {.snapshot = "source-c", .documents = documents, .source_files = sources, .model = generated});
                expect(repeated.id == opened.id && repeated.epoch == third.epoch && repeated.phase == "ready");
                co_await session.close();
                expect(session.state().phase == "closed");
                co_return 0;
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
            expect(occurrences("config.json") == 2u) << "non-document input changes reach the retained language process";
            expect(occurrences("textDocument/diagnostic\n") == 1u && occurrences("textDocument/documentSymbol\n") == 1u)
                << "each demand requests only its own LSP channel";
        };

        "cancelled provider operations cannot return to ready through retained or deferred completion"_test = [] {
            class interrupted : public language::reader_adapter {
            public:
                using reader_adapter::reader_adapter;
                unsigned closes = 0;

            protected:
                auto prepare(const semantic::extent&) -> coroutine::task<> override {
                    request_cancel();
                    co_return;
                }
                auto do_close() -> coroutine::task<> override {
                    ++closes;
                    co_return;
                }
                auto do_units(const semantic::extent&) -> coroutine::task<std::vector<semantic::unit_key>> override {
                    co_return std::vector<semantic::unit_key>{};
                }
                auto do_evaluate(const language::evaluation_request&) -> coroutine::task<language::evaluated_unit> override {
                    co_return language::evaluated_unit{};
                }
            };
            native::context context;
            auto run_operation = [&]() -> coroutine::task<> {
                for (bool reused : {false, true}) {
                    interrupted session{
                        context.get_scheduler(), {}, {.slot = "slot", .project = "app", .provider = "reader", .options = "options", .stage = "stage"}};
                    auto opened = co_await session.open({});
                    auto installed = co_await session.update(opened.epoch, {.snapshot = "snapshot"});
                    semantic::unit_key key{"app", "stage", "reader", "evaluate", semantic::unit_kind::module, "main"};
                    std::vector<semantic::candidate> candidates;
                    if (reused)
                        candidates.push_back({.id = "retained", .recipe = {key, "options"}, .retained = true});
                    bool cancelled = false;
                    try {
                        (void)co_await session.evaluate({.execution = {"request", "computation", "stage"},
                                                         .slot_id = "slot",
                                                         .unit = key,
                                                         .epoch = installed.epoch,
                                                         .inputs = installed.inputs,
                                                         .retained_only = true},
                                                        candidates);
                    } catch (const std::runtime_error&) {
                        cancelled = true;
                    }
                    expect(cancelled && session.state().phase == "failed");
                    co_await session.cancel();
                    co_await session.cancel();
                    co_await session.close();
                    expect(session.closes == 1u);
                }
            };
            expect(cores::run(context, run_operation()));
        };

        "a survey collects diagnostics from every visited document"_test = [] {
            mock m{"lsp-survey", R"({
                "capabilities": {"diagnosticProvider": {"interFileDependencies": false, "workspaceDiagnostics": false}},
                "pull": [[0, 0, 1, 1, "E3", "bad"]]})"};
            native::context context;
            auto run_operation = [&]() -> coroutine::task<std::vector<semantic::shard>> {
                language::lsp_adapter session{context.get_scheduler(),
                                              {.argv = m.argv, .cwd = m.tmp.dir},
                                              {.slot = "python", .project = "app", .provider = "ty", .options = "python", .stage = "python"}};
                auto opened = co_await session.open(*json::read<json::value>("{}"));
                auto state = co_await session.update(opened.epoch, {.snapshot = "source",
                                                                    .documents = {{"a.py", "python", "x = 1\n"}, {"b.py", "python", "y = 2\n"}},
                                                                    .source_files = {{"a.py", "a"}, {"b.py", "b"}},
                                                                    .model = {}});
                semantic::extent demand{.facts = {std::string{"diagnostics"}}};
                std::vector<semantic::shard> rows;
                for (const auto& unit : co_await session.units(state.epoch, demand)) {
                    auto evaluated = co_await session.evaluate({.execution = {"request", "computation", "python"},
                                                                .slot_id = state.slot,
                                                                .unit = unit,
                                                                .epoch = state.epoch,
                                                                .inputs = state.inputs,
                                                                .demand = demand});
                    std::ranges::move(evaluated.shards, std::back_inserter(rows));
                }
                co_await session.close();
                co_return rows;
            };
            auto ran = cores::run(context, run_operation());
            expect((ran.has_value() && ran->size() == 2u) >> fatal);
            expect(ran->at(0).diagnostics.front().location.source->path == "a.py" && ran->at(1).diagnostics.front().location.source->path == "b.py");
        };

        "clice document symbols produce semantic scopes and declarations"_test = [] {
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
            auto done = describe({clice, "serve"}, tmp.dir, R"({"rules":[{"compile_commands":["compile_commands.json"]}],"project":{"cache_dir":"clice"}})",
                                 {.path = "a b#1.cpp", .language = "cpp", .text = text});
            expect((done.report.has_value()) >> fatal) << done.error;
            expect(done.code == 0) << "the server exits after shutdown";
            auto& records = *done.report;
            auto find = [&](std::string_view name) { return std::ranges::find(records.symbols, name, &semantic::records::symbol::name); };
            expect((find("demo") != records.symbols.end() && find("shape") != records.symbols.end() && find("twice") != records.symbols.end()) >> fatal);
            auto area = find("area");
            expect((area != records.symbols.end()) >> fatal);
            expect(area->owner == find("shape")->id) << "members nest under their class";
            auto& definition = source_definition(records, find("twice")->id);
            auto scalars = [](std::string_view s) { return std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }); };
            auto expected = scalars(std::string_view{text}.substr(0, text.find("twice")));
            expect(definition.path == "a b#1.cpp" && definition.begin == static_cast<std::uint64_t>(expected) && definition.end == definition.begin + 5)
                << definition.begin << " " << expected;
            auto scope = std::ranges::find(records.scopes, std::optional{find("demo")->id}, &semantic::records::scope::semantic_owner);
            expect((scope != records.scopes.end()) >> fatal);
            expect(scope->location.source->begin == 0u && scope->kind == "namespace");
            expect(find("area")->kind == "function" && find("shape")->kind == "type");
            expect(find("demo")->id == "a b#1.cpp#demo" && area->id == "a b#1.cpp#demo::shape::area");
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"lsp">{lsp_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
