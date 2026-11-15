import std;
import boost.ut;
import support;
import clr;
import pinvoke;
import artifact;
import fsharp;
import knowledge;
import channels;
import reflection;
import native;
import coroutine;
import language;
import fixture;
import reuse;

namespace {
    using namespace boost::ut;
    std::filesystem::path report_file, shard_file, source_file, native_file, edited_report, edited_shard, edited_source;
    auto load(const std::filesystem::path& path) -> std::string {
        std::ifstream stream{path};
        return {std::istreambuf_iterator<char>{stream}, {}};
    }
    const auto cases = [] {
        "CLR IL and portable symbols connect source declarations, calls and PInvoke imports"_test = [] {
            const auto report = rfl::json::read<clr::object>(load(report_file)).value();
            auto source = rfl::json::read<semantic::shard>(load(shard_file)).value();
            source.unit.id = semantic::unit_identifier(source.unit.recipe.unit);
            auto own = [&](auto& rows) {
                for (auto& row : rows)
                    row.ownership.owner_unit = source.unit.id;
            };
            own(source.symbols);
            own(source.occurrences);
            own(source.references);
            own(source.scopes);
            source.unit.channel_definitions = language::server<"fsharp">::channels();
            source.unit.direct_inputs = {{semantic::input_kind::source, "Library.fs", "source-one"}};
            std::map<std::string, semantic::source_anchor> anchors;
            for (const auto& occurrence : source.occurrences)
                if (occurrence.location.source) {
                    const auto& range = *occurrence.location.source;
                    anchors.try_emplace(occurrence.symbol, semantic::source_anchor{range.path, "anchor:" + std::to_string(range.begin)});
                }
            test::scratch scratch{"clr-records"};
            semantic::store store{scratch.dir};
            semantic::view view{.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "dotnet"};
            const auto source_view = store.publish_records(view, {source}, anchors);
            const auto selected = store.load(source_view);
            const auto bodies = channels::definition_bodies({selected->pieces.front()->materialized().get()}, {std::string{"function"}, std::string{"method"}});
            expect(!bodies.empty() >> fatal);
            semantic::recipe recipe{{"app", "clr", "clr-artifact", "evaluate", semantic::unit_kind::artifact, "Library.dll"}, "options"};
            auto shard = clr::records(report, recipe, "Library", {{semantic::input_kind::artifact, "Library.dll", "assembly-object"}});
            shard.unit.channel_definitions = language::server<"clr-artifact">::channels();
            auto member_id = [&](const std::string& documentation) {
                const auto found = std::ranges::find_if(shard.symbols, [&](const auto& symbol) {
                    const auto value = symbol.extensions.find("clr.documentation_id");
                    return value != symbol.extensions.end() && rfl::from_generic<std::string>(value->second).value() == documentation;
                });
                expect((found != shard.symbols.end()) >> fatal) << documentation;
                return found->id;
            };
            const auto mutable_property = member_id("P:Sample.DerivedValue.Mutable");
            const auto getter = member_id("M:Sample.DerivedValue.get_Mutable");
            const auto setter = member_id("M:Sample.DerivedValue.set_Mutable(System.Int32)");
            const auto changed = member_id("E:Sample.DerivedValue.Changed");
            const auto added = member_id("M:Sample.DerivedValue.add_Changed(System.EventHandler)");
            const auto removed = member_id("M:Sample.DerivedValue.remove_Changed(System.EventHandler)");
            const auto& property = *std::ranges::find(shard.symbols, mutable_property, &semantic::records::symbol::id);
            const auto& event = *std::ranges::find(shard.symbols, changed, &semantic::records::symbol::id);
            expect(property.kind == "property" && property.type == "System.Int32" && property.visibility == "public");
            expect(event.kind == "event" && event.type.has_value());
            for (const auto& [accessor, owner] :
                 std::vector<std::pair<std::string, std::string>>{{getter, mutable_property}, {setter, mutable_property}, {added, changed}, {removed, changed}})
                expect(std::ranges::find(shard.symbols, accessor, &semantic::records::symbol::id)->owner == owner);
            const auto indexer = member_id("P:Sample.DerivedValue.Item(System.Int32)");
            const auto& indexed = *std::ranges::find(shard.symbols, indexer, &semantic::records::symbol::id);
            const auto& index_signature = *std::ranges::find(shard.types, *indexed.type, &semantic::records::type::id);
            expect(index_signature.kind == "function" && index_signature.parameter_groups.size() == 1_u &&
                   index_signature.parameter_groups.front().parameters.size() == 1_u);
            const auto derived = member_id("T:Sample.DerivedValue");
            const auto base = member_id("T:Sample.BaseValue");
            const auto contract = member_id("T:Sample.IValue");
            auto relation = [&](const std::string& kind, const std::string& from, const std::string& to) {
                return std::ranges::any_of(shard.relations, [&](const auto& edge) {
                    return edge.kind.str() == kind && edge.from == from && edge.to == to && edge.resolution == "resolved";
                });
            };
            expect(relation("inherits", derived, base) && relation("implements", derived, contract));
            expect(relation("overrides", member_id("M:Sample.DerivedValue.Transform(System.Int32)"), member_id("M:Sample.BaseValue.Transform(System.Int32)")));
            expect(relation("implements", member_id("M:Sample.DerivedValue.Sample#IValue#Transform(System.Int32)"),
                            member_id("M:Sample.IValue.Transform(System.Int32)")));
            expect(relation("overrides", member_id("M:Sample.GenericDerived.Echo(System.Int32)"), member_id("M:Sample.GenericBase`1.Echo(`0)")));

            const auto native = std::ranges::find(shard.symbols, "Native", &semantic::records::symbol::name);
            expect((native != shard.symbols.end()) >> fatal);
            expect(std::ranges::contains(native->modifiers, "pinvoke"));
            expect(rfl::json::write(native->extensions.at("clr.pinvoke.entry_point")) == "\"native_twice\"");
            expect(rfl::json::write(native->extensions.at("clr.pinvoke.convention")) == "\"cdecl\"");
            expect(std::ranges::any_of(shard.relations, [&](const auto& edge) { return edge.kind == "calls" && edge.to == native->id; }));
            std::vector<const semantic::shard*> source_parts;
            for (const auto& part : selected->pieces)
                source_parts.push_back(part->materialized().get());
            clr::associate(shard, report, source_parts, {{"Library.fs", load(source_file)}});
            expect(std::ranges::any_of(shard.relations, [](const auto& edge) { return edge.kind == "emits"; }));
            auto emitted = [&](const std::string& target, const std::string& source_key, const std::string& evidence) {
                return std::ranges::any_of(shard.relations, [&](const auto& edge) {
                    if (edge.kind != "emits" || edge.to != target || !edge.detail)
                        return false;
                    const bool matches = edge.detail->variant().visit([&](const auto& detail) -> bool {
                        if constexpr (std::same_as<std::decay_t<decltype(detail)>, semantic::records::emission_detail>)
                            return detail.evidence.str() == evidence;
                        else
                            return false;
                    });
                    if (!matches)
                        return false;
                    for (const auto* part : source_parts)
                        for (const auto& symbol : part->symbols) {
                            if (symbol.id != edge.from || !symbol.native_key)
                                continue;
                            const auto* native = std::get_if<semantic::native_identity>(&*symbol.native_key);
                            if (native && native->native_key == source_key)
                                return true;
                        }
                    return false;
                });
            };
            expect(emitted(derived, "T:Sample.DerivedValue", "metadata_signature"));
            expect(emitted(mutable_property, "P:Sample.DerivedValue.Mutable", "metadata_signature"));
            expect(emitted(setter, "P:Sample.DerivedValue.Mutable(System.Int32)", "metadata_signature"));
            expect(emitted(changed, "E:Sample.DerivedValue.Changed", "metadata_signature"));
            expect(emitted(added, "E:Sample.DerivedValue.Changed", "metadata_signature"));
            const auto point = member_id("M:Sample.Shape.NewPoint(System.Int32,System.Int32)");
            expect(emitted(point, "T:Sample.Shape.Point", "compilation_mapping"));
            const semantic::records::symbol* source_field = nullptr;
            for (const auto* part : source_parts)
                for (const auto& symbol : part->symbols) {
                    if (symbol.kind != "field")
                        continue;
                    const auto owner = symbol.extensions.find("fsharp.declaring_entity");
                    const auto variant = symbol.extensions.find("fsharp.variant");
                    const auto index = symbol.extensions.find("fsharp.sequence");
                    if (owner != symbol.extensions.end() && variant != symbol.extensions.end() && index != symbol.extensions.end() &&
                        rfl::from_generic<std::string>(owner->second).value() == "T:Sample.Shape" && rfl::from_generic<int>(variant->second).value() == 0 &&
                        rfl::from_generic<int>(index->second).value() == 0)
                        source_field = &symbol;
                }
            expect((source_field != nullptr) >> fatal);
            const auto field_target = member_id("P:Sample.Shape.Point.x");
            expect(std::ranges::any_of(shard.relations, [&](const auto& edge) {
                return edge.kind == "emits" && edge.from == source_field->id && edge.to == field_target && edge.detail &&
                       rfl::get<semantic::records::emission_detail>(edge.detail->variant()).evidence == "compilation_mapping";
            }));

            const auto combined = store.publish_records(store.describe(source_view), {shard}, {});
            const auto result =
                store.load(combined)->query({.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"Native", "Library"}}});
            expect((result.items.size() == 1_u) >> fatal);
            const auto references = store.load(combined)->query(
                {.action = std::string{"references"}, .selector = semantic::selection{.entity = semantic::entity{result.items.front().symbol_id}}});
            expect(!references.references.empty());
            const auto library = native_artifact::read(load(native_file));
            expect(std::ranges::any_of(library.symbols, [](const auto& symbol) { return symbol.name == "native_twice" && symbol.exported; }));
            auto exported = native_artifact::records(library, "app", "options", "native", "native", "libnative.so", {});
            exported.unit.channel_definitions = language::server<"native-artifact">::channels();
            auto with_native = store.publish_records(store.describe(combined), {exported}, {});
            const auto ready = store.load(with_native);
            std::vector<const semantic::shard*> inputs;
            for (const auto& input : ready->pieces)
                inputs.push_back(input->materialized().get());
            semantic::recipe binding_recipe{{"app", "bindings", "pinvoke", "evaluate", semantic::unit_kind::group, "imports"}, "options"};
            auto bound = pinvoke::resolve(binding_recipe, {{{"native", "native"}}}, inputs);
            expect((bound.relations.size() == 2_u) >> fatal);
            expect(bound.relations.front().resolution == "resolved");
            expect(bound.relations.front().from_context != bound.relations.front().to_context);
            const auto detail = rfl::json::write(bound.relations.front().detail);
            expect(detail.contains("identity") && detail.contains("return_value"));
            const auto marshaled = std::ranges::find_if(bound.relations, [](const auto& relation) {
                return relation.detail && rfl::get<semantic::records::binding_detail>(relation.detail->variant()).entry_point == "native_sum";
            });
            expect((marshaled != bound.relations.end()) >> fatal);
            const auto& marshal_detail = rfl::get<semantic::records::binding_detail>(marshaled->detail->variant());
            expect((marshal_detail.marshalling.has_value() && marshal_detail.parameter_mapping.size() == 5_u) >> fatal);
            expect(marshal_detail.marshalling->charset == "unicode" && marshal_detail.marshalling->exact_spelling &&
                   marshal_detail.marshalling->set_last_error);
            const auto& parameters = marshal_detail.parameter_mapping;
            expect((parameters[0].marshalling.has_value() && parameters[1].marshalling.has_value() && parameters[3].marshalling.has_value()) >> fatal);
            expect((parameters[0].marshalling->native.has_value() && parameters[1].marshalling->native.has_value() &&
                    parameters[1].marshalling->native->size_parameter.has_value()) >>
                   fatal);
            expect(parameters[0].conversion == "string" && parameters[0].marshalling->native->name == "LPUTF8Str");
            expect(parameters[1].conversion == "array" && parameters[1].marshalling->direction == "in_out" &&
                   *parameters[1].marshalling->native->size_parameter == 2_u);
            expect((parameters[3].marshalling->layout.has_value()) >> fatal);
            expect(parameters[3].marshalling->by_reference && parameters[3].marshalling->layout->kind == "sequential" &&
                   parameters[3].marshalling->layout->packing == 1_u);
            expect(parameters[3].marshalling->layout->fields.size() == 1_u && parameters[3].marshalling->layout->fields.front().type == "System.Int32");
            expect(bound.diagnostics.empty());
            const auto bindings = store.publish_records(store.describe(with_native), {bound}, {});
            expect(
                store.load(bindings)
                    ->query({.action = std::string{"relations"}, .selector = semantic::selection{.entity = semantic::entity{result.items.front().symbol_id}}})
                    .relations.size() >= 1_u);
            const auto linked = store.load(bindings);
            const auto entry =
                linked->query({.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"native_twice", "native"}}});
            expect((entry.items.size() == 1_u) >> fatal);
            semantic::request impact{.action = std::string{"impact"},
                                     .selector = semantic::selection{.entity = semantic::entity{entry.items.front().symbol_id, entry.items.front().context}},
                                     .changes = {"body"},
                                     .depth = 4};
            const auto affected = linked->query(impact);
            auto source_run = std::ranges::find_if(affected.items, [](const auto& item) { return item.domain == "fsharp" && item.name == "run"; });
            expect((source_run != affected.items.end()) >> fatal);
            expect(std::ranges::any_of(affected.impact, [&](const auto& path) {
                return path.category == "behavior" && path.target.symbol_id == source_run->symbol_id && path.steps.size() == 3;
            }));
            impact.limit = 1;
            std::size_t count = 0;
            do {
                const auto page = linked->query(impact);
                count += page.impact.size();
                impact.cursor = page.cursor;
            } while (impact.cursor);
            expect(count == affected.impact.size());
            impact.budget.nodes = 1;
            const auto limited = linked->query(impact);
            expect(limited.status == "partial" && limited.cursor.has_value());
            auto unmapped = pinvoke::resolve(binding_recipe, {}, inputs);
            expect(unmapped.relations.empty() && unmapped.diagnostics.size() == 2_u);
            expect(unmapped.diagnostics.front().code == "library_unmapped");
            native::context runtime;
            auto evaluate = [&]() -> coroutine::task<> {
                language::stage stage{.server = language::with<"pinvoke">{.options = {{{"native", "native"}}}},
                                      .build_configuration = "native",
                                      .inputs = {{"managed", {language::reference{"clr", "facts"}}}, {"native", {language::reference{"native", "facts"}}}}};
                auto plan = language::prepare(stage, {{"native", fixture::cmake("build", "cc")}});
                auto command = plan.server;
                expect(command.argv.empty());
                auto adapter = language::make_adapter(runtime.get_scheduler(), {.cwd = scratch.dir},
                                                      {.slot = "slot", .project = "app", .provider = "pinvoke", .options = "options", .stage = "bindings"}, {},
                                                      {.execution = "runtime"});
                auto opened = co_await adapter->open(command.initialization);
                language::installation installation{.snapshot = "snapshot"};
                for (const auto& input : ready->pieces)
                    if (input->unit.recipe.unit.provider == "clr-artifact" || input->unit.recipe.unit.provider == "native-artifact")
                        installation.source_facts[input->unit.recipe.unit.provider == "clr-artifact" ? "managed" : "native"].push_back(
                            std::make_shared<semantic::shard_object>(input->materialized(), nullptr, std::filesystem::path{}));
                auto other_recipe = recipe;
                other_recipe.unit.name = "Other.dll";
                auto other_module = clr::records(report, other_recipe, "Other", {{semantic::input_kind::artifact, "Other.dll", "other-object"}});
                other_module.unit.channel_definitions = language::server<"clr-artifact">::channels();
                const auto multiple = store.load(store.publish_records(ready->description, {other_module}, {}));
                for (const auto& input : multiple->pieces)
                    if (std::ranges::contains(input->unit.modules, "Other"))
                        installation.source_facts["managed"].push_back(
                            std::make_shared<semantic::shard_object>(input->materialized(), nullptr, std::filesystem::path{}));
                auto updated = co_await adapter->update(opened.epoch, std::move(installation));
                const auto units = co_await adapter->units(updated.epoch, {});
                expect((units.size() == 4_u) >> fatal);
                const auto selected_unit = std::ranges::find_if(units, [](const auto& unit) {
                    const auto group = rfl::json::read<pinvoke::group>(unit.name).value();
                    return group.module == "Library" && !group.native.empty();
                });
                expect((selected_unit != units.end()) >> fatal);
                auto result = co_await adapter->evaluate({.execution = {"request", "computation", "bindings"},
                                                          .slot_id = "slot",
                                                          .unit = *selected_unit,
                                                          .epoch = updated.epoch,
                                                          .inputs = updated.inputs});
                expect((result.shards.size() == 1_u) >> fatal);
                expect(result.shards.front().relations.size() == 2_u);
                const auto& contribution = result.shards.front().unit;
                semantic::candidate candidate{.recipe = contribution.recipe, .units = {contribution}};
                semantic::source_vector sources;
                semantic::input_vector runtime_inputs{{"app", updated.inputs}};
                reuse::channel_values individual;
                reuse::environment environment{sources, runtime_inputs, individual, "app"};
                std::vector<const semantic::unit*> managed, libraries;
                for (const auto& input : multiple->pieces) {
                    if (input->unit.recipe.unit.provider == "clr-artifact")
                        managed.push_back(&input->unit);
                    if (input->unit.recipe.unit.provider == "native-artifact")
                        libraries.push_back(&input->unit);
                }
                auto refresh = [&] {
                    for (const auto& read : contribution.observed_dependencies)
                        if (read.kind == semantic::dependency_kind::record_set) {
                            const auto selection = read.records.value_or(semantic::record_selection{});
                            environment.collections[{read.key, *read.channel, selection}] =
                                channels::collection_value(read.key == "managed" ? managed : libraries, *read.channel, selection);
                        }
                };
                refresh();
                expect(reuse::accept(candidate, updated, {}, environment));
                auto another = *libraries.front();
                another.id = "another-library";
                another.modules = {"unrelated-library"};
                for (auto& channel : another.output_channels)
                    channel.value = "unrelated-content";
                libraries.push_back(&another);
                refresh();
                expect(reuse::accept(candidate, updated, {}, environment)) << "an unrelated library does not invalidate this binding group";
                another.modules = libraries.front()->modules;
                refresh();
                expect(!reuse::accept(candidate, updated, {}, environment)) << "a new artifact in the selected module invalidates export lookup";
                libraries.pop_back();
                refresh();
                expect(reuse::accept(candidate, updated, {}, environment));
                libraries.clear();
                refresh();
                expect(!reuse::accept(candidate, updated, {}, environment)) << "withdrawn library bindings cannot be reused";
                language::installation missing{.snapshot = "missing-library"};
                for (const auto& input : ready->pieces)
                    if (input->unit.recipe.unit.provider == "clr-artifact")
                        missing.source_facts["managed"].push_back(
                            std::make_shared<semantic::shard_object>(input->materialized(), nullptr, std::filesystem::path{}));
                const auto absent = co_await adapter->update(updated.epoch, std::move(missing));
                const auto absent_groups = co_await adapter->units(absent.epoch, {});
                const auto missing_group =
                    std::ranges::find_if(absent_groups, [](const auto& unit) { return rfl::json::read<pinvoke::group>(unit.name)->module == "Library"; });
                expect((missing_group != absent_groups.end()) >> fatal);
                auto unresolved = co_await adapter->evaluate({.execution = {"request", "computation", "bindings"},
                                                              .slot_id = "slot",
                                                              .unit = *missing_group,
                                                              .epoch = absent.epoch,
                                                              .inputs = absent.inputs});
                expect(unresolved.shards.front().relations.empty() && unresolved.shards.front().diagnostics.size() == 2_u);
                const auto& absent_unit = unresolved.shards.front().unit;
                semantic::candidate negative{.recipe = absent_unit.recipe, .units = {absent_unit}};
                runtime_inputs["app"] = absent.inputs;
                refresh();
                expect(reuse::accept(negative, absent, {}, environment)) << "an empty selected library collection is reusable";
                const auto present_library =
                    std::ranges::find_if(ready->pieces, [](const auto& part) { return part->unit.recipe.unit.provider == "native-artifact"; });
                expect((present_library != ready->pieces.end()) >> fatal);
                libraries.push_back(&(*present_library)->unit);
                refresh();
                expect(!reuse::accept(negative, absent, {}, environment)) << "a previously absent mapped export invalidates its binding group";
                co_await adapter->close();
            };
            cores::run(runtime, evaluate());

            auto revised_source = rfl::json::read<semantic::shard>(load(edited_shard)).value();
            revised_source.unit.id = semantic::unit_identifier(revised_source.unit.recipe.unit);
            auto revised_ownership = [&](auto& rows) {
                for (auto& row : rows)
                    row.ownership.owner_unit = revised_source.unit.id;
            };
            revised_ownership(revised_source.symbols);
            revised_ownership(revised_source.occurrences);
            revised_ownership(revised_source.references);
            revised_ownership(revised_source.scopes);
            revised_source.unit.channel_definitions = language::server<"fsharp">::channels();
            revised_source.unit.direct_inputs = {{semantic::input_kind::source, "Library.fs", "source-two"}};
            auto revised_view = linked->description;
            revised_view.snapshots = {{"app", "main", "v2", "edited-source"}};
            const auto source_updated = store.load(store.publish_records(revised_view, {revised_source}, anchors));
            std::vector<const semantic::shard*> revised_parts;
            for (const auto& part : source_updated->pieces)
                if (part->unit.recipe.unit.provider == "fsharp")
                    revised_parts.push_back(part->materialized().get());
            const auto revised_object = rfl::json::read<clr::object>(load(edited_report)).value();
            auto revised_clr = clr::records(revised_object, recipe, "Library", {{semantic::input_kind::artifact, "Library.dll", "assembly-two"}});
            revised_clr.unit.channel_definitions = language::server<"clr-artifact">::channels();
            clr::associate(revised_clr, revised_object, revised_parts, {{"Library.fs", load(edited_source)}});
            const auto artifact_updated = store.load(store.publish_records(source_updated->description, {revised_clr}, {}));
            std::vector<const semantic::shard*> revised_inputs;
            for (const auto& part : artifact_updated->pieces)
                revised_inputs.push_back(part->materialized().get());
            auto revised_binding = pinvoke::resolve(binding_recipe, {{{"native", "native"}}}, revised_inputs);
            expect(revised_binding.relations.size() == 2_u && revised_binding.diagnostics.empty());
            const auto revised = store.load(store.publish_records(artifact_updated->description, {revised_binding}, {}));
            impact.limit = 100;
            impact.budget = {};
            impact.cursor.reset();
            const auto revised_affected = revised->query(impact);
            expect(std::ranges::any_of(revised_affected.impact, [&](const auto& path) {
                return path.category == "behavior" && path.target.symbol_id == source_run->symbol_id && path.steps.size() == 3;
            })) << "the edited and rebuilt project retains the native-to-F# path";
            const auto compared = semantic::compare_views(
                *linked, *revised,
                {.action = std::string{"impact"}, .selector = semantic::selection{.entity = semantic::entity{source_run->symbol_id, source_run->context}}});
            expect(compared.comparison.has_value() >> fatal);
            expect(std::ranges::any_of(compared.comparison->changes, [&](const auto& change) {
                return change.subject.symbol_id == source_run->symbol_id && std::ranges::contains(change.kinds, std::string{"inputs"});
            })) << "a real helper source edit remains attributable in the two-view comparison";
            expect(std::ranges::any_of(compared.comparison->reanalysis, [&](const auto& change) { return change.id == revised_source.unit.id; }));
        };
    };
} // namespace
int main(int argc, char** argv) {
    if (argc != 8)
        return 2;
    report_file = argv[1];
    shard_file = argv[2];
    source_file = argv[3];
    native_file = argv[4];
    edited_report = argv[5];
    edited_shard = argv[6];
    edited_source = argv[7];
    boost::ut::suite<"clr_artifacts">{cases};
    return boost::ut::cfg<>.run() ? 1 : 0;
}
