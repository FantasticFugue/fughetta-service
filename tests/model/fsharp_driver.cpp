#include "managed_session.hpp"

namespace {
    using namespace managed_test;

    constexpr std::string_view project = R"fixture(<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
<ItemGroup><Compile Include="Library.fs"/><Compile Include="App.fs"/></ItemGroup>
</Project>)fixture";

    constexpr std::string_view library = R"fixture(namespace Sample
module Library =
    let value = 42
)fixture";

    constexpr std::string_view consumer = R"fixture(namespace Sample
module App =
    let result = Library.value
    let scoped (input: int) =
        let local = input + 1
        local

type Payload<'T when 'T : comparison> = { Value: 'T; Count: int }
type Shape = Point of x: int * y: int | Label of caption: string
[<Measure>] type kg
[<Measure>] type second
module Features =
    let private hidden (left: int) right = left + right
    let curried (left: int, right: int) (label: string) = label + string (left + right)
    let inline comparable<'T when 'T : comparison> (value: 'T) = compare value value
    let inline length< ^T when ^T : (member Length : int)> (value: ^T) = (^T : (member Length : int) value)
    let speed (distance: float<kg>) (elapsed: float<second>) = distance / elapsed
    let (|Even|Odd|) (value: int) = if value % 2 = 0 then Even else Odd
    let classify value = match value with Even -> 1 | Odd -> 0
module Interop =
    open System.Runtime.InteropServices
    [<DllImport("native", EntryPoint = "add_numbers", CallingConvention = CallingConvention.Cdecl)>]
    extern int add(int left, int right)
    [<DllImport("native")>]
    extern void reset()
)fixture";

    constexpr std::string_view signature = R"fixture(namespace Sample
module Library =
    val value: int
)fixture";

    constexpr std::string_view dependency = R"fixture(namespace Cross
module Dependency =
    [<Literal>]
    let Code = 1
    let compute (value: int) = value + 1
    let inline twice (value: int) = value + value
)fixture";

    constexpr std::string_view consume = R"fixture(namespace Cross
