import std;
import boost.ut;
import support;
import jni;
import pinvoke;
import json;
import llvm;
import language;
import channels;
import objects;
import native;
import coroutine;
import knowledge;

namespace {
    using namespace boost::ut;
    // One source unit's records: each symbol is defined in `path`, unless it
    // is only declared.
    struct source_unit {
        std::string project, provider;
        semantic::shard shard;
        void add(semantic::records::symbol symbol, const std::string& path, bool defined = true) {
            symbol.ownership = {provider + "-unit", provider + "-context", {}};
            semantic::records::occurrence occurrence{.id = symbol.id + ":definition", .ownership = symbol.ownership, .symbol = symbol.id};
            occurrence.location.source = semantic::place{path, 0, 1};
            occurrence.roles = {std::string{"declaration"}};
            if (defined)
                occurrence.roles.push_back(std::string{"definition"});
            shard.occurrences.push_back(std::move(occurrence));
            shard.identities.push_back({semantic::native_identity{project, symbol.module, provider, symbol.domain, symbol.id}, symbol.id, {}});
            shard.symbols.push_back(std::move(symbol));
        }
    };
    auto symbol(std::string id, std::string language, std::string domain, std::string module, std::string name, std::string qualified, std::string kind,
                std::vector<std::string> modifiers = {}) {
        return semantic::records::symbol{.id = std::move(id),
                                         .language = std::move(language),
                                         .domain = std::move(domain),
                                         .module = std::move(module),
                                         .name = std::move(name),
                                         .qualified_name = std::move(qualified),
                                         .kind = std::move(kind),
                                         .modifiers = std::move(modifiers)};
    }
    auto binding(const semantic::shard& shard, const std::string& from) -> const semantic::records::binding_detail& {
        const auto found = std::ranges::find(shard.relations, from, &semantic::records::relation::from);
        if (found == shard.relations.end() || !found->detail)
            throw std::invalid_argument("no binding from " + from);
        return std::get<semantic::records::binding_detail>(found->detail->variant());
    }
    const auto cases = [] {
        "Python runtime reports bind every captured overload to its native definition"_test = [] {
            ::test::scratch scratch{"python-native-bindings"};
            std::ofstream{scratch.dir / "report"} << R"json({
              "domain":"python","module":"example","library":"example.so",
              "symbols":[{"name":"Counter","kind":"type","surface":{}},
                {"name":"Counter.renamed","owner":"Counter","kind":"method","surface":{},"wrapper":"nanobind","native_targets":[
                  {"library":"example.so","offset":16,"linkage_name":"first"},{"library":"impl.so","offset":16,"linkage_name":"second"}]}],
              "calls":[{"member":"Counter.renamed","path":"caller.py","caller":"<module>","first_line":1,"begin":5,"end":17,
                "native_targets":[{"native":{"library":"impl.so","offset":16,"linkage_name":"second"},"dispatch":"direct"}],
                "unresolved_native_dispatch":true}]
            })json";
            semantic::shard cpp;
            cpp.unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "impl.cpp"}, "cpp"}, "cpp-context");
            const semantic::ownership definition_owner{cpp.unit.id, cpp.unit.context};
            const semantic::place body{"impl.cpp", 0, 25};
            cpp.identities = {{semantic::native_identity{"app", "impl", "clice", "source", "implementation"}, "implementation"}};
            cpp.symbols = {{.id = "implementation", .ownership = definition_owner, .language = "cpp", .domain = "source", .module = "impl",
                            .name = "implementation", .qualified_name = "implementation", .kind = "function"}};
            cpp.occurrences = {{.id = "implementation:definition", .ownership = definition_owner, .symbol = "implementation", .location = {.source = body},
                                .roles = {std::string{"definition"}}, .body = body}};
            const auto definitions = channels::source_definitions({&cpp}, {std::string{"function"}});
            semantic::store views{scratch.dir / "views"};
            auto graph = views.describe(views.publish_records({.snapshots = {{"app", "main", "v1", "snapshot"}}}, {cpp}, {}));
            auto publish = [&](semantic::shard records) {
                const auto unit = records.unit.id;
                auto view = views.load(views.publish_records(graph, {std::move(records)}, {}));
                graph = view->description;
                const auto found = std::ranges::find_if(view->pieces, [&](const auto& piece) { return piece->unit.id == unit; });
                if (found == view->pieces.end())
                    throw std::runtime_error("published input unit has no shard");
                return std::make_shared<semantic::shard_object>((*found)->materialized(), nullptr, std::filesystem::path{});
            };
            auto library = [&](std::string module, std::string artifact, std::string name) {
                llvm::object value{.format = "elf", .arch = "x86_64",
                                   .symbols = {{.name = std::move(name), .kind = "function", .binding = "Local", .address = 16, .size = 8}}};
                auto records = llvm::records(value, "app", "options", module, module, artifact, {});
                if (module == "impl")
                    llvm::associate(records, {{16, {{"impl.cpp", 1, 5}}}}, definitions, {{"impl.cpp", "int implementation() { return 0; }\n"}},
                                    scratch.dir);
                records.unit.modules = {module};
                records.unit.channel_definitions = language::server<"native-artifact">::channels();
                records.unit.output_channels = channels::outputs(records.unit.channel_definitions, {&records});
                return publish(std::move(records));
            };
            std::set<std::string> bound;
            std::vector<semantic::shard> resolved;
            semantic::entity caller;
            cores::io_context<native::context> runtime{1};
            runtime.run([&](auto& io, std::size_t) -> coroutine::task<> {
                using reader = language::server<"python-runtime">;
                reader::adapter observe{io, {.cwd = scratch.dir}, {.project = "app", .stage = "observe", .provider = "python-runtime", .options = "observe"}};
                observe.objects({scratch.dir, {"report"}});
                auto session = co_await observe.open(reader::launch({}, {{"artifact", "python-runtime", "runtime.json", "file"}}).initialization);
                session = co_await observe.update(session.epoch, {.snapshot = "snapshot", .source_files = {{"caller.py", "caller"}},
                                                                 .model = {{"runtime.json", "report"}}});
                const auto entries = co_await observe.units(session.epoch, {});
                auto read = co_await observe.evaluate({.execution = {"request", "compute", "observe"}, .slot_id = session.slot, .unit = entries.front(),
                                                       .epoch = session.epoch, .inputs = session.inputs});
                co_await observe.close();
                auto& observed = read.shards.front();
                const auto method = std::ranges::find(observed.symbols, std::string{"python:Counter.renamed"}, &semantic::records::symbol::id);
                expect((method != observed.symbols.end()) >> fatal);
                expect(method->name == "renamed" && method->owner == "python:Counter");
                expect(std::ranges::any_of(observed.relations, [&](const auto& relation) {
                    if (relation.kind != "calls" || relation.to != method->id || !relation.detail)
                        return false;
                    const auto& detail = std::get<semantic::records::native_call_detail>(relation.detail->variant());
                    return detail.evidence == "unresolved" && !detail.dispatch;
                }));
                expect(std::ranges::any_of(observed.relations, [&](const auto& relation) {
                    if (relation.kind != "calls" || relation.to == method->id || !relation.detail)
                        return false;
                    const auto& detail = std::get<semantic::records::native_call_detail>(relation.detail->variant());
                    return detail.evidence == "observed" && detail.dispatch && *detail.dispatch == "direct";
                }));
                observed.unit.channel_definitions = reader::channels();
                observed.unit.output_channels = channels::outputs(observed.unit.channel_definitions, {&observed});
                const auto report = publish(std::move(observed));
                const auto& records = report->data->symbols;
                const auto method_id = std::ranges::find_if(records, [](const auto& symbol) {
                    return symbol.qualified_name == "example.Counter.renamed" && !symbol.extensions.contains("python.observed_dispatch");
                });
                const auto caller_symbol = std::ranges::find(records, std::string{"<module>"}, &semantic::records::symbol::qualified_name);
                expect((method_id != records.end() && caller_symbol != records.end()) >> fatal);
                caller = {caller_symbol->id};
                auto bridge = language::make_adapter(io, {.cwd = scratch.dir},
                                                     {.slot = "slot", .project = "app", .provider = "pybind", .options = "options", .stage = "bind"}, {},
                                                     {.execution = "runtime"});
                auto command = language::server<"pybind">::launch({.libraries = {{"example.so", "example"}, {"impl.so", "impl"}}}, {});
                auto active = co_await bridge->open(command.initialization);
                const auto exported = library("example", "example.so", "first");
                const auto implementation = library("impl", "impl.so", "second");
                active = co_await bridge->update(active.epoch, {.snapshot = "snapshot", .source_facts = {
                    {"managed", {report}}, {"native", {exported, implementation}}}});
                for (const auto& unit : co_await bridge->units(active.epoch, {})) {
                    auto result = co_await bridge->evaluate({.execution = {"request", "compute", "bind"}, .slot_id = active.slot, .unit = unit,
                                                            .epoch = active.epoch, .inputs = active.inputs});
                    for (const auto& relation : result.shards.front().relations)
                        if (relation.kind == "binds" && relation.from == method_id->id && relation.resolution == "resolved" && relation.detail)
                            if (const auto& detail = std::get<semantic::records::binding_detail>(relation.detail->variant()); detail.entry_point)
                                bound.insert(*detail.entry_point);
                    resolved.push_back(std::move(result.shards.front()));
                }
                co_await bridge->close();
            });
            expect(bound == std::set<std::string>{"first", "second"});
            auto view = views.load(views.publish_records(graph, std::move(resolved), {}));
            const auto chain = view->query({.action = std::string{"relations"}, .selector = semantic::selection{.entity = caller},
                                            .relations = {"calls", "binds", "emits"}, .direction = std::string{"both"}, .depth = 3});
            expect(chain.relations.size() == 6_u) << json::write(chain.relations);
            expect(std::ranges::count(chain.relations, std::string{"calls"}, &semantic::records::relation::kind) == 2);
            expect(std::ranges::any_of(chain.relations, [](const auto& relation) {
                const auto& at = relation.ownership.origin.location;
                return relation.kind == "emits" && at && at->source && at->source->path == "impl.cpp";
            }));
        };
        "Kotlin external declarations bind to C++ Java_ definitions without artifacts"_test = [] {
            source_unit kotlin{"app", "kotlin"}, cpp{"app", "clice"};
            kotlin.add(symbol("add", "kotlin", "kotlin", "app", "add", "sample/Native.Inner.add", "method", {"external"}), "src/Native.kt");
            kotlin.add(symbol("ping", "kotlin", "kotlin", "app", "ping", "sample/ping", "function", {"external"}), "src/native-util.kt");
            kotlin.add(symbol("gone", "kotlin", "kotlin", "app", "gone", "sample/Native.gone", "method", {"external"}), "src/Native.kt");
            kotlin.add(symbol("score", "kotlin", "kotlin", "app", "<get-score>", "sample/Native.score.get", "method", {"external"}), "src/Native.kt");
            cpp.add(symbol("c-add", "cpp", "source", "app", "Java_sample_Native_00024Inner_add", "Java_sample_Native_00024Inner_add", "function"),
                    "native/add.cpp");
            cpp.add(symbol("c-ping", "cpp", "source", "app", "Java_sample_Native_1utilKt_ping__I", "Java_sample_Native_1utilKt_ping__I", "function"),
                    "native/ping.cpp");
            kotlin.add(symbol("digit", "kotlin", "kotlin", "app", "digit", "sample/digit", "function", {"external"}), "src/2d.kt");
            cpp.add(symbol("c-digit", "cpp", "source", "app", "Java_sample__12dKt_digit", "Java_sample__12dKt_digit", "function"), "native/digit.cpp");
            kotlin.add(symbol("twice", "kotlin", "kotlin", "app", "twice", "sample/Native.twice", "method", {"external"}), "src/Native.kt");
            for (const auto* overload : {"Java_sample_Native_twice__I", "Java_sample_Native_twice__J"})
                cpp.add(symbol(overload, "cpp", "source", "app", overload, overload, "function"), "native/twice.cpp");
            cpp.add(symbol("c-gone", "cpp", "source", "app", "Java_sample_Native_gone", "Java_sample_Native_gone", "function"), "native/gone.h", false);
            const semantic::recipe recipe{{"app", "jni", "jni", "evaluate", semantic::unit_kind::group, "imports"}, "options"};
            const auto bound = jni::resolve(recipe, {}, {&kotlin.shard, &cpp.shard});
            expect((bound.relations.size() == 5_u) >> fatal);
            const auto& member = binding(bound, "add");
            expect(member.library_selection.strategy.name() == "source");
            expect(member.library_selection.candidates.empty() && !member.library_selection.selected);
            expect(member.entry_point == std::optional<std::string>{"Java_sample_Native_00024Inner_add"});
            expect((member.parameter_mapping.size() == 2_u) >> fatal) << "a source declaration has no descriptor to map";
            expect(member.parameter_mapping[1].from.kind.name() == "receiver");
            expect(member.parameter_mapping[1].jni->descriptor == "Lsample/Native$Inner;");
            const auto& facade = binding(bound, "ping");
            expect(facade.parameter_mapping[1].from.kind.name() == "class") << "a top-level function is static on its file facade";
            expect(facade.entry_point == std::optional<std::string>{"Java_sample_Native_1utilKt_ping__I"}) << "the long name binds when no short name exists";
            expect(binding(bound, "digit").entry_point == std::optional<std::string>{"Java_sample__12dKt_digit"}) << "a digit-first file stem is prefixed";
            for (const auto& row : bound.relations)
                expect(row.resolution.name() == (row.from == "twice" ? "candidate" : "resolved"));
            expect((bound.diagnostics.size() == 1_u) >> fatal) << "an accessor or a declared but undefined native function is skipped without a warning";
            expect(bound.diagnostics.front().code == "entry_ambiguous") << "competing overload definitions remain candidates";
        };
        "F# DllImport declarations bind to C++ definitions without artifacts"_test = [] {
            source_unit fsharp{"app", "fsharp"}, cpp{"app", "clice"};
            auto imported = symbol("add", "fsharp", "fsharp", "app", "add", "Native.add", "function", {"pinvoke"});
            imported.extensions = {{"clr.pinvoke.library", json::value{std::string{"native"}}},
                                   {"clr.pinvoke.entry_point", json::value{std::string{"add_numbers"}}},
                                   {"clr.pinvoke.convention", json::value{std::string{"cdecl"}}}};
            fsharp.add(imported, "src/Native.fs");
            auto missing = imported;
            missing.id = missing.name = "missing";
            missing.extensions["clr.pinvoke.entry_point"] = json::value{std::string{"absent"}};
            fsharp.add(missing, "src/Native.fs");
            cpp.add(symbol("c-add", "cpp", "source", "app", "add_numbers", "add_numbers", "function"), "native/add.cpp");
            cpp.add(symbol("c-scoped", "cpp", "source", "app", "add_numbers", "detail::add_numbers", "function"), "native/detail.cpp");
            const semantic::recipe recipe{{"app", "pinvoke", "pinvoke", "evaluate", semantic::unit_kind::group, "imports"}, "options"};
            const auto bound = pinvoke::resolve(recipe, {}, {&fsharp.shard, &cpp.shard});
            expect((bound.relations.size() == 1_u) >> fatal);
            expect(bound.relations.front().to == "c-add") << "a namespaced function is not an unqualified entry point";
            expect(bound.relations.front().resolution.name() == "resolved");
            const auto& detail = binding(bound, "add");
            expect(detail.library_selection.strategy.name() == "source" && detail.library_selection.name == "native");
            expect(detail.convention.name() == "cdecl");
            expect(detail.entry_point == std::optional<std::string>{"add_numbers"});
            expect(bound.diagnostics.empty()) << "an entry point without a source definition may come from a prebuilt library";
        };
    };
} // namespace
int main() {
    boost::ut::suite<"bindings">{cases};
    return boost::ut::cfg<>.run() ? 1 : 0;
}