module Consumer =
    let result = Dependency.compute 2 + Dependency.twice 3
)fixture";

    constexpr std::string_view dependency_project = R"fixture(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup><Compile Include="Dependency.fs"/></ItemGroup></Project>)fixture";

    constexpr std::string_view cross_project = R"(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup><ProjectReference Include="dependency/Dependency.fsproj"/><Compile Include="Consumer.fs"/></ItemGroup></Project>)";

    auto has(const array& rows, std::string_view key, std::string_view name) -> bool {
        return std::ranges::any_of(rows, [&](const auto& row) { return optional_field(row, key) == value{name}; });
    }

    void check_features(const value& shard) {
        const auto& symbols = items(shard, "symbols");
        const auto& scopes = items(shard, "scopes");
        const auto& types = items(shard, "types");
        auto named = [&](std::string_view name) -> const value& { return by(symbols, "name", name); };
        auto type = [&](const value& symbol) -> const value& { return by(types, "id", text(symbol, "type")); };
        auto extension = [](const value& row, std::string_view key) -> const value& { return field(field(row, "extensions"), key); };

        const auto& library_value = by(symbols, "qualified_name", "Sample.Library.value");
        const auto& result = named("result");
        const auto& function = named("scoped");
        require(has(items(shard, "references"), "target", text(library_value, "id")), "ordered source reference");
        const auto& function_scope = select(scopes, [&](const auto& row) { return optional_field(row, "semantic_owner") == field(function, "id"); },
                                            "expected the scoped function's lexical scope");
        require(text(function_scope, "kind") == "function", "function scope");
        require(text(by(scopes, "id", text(function_scope, "lexical_parent")), "kind") == "translation_unit", "file scope");
        const auto& members = items(function_scope, "declared_members");
        for (const auto& symbol : symbols)
            if (optional_field(symbol, "owner") == field(function, "id"))
                require(field(symbol, "declaring_scope") == field(function_scope, "id") && std::ranges::contains(members, field(symbol, "id")),
                        "local belongs to the function scope");
        for (const auto* name : {"input", "local"})
            require(field(named(name), "owner") == field(function, "id"), "function has binder " + std::string{name});
        require(text(named("input"), "kind") == "parameter", "implicit parameter binder");
        require(std::ranges::any_of(scopes, [&](const auto& scope) {
            return optional_field(scope, "semantic_owner") == field(result, "id") && text(scope, "kind") == "initializer";
        }), "result initializer scope");

        const auto& curried = named("curried");
        require(text(curried, "visibility") == "public" && text(named("hidden"), "visibility") == "private", "member visibility");
        const auto& groups = items(type(curried), "parameter_groups");
        require(groups.size() == 2 && items(groups[0], "parameters").size() == 2 && items(groups[1], "parameters").size() == 1,
                "curried parameter groups");
        const auto& parameters = items(curried, "parameters");
        require(parameters.size() == 3, "curried parameters");
        const std::array names{"left", "right", "label"};
        for (std::size_t i = 0; i < names.size(); ++i)
            require(number(parameters[i], "group") == (i == 2 ? 1 : 0) && number(parameters[i], "index") == (i == 1 ? 1 : 0) &&
                        text(parameters[i], "name") == names[i], "curried parameter coordinates");

        const auto& payload = named("Payload");
        const auto& payload_type = type(payload);
        require(field(payload_type, "declaration") == field(payload, "id") && std::ranges::contains(items(payload, "modifiers"), value{"record"}),
                "record declaration");
        const auto& generic = by(types, "id", only(items(payload_type, "type_parameters")).to_string().value());
        require(field(generic, "binder") == field(payload, "id") && number(generic, "parameter_index") == 0 &&
                    std::ranges::contains(items(generic, "qualifiers"), value{"comparison"}), "record generic constraint");
        const auto& length_generic = by(types, "id", only(items(type(named("length")), "type_parameters")).to_string().value());
        const auto& constraint = by(items(extension(length_generic, "fsharp.constraints")), "kind", "member");
        require(text(constraint, "name") == "get_Length" && has(types, "id", text(constraint, "result")), "member constraint");
        const auto& returned = by(types, "id", text(type(named("speed")), "return_type"));
        const auto& measure = by(types, "id", only(items(returned, "arguments")).to_string().value());
        const auto& measure_args = items(measure, "arguments");
        require(std::ranges::contains(items(measure, "qualifiers"), value{"measure"}) && measure_args.size() == 2, "compound measure");
        const auto& inverse = by(types, "id", measure_args[1].to_string().value());
        require(std::ranges::contains(items(inverse, "qualifiers"), value{"measure"}) && items(inverse, "arguments").size() == 1, "inverse measure");

        const auto& even = extension(named("Even"), "fsharp.active_pattern");
        require(fields(even).size() == 3 && items(even, "names") == array{"Even", "Odd"} && number(even, "index") == 0 && field(even, "total") == value{true} &&
                    number(extension(named("Odd"), "fsharp.active_pattern"), "index") == 1, "active pattern projection");
        const auto& shape_key = field(field(named("Shape"), "native_key"), "native_key");
        for (const auto& [name, index] : {std::pair{"Point", 0}, {"Label", 1}}) {
            const auto& row = named(name);
            require(extension(row, "fsharp.sequence").to_int().value() == index && extension(row, "fsharp.declaring_entity") == shape_key, "union case metadata");
        }
        for (const auto& [name, variant, index] : {std::tuple{"x", 0, 0}, {"y", 0, 1}, {"caption", 1, 0}}) {
            const auto& row = named(name);
            require(text(row, "kind") == "field" && fields(field(row, "native_key")).get("anchor") &&
                        has(items(shard, "occurrences"), "symbol", text(row, "id")), "union field identity and definition");
            require(extension(row, "fsharp.variant").to_int().value() == variant && extension(row, "fsharp.sequence").to_int().value() == index &&
                        extension(row, "fsharp.declaring_entity") == shape_key, "union field metadata");
        }
        for (const auto& row : symbols)
            require(extension(row, "fsharp.assembly") == value{"App"}, "symbol assembly");
        const auto& add = named("add");
        const auto& reset = named("reset");
        require(std::ranges::contains(items(add, "modifiers"), value{"pinvoke"}) &&
                    std::ranges::contains(items(reset, "modifiers"), value{"pinvoke"}) &&
                    !std::ranges::contains(items(curried, "modifiers"), value{"pinvoke"}), "P/Invoke modifiers");
        require(extension(add, "clr.pinvoke.library") == value{"native"} && extension(add, "clr.pinvoke.entry_point") == value{"add_numbers"} &&
                    extension(add, "clr.pinvoke.convention") == value{"cdecl"} && extension(reset, "clr.pinvoke.entry_point") == value{"reset"} &&
                    extension(reset, "clr.pinvoke.convention") == value{"platform"}, "P/Invoke metadata");
    }

    auto result_type(const value& shard) -> const std::string& {
        return text(by(items(shard, "types"), "id", text(by(items(shard, "symbols"), "name", "result"), "type")), "name");
    }

    void transfer_model(const value& model, const fs::path& root, const fs::path& destination) {
        std::set<std::string> paths{"model/model.json"};
        for (const auto& unit : items(model, "units")) {
            for (const auto& source : items(unit, "sources"))
                paths.insert(source.to_string().value());
            paths.insert(text(unit, "project"));
            for (const auto& [_, generated] : fields(field(unit, "generated")))
                paths.insert("objects/" + text(generated, "hash"));
            for (const auto& reference : items(unit, "references")) {
                if (text(reference, "kind") == "artifact")
                    paths.insert("objects/" + text(field(reference, "object"), "hash"));
                else if (text(reference, "kind") == "tree")
                    paths.insert(text(reference, "path"));
            }
        }
        for (const auto& path : paths) {
            fs::create_directories((destination / path).parent_path());
            fs::copy_file(root / path, destination / path);
        }
    }

    auto scenario(driver& runtime, const char** argv) -> coroutine::task<> {
        const auto dotnet = fs::canonical(argv[1]).string();
        const auto host = fs::absolute(argv[2]).string();
        const auto adapter = fs::absolute(argv[3]).string();
        const auto validator = fs::absolute(argv[4]).string();
        const auto worker = fs::absolute(argv[5]).string();
        const auto exporter = fs::absolute(argv[7]).string();
        const auto sdk = fs::path{dotnet}.parent_path();
        workspace work{argv[6], "fcs"};
        const auto& root = work.root;
        runtime.configure(root, "sdk/dotnet", "installed-sdk", sdk);
        test::spit(root / "App.fsproj", project);
        test::spit(root / "Library.fs", library);
        test::spit(root / "App.fs", consumer);
        auto model = co_await runtime.export_model(root, "msbuild", dotnet, exporter, {"App.fsproj"});
        require(!fs::exists(root / "bin") || std::ranges::none_of(fs::recursive_directory_iterator(root / "bin"), [](const auto& entry) {
            return entry.path().extension() == ".dll";
        }), "analysis must precede compilation");
        (void)only(items(model, "units"));
        co_await runtime.run({worker, "--fsharp", root.string(), dotnet, host, adapter, sdk.string()}, root);

        const auto transcript_path = root / "transcript.jsonl";
        std::ofstream transcript{transcript_path, std::ios::binary};
        require(bool(transcript), "cannot open helper transcript");
        conversation helper{runtime, &transcript};
        helper.start({dotnet, host, "--adapter", adapter}, root);
        auto state = session("workspace-fcs");
        state["snapshot"] = "snapshot";
        state["inputs"] = array{};
        state["phase"] = "ready";
        int epoch = 0;
        const auto requested = demand({"symbols", "types", "references", "diagnostics"});
        co_await helper.call("open", opening(state, root, "sdk/dotnet", sdk));
        auto update = [&](std::string source, std::optional<std::string> interface = {}) -> coroutine::task<> {
            state["epoch"] = ++epoch;
            array documents{document("Library.fs", "fsharp", source), document("App.fs", "fsharp", consumer)};
            if (interface)
                documents.emplace_back(document("Library.fsi", "fsharp", *interface));
            co_await helper.call("update", installation(state, std::move(documents), "model-object", {}, "Library.fs"));
        };
        co_await update(std::string{library});
        auto key = only(items(co_await helper.call("units", {{"epoch", epoch}, {"demand", requested}})));
        require(field(key, "provider") == state["provider"], "unit belongs to the installed provider");
        co_await runtime.run({validator, "--unit", (root / "unit.json").string()}, root);
        auto unit_template = parse(test::slurp(root / "unit.json"));
        fields(unit_template)["coverage"] = object{{"requested", requested}, {"available", array{}}, {"missing", array{}}, {"unresolved_count", 0}};
        auto evaluate = [&](array files = {}) -> coroutine::task<value> {
            auto selected = unit_template;
            fields(selected)["recipe"] = object{{"unit", key}, {"options", "options"}};
            auto& selection = fields(fields(fields(selected)["coverage"])["requested"]);
            selection["files"] = std::move(files);
            object request{{"execution", object{{"request_id", "request"}, {"computation_id", "computation"}, {"stage_id", "source"}}},
                           {"slot_id", "slot"}, {"unit", key}, {"epoch", epoch}, {"inputs", array{}}, {"demand", selection},
                           {"budget", object{}}, {"retained_only", false}};
            const auto reply = co_await helper.call("evaluate", {{"request", std::move(request)}, {"unit", selected}});
            require(items(reply, "receipts").empty(), "unexpected evaluation receipts");
            const auto& shard = only(items(reply, "shards"));
            const auto& producer_keys = field(reply, "producer_keys");
            const auto& reads = items(field(shard, "unit"), "observed_dependencies");
            std::set<std::string> read_keys;
            for (const auto& read : reads) {
                auto id = text(read, "key");
                read_keys.insert(id);
                const auto& producer = only(items(producer_keys, id));
                require(fields(producer).size() == 6 && field(producer, "project") == state["project"] &&
                            field(producer, "stage") == state["stage"] && field(producer, "provider") == state["provider"] &&
                            text(producer, "operation") == "evaluate" && text(producer, "kind") == "module" &&
                            field(producer, "name") == field(read, "scope"), "typed producer key");
                require(has(items(model, "units"), "id", text(producer, "name")) && items(read, "producers").empty(), "host binds a declared producer");
            }
            require(read_keys.size() == fields(producer_keys).size(), "producer keys correspond to observed reads");
            const auto& symbols = items(shard, "symbols");
            for (const auto& reference : items(shard, "references")) {
                const auto target = optional_field(reference, "target").to_string();
                const auto expected = target && has(symbols, "id", *target) ? field(selected, "context") : value{};
                require(optional_field(reference, "target_context") == expected, "reference context ownership");
            }
            for (const auto& identity : items(shard, "identities"))
                require(field(field(identity, "declaration"), "provider") == state["provider"], "identity provider");
            for (const auto& symbol : symbols)
                require(field(field(field(symbol, "ownership"), "origin"), "provider") == state["provider"], "symbol provider");
            for (const auto& diagnostic : items(shard, "diagnostics"))
                require(field(diagnostic, "producer") == state["provider"], "diagnostic provider");
            test::spit(root / "shard.json", json::write(shard));
            co_await runtime.run({validator, "--shard", (root / "shard.json").string()}, root);
            co_return shard;
        };

        const auto before = co_await evaluate();
        require(no_errors(before), json::write(field(before, "diagnostics")));
        check_features(before);
        const auto model_path = root / "model/model.json";
        const auto original_model = test::slurp(model_path);
        auto missing_model = parse(original_model);
        auto& arguments = fields(std::get<array>(fields(missing_model)["units"].data).front())["compiler_arguments"];
        std::get<array>(arguments.data).emplace_back("-r:${tree}/MissingReference.dll");
        test::spit(model_path, json::write(missing_model));
        co_await update(std::string{library});
        const auto missing = co_await evaluate();
        bool reported = false;
        for (const auto& diagnostic : items(missing, "diagnostics"))
            if (text(diagnostic, "message").contains("MissingReference.dll")) {
                reported = true;
                require(fields(field(diagnostic, "location")).size() == 1 && field(field(diagnostic, "location"), "project") == state["project"] &&
                            text(field(field(diagnostic, "ownership"), "origin"), "kind") == "unknown", "project diagnostic location");
            }
        require(reported, json::write(field(missing, "diagnostics")));
        test::spit(model_path, original_model);

        co_await update(std::string{library});
        const auto local = co_await evaluate({"App.fs"});
        const auto& local_unit = field(local, "unit");
        require(items(field(local_unit, "contributions"), "files") == array{"App.fs"} &&
                    items(items(field(local_unit, "coverage"), "available").at(0), "files") == array{"App.fs"}, "file coverage");
        (void)by(items(local, "symbols"), "name", "result");
        require(!has(items(local, "symbols"), "qualified_name", "Sample.Library.value"), "file demand excludes prefix definitions");
        const auto changed_library = replaced(std::string{library}, "42", "\"changed\"");
        co_await update(changed_library);
        const auto changed = co_await evaluate({"App.fs"});
        require(result_type(changed) == "Microsoft.FSharp.Core.string" &&
                    field(by(items(changed, "symbols"), "name", "result"), "id") == field(by(items(before, "symbols"), "name", "result"), "id"),
                "type changes preserve source identity");
        const auto complete = co_await evaluate();
        const auto& contributed = items(field(field(complete, "unit"), "contributions"), "files");
        require(std::ranges::contains(contributed, value{"App.fs"}) && std::ranges::contains(contributed, value{"Library.fs"}) &&
                    has(items(complete, "symbols"), "qualified_name", "Sample.Library.value"), "complete demand restores definitions");
        co_await update(replaced(std::string{library}, "42", "missingValue"));
        require(!no_errors(co_await evaluate()), "broken source diagnostic");
        require(test::slurp(root / "Library.fs") == library, "updates stay in the helper snapshot");

        test::spit(root / "Library.fsi", signature);
        test::spit(root / "App.fsproj", replaced(std::string{project}, "<Compile Include=\"Library.fs\"/>",
                                                 "<Compile Include=\"Library.fsi\"/><Compile Include=\"Library.fs\"/>"));
        co_await runtime.export_model(root, "msbuild", dotnet, exporter, {"App.fsproj"});
        co_await update(replaced(std::string{library}, "42", "\"wrong implementation\""), std::string{signature});
        const auto bounded = co_await evaluate({"App.fs"});
        require(no_errors(bounded) && result_type(bounded) == "Microsoft.FSharp.Core.int", "consumer observes signature despite invalid implementation");
        require(!no_errors(co_await evaluate()), "project demand diagnoses invalid implementation");
        co_await update(changed_library, replaced(std::string{signature}, "int", "string"));
        require(result_type(co_await evaluate({"App.fs"})) == "Microsoft.FSharp.Core.string", "changed signature type");
        co_await update(std::string{library}, std::string{signature});
        require(result_type(co_await evaluate({"App.fs"})) == "Microsoft.FSharp.Core.int", "restored signature type");

        test::spit(root / "dependency/Dependency.fsproj", dependency_project);
        test::spit(root / "dependency/Dependency.fs", dependency);
        test::spit(root / "Consumer.fs", consume);
        test::spit(root / "App.fsproj", cross_project);
        model = co_await runtime.export_model(root, "msbuild", dotnet, exporter, {"App.fsproj"});
        const auto& consumer_unit = select(items(model, "units"), [](const auto& row) {
            return std::ranges::contains(items(row, "sources"), value{"Consumer.fs"});
        }, "expected the consumer unit");
        object sources;
        for (const auto& module : items(model, "units"))
            for (const auto& path : items(module, "sources")) {
                const auto name = path.to_string().value();
                sources[name] = "source:" + name;
            }
        auto cross_update = [&](std::string source, std::string revision) -> coroutine::task<> {
            state["epoch"] = ++epoch;
            sources["dependency/Dependency.fs"] = std::move(revision);
            co_await helper.call("update", installation(state, {document("dependency/Dependency.fs", "fsharp", source),
                                                               document("Consumer.fs", "fsharp", consume)},
                                                        "cross-model", sources, "dependency/Dependency.fs"));
        };
        co_await cross_update(std::string{dependency}, "dependency-one");
        const auto keys = co_await helper.call("units", {{"epoch", epoch}, {"demand", requested}});
        const auto consumer_key = by(items(keys), "name", text(consumer_unit, "id"));
        key = consumer_key;
        fields(unit_template)["direct_inputs"] = array{object{{"kind", "source"}, {"name", "app"}, {"object", "snapshot"}, {"type", ""}}};
        const auto initial = co_await evaluate();
        require(no_errors(initial), json::write(field(initial, "diagnostics")));
        key = select(items(keys), [&](const auto& row) { return field(row, "name") != field(consumer_key, "name"); }, "expected the dependency unit");
        const auto dependency_shard = co_await evaluate();
        const auto& compute = by(items(dependency_shard, "symbols"), "qualified_name", "Cross.Dependency.compute");
        key = consumer_key;
        require(has(items(initial, "references"), "target", text(compute, "id")), "reference targets the producer's declaration");
        const auto& reads = items(field(initial, "unit"), "observed_dependencies");
        std::set<std::string> channels;
        std::map<std::string, std::string> before_edit;
        for (const auto& read : reads) {
            channels.insert(text(read, "channel"));
            before_edit.emplace(text(read, "key"), text(read, "value"));
        }
        require(channels == std::set<std::string>{"declarations", "inline"}, "declaration and inline channel reads");
        const auto& inputs = items(field(initial, "unit"), "direct_inputs");
        require(!has(inputs, "kind", "source") && !has(inputs, "name", "dependency/Dependency.fs"), "channel reads replace snapshot and dependency source");
        auto observe = [&]() -> coroutine::task<std::map<std::string, std::string>> {
            const auto reply = co_await helper.call("observe", {{"session", state}, {"reads", reads}});
            std::map<std::string, std::string> observed;
            for (const auto& item : items(reply))
                observed.emplace(text(item, "name"), text(item, "object"));
            co_return observed;
        };
        require((co_await observe()) == before_edit, "observation of the original read set");
        const auto body_edit = replaced(std::string{dependency}, "value + 1", "value + 2");
        co_await cross_update(body_edit, "dependency-two");
        require((co_await observe()) == before_edit, "body edit retains declaration and inline reads");
        co_await cross_update(replaced(body_edit, "value + value", "value * 2"), "dependency-inline");
        const auto inline_edit = co_await observe();
        for (const auto& read : reads)
            require((inline_edit.at(text(read, "key")) != text(read, "value")) == (text(read, "channel") == "inline"), "inline-only invalidation");
        auto declaration_edit = [&](std::string contents, std::string revision) -> coroutine::task<> {
            co_await cross_update(std::move(contents), revision);
            const auto observed = co_await observe();
            require(std::ranges::any_of(reads, [&](const auto& read) {
                return text(read, "channel") == "declarations" && observed.at(text(read, "key")) != text(read, "value");
            }), revision + " changes the declaration bucket");
        };
        co_await declaration_edit(body_edit + "    let introduced (value: int) = value\n", "dependency-added");
        co_await declaration_edit(replaced(body_edit, "(value: int) = value + 2", "(value: int64) = value + 2L"), "dependency-signature");
        co_await declaration_edit(replaced(std::string{dependency}, "let Code = 1", "let Code = 2"), "dependency-constant");
        co_await cross_update(std::string{dependency} + "    type Payload = { Value: int }\n", "dependency-complex");
        require((co_await observe()).empty(), "unmodeled types invalidate the prior reads");
        const auto fallback = co_await evaluate();
        require(has(items(field(fallback, "unit"), "direct_inputs"), "name", "dependency/Dependency.fs"), "unsupported signatures retain dependency source");
        co_await helper.finish();

        workspace next{argv[6], "fcs-worker"};
        transfer_model(model, root, next.root);
        runtime.configure(next.root, "sdk/dotnet", "installed-sdk", sdk);
        helper.start({dotnet, host, "--adapter", adapter}, next.root);
        state["id"] = "worker-two-session";
        state["slot"] = "worker-two-slot";
        state["epoch"] = epoch = 0;
        co_await helper.call("open", opening(state, next.root, "sdk/dotnet", sdk));
        co_await cross_update(body_edit, "worker-two-dependency");
        require((co_await observe()) == before_edit, "persisted reads reproduce in a fresh worker tree");
        co_await helper.finish();
        transcript.close();
        co_await runtime.run({validator, "--helper", transcript_path.string()}, root);
        std::cout << "F# model, ordered edits, references and record ingestion passed\n";
    }
}

int main(int argc, const char** argv) {
    try {
        test::require(argc == 8, "usage: fsharp-driver <dotnet> <host> <adapter> <validator> <worker> <work> <exporter>");
        native::context context;
        managed_test::driver runtime{context.get_scheduler()};
        test::require(cores::run(context, scenario(runtime, argv)), "F# scenario stopped");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
