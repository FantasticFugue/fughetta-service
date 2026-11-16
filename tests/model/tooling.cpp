import std;
import support;
import boost.ut;
import language;
import reflection;
import fixture;
import semantic;
import knowledge;
import reuse;
import channels;
import kotlin;
import enumeration;

namespace {
    using namespace boost::ut;
    auto configured() -> builds::configuration {
        auto c = fixture::cmake("build/debug", "cc");
        auto& cm = fixture::options<"cmake">(c.builders.at("main"));
        cm.export_compile_commands = true;
        cm.toolchain_file = ::test::absolute("/opt/vcpkg/scripts/buildsystems/vcpkg.cmake");
        cm.inputs = {{"packages", "directory"}};
        cm.variables = {{"VCPKG_INSTALLED_DIR", "${input:packages}"}, {"VCPKG_MANIFEST_INSTALL", "OFF"}};
        c.builders["deps"] =
            builds::with<"vcpkg">{.options = {.root = ::test::absolute("/opt/vcpkg"), .install_dir = "build/installed", .triplet = "x64-linux"}};
        c.tasks["dependencies"] = {.builder = "deps", .operation = "install"};
        c.tasks.at("configure").inputs = {{"packages", {"dependencies", "installed"}}};
        c.outputs = {{"database", {"configure", "compilation_database"}}};
        return c;
    }
    auto published_identity(semantic::store& store, const semantic::native_identity& identity) -> std::pair<semantic::symbol_id, semantic::view_id> {
        semantic::shard records;
        records.unit = semantic::make_unit(
            {{identity.project, "identity", identity.provider, "evaluate", semantic::unit_kind::group, identity.native_key}, "test"}, "cpp");
        records.identities = {{identity, "local", {}}};
        records.symbols = {{.id = "local", .ownership = {records.unit.id, records.unit.context}, .name = "declaration"}};
        const auto view = store.publish_records({.snapshots = {{identity.project, "main", "v1", "source"}}}, {std::move(records)}, {});
        return {store.load(view)->pieces.front()->symbols().front().id, view};
    }
    static constexpr auto cases = [] {
        "semantic views replace unit contributions and retain shared shards across restart"_test = [] {
            ::test::scratch scratch{"semantic-views"};
            semantic::native_identity declaration{"app", "main", "clice", "source", "c:@F@read#"};
            std::string symbol, first, second, shared;
            std::map<std::string, std::string> sources{{"main.cpp", "object-main"}, {"caller.cpp", "object-caller"}, {"shared.hpp", "object-header"}};
            semantic::request references{.action = std::string{"references"}, .include = {"type", "declarations"}, .limit = 1};
            auto unit = [](std::string id, std::string symbol, std::string name) {
                semantic::shard out;
                out.unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, id + ".cpp"}, "cpp"}, "cpp");
                out.unit.direct_inputs = {{semantic::input_kind::file, id + ".cpp", "object-" + id}};
                out.unit.observed_dependencies = {{.kind = semantic::dependency_kind::source, .project = "app", .key = "shared.hpp", .value = "object-header"},
                                                  {.kind = semantic::dependency_kind::negative_lookup, .project = "app", .key = "generated"}};
                out.symbols.push_back(
                    {.id = symbol, .ownership = {.owner_unit = out.unit.id, .context = "cpp"}, .name = name, .qualified_name = "api::" + name});
                out.occurrences.push_back({.id = "occ-" + symbol,
                                           .ownership = {.owner_unit = out.unit.id, .context = "cpp"},
                                           .symbol = symbol,
                                           .location = {.source = semantic::place{.path = id + ".cpp", .begin = 0, .end = 4}},
                                           .roles = {std::string{"definition"}}});
                return out;
            };
            {
                semantic::store store{scratch.dir};
                const auto seeded = published_identity(store, declaration);
                symbol = seeded.first;
                auto main = unit("main", symbol, "read");
                main.identities.push_back({declaration, symbol, {}});
                auto caller = unit("caller", "caller-symbol", "use");
                caller.symbols.front().language = "cpp";
                caller.symbols.front().module = "consumer";
                caller.references.push_back({.id = "call-read",
                                             .ownership = {.owner_unit = caller.unit.id, .context = "cpp"},
                                             .from = "caller-symbol",
                                             .location = {.source = semantic::place{.path = "caller.cpp", .begin = 0, .end = 4}},
                                             .target = symbol,
                                             .roles = {std::string{"call"}},
                                             .resolution = std::string{"resolved"}});
                auto another = caller.references.front();
                another.id = "second-call";
                another.location.source = semantic::place{.path = "caller.cpp", .begin = 10, .end = 14};
                // Candidate and resolved indexes may both point to this use.
                another.candidates = {symbol};
                caller.references.push_back(another);
                caller.types = {{.id = "integer", .domain = "cpp", .kind = std::string{"primitive"}, .name = "int"},
                                {.id = "parameter", .domain = "cpp", .kind = std::string{"parameter"}, .name = "T"}};
                caller.typed_ranges = {{.ownership = {.owner_unit = caller.unit.id, .context = "cpp"},
                                        .range = {.path = "caller.cpp", .begin = 0, .end = 6},
                                        .type = "integer",
                                        .overload = symbol,
                                        .substitutions = {{"parameter", "integer"}}},
                                       {.ownership = {.owner_unit = caller.unit.id, .context = "cpp"},
                                        .range = {.path = "caller.cpp", .begin = 10, .end = 16},
                                        .type = "integer",
                                        .overload = symbol}};
                first = store.publish({.snapshots = {{"app", "main", "v1", "source-1"}}, .configuration = "cpp"}, {main, caller});
                store.retire(seeded.second);
                store.remember(first, {"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"});
                auto original = store.load(first);
                expect(original->names.count("api::read") == 1u);
                expect(original->positions.count("main.cpp") == 1u);
                expect(original->references.count(symbol) == 3u);
                semantic::request impact{.action = std::string{"impact"},
                                         .selector = semantic::selection{.entity = semantic::entity{symbol, "cpp"}},
                                         .changes = {"declaration"},
                                         .depth = 3};
                const auto affected = original->query(impact);
                expect((affected.impact.size() == 1_u) >> fatal);
                expect(affected.impact.front().category == "semantic" && affected.impact.front().target.symbol_id == "caller-symbol");
                expect(affected.impact.front().steps.front().evidence == "call-read");
                impact.budget.nodes = 1;
                std::vector<semantic::impact_path> paths;
                std::size_t steps = 0;
                do {
                    auto part = original->query(impact);
                    expect(part.query_nodes <= 1_u);
                    std::ranges::move(part.impact, std::back_inserter(paths));
                    impact.cursor = part.cursor;
                } while (impact.cursor && ++steps < 100);
                expect(!impact.cursor && paths.size() == affected.impact.size());
                expect(paths.front().steps.front().evidence == affected.impact.front().steps.front().evidence);
                impact.budget = {};

                impact.changes = {"body"};
                expect(original->query(impact).impact.empty()) << "a body change does not imply declaration reanalysis";
                references.view = first;
                references.selector = semantic::selection{.entity = semantic::entity{.symbol_id = symbol, .context = "cpp"}};
                references.scope.files = {"caller.cpp"};
                auto page = original->query(references);
                expect((page.references.size() == 1u && page.cursor.has_value()) >> fatal);
                expect(page.references.front().id == "call-read");
                expect(page.items.size() == 1_u && page.items.front().symbol_id == symbol);
                const auto owner = std::ranges::find(page.symbols, "caller-symbol", &semantic::records::symbol::id);
                expect((owner != page.symbols.end()) >> fatal);
                expect(owner->language == "cpp" && owner->module == "consumer")
                    << "the reference page includes real caller declaration metadata without changing its target items";
                expect((page.typed_ranges.size() == 1u) >> fatal);
                expect(page.typed_ranges.front().range.begin == 0u && page.types.size() == 2u);
                expect(page.typed_ranges.front().substitutions.at("parameter") == "integer");
                references.cursor = page.cursor;
                shared = original->description.units.at(semantic::unit_identifier({"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "caller.cpp"}))
                             .front();
                auto changed = original->description;
                changed.snapshots = {{"app", "feature", "v2", "source-2"}};
                // An empty replacement retracts every old record from that unit.
                main.symbols.clear();
                main.occurrences.clear();
                main.unit.direct_inputs.front().object = "changed-main";
                second = store.publish(changed, {main});
                const auto compared = semantic::compare_views(*original, *store.load(second), {.action = std::string{"impact"}, .depth = 3});
                expect(compared.comparison.has_value() >> fatal);
                expect((compared.comparison->changes.size() == 1_u) >> fatal);
                expect(compared.comparison->changes.front().kinds.front() == "removed");
                expect(compared.comparison->changes.front().subject.symbol_id == symbol);
                expect(std::ranges::any_of(compared.impact, [&](const auto& path) { return path.view == first && path.target.symbol_id == "caller-symbol"; }));
                expect(semantic::compare_views(*original, *original, {.action = std::string{"impact"}}).comparison->changes.empty());
                store.remember(second, {"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"});
                expect(store.load(second)->names.count("read") == 0u);
                expect(store.load(first)->names.count("read") == 1u);
                expect(store.describe(second)
                           .units.at(semantic::unit_identifier({"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "caller.cpp"}))
                           .front() == shared);
                auto reused = store.load(second);
                expect(original == store.load(first) &&
                       std::ranges::any_of(original->pieces,
                                           [&](const auto& shard) { return shard->id == shared && std::ranges::contains(reused->pieces, shard); }))
                    << "retained views share immutable shard objects and reuse their query indexes";
                auto invalid = unit("bad", "bad-symbol", "bad");
                invalid.symbols.front().ownership.context = "wrong";
                expect(throws<std::invalid_argument>([&] { store.publish(changed, {main, invalid}); }));
                expect(store.load(second)->names.count("use") == 1u);
            }
            {
                semantic::store restored{scratch.dir, {.bytes = 0}};
                expect(restored.load(first)->symbols.contains(symbol));
                auto original = restored.candidates("app", "cpp", {{"app", sources}});
                sources["main.cpp"] = "changed-main";
                auto candidates = restored.candidates("app", "cpp", {{"app", sources}});
                expect(original.size() == 1u &&
                       original.front().shards == restored.describe(first).units.at(semantic::unit_identifier(original.front().recipe.unit)) &&
                       candidates.size() == 1u &&
                       candidates.front().shards == restored.describe(second).units.at(semantic::unit_identifier(candidates.front().recipe.unit)));
                // Worldline alternation resolves each candidate against its own recorded
                // input state. Results resolved for one source state must not leak into
                // another, and returning to a resolved state keeps every earlier result.
                for (auto round = 0; round < 3; ++round) {
                    sources["main.cpp"] = "object-main";
                    const auto older = restored.candidates("app", "cpp", {{"app", sources}});
                    expect(older.size() == 1u &&
                           older.front().shards == restored.describe(first).units.at(semantic::unit_identifier(older.front().recipe.unit)))
                        << "returning to an earlier worldline source state reuses its candidate";
                    sources["main.cpp"] = "changed-main";
                    const auto newer = restored.candidates("app", "cpp", {{"app", sources}});
                    expect(newer.size() == 1u &&
                           newer.front().shards == restored.describe(second).units.at(semantic::unit_identifier(newer.front().recipe.unit)))
                        << "alternating source states keep each state's candidate";
                }
                expect(restored.candidates("another", "cpp", {{"app", sources}}).empty());
                sources["notes.txt"] = "unrelated";
                expect(restored.candidates("app", "cpp", {{"app", sources}}).size() == 1u);
                sources["shared.hpp"] = "changed-header";
                expect(restored.candidates("app", "cpp", {{"app", sources}}).empty()) << "shared input changes withdraw every reader";
                sources.erase("shared.hpp");
                expect(restored.candidates("app", "cpp", {{"app", sources}}).empty()) << "removed source inputs withdraw their readers";
                sources["shared.hpp"] = "object-header";
                sources["generated/optional.hpp"] = "new-header";
                expect(restored.candidates("app", "cpp", {{"app", sources}}).empty()) << "creating a descendant invalidates a missing directory lookup";
                sources.erase("generated/optional.hpp");
                // Replacing the same remembered state must preserve its reverse reads.
                restored.remember(second, {"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"});
                expect(restored.candidates("app", "cpp", {{"app", sources}}).size() == 1u);
                expect(restored.load(first)->names.count("read") == 1u);
                expect(restored.load(second)->names.count("read") == 0u);
                auto page = restored.load(first)->query(references);
                expect((page.references.size() == 1u && !page.cursor.has_value()) >> fatal);
                expect(page.references.front().id == "second-call");
                expect((page.typed_ranges.size() == 1u) >> fatal);
                expect(page.typed_ranges.front().range.begin == 10u && page.types.size() == 1u);
                expect(page.items.size() == 1u && page.items.front().symbol_id == symbol);
                auto changed_query = references;
                changed_query.view = second;
                expect(throws<std::invalid_argument>([&] { restored.load(second)->query(changed_query); }));
                auto retained = restored.load(first);
                restored.retire(first);
                expect(restored.candidates("app", "cpp", {{"app", sources}}).size() == 1u);
                expect(throws<semantic::expired_view>([&] { restored.load(first); }));
                expect(retained->names.count("read") == 1u);
                expect(restored.load(second)->references.count(symbol) == 3u);
                expect(std::filesystem::exists(scratch.dir / "shards" / shared));
                semantic::view checkpoint{.snapshots = restored.describe(second).snapshots, .checkpoint = restored.describe(second).units};
                auto continuation = restored.publish(std::move(checkpoint), {});
                restored.retire(second);
                expect(restored.candidates("app", "cpp", {{"app", sources}}).size() == 1u);
                expect(std::filesystem::exists(scratch.dir / "shards" / shared)) << "a continuation retains unexposed stage contributions";
                restored.retire(continuation);
                restored.sweep();
                expect(std::filesystem::is_empty(scratch.dir / "shards"));
            }
        };
        "view comparison pages declaration and input edits with view-specific evidence"_test = [] {
            ::test::scratch scratch{"view-comparison"};
            semantic::store store{scratch.dir};
            auto make = [](std::string name) {
                semantic::shard out;
                out.unit = semantic::make_unit({{"app", "source", "clice", "evaluate", semantic::unit_kind::file, name + ".cpp"}, "options"}, "cpp");
                out.unit.direct_inputs = {{semantic::input_kind::file, name + ".cpp", "old"}};
                out.symbols.push_back({.id = name,
                                       .ownership = {out.unit.id, "cpp"},
                                       .name = name,
                                       .qualified_name = name,
                                       .kind = std::string{"function"},
                                       .visibility = "public"});
                return out;
            };
            auto declaration = make("a"), caller = make("b");
            declaration.symbols.front().type = "signature";
            auto type_part = make("types");
            type_part.symbols.clear();
            type_part.types = {{.id = "signature", .kind = std::string{"function"}, .type_parameters = {"T"}, .return_type = "result"},
                               {.id = "result", .kind = std::string{"alias"}, .arguments = {"integer"}},
                               {.id = "integer", .kind = std::string{"primitive"}, .name = "int"},
                               {.id = "T", .kind = std::string{"parameter"}, .name = "T", .constraints = {"constraint"}},
                               {.id = "constraint", .kind = std::string{"named"}, .name = "Number"}};
            semantic::shard body_part;
            body_part.unit = semantic::make_unit({{"app", "source", "clice", "records", semantic::unit_kind::file, "b.cpp"}, "options"}, "cpp");
            body_part.unit.direct_inputs = {{semantic::input_kind::file, "b.cpp", "body-one"}};
            body_part.occurrences = {{.id = "b-body",
                                      .ownership = {body_part.unit.id, "cpp"},
                                      .symbol = "b",
                                      .location = {.source = semantic::place{"b.cpp", 0, 30}},
                                      .roles = {std::string{"definition"}},
                                      .body = semantic::place{"b.cpp", 5, 30}}};
            caller.references.push_back({.id = "use", .ownership = {caller.unit.id, "cpp"}, .from = "b", .target = "a", .resolution = std::string{"resolved"}});
            auto library = make("library");
            library.symbols.clear();
            for (unsigned at = 0; at < 1024; ++at)
                library.symbols.push_back({.id = "library-" + std::to_string(at),
                                           .ownership = {library.unit.id, "cpp"},
                                           .name = "unchanged-" + std::to_string(at),
                                           .kind = std::string{"function"}});
            auto alternative = caller.symbols.front();
            alternative.ownership.owner_unit = library.unit.id;
            alternative.name = "alternative-b";
            library.symbols.push_back(alternative);
            library.relations.push_back({.id = "shared-call",
                                         .kind = std::string{"calls"},
                                         .ownership = {library.unit.id, "cpp"},
                                         .from = "library-0",
                                         .to = "b",
                                         .resolution = std::string{"resolved"},
                                         .from_context = "cpp",
                                         .to_context = "cpp"});
            auto edges = make("edges");
            edges.symbols.clear();
            edges.references.push_back({.id = "edge-use",
                                        .ownership = {edges.unit.id, "cpp"},
                                        .from = "b",
                                        .target = "a",
                                        .roles = {std::string{"reference"}},
                                        .resolution = std::string{"resolved"}});
            auto semantic_consumer = make("semantic-consumer");
            semantic_consumer.unit.observed_dependencies = {{.kind = semantic::dependency_kind::lookup,
                                                             .project = "app",
                                                             .key = "compiler-channel-key",
                                                             .value = "semantic-one",
                                                             .channel = "declarations",
                                                             .scope = "opaque-helper-scope",
                                                             .producers = {declaration.unit.id}}};
            declaration.unit.channel_definitions = language::server<"kotlin">::channels();
            semantic::view initial{.snapshots = {{"app", "main", "v1", "s1"}}};
            initial.computation = language::computation{.builds = {{"native", fixture::cmake("build/debug", "cc")}}, .project = "app"};
            initial.computation->profile.stages["source"].build_configuration = "native";
            const auto producer = store.load(store.publish(initial, {declaration}));
            caller.unit.observed_dependencies = {
                channels::consume("app", "source", *producer->pieces.front()->materialized(), "declarations", semantic::dependency_kind::declaration)};
            const auto first = store.publish(initial, {declaration, caller, library, edges, type_part, body_part, semantic_consumer});
            auto input_only = declaration;
            input_only.unit.direct_inputs.front().object = "body-edit";
            const auto cutoff = store.load(store.publish(store.describe(first), {input_only}));
            const auto early = semantic::compare_views(*store.load(first), *cutoff, {.action = std::string{"impact"}});
            expect(early.comparison->reanalysis.size() == 1_u);
            expect(early.comparison->reanalysis.front().id == declaration.unit.id) << "unchanged declaration channels stop propagation to consumers";
            declaration.symbols.front().visibility = "private";
            declaration.symbols.front().extensions["annotation"] = rfl::to_generic(std::string{"changed"});
            caller.unit.direct_inputs.front().object = "new";
            caller.references.front().roles.push_back(std::string{"type"});
            auto view = store.describe(first);
            view.snapshots = {{"app", "main", "v2", "s2"}};
            const auto second = store.publish(view, {declaration, caller});
            semantic::store reopened{scratch.dir};
            const auto before = reopened.load(first), after = reopened.load(second);
            const auto library_shard = before->description.units.at(library.unit.id).front();
            const auto shared_before = std::ranges::find_if(before->pieces, [&](const auto& part) { return part->id == library_shard; });
            const auto shared_after = std::ranges::find_if(after->pieces, [&](const auto& part) { return part->id == library_shard; });
            expect((shared_before != before->pieces.end() && shared_after != after->pieces.end()) >> fatal);
            expect((*shared_before)->index == (*shared_after)->index && (*shared_before)->index->comparisons.size() == 1025_u)
                << "view comparisons and pages reuse the persisted projections of a shared library";
            semantic::request query{.action = std::string{"impact"}, .depth = 3, .limit = 1};
            const auto page = semantic::compare_views(*before, *after, query);
            expect(page.cursor.has_value() >> fatal);
            expect(page.comparison->reanalysis.size() == 1_u);
            const auto& unit = page.comparison->reanalysis.front();
            expect(unit.id == caller.unit.id && unit.inputs.size() == 1_u && unit.dependencies.size() == 1_u);
            expect(unit.dependencies.front().before != unit.dependencies.front().after);
            expect(std::ranges::any_of(unit.tasks, [](const auto& task) { return task.operation == "build"; }));
            expect(std::ranges::any_of(unit.tasks, [](const auto& task) { return task.operation == "test"; }));
            const auto& a = page.comparison->changes.front();
            expect(a.subject.symbol_id == "a" && std::ranges::contains(a.kinds, std::string{"declaration"}) &&
                   std::ranges::contains(a.kinds, std::string{"metadata"}));
            expect(std::ranges::any_of(page.impact, [&](const auto& path) { return path.view == first; }));
            expect(std::ranges::any_of(page.impact, [&](const auto& path) { return path.view == second; }));
            query.cursor = page.cursor;
            const auto next = semantic::compare_views(*before, *after, query);
            expect(!next.cursor && next.comparison->changes.size() == 1u);
            const auto& b = next.comparison->changes.front();
            expect(b.subject.symbol_id == "b" && std::ranges::contains(b.kinds, std::string{"inputs"}) &&
                   std::ranges::contains(b.kinds, std::string{"relations"}));
            body_part.unit.direct_inputs.front().object = "body-two";
            const auto body_changed = store.publish(store.describe(second), {body_part});
            semantic::request selected_body{.action = std::string{"impact"}, .selector = semantic::selection{.entity = semantic::entity{"b", "cpp"}}};
            const auto body_impact = semantic::compare_views(*after, *store.load(body_changed), selected_body);
            expect(body_impact.comparison->changes.size() == 1_u && body_impact.comparison->changes.front().kinds.front() == "inputs");
            expect(std::ranges::any_of(body_impact.impact, [](const auto& path) {
                return path.category == "behavior" && path.target.symbol_id == "library-0";
            })) << "symbol-free definition parts preserve source body impact through the shared declaration";
            expect(std::ranges::any_of(body_impact.comparison->reanalysis, [&](const auto& unit) { return unit.id == body_part.unit.id; }));
            std::ranges::find(type_part.types, "integer", &semantic::records::type::id)->name = "long";
            std::ranges::find(type_part.types, "constraint", &semantic::records::type::id)->name = "Decimal";
            type_part.unit.direct_inputs.front().object = "types-two";
            const auto type_changed = store.publish(store.describe(body_changed), {type_part});
            semantic::request selected_type{.action = std::string{"impact"}, .selector = semantic::selection{.entity = semantic::entity{"a", "cpp"}}};
            const auto type_impact = semantic::compare_views(*store.load(body_changed), *store.load(type_changed), selected_type);
            expect(type_impact.comparison->changes.size() == 1_u);
            expect(type_impact.comparison->changes.front().kinds == decltype(semantic::view_change::kinds){std::string{"declaration"}})
                << "a stable signature id still observes cross-part return types and generic constraints";
            expect(std::ranges::any_of(type_impact.impact, [](const auto& path) { return path.category == "semantic" && path.target.symbol_id == "b"; }));
            expect(std::ranges::any_of(type_impact.comparison->reanalysis, [&](const auto& unit) { return unit.id == type_part.unit.id; }));
            declaration.symbols.front().visibility = "protected";
            declaration.unit.direct_inputs.front().object = "semantic-api-two";
            semantic_consumer.unit.observed_dependencies.front().value = "semantic-two";
            const auto observed = store.publish(store.describe(type_changed), {declaration, semantic_consumer});
            const auto semantic_impact = semantic::compare_views(*store.load(type_changed), *store.load(observed), selected_type);
            const auto propagated = std::ranges::find(semantic_impact.comparison->reanalysis, semantic_consumer.unit.id, &semantic::reanalysis_change::id);
            expect((propagated != semantic_impact.comparison->reanalysis.end()) >> fatal);
            expect(std::ranges::any_of(propagated->dependencies, [&](const auto& read) {
                return !read.read.producer && read.read.scope == "opaque-helper-scope" && std::ranges::contains(read.read.producers, declaration.unit.id) &&
                       std::ranges::contains(read.producers, declaration.unit.id);
            })) << "typed producer provenance connects helper observations to a producer-filtered comparison";
            edges.references.front().roles.push_back(std::string{"call"});
            edges.unit.direct_inputs.front().object = "edited-edges";
            const auto third = store.publish(store.describe(observed), {edges});
            semantic::request selected{.action = std::string{"impact"}, .selector = semantic::selection{.entity = semantic::entity{"b", "cpp"}}};
            const auto relation_only = semantic::compare_views(*store.load(observed), *store.load(third), selected);
            expect(relation_only.comparison->changes.size() == 1_u);
            expect(relation_only.comparison->changes.front().kinds == decltype(semantic::view_change::kinds){std::string{"relations"}})
                << "a changed relation-only part finds its declaration in a shared part";
            auto withdrawn = store.describe(third);
            withdrawn.units.erase(caller.unit.id);
            const auto fourth = store.publish(withdrawn, {});
            const auto retained = semantic::compare_views(*store.load(third), *store.load(fourth), selected);
            expect(retained.comparison->changes.size() == 1_u);
            expect(!std::ranges::contains(retained.comparison->changes.front().kinds, std::string{"removed"}))
                << "a shared alternative declaration survives withdrawing another part";
            withdrawn = store.describe(fourth);
            withdrawn.units.erase(library.unit.id);
            const auto fifth = store.publish(withdrawn, {});
            const auto removed = semantic::compare_views(*store.load(fourth), *store.load(fifth), selected);
            expect(removed.comparison->changes.size() == 1_u && removed.comparison->changes.front().kinds.front() == "removed");
        };
        "snapshot vectors retain module units and qualify cross-project dependencies"_test = [] {
            ::test::scratch scratch{"snapshot-vectors"};
            auto module = [](std::string project) {
                semantic::shard shard;
                semantic::recipe recipe{{project, "source", "clice", "evaluate", semantic::unit_kind::module, "main"}, "options"};
                shard.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
                shard.unit.direct_inputs = {{semantic::input_kind::file, "same.cpp", project + "-content"}};
                shard.unit.coverage.available = {{.facts = {std::string{"symbols"}, std::string{"occurrences"}}}};
                shard.symbols.push_back(
                    {.id = project, .ownership = {.owner_unit = shard.unit.id, .context = shard.unit.context}, .name = "value", .qualified_name = "value"});
                shard.occurrences.push_back({.id = project + "-definition",
                                             .ownership = {.owner_unit = shard.unit.id, .context = shard.unit.context},
                                             .symbol = project,
                                             .location = {.source = semantic::place{"same.cpp", 0, 5}},
                                             .roles = {std::string{"definition"}}});
                return shard;
            };
            auto app = module("app"), library = module("library");
            app.unit.observed_dependencies = {{.kind = semantic::dependency_kind::source, .project = "library", .key = "same.cpp", .value = "library-content"}};
            semantic::source_vector inputs{{"app", {{"same.cpp", "app-content"}}}, {"library", {{"same.cpp", "library-content"}}}};
            std::string id;
            {
                semantic::store store{scratch.dir};
                id = store.publish({.snapshots = {{"library", "release", "l1", "library-tree"}, {"app", "main", "a1", "app-tree"}}, .configuration = "options"},
                                   {app, library}, true);
                store.remember(id, app.unit.recipe.unit);
                store.remember(id, library.unit.recipe.unit);
                expect(store.candidates("app", "options", inputs).size() == 1u);
            }
            semantic::store reopened{scratch.dir};
            const auto view = reopened.load(id);
            const auto result = view->query({.action = std::string{"symbols"}, .scope = {.files = {"same.cpp"}}});
            expect(result.snapshots.size() == 2u && result.items.size() == 2u && result.occurrences.size() == 2u);
            std::set<std::string> projects;
            for (const auto& occurrence : result.occurrences)
                projects.insert(occurrence.location.project.value());
            expect(projects == std::set<std::string>{"app", "library"});
            const auto selected = view->query({.action = std::string{"symbols"}, .scope = {.files = {"same.cpp"}, .projects = {"library"}}});
            expect(selected.items.size() == 1u && selected.items.front().symbol_id == "library");
            auto offered = reopened.candidates("app", "options", inputs);
            expect(offered.size() == 1u && offered.front().snapshots.size() == 2u && offered.front().recipe.unit.kind == semantic::unit_kind::module);
            expect(reopened.candidates("library", "options", {{"library", inputs.at("library")}}).size() == 1u &&
                   reopened.candidates("app", "options", inputs).front().id == offered.front().id)
                << "alternating projects preserves each project's source-vector validation state";
            inputs["library"]["same.cpp"] = "changed-library";
            expect(reopened.candidates("app", "options", inputs).empty()) << "a foreign source change invalidates the consuming module";
            inputs.erase("library");
            expect(reopened.candidates("app", "options", inputs).empty());
            inputs["library"]["same.cpp"] = "library-content";
            inputs["app"]["other.cpp"] = "unrelated";
            expect(reopened.candidates("app", "options", inputs).size() == 1u);
            semantic::input_vector installed{
                {"app", {{semantic::input_kind::artifact, "library.jar", "wrong-project"}}},
                {"library", {{semantic::input_kind::artifact, "library.jar", "jar-object"}, {semantic::input_kind::project_model, "gradle", "model-object"}}}};
            reuse::channel_values channels;
            reuse::environment environment{inputs, installed, channels, "app"};
            semantic::dependency artifact{
                .kind = semantic::dependency_kind::artifact, .project = "library", .key = "member.class", .value = "jar-object", .scope = "library.jar"};
            semantic::dependency model{.kind = semantic::dependency_kind::project_model, .project = "library", .key = "gradle", .value = "model-object"};
            expect(reuse::dependency(artifact, environment) && reuse::dependency(model, environment));
            artifact.kind = semantic::dependency_kind::negative_lookup;
            expect(reuse::dependency(artifact, environment));
            installed["library"].clear();
            expect(!reuse::dependency(artifact, environment) && !reuse::dependency(model, environment));
            semantic::input_object command{semantic::input_kind::provider, "main.cpp", "command", "clice/configuration"};
            installed["app"].push_back(command);
            expect(!reuse::input(command, "app", environment)) << "provider inputs require a registered validator";
            const auto& validators = language::providers::validators();
            environment.validators = validators;
            expect(reuse::input(command, "app", environment));
            installed["library"].push_back(command);
            expect(reuse::input(command, "library", environment)) << "a consumer validates inputs from another provider and project";
            command.type = "another/configuration";
            expect(!reuse::input(command, "app", environment));
        };
        "file collection readers follow membership across candidate replacement and restart"_test = [] {
            ::test::scratch scratch{"semantic-collections"};
            std::map<std::string, std::string> sources{{"src/a.cpp", "a"}, {"src/nested/b.cpp", "b"}, {"src2/c.cpp", "c"}};
            semantic::shard consumer;
            auto reads = [&] {
                language::cpp::publication exported{.column_encoding = "utf8", .directories = {(scratch.dir / "src/.").string()}};
                consumer = language::cpp::translate(exported, scratch.dir, {}, "app", "cpp", "cpp", "src/a.cpp", {}, sources);
                consumer.unit.observed_dependencies.push_back(enumeration::read("app", "documents", sources, {.files = {"optional.hpp"}}));
            };
            std::string first, second;
            {
                semantic::store store{scratch.dir};
                auto publish = [&] {
                    reads();
                    auto id = store.publish({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"}, {consumer});
                    store.remember(id, {"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "src/a.cpp"});
                    return id;
                };
                auto candidates = [&] { return store.candidates("app", "cpp", {{"app", sources}}); };
                first = publish();
                sources["src2/new.cpp"] = "new";
                sources["src/nested/b.cpp"] = "changed";
                expect(candidates().size() == 1u) << "enumeration reads names; content reads are recorded separately";
                sources["src/a.cpp"] = "edited";
                expect(candidates().empty()) << "the consumer also read this file's contents";
                sources["src/a.cpp"] = "a";
                sources["optional.hpp"] = "present";
                expect(candidates().empty()) << "a selected file appeared outside the selected directory";
                sources.erase("optional.hpp");
                sources["src/new.cpp"] = "new";
                expect(candidates().empty()) << "the directory gained a member";
                second = publish();
                const auto current = candidates();
                expect(current.size() == 1u &&
                       current.front().shards == store.describe(second).units.at(semantic::unit_identifier(current.front().recipe.unit)));
            }
            {
                semantic::store restored{scratch.dir, {.bytes = 0}};
                auto candidates = [&] { return restored.candidates("app", "cpp", {{"app", sources}}); };
                auto current = candidates();
                expect(current.size() == 1u &&
                       current.front().shards == restored.describe(second).units.at(semantic::unit_identifier(current.front().recipe.unit)));
                sources.erase("src/new.cpp");
                current = candidates();
                expect(current.size() == 1u &&
                       current.front().shards == restored.describe(first).units.at(semantic::unit_identifier(current.front().recipe.unit)))
                    << "removing the added member recovers the prior input state";
                auto renamed = sources.extract("src/nested/b.cpp");
                renamed.key() = "src/nested/renamed.cpp";
                sources.insert(std::move(renamed));
                expect(candidates().empty()) << "a rename changes membership even when the member count stays constant";
                renamed = sources.extract("src/nested/renamed.cpp");
                renamed.key() = "src/nested/b.cpp";
                sources.insert(std::move(renamed));
                restored.retire(first);
                expect(candidates().size() == 1u) << "candidates retain shards independently of query views";
                sources["src/new.cpp"] = "new";
                current = candidates();
                expect(current.size() == 1u &&
                       current.front().shards == restored.describe(second).units.at(semantic::unit_identifier(current.front().recipe.unit)));
                restored.retire(second);
                expect(candidates().size() == 1u);
            }
        };
        "output channels survive republication and separate declarations from definition bodies"_test = [] {
            ::test::scratch scratch{"semantic-channels"};
            semantic::store store{scratch.dir, {.bytes = 0}};
            semantic::native_identity identity{"app", "main", "clice", "source", "c:@F@read#"};
            const auto [symbol, symbol_seed] = published_identity(store, identity);
            semantic::shard source;
            source.unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"}, "cpp"}, "cpp");
            source.unit.direct_inputs = {{semantic::input_kind::file, "main.cpp", "source-one"}};
            source.unit.channel_definitions = language::server<"kotlin">::channels();
            source.identities = {{identity, symbol, {}}};
            source.symbols = {{.id = symbol,
                               .ownership = {.owner_unit = source.unit.id, .context = "cpp"},
                               .name = "read",
                               .kind = std::string{"function"},
                               .type = "result"}};
            const semantic::native_identity constant_identity{"app", "main", "kotlin", "kotlin", "limit"};
            const auto [constant, constant_seed] = published_identity(store, constant_identity);
            source.identities.push_back({constant_identity, constant, {}});
            source.symbols.push_back({.id = constant,
                                      .ownership = {.owner_unit = source.unit.id, .context = "cpp"},
                                      .name = "limit",
                                      .kind = std::string{"property"},
                                      .type = "integer",
                                      .modifiers = {"const", "val"},
                                      .extensions = {{"kotlin.constant_value", std::string{"22"}}}});
            source.types = {{.id = "result", .kind = std::string{"function"}, .return_type = "integer"},
                            {.id = "integer", .kind = std::string{"primitive"}, .name = "int"}};
            source.occurrences = {{.id = "definition",
                                   .ownership = {.owner_unit = source.unit.id, .context = "cpp"},
                                   .symbol = symbol,
                                   .location = {.source = semantic::place{"main.cpp", 0, 4}},
                                   .roles = {std::string{"definition"}},
                                   .body = semantic::place{"main.cpp", 10, 30}}};
            semantic::view view{.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"};
            auto first = store.load(store.publish(view, {source}));
            store.retire(symbol_seed);
            store.retire(constant_seed);
            const semantic::source_vector offered_sources{{"app", {{"main.cpp", "source-one"}}}};
            auto candidates = store.offer(view.snapshots, "app", "cpp", offered_sources, first->description.units, {});
            expect(candidates.size() == 1_u);
            store.remember(first->description.id, source.unit.recipe.unit);
            store.make_current(first->description.id);
            store.sweep();
            expect(store.candidates("app", "cpp", offered_sources).size() == 1_u)
                << "byte pressure preserves candidates whose shards are held by the current view";
            for (const bool recipe : {true, false}) {
                auto inconsistent = source;
                if (recipe)
                    inconsistent.unit.recipe.options = "other-options";
                else
                    inconsistent.unit.channel_definitions.clear();
                const auto separate = store.describe(store.publish(view, {inconsistent}));
                auto checkpoint = first->description.units;
                auto& shards = checkpoint.at(source.unit.id);
                shards.push_back(separate.units.at(source.unit.id).front());
                bool rejected = false;
                try {
                    store.offer(view.snapshots, "app", "cpp", offered_sources, checkpoint, {});
                } catch (const std::invalid_argument& error) {
                    rejected = std::string_view{error.what()} == "candidate shards have different recipes or channel definitions";
                }
                expect(rejected) << "checkpoint reconstruction validates candidate shard metadata";
            }
            const auto declarations =
                channels::consume("app", "source", *first->pieces.front()->materialized(), "declarations", semantic::dependency_kind::declaration);
            auto lookup = declarations;
            lookup.kind = semantic::dependency_kind::lookup;
            semantic::source_vector sources;
            semantic::input_vector inputs;
            reuse::channel_values values{{{*lookup.producer, *lookup.channel}, lookup.value}};
            reuse::environment current{sources, inputs, values, "app"};
            expect(reuse::dependency(lookup, current)) << "lookup reads reuse an unchanged producer channel";
            values.begin()->second = "changed-declarations";
            expect(!reuse::dependency(lookup, current)) << "a changed lookup channel invalidates its consumer";

            const auto bodies =
                channels::consume("app", "source", *first->pieces.front()->materialized(), "definition_bodies", semantic::dependency_kind::body);
            // Re-exported input and diagnostic changes do not alter either consumed semantic value.
            source.unit.direct_inputs.front().object = "source-two";
            source.symbols.front().ownership.origin.location = semantic::locator{.source = semantic::place{"main.cpp", 2, 6}};
            source.diagnostics = {
                {.id = "warning", .severity = "warning", .message = "unused setting", .ownership = {.owner_unit = source.unit.id, .context = "cpp"}}};
            auto body = source;
            source.occurrences.clear();
            body.symbols.clear();
            body.types = {source.types.back()};
            source.types.pop_back();
            body.diagnostics.clear();
            auto conflicting = body;
            conflicting.unit.recipe.options = "other-options";
            expect(throws<std::invalid_argument>([&] { store.publish(view, {conflicting, source}); }));
            conflicting = body;
            conflicting.unit.channel_definitions.clear();
            expect(throws<std::invalid_argument>([&] { store.publish(view, {conflicting, source}); }));
            const auto republished = store.publish(view, {body, source});
            auto split = store.load(republished);
            expect(split->pieces.size() == 2u && split->pieces.front()->id != first->pieces.front()->id);
            expect(std::ranges::all_of(split->pieces, [&](const auto& part) {
                const auto& shard = *part->materialized();
                return channels::consume("app", "source", shard, "declarations", semantic::dependency_kind::declaration) == declarations &&
                       channels::consume("app", "source", shard, "definition_bodies", semantic::dependency_kind::body) == bodies;
            })) << "a channel describes the complete unit independently of its shard layout and publication";
            source.occurrences = body.occurrences;
            source.types.push_back(body.types.front());
            source.occurrences.front().location.source->begin = 3;
            source.occurrences.front().body->begin = 13;
            auto moved = store.load(store.publish(view, {source}));
            expect(channels::consume("app", "source", *moved->pieces.front()->materialized(), "declarations", semantic::dependency_kind::declaration) ==
                       declarations &&
                   channels::consume("app", "source", *moved->pieces.front()->materialized(), "definition_bodies", semantic::dependency_kind::body) != bodies)
                << "moving a definition invalidates its mapping consumers while retaining declarations";
            source.symbols.front().declaring_scope = "moved-scope";
            source.scopes = {{.id = "moved-scope",
                              .ownership = {.owner_unit = source.unit.id, .context = "cpp"},
                              .location = {.source = semantic::place{"main.cpp", 0, 40}},
                              .declared_members = {symbol},
                              .kind = std::string{"translation_unit"}}};
            source.symbols.push_back({.id = "local",
                                      .ownership = {.owner_unit = source.unit.id, .context = "cpp"},
                                      .name = "temporary",
                                      .type = "body-only",
                                      .modifiers = {"local"}});
            source.symbols.push_back(
                {.id = "local-member", .ownership = {.owner_unit = source.unit.id, .context = "cpp"}, .name = "member", .owner = "local", .type = "body-only"});
            source.types.push_back({.id = "body-only", .kind = std::string{"primitive"}, .name = "float"});
            auto body_types = store.load(store.publish(view, {source}));
            expect(channels::consume("app", "source", *body_types->pieces.front()->materialized(), "declarations", semantic::dependency_kind::declaration) ==
                   declarations)
                << "local declarations, their members, scopes and body-only types do not invalidate declaration consumers";
            // The helper exports the evaluated value, so equivalent source
            // expressions produce the same declaration channel. A changed
            // constant must invalidate consumers even with the same signature.
            source.symbols.at(1).extensions["kotlin.constant_value"] = std::string{"23"};
            auto changed_constant = store.load(store.publish(view, {source}));
            expect(channels::consume("app", "source", *changed_constant->pieces.front()->materialized(), "declarations",
                                     semantic::dependency_kind::declaration) != declarations);
            source.symbols.at(1).extensions["kotlin.constant_value"] = std::string{"22"};
            auto restored_constant = store.load(store.publish(view, {source}));
            expect(channels::consume("app", "source", *restored_constant->pieces.front()->materialized(), "declarations",
                                     semantic::dependency_kind::declaration) == declarations);
            source.types.at(1).name = "long";
            auto signature = store.load(store.publish(view, {source}));
            expect(channels::consume("app", "source", *signature->pieces.front()->materialized(), "declarations", semantic::dependency_kind::declaration) !=
                   declarations);
            semantic::store reopened{scratch.dir};
            expect(channels::consume("app", "source", *reopened.load(republished)->pieces.front()->materialized(), "definition_bodies",
                                     semantic::dependency_kind::body) == bodies)
                << "persisted channel dependencies survive reopening the store";
        };
        "relative native identities bind after their anchored owner across helper sessions"_test = [] {
            ::test::scratch scratch{"semantic-owned-identities"};
            semantic::store store{scratch.dir};
            semantic::view view{.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "kotlin"};
            auto publish = [&](const std::string& label) {
                semantic::shard shard;
                shard.unit = semantic::make_unit({{"app", "source", "kotlin", "evaluate", semantic::unit_kind::module, "main"}, "options"}, "kotlin");
                const semantic::anchored_identity parent{"app", "main", "kotlin", {"Main.kt", label}};
                const semantic::native_identity child{"app", "main", "kotlin", "kotlin", "value_parameter:0", label};
                shard.identities = {{child, "parameter", {}}, {parent, label, {}}};
                shard.symbols = {{.id = label,
                                  .ownership = {.owner_unit = shard.unit.id, .context = "kotlin"},
                                  .name = "lambda",
                                  .kind = std::string{"function"},
                                  .native_key = parent}};
                // An implicit parameter can be referenced without a source declaration row.
                return store.load(store.publish_records(view, {shard}, {{label, {"file-id", "stable-source-anchor"}}}));
            };
            const auto first = publish("pending-one");
            const auto second = publish("pending-two");
            auto relative = [](const auto& loaded) -> const semantic::identity_binding& {
                return *std::ranges::find_if(loaded->pieces.front()->identities(), [](const auto& binding) {
                    const auto* key = std::get_if<semantic::native_identity>(&binding.declaration);
                    return key && key->owner.has_value();
                });
            };
            const auto& initial = relative(first);
            const auto& updated = relative(second);
            expect(initial.symbol == updated.symbol && initial.declaration == updated.declaration);
            expect(std::get<semantic::native_identity>(updated.declaration).owner == second->pieces.front()->symbols().front().id);
            semantic::store reopened{scratch.dir};
            expect(relative(reopened.load(second->description.id)).declaration == updated.declaration);
        };
        "view retention preserves paging, active work and worldline heads while reclaiming expired contributions"_test = [] {
            using namespace std::chrono;
            ::test::scratch scratch{"semantic-retention"};
            auto now = system_clock::time_point{seconds{100}};
            semantic::retention limits{.lifetime = seconds{10}, .bytes = 0};
            auto clock = [&] { return now; };
            semantic::shard shard{.unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"}, "cpp"}, "cpp")};
            shard.unit.direct_inputs = {{semantic::input_kind::file, "main.cpp", "source"}};
            shard.unit.cacheable = false;
            std::string old, main, feature, checkpoint, shared;
            {
                semantic::store store{scratch.dir, limits, clock};
                auto header = shard;
                header.diagnostics = {{.id = "header", .severity = "warning", .message = "header", .ownership = {shard.unit.id, shard.unit.context}}};
                old = store.publish({.snapshots = {{"app", "main", "v1", "source-1"}}, .configuration = "cpp"}, {shard, header});
                store.remember(old, {"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"});
                auto loaded = store.load(old);
                shared = loaded->pieces.front()->id;
                now += seconds{1};
                auto edited = shard;
                edited.unit.direct_inputs = {{semantic::input_kind::file, "main.cpp", "edited"}};
                main = store.publish({.snapshots = {{"app", "main", "v2", "source-2"}}, .configuration = "cpp"}, {edited});
                feature = store.publish({.snapshots = {{"app", "feature", "v1", "source-1"}}, .configuration = "cpp"}, {edited});
                store.make_current(main);
                store.make_current(feature);
                const auto dependency = store.publish({.snapshots = {{"library", "main", "v1", "library-source"}}, .configuration = "cpp"}, {});
                store.make_current(dependency);
                auto combined = store.describe(main);
                combined.snapshots.push_back({"library", "main", "v1", "library-source"});
                combined.computation = language::computation{.project = "app"};
                main = store.publish(std::move(combined), {});
                store.make_current(main);
                expect(store.current("library", "main", "cpp") == dependency) << "a dependency snapshot never replaces that project's current view";

                now += seconds{4};
                store.renew(old);
                now += seconds{7};
                store.sweep();
                expect(store.describe(old).id == old && store.describe(main).id == main && store.describe(feature).id == feature)
                    << "renewed pagination and both worldline heads survive storage pressure";
                auto reading = store.protect({old});
                now += seconds{8};
                store.sweep();
                expect(store.describe(old).id == old) << "an active reader outlives its pagination lease";
                checkpoint = store.publish({.snapshots = loaded->description.snapshots, .checkpoint = loaded->description.units}, {});
                auto task = store.protect({checkpoint});
                reading.reset();
                now += seconds{11};
                store.sweep();
                expect(throws<semantic::expired_view>([&] { store.describe(old); }) &&
                       store.candidates("app", "cpp", {{"app", {{"main.cpp", "source"}}}}).size() == 1_u)
                    << "expired views are removed while a checkpoint retains the candidate shards";
                expect(store.describe(checkpoint).id == checkpoint && std::filesystem::exists(scratch.dir / "shards" / shared) && loaded->pieces.size() == 2u)
                    << "an active continuation owns internal shards while an already loaded reader owns its records";
                auto offered = store.offer(loaded->description.snapshots, "app", "cpp", {{"app", {{"main.cpp", "source"}}}}, loaded->description.units, task);
                expect(offered.size() == 1u && offered.front().retained && !offered.front().cacheable &&
                       offered.front().shards == loaded->description.units.begin()->second)
                    << "a checkpoint offers the whole retained recipe";
                language::result result;
                result.roots.value() = task;
                auto joined = result;
                task.reset();
                result.roots.value().reset();
                store.sweep();
                auto decoded = rfl::json::read<language::result>(rfl::json::write(joined));
                expect(std::filesystem::exists(scratch.dir / "shards" / offered.front().shards.front()) && decoded && !decoded->roots.value())
                    << "shared computation results own their internal views without exporting runtime roots";
            }
            {
                semantic::store restored{scratch.dir, limits, clock};
                restored.sweep();
                expect(throws<semantic::expired_view>([&] { restored.describe(checkpoint); }) && !std::filesystem::exists(scratch.dir / "shards" / shared) &&
                       restored.describe(main).id == main && restored.describe(feature).id == feature)
                    << "restart preserves durable heads and releases expired task roots";
                restored.release_current("app", "feature", "cpp");
                restored.sweep();
                expect(throws<semantic::expired_view>([&] { restored.describe(feature); }) && restored.describe(main).id == main);
                auto replacement = restored.publish(restored.describe(main), {});
                restored.make_current(replacement);
                restored.sweep();
                expect(throws<semantic::expired_view>([&] { restored.describe(main); }) && restored.load(replacement)->pieces.size() == 1u)
                    << "replacing a head retains shared records through the new view";
                restored.release_current("app", "main", "cpp");
                now += seconds{11};
                restored.sweep();
                expect(std::filesystem::is_empty(scratch.dir / "shards")) << "unrooted contributions are reclaimed after their final lease";
            }
        };
        "accepted reuse refreshes candidate LRU across restart and collection expires only reclaimed symbols"_test = [] {
            using namespace std::chrono;
            ::test::scratch scratch{"semantic-lru"};
            auto now = system_clock::time_point{seconds{100}};
            auto clock = [&] { return now; };
            semantic::source_vector sources{{"app", {{"hot.cpp", "hot"}, {"cold.cpp", "cold"}, {"new.cpp", "new"}}}};
            std::uint64_t keep_bytes = 0;
            std::string hot_candidate, cold_symbol, hot_symbol;
            {
                semantic::store store{scratch.dir, {.lifetime = seconds{1}}, clock};
                auto publish = [&](const std::string& name) {
                    semantic::shard shard{
                        .unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, name + ".cpp"}, "cpp"}, "cpp")};
                    shard.unit.direct_inputs = {{semantic::input_kind::file, name + ".cpp", name}};
                    shard.identities = {{semantic::native_identity{"app", "main", "clice", "source", name}, name, {}}};
                    shard.symbols = {{.id = name, .ownership = {.owner_unit = shard.unit.id, .context = "cpp"}, .name = name}};
                    auto id = store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"}, {shard}, {});
                    auto view = store.load(id);
                    const auto& object = view->pieces.front();
                    if (name != "cold")
                        keep_bytes +=
                            std::filesystem::file_size(scratch.dir / "shards" / object->id) + std::filesystem::file_size(scratch.dir / "indexes" / object->id);
                    now += milliseconds{1};
                    return id;
                };
                auto hot = publish("hot");
                hot_symbol = store.load(hot)->pieces.front()->symbols().front().id;
                auto original = store.candidates("app", "cpp", sources);
                hot_candidate = original.front().id;
                auto cold = publish("cold");
                cold_symbol = store.load(cold)->pieces.front()->symbols().front().id;
                publish("new");
                now += seconds{1};
                store.publish_records(store.describe(hot), {}, {});
                auto reused = store.candidates("app", "cpp", sources);
                expect(reused.size() == 3u && reused.front().id == hot_candidate && reused.front().accessed > original.front().accessed)
                    << "accepted reuse keeps candidate identity and refreshes recency";
                store.require_symbol(cold_symbol);
            }
            now += seconds{10};
            semantic::store reopened{scratch.dir, {.lifetime = seconds{1}, .bytes = keep_bytes}, clock};
            reopened.sweep();
            const auto remaining = reopened.candidates("app", "cpp", sources);
            expect(remaining.size() == 2u && remaining.front().id == hot_candidate &&
                   std::ranges::none_of(remaining, [](const auto& value) { return value.recipe.unit.name == "cold.cpp"; }))
                << "pressure evicts the least recently reused candidate, not the oldest created candidate";
            reopened.require_symbol(hot_symbol);
            expect(throws<semantic::expired_symbol>([&] { reopened.require_symbol(cold_symbol); }));
        };
        "candidate references retain their call links and unresolved uses across reload"_test = [] {
            ::test::scratch scratch{"candidate-references"};
            language::cpp::publication publication{
                .column_encoding = "utf8",
                .symbols = {{.id = "a", .name = "first", .kind = std::string{"Function"}},
                            {.id = "b", .name = "second", .kind = std::string{"Function"}},
                            {.id = "caller", .name = "caller", .kind = std::string{"Function"}}},
                .occurrences = {{.role = "Call", .location = {"generated.hpp", 0, 1}, .owner = "caller", .candidates = {"a", "b"}, .resolution = "ambiguous"},
                                {.symbol = "a", .role = "Call", .location = {"generated.hpp", 2, 3}, .owner = "caller", .dispatch = "direct"},
                                {.role = "Call", .location = {"generated.hpp", 4, 5}, .owner = "caller", .resolution = "unresolved"}},
                .relations = {{.from = "caller",
                               .to = "a",
                               .kind = std::string{"calls"},
                               .location = language::cpp::range{"generated.hpp", 0, 1},
                               .resolution = "candidate"},
                              {.from = "caller",
                               .to = "b",
                               .kind = std::string{"calls"},
                               .location = language::cpp::range{"generated.hpp", 0, 1},
                               .resolution = "candidate"},
                              {.from = "caller",
                               .to = "a",
                               .kind = std::string{"calls"},
                               .location = language::cpp::range{"generated.hpp", 2, 3},
                               .dispatch = "direct"}}};
            std::string id;
            {
                semantic::store store{scratch.dir};
                auto shard = language::cpp::translate(publication, scratch.dir, {}, "app", "cpp", "cpp", "main.cpp", {}, {{"main.cpp", "source"}});
                id = store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"}, {shard}, {});
            }
            semantic::store restored{scratch.dir, {.bytes = 0}};
            auto view = restored.load(id);
            auto all = view->query({.action = std::string{"references"}});
            expect((all.references.size() == 3u && all.coverage.unresolved_count == 2u) >> fatal);
            const auto& candidate = all.references.front();
            expect(!candidate.target && candidate.candidates.size() == 2u && !candidate.dispatch);
            const auto& relations = view->pieces.front()->relations();
            expect(relations[0].resolution == "candidate" && relations[1].resolution == "candidate" && relations[0].reference == candidate.id &&
                   relations[1].reference == candidate.id);
            expect(std::ranges::contains(candidate.candidates, relations[0].to) && std::ranges::contains(candidate.candidates, relations[1].to));
            expect(relations[2].reference == all.references[1].id && all.references[1].dispatch == "direct");
            auto first = view->query({.action = std::string{"references"}, .selector = semantic::selection{.name = semantic::named{"first"}}});
            auto second = view->query({.action = std::string{"references"}, .selector = semantic::selection{.name = semantic::named{"second"}}});
            expect(first.references.size() == 2u && second.references.size() == 1u && second.references.front().id == candidate.id);
        };
        "lexical scopes retain distinct members and lookup targets after publication"_test = [] {
            ::test::scratch scratch{"lexical-scopes"};
            language::cpp::publication publication{
                .column_encoding = "utf8",
                .symbols = {{.id = "f", .name = "function", .kind = std::string{"Function"}},
                            {.id = "a", .name = "local", .kind = std::string{"Variable"}, .owner = "f", .declaring_scope = "first"},
                            {.id = "b", .name = "local", .kind = std::string{"Variable"}, .owner = "f", .declaring_scope = "second"},
                            {.id = "n", .name = "library", .kind = std::string{"Namespace"}}},
                .scopes = {{.id = "function", .owner = "f", .location = {"main.cpp", 0, 8}, .kind = std::string{"Function"}},
                           {.id = "first",
                            .parent = "function",
                            .owner = "f",
                            .location = {"main.cpp", 1, 3},
                            .declared_members = {"a"},
                            .lookup = {{std::string{"using_namespace"}, "n"}},
                            .kind = std::string{"block"}},
                           {.id = "second",
                            .parent = "function",
                            .owner = "f",
                            .location = {"main.cpp", 4, 6},
                            .declared_members = {"b"},
                            .kind = std::string{"block"}}}};
            auto translate = [&] {
                return language::cpp::translate(publication, scratch.dir, {{"main.cpp", "01234567"}}, "app", "cpp", "cpp", "main.cpp", {},
                                                {{"main.cpp", "source"}});
            };
            std::string id;
            semantic::view selected{.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"};
            {
                semantic::store store{scratch.dir};
                id = store.publish_records(selected, {translate()}, {});
            }
            semantic::store store{scratch.dir};
            auto view = store.load(id);
            auto result = view->query({.action = std::string{"scopes"}, .selector = semantic::selection{.name = semantic::named{"function"}}});
            expect((result.scopes.size() == 3u) >> fatal);
            const auto& first = result.scopes[1];
            const auto& second = result.scopes[2];
            expect(first.id != second.id && first.lexical_parent == result.scopes[0].id && second.lexical_parent == first.lexical_parent);
            const auto& symbols = view->pieces.front()->symbols();
            expect(first.declared_members == std::vector{symbols[1].id} && second.declared_members == std::vector{symbols[2].id});
            expect(symbols[1].declaring_scope == first.id && symbols[2].declaring_scope == second.id);
            expect(first.lookup.front().target == symbols[3].id && second.lookup.empty());
            publication.scopes[1].lookup.clear();
            selected.snapshots.front().version = "v2";
            auto changed = store.publish_records(selected, {translate()}, {});
            expect(store.load(changed)->pieces.front()->scopes()[1].lookup.empty() && !store.load(id)->pieces.front()->scopes()[1].lookup.empty());
            auto declarations = translate(), bodies = declarations;
            declarations.scopes.clear();
            bodies.symbols.clear();
            bodies.types.clear();
            auto part = [](auto& shard, const std::string& name) {
                shard.unit.recipe.unit.kind = semantic::unit_kind::group;
                shard.unit.recipe.unit.name = name;
                shard.unit.id = semantic::unit_identifier(shard.unit.recipe.unit);
                auto own = [&](auto& rows) {
                    for (auto& row : rows)
                        row.ownership.owner_unit = shard.unit.id;
                };
                own(shard.symbols);
                own(shard.scopes);
            };
            part(declarations, "declarations");
            part(bodies, "scopes");
            selected.snapshots.front().version = "v3";
            expect(throws<std::invalid_argument>([&] { store.publish_records(selected, {declarations}, {}); }))
                << "complete contributions cannot refer to scopes absent from the view";
            selected.coverage.missing = {{.scope = {.contexts = {declarations.unit.context}, .facts = {std::string{"scopes"}}},
                                          .units = {bodies.unit.id},
                                          .reason = "scope contribution deferred",
                                          .deferred = true}};
            const auto partial = store.publish_records(selected, {declarations}, {});
            expect(store.load(partial)->query({.action = std::string{"scopes"}}).status == "partial");
            auto completed = store.describe(partial);
            completed.coverage.missing.clear();
            const auto split = store.load(store.publish_records(completed, {bodies}, {}));
            const auto scopes = split->query({.action = std::string{"scopes"}, .selector = semantic::selection{.name = semantic::named{"function"}}});
            expect(scopes.status == "complete" && scopes.scopes.size() == 3u);
            expect(scopes.scopes[1].id == first.id && scopes.scopes[2].id == second.id && scopes.scopes[1].lexical_parent == first.lexical_parent)
                << "scope identities survive declaration/body splitting within the same semantic context";
        };
        "relation traversal retains intermediate declarations and sources across pages and reload"_test = [] {
            ::test::scratch scratch{"relation-traversal"};
            language::cpp::publication publication{
                .column_encoding = "utf8",
                .symbols = {{.id = "a", .name = "A", .kind = std::string{"Type"}},
                            {.id = "b", .name = "B", .kind = std::string{"Type"}},
                            {.id = "c", .name = "C", .kind = std::string{"Type"}},
                            {.id = "d", .name = "D", .kind = std::string{"Function"}}},
                .occurrences = {{.symbol = "a", .role = "Definition", .location = {"main.cpp", 0, 1}},
                                {.symbol = "b", .role = "Reference", .location = {"main.cpp", 2, 3}, .owner = "a", .roles = {std::string{"type"}}},
                                {.symbol = "b", .role = "Definition", .location = {"main.cpp", 4, 5}},
                                {.symbol = "c", .role = "Reference", .location = {"main.cpp", 6, 7}, .owner = "b", .roles = {std::string{"type"}}},
                                {.symbol = "c", .role = "Definition", .location = {"main.cpp", 8, 9}},
                                {.symbol = "a", .role = "Reference", .location = {"main.cpp", 10, 11}, .owner = "c", .roles = {std::string{"type"}}}},
                .relations = {{.from = "a", .to = "b", .kind = std::string{"aliases"}, .location = language::cpp::range{"main.cpp", 0, 3}},
                              {.from = "b", .to = "c", .kind = std::string{"aliases"}, .location = language::cpp::range{"main.cpp", 4, 7}},
                              {.from = "c", .to = "a", .kind = std::string{"aliases"}, .location = language::cpp::range{"main.cpp", 8, 11}},
                              {.from = "b", .to = "d", .kind = std::string{"calls"}}}};
            auto translate = [&] {
                return language::cpp::translate(publication, scratch.dir, {{"main.cpp", std::string(12, ' ')}}, "app", "cpp", "cpp", "main.cpp", {},
                                                {{"main.cpp", "source"}});
            };
            std::string id;
            semantic::view selected{.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"};
            {
                semantic::store store{scratch.dir};
                auto records = translate();
                for (int at = 0; at < 3; ++at) {
                    records.scopes.push_back(
                        {.id = "scope-" + std::to_string(at), .ownership = {records.unit.id, records.unit.context}, .kind = std::string{"translation_unit"}});
                    records.diagnostics.push_back({.id = "diagnostic-" + std::to_string(at),
                                                   .severity = "warning",
                                                   .message = "warning",
                                                   .ownership = {records.unit.id, records.unit.context}});
                }
                id = store.publish_records(selected, {std::move(records)}, {});
            }
            semantic::store store{scratch.dir};
            auto view = store.load(id);
            for (const auto action : {"scopes", "diagnostics"}) {
                semantic::request rows{.action = semantic::query_action{std::string{action}}, .limit = 1};
                std::set<std::string> seen;
                std::size_t pages = 0;
                do {
                    auto page = view->query(rows);
                    expect(page.scopes.size() + page.diagnostics.size() == 1_u);
                    for (const auto& row : page.scopes)
                        expect(seen.insert(row.id).second);
                    for (const auto& row : page.diagnostics)
                        expect(seen.insert(row.id).second);
                    rows.cursor = page.cursor;
                } while (rows.cursor && ++pages < 10);
                expect(!rows.cursor && seen.size() == 3_u);
            }
            semantic::request query{.action = std::string{"relations"},
                                    .selector = semantic::selection{.name = semantic::named{"A"}},
                                    .relations = {"aliases"},
                                    .depth = 3,
                                    .limit = 1};
            auto first = view->query(query);
            expect((first.relations.size() == 1u && first.cursor.has_value()) >> fatal);
            expect(first.items.size() == 2u && first.references.size() == 1u &&
                   first.references.front().roles == std::vector<semantic::role>{std::string{"type"}});
            expect(first.relations.front().reference == first.references.front().id && first.relations.front().occurrence == first.occurrences.front().id);
            query.cursor = first.cursor;
            auto second = store.load(id)->query(query);
            expect((second.relations.size() == 1u && second.cursor.has_value()) >> fatal);
            expect(second.relations.front().from == first.relations.front().to);
            query.cursor = second.cursor;
            auto third = store.load(id)->query(query);
            expect(third.relations.size() == 1u && !third.cursor && third.relations.front().to == first.relations.front().from);
            query.cursor.reset();
            query.limit = 50;
            query.depth = 2;
            query.direction = std::string{"incoming"};
            auto incoming = view->query(query);
            expect(incoming.relations.size() == 2u && incoming.items.size() == 3u);
            query.direction = std::string{"both"};
            query.depth = 1;
            expect(view->query(query).relations.size() == 2u);
            query.direction = std::string{"outgoing"};
            query.depth = 3;
            query.budget.nodes = 1;
            auto partial = view->query(query);
            expect(partial.status == "partial" && partial.cursor && !partial.coverage.missing.empty());
            std::set<std::string> resumed;
            auto page = partial;
            std::size_t pages = 0;
            do {
                expect(page.query_nodes <= 1_u);
                for (const auto& relation : page.relations)
                    expect(resumed.insert(relation.id).second);
                query.cursor = page.cursor;
                if (!query.cursor)
                    break;
                page = store.load(id)->query(query);
            } while (++pages < 100);
            expect(!query.cursor && resumed.size() == 3_u) << "budgeted pages progress through each edge exactly once";
            query.cursor.reset();
            query.budget.nodes.reset();
            query.scope.files = {"other.cpp"};
            expect(view->query(query).relations.empty());
            query.scope.files.clear();
            publication.relations.erase(publication.relations.begin() + 2);
            selected.snapshots.front().version = "v2";
            auto changed = store.publish_records(selected, {translate()}, {});
            expect(store.load(changed)->query(query).relations.size() == 2u && store.load(id)->query(query).relations.size() == 3u);
        };
        "shared header interpretations keep TU types and coverage through replacement"_test = [] {
            ::test::scratch scratch{"tu-contexts"};
            semantic::store store{scratch.dir};
            auto unit = [&](std::string path, std::string type, bool complete = true) {
                language::cpp::publication publication{
                    .column_encoding = "utf8",
                    .complete = complete,
                    .symbols = {{.id = "value", .name = "value", .qualified_name = "value", .kind = std::string{"Variable"}, .type = "type"}},
                    .types = {{.id = "type", .kind = std::string{"primitive"}, .name = type}},
                    .occurrences = {{.symbol = "value", .role = "Definition", .location = {"shared.hpp", 0, 5}}},
                    .scopes = {{.id = "scope", .owner = "value", .location = {"shared.hpp", 0, 5}}}};
                return language::cpp::translate(publication, scratch.dir, {{"shared.hpp", "value"}}, "app", "cpp", "cpp", path, {}, {{path, "input"}});
            };
            auto a = unit("a.cpp", "int"), b = unit("b.cpp", "long", false);
            semantic::view view{.snapshots = {{"app", "main", "v1", "source-1"}}, .configuration = "cpp"};
            const auto first = store.publish_records(view, {a, b}, {});
            auto original = store.load(first);
            auto symbols = original->query({.action = std::string{"symbols"}});
            expect((symbols.items.size() == 2u) >> fatal);
            expect(symbols.items[0].symbol_id == symbols.items[1].symbol_id && symbols.items[0].context != symbols.items[1].context);
            semantic::request inspect{.action = std::string{"inspect"},
                                      .selector = semantic::selection{.entity = semantic::entity{symbols.items[0].symbol_id, {}}}};
            expect(original->query(inspect).status == "ambiguous");
            inspect.selector->entity->context = a.unit.context;
            auto selected = original->query(inspect);
            expect(selected.status == "complete" && selected.coverage.missing.empty());
            expect((selected.types.size() == 1u) >> fatal);
            expect(selected.types.front().name == "int");
            expect(original->pieces[0]->scopes().front().id != original->pieces[1]->scopes().front().id);
            inspect.selector->entity->context = b.unit.context;
            auto incomplete = original->query(inspect);
            expect(incomplete.status == "partial" && incomplete.types.front().name == "long");
            expect(incomplete.types.front().id != selected.types.front().id);
            view.snapshots.front().version = "v2";
            const auto second = store.publish_records(view, {a, unit("b.cpp", "short")}, {});
            auto replaced = store.load(second)->query(inspect);
            expect(replaced.status == "complete" && replaced.items.front().symbol_id == symbols.items.front().symbol_id);
            expect(replaced.types.front().name == "short");
            expect(original->query(inspect).types.front().name == "long");
            expect(store.load(second)->query({.action = std::string{"symbols"}}).status == "complete");
        };
        "application arguments retain their graph through publication and reload"_test = [] {
            ::test::scratch scratch{"application-arguments"};
            language::cpp::publication publication{
                .column_encoding = "utf8",
                .symbols = {{.id = "value", .name = "value", .kind = std::string{"Variable"}, .type = "application"},
                            {.id = "marker", .name = "marker", .kind = std::string{"Variable"}, .type = "integer"},
                            {.id = "box", .name = "Box", .kind = std::string{"Struct"}}},
                .types = {{.id = "application",
                           .kind = std::string{"application"},
                           .declaration = "box",
                           .arguments = {"floating"},
                           .template_arguments = {{.kind = std::string{"declaration"}, .declaration = "marker"},
                                                  {.kind = std::string{"integral"}, .value = "7", .type = "integer"},
                                                  {.kind = std::string{"pack"},
                                                   .elements = {{.kind = std::string{"type"}, .type = "floating"},
                                                                {.kind = std::string{"pack"},
                                                                 .elements = {{.kind = std::string{"declaration"}, .declaration = "marker"}}}}}}},
                          {.id = "integer", .kind = std::string{"primitive"}, .name = "int"},
                          {.id = "floating", .kind = std::string{"primitive"}, .name = "double"}}};
            std::string id;
            {
                semantic::store store{scratch.dir};
                auto shard = language::cpp::translate(publication, scratch.dir, {}, "app", "cpp", "cpp", "main.cpp", {}, {{"main.cpp", "source"}});
                id = store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"}, {shard}, {});
            }
            semantic::store restored{scratch.dir, {.bytes = 0}};
            auto view = restored.load(id);
            semantic::request inspect{
                .action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"value"}}, .budget = {.nodes = 4}};
            auto result = view->query(inspect);
            expect((result.status == "complete" && result.types.size() == 3u && result.coverage.unresolved_count == 0u) >> fatal);
            const auto& arguments = result.types.front().application_arguments;
            expect((arguments.size() == 3u && arguments[2].elements.size() == 2u) >> fatal);
            expect(arguments[1].value == "7" && arguments[0].declaration != "marker");
            auto integer = std::ranges::find(result.types, *arguments[1].type, &semantic::records::type::id);
            auto floating = std::ranges::find(result.types, *arguments[2].elements[0].type, &semantic::records::type::id);
            expect(integer != result.types.end() && integer->name == "int" && floating != result.types.end() && floating->name == "double");
            expect(arguments[2].elements[1].elements[0].declaration == arguments[0].declaration);
            auto declaration =
                view->query({.action = std::string{"inspect"}, .selector = semantic::selection{.entity = semantic::entity{*arguments[0].declaration}}});
            expect(declaration.items.size() == 1u && declaration.items.front().name == "marker");
            inspect.budget.nodes = 2;
            auto limited = view->query(inspect);
            expect(limited.status == "partial" && limited.types.size() == 1u && !limited.coverage.missing.empty());
        };
        "inspect expands a callable's signature types one hop and sees through aliases"_test = [] {
            ::test::scratch scratch{"type-surface"};
            language::cpp::publication publication{
                .column_encoding = "utf8",
                .symbols = {{.id = "make", .name = "make", .kind = std::string{"Function"}, .type = "signature", .visibility = "public"},
                            {.id = "options", .name = "Options", .kind = std::string{"Struct"}, .visibility = "public"},
                            {.id = "level", .name = "level", .kind = std::string{"Field"}, .owner = "options", .type = "integer", .visibility = "public"},
                            {.id = "secret", .name = "secret", .kind = std::string{"Field"}, .owner = "options", .type = "integer", .visibility = "private"},
                            {.id = "handle", .name = "Handle", .kind = std::string{"Struct"}, .visibility = "public"},
                            {.id = "close", .name = "close", .kind = std::string{"Method"}, .owner = "handle", .type = "closing", .visibility = "public"},
                            {.id = "count", .name = "Count", .kind = std::string{"Type"}, .type = "underlying", .visibility = "public"},
                            {.id = "optional", .name = "optional", .kind = std::string{"Class"}, .visibility = "public"},
                            {.id = "task", .name = "Task", .kind = std::string{"Class"}, .visibility = "public"},
                            {.id = "run", .name = "run", .kind = std::string{"Method"}, .owner = "task", .type = "closing", .visibility = "public"}},
                .types = {
                    {.id = "signature", .kind = std::string{"function"}, .return_type = "wrapped", .parameters = {"options_ref", "counted", "job"}},
                    {.id = "job", .kind = std::string{"application"}, .name = "app::Task", .declaration = "task", .arguments = {"handle_type"}},
                    {.id = "options_ref", .kind = std::string{"reference"}, .arguments = {"options_type"}},
                    {.id = "options_type", .kind = std::string{"named"}, .name = "Options", .declaration = "options"},
                    {.id = "counted", .kind = std::string{"named"}, .name = "Count", .declaration = "count"},
                    {.id = "underlying", .kind = std::string{"primitive"}, .name = "long"},
                    {.id = "wrapped", .kind = std::string{"application"}, .name = "std::optional", .declaration = "optional", .arguments = {"handle_type"}},
                    {.id = "handle_type", .kind = std::string{"named"}, .name = "Handle", .declaration = "handle"},
                    {.id = "closing", .kind = std::string{"function"}, .return_type = "integer"},
                    {.id = "integer", .kind = std::string{"primitive"}, .name = "int"}}};
            semantic::store store{scratch.dir};
            auto shard = language::cpp::translate(publication, scratch.dir, {}, "app", "cpp", "cpp", "main.cpp", {}, {{"main.cpp", "source"}});
            auto view = store.load(store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"}, {shard}, {}));
            semantic::request inspect{.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"make"}}};
            auto result = view->query(inspect);
            expect((result.status == "complete") >> fatal);
            auto named = [&](std::string_view name) { return std::ranges::contains(result.symbols, name, &semantic::records::symbol::name); };
            expect(named("make") && named("level") && named("close") && named("run") && !named("secret") && !named("Options"));
            expect(std::ranges::contains(result.types, std::optional<std::string>{"long"}, &semantic::records::type::name));
            inspect.budget.nodes = 2;
            auto limited = view->query(inspect);
            expect(limited.status == "partial" && limited.symbols.size() == 2u);
            expect(std::ranges::contains(limited.coverage.missing, std::string{"type surface budget exhausted"}, &semantic::missing::reason));
        };
        "inspect lists a type's producers and consumers by module and visibility"_test = [] {
            ::test::scratch scratch{"type-capabilities"};
            language::cpp::publication publication{
                .column_encoding = "utf8",
                .symbols =
                    {{.id = "handle", .name = "Handle", .kind = std::string{"Struct"}, .visibility = "public"},
                     {.id = "optional", .name = "optional", .kind = std::string{"Class"}, .visibility = "public"},
                     {.id = "build", .name = "Handle", .kind = std::string{"Constructor"}, .owner = "handle", .type = "building", .visibility = "public"},
                     {.id = "open", .name = "open", .kind = std::string{"Function"}, .type = "opening", .visibility = "public"},
                     {.id = "start", .name = "start", .kind = std::string{"Function"}, .type = "starting", .visibility = "public"},
                     {.id = "make",
                      .name = "make",
                      .kind = std::string{"Method"},
                      .owner = "handle",
                      .type = "making",
                      .visibility = "public",
                      .modifiers = {"static"}},
                     {.id = "close", .name = "close", .kind = std::string{"Method"}, .owner = "handle", .type = "closing", .visibility = "public"},
                     {.id = "describe", .name = "describe", .kind = std::string{"Function"}, .type = "describing", .visibility = "public"},
                     {.id = "helper", .name = "helper", .kind = std::string{"Function"}, .type = "describing", .visibility = "private"},
                     {.id = "nested", .name = "nested", .kind = std::string{"Function"}, .type = "describing", .visibility = "local"}},
                .types =
                    {{.id = "building", .kind = std::string{"function"}, .return_type = "nothing"},
                     {.id = "opening", .kind = std::string{"function"}, .return_type = "wrapped"},
                     {.id = "making", .kind = std::string{"function"}, .return_type = "handle_type"},
                     {.id = "starting", .kind = std::string{"function"}, .return_type = "task"},
                     {.id = "task",
                      .kind = std::string{"application"},
                      .name = "[System.Runtime,8.0.0.0]System.Threading.Tasks.Task`1",
                      .arguments = {"handle_type"}},
                     {.id = "closing", .kind = std::string{"function"}, .return_type = "integer"},
                     {.id = "describing", .kind = std::string{"function"}, .return_type = "integer", .parameters = {"handle_ref", "integer"}},
                     {.id = "wrapped", .kind = std::string{"application"}, .name = "std::optional", .declaration = "optional", .arguments = {"handle_type"}},
                     {.id = "handle_ref", .kind = std::string{"reference"}, .arguments = {"handle_type"}},
                     {.id = "handle_type", .kind = std::string{"named"}, .name = "Handle", .declaration = "handle"},
                     {.id = "nothing", .kind = std::string{"primitive"}, .name = "void"},
                     {.id = "integer", .kind = std::string{"primitive"}, .name = "int"}},
                .occurrences = {{.symbol = "handle", .role = "Reference", .location = {"main.cpp", 0, 6}, .owner = "open"}}};
            semantic::store store{scratch.dir};
            auto shard = language::cpp::translate(publication, scratch.dir, {}, "app", "cpp", "cpp", "main.cpp", {}, {{"main.cpp", "source"}});
            auto view = store.load(store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"}, {shard}, {}));
            semantic::request inspect{.action = std::string{"inspect"},
                                      .selector = semantic::selection{.name = semantic::named{.text = "Handle", .kind = semantic::symbol_kind::make<"type">()}},
                                      .include = {"producers", "consumers"}};
            auto result = view->query(inspect);
            expect((result.status == "complete") >> fatal) << result.status.str();
            auto names = [](const std::vector<semantic::records::symbol>& rows) {
                return rows | std::views::transform(&semantic::records::symbol::name) | std::ranges::to<std::vector>();
            };
            const auto producers = names(result.producers);
            expect(std::ranges::is_permutation(producers, std::vector<std::string>{"Handle", "open", "start", "make"})) << rfl::json::write(producers);
            const auto consumers = names(result.consumers);
            expect(std::ranges::is_permutation(consumers, std::vector<std::string>{"close", "describe", "helper"})) << rfl::json::write(consumers);
            expect(!consumers.empty() && consumers.back() == "helper");
            inspect.budget.nodes = result.symbols.size() + result.producers.size() + 1;
            auto limited = view->query(inspect);
            expect(limited.status == "partial" && limited.producers.size() == 4u && limited.consumers.size() == 1u);
            expect(std::ranges::contains(limited.coverage.missing, std::string{"consumers budget exhausted"}, &semantic::missing::reason));
            inspect.budget.nodes = 0;
            auto empty = view->query(inspect);
            expect(empty.status == "partial" && empty.symbols.empty() && empty.producers.empty() && empty.consumers.empty());
            expect(std::ranges::contains(empty.coverage.missing, std::string{"declaration budget exhausted"}, &semantic::missing::reason));
            inspect.include = {"references", "declarations", "producers", "consumers"};
            auto referenced = view->query(inspect);
            expect(!referenced.references.empty() && referenced.symbols.empty() && referenced.producers.empty() && referenced.consumers.empty());
        };
        "inspect and scopes return the inferred types written inside a body"_test = [] {
            ::test::scratch scratch{"typed-bodies"};
            semantic::shard shard;
            shard.unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"}, "cpp"}, "cpp");
            shard.unit.direct_inputs = {{semantic::input_kind::file, "main.cpp", "source"}};
            const semantic::ownership owned{.owner_unit = shard.unit.id, .context = "cpp"};
            shard.symbols.push_back({.id = "run", .ownership = owned, .name = "run", .kind = std::string{"function"}});
            shard.occurrences.push_back({.id = "run-definition",
                                         .ownership = owned,
                                         .symbol = "run",
                                         .location = {.source = semantic::place{.path = "main.cpp", .begin = 5, .end = 8}},
                                         .roles = {std::string{"definition"}},
                                         .body = semantic::place{.path = "main.cpp", .begin = 10, .end = 40}});
            shard.scopes.push_back({.id = "later",
                                    .ownership = owned,
                                    .location = {.source = semantic::place{.path = "main.cpp", .begin = 45, .end = 60}},
                                    .kind = std::string{"block"}});
            shard.types = {{.id = "integer", .domain = "cpp", .kind = std::string{"primitive"}, .name = "int"},
                           {.id = "text", .domain = "cpp", .kind = std::string{"primitive"}, .name = "char"}};
            shard.typed_ranges = {{.ownership = owned, .range = {.path = "main.cpp", .begin = 50, .end = 55}, .type = "text"},
                                  {.ownership = owned, .range = {.path = "main.cpp", .begin = 30, .end = 34}, .type = "integer"},
                                  {.ownership = owned, .range = {.path = "main.cpp", .begin = 15, .end = 20}, .type = "integer"},
                                  {.ownership = owned, .range = {.path = "other.cpp", .begin = 15, .end = 20}, .type = "text"}};
            semantic::store store{scratch.dir};
            auto view = store.load(store.publish({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "cpp"}, {shard}));
            auto starts = [](const semantic::result& result) {
                return result.typed_ranges | std::views::transform([](const auto& row) { return row.range.begin; }) | std::ranges::to<std::vector>();
            };
            const auto inspected = view->query({.action = std::string{"inspect"}, .selector = semantic::selection{.entity = semantic::entity{"run", "cpp"}}});
            expect(std::ranges::is_permutation(starts(inspected), std::vector<std::uint64_t>{15, 30})) << rfl::json::write(starts(inspected));
            expect(inspected.types.size() == 1u && inspected.types.front().id == "integer");
            const auto scoped = view->query({.action = std::string{"scopes"}, .include = {"type"}});
            expect(starts(scoped) == std::vector<std::uint64_t>{50}) << rfl::json::write(starts(scoped));
            expect(view->query({.action = std::string{"scopes"}}).typed_ranges.empty());
        };
        "module candidates follow declared inputs across restart"_test = [] {
            ::test::scratch scratch{"semantic-module"};
            semantic::recipe recipe{{"app", "analyse", "module-provider", "evaluate", semantic::unit_kind::module, "library:main"}, "options"};
            const std::map<std::string, std::string> sources{{"src/main.kt", "source-one"}};
            std::string published;
            {
                semantic::store store{scratch.dir};
                semantic::shard shard{.unit = semantic::make_unit(recipe, "module-context")};
                shard.unit.direct_inputs = {{semantic::input_kind::file, "src/main.kt", "source-one"}};
                published = store.publish({.snapshots = {{"app", "branch", "v1", "snapshot"}}, .configuration = "profile"}, {shard});
                store.remember(published, recipe.unit);
            }
            semantic::store reopened{scratch.dir};
            auto available = reopened.candidates("app", "options", {{"app", sources}});
            expect(available.size() == 1u &&
                   available.front().shards == reopened.describe(published).units.at(semantic::unit_identifier(available.front().recipe.unit)) &&
                   available.front().recipe == recipe);
            expect(reopened.candidates("app", "options", {{"app", {{"src/main.kt", "source-two"}}}}).empty());
        };
        "identical semantic shards share one content addressed object"_test = [] {
            ::test::scratch scratch{"semantic-content-address"};
            auto make = [](const std::string& source) {
                semantic::shard shard{.unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, "main.cpp"}, "cpp"}, "cpp")};
                shard.unit.direct_inputs = {{semantic::input_kind::file, "main.cpp", source}};
                shard.identities = {{semantic::native_identity{"app", "main", "clice", "source", "main"}, "main", {}}};
                shard.symbols = {{.id = "main", .ownership = {.owner_unit = shard.unit.id, .context = "cpp"}, .name = "main"},
                                 {.id = "local", .ownership = {.owner_unit = shard.unit.id, .context = "cpp"}, .name = "local"}};
                return shard;
            };
            auto stored = [&] { return std::ranges::distance(std::filesystem::directory_iterator{scratch.dir / "shards"}); };
            auto only = [](const semantic::view& view) {
                expect((view.units.size() == 1u && view.units.begin()->second.size() == 1u) >> fatal);
                return view.units.begin()->second.front();
            };
            std::string shard;
            {
                semantic::store store{scratch.dir};
                auto publish = [&](const std::string& worldline, const std::string& source) {
                    return store.publish_records({.snapshots = {{"app", worldline, "v1", source}}, .configuration = "cpp"}, {make(source)}, {});
                };
                const auto first = publish("main", "one");
                const auto second = publish("alternative", "one");
                shard = only(store.describe(first));
                expect(only(store.describe(second)) == shard) << "another worldline with identical content reuses the shard";
                expect(shard.starts_with("shard-") && shard.size() == 70u) << shard;
                expect(stored() == 1);
                std::ifstream file{scratch.dir / "shards" / shard, std::ios::binary};
                auto document = rfl::json::read<semantic::shard>(std::string{std::istreambuf_iterator<char>{file}, {}});
                expect((document.has_value() && document->id == shard) >> fatal);
                expect(semantic::shard_key(*document) == shard) << "the stored document verifies against its ID";
                document->symbols.front().name = "forged";
                expect(semantic::shard_key(*document) != shard);
                const auto changed = publish("main", "two");
                expect(only(store.describe(changed)) != shard && stored() == 2);
                const auto repeated =
                    store.publish_records({.snapshots = {{"app", "repeat", "v1", "one"}}, .configuration = "cpp"}, {make("one"), make("one")}, {});
                expect(only(store.describe(repeated)) == shard && stored() == 2) << "a repeated shard in one batch is selected once";
                store.retire(first);
                expect(store.load(second)->pieces.front()->id == shard) << "retiring one view keeps a shard another view selects";
            }
            semantic::store reopened{scratch.dir};
            const auto again = reopened.publish_records({.snapshots = {{"app", "later", "v1", "one"}}, .configuration = "cpp"}, {make("one")}, {});
            expect(only(reopened.describe(again)) == shard && stored() == 2) << "a reopened store recognizes stored content";
            expect(reopened.load(again)->pieces.front()->symbols().front().name == "main");
            ::test::scratch elsewhere{"semantic-content-address-elsewhere"};
            semantic::store other{elsewhere.dir};
            const auto foreign = other.publish_records({.snapshots = {{"app", "main", "v1", "one"}}, .configuration = "cpp"}, {make("one")}, {});
            expect(only(other.describe(foreign)) == shard) << "another store derives the same ID for the same records";
        };
        "semantic publications serialize shared types, declaration identities and unit ownership"_test = [] {
            semantic::shard published;
            published.id = "shard-main";
            published.unit = semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::module, "main"}, "debug"}, "cpp-debug");
            semantic::native_identity key{"app", "main", "clice", "source", "c:@F@visit#"};
            published.identities.push_back({.declaration = key, .symbol = "visit"});
            published.symbols.push_back({.id = "visit",
                                         .ownership = {.owner_unit = published.unit.id, .context = "cpp-debug"},
                                         .language = "cpp",
                                         .domain = "source",
                                         .name = "visit",
                                         .kind = std::string{"function"},
                                         .type = "signature"});
            published.types.push_back(
                {.id = "signature", .domain = "cpp", .kind = std::string{"function"}, .return_type = "node", .parameter_groups = {{{{.type = "node"}}}}});
            published.types.push_back({.id = "node", .domain = "cpp", .kind = std::string{"named"}, .declaration = "node-declaration"});
            published.unit.contributions.symbols = {"visit"};
            published.unit.contributions.types = {"signature", "node"};
            auto decoded = rfl::json::read<semantic::shard>(rfl::json::write(published));
            expect(decoded.has_value() >> fatal);
            expect(decoded->symbols.front().ownership.owner_unit == decoded->unit.id);
            expect(decoded->types.front().parameter_groups.front().parameters.front().type == decoded->types.back().id);
            expect(std::get<semantic::native_identity>(decoded->identities.front().declaration) == key);
            auto other = key;
            other.module = "dependency";
            expect(other != key) << "a provider key is scoped to its module";
            semantic::view view{.id = "view-a",
                                .snapshots = {{"app", "a", "v1", "source-1"}},
                                .configuration = "cpp",
                                .units = {{published.unit.id, {published.id}}},
                                .state = std::string{"published"}};
            auto restored = rfl::json::read<semantic::view>(rfl::json::write(view));
            expect(restored.has_value() >> fatal);
            expect(restored->units.at(decoded->unit.id) == std::vector<std::string>{decoded->id});
            expect(restored->snapshots.front().version == "v1");
            // A location-only SDK declaration participates in the graph without claiming a native key.
            published.symbols.push_back({.id = "node-declaration",
                                         .ownership = {.owner_unit = published.unit.id, .context = "cpp-debug"},
                                         .module = "sdk",
                                         .name = "node",
                                         .kind = std::string{"type"}});
            published.occurrences.push_back({.id = "sdk-definition",
                                             .ownership = {.owner_unit = published.unit.id, .context = "cpp-debug"},
                                             .symbol = "node-declaration",
                                             .location = {.artifact = semantic::artifact_location{"sdk-object", "sdk", "node.hpp", 20}},
                                             .roles = {std::string{"definition"}}});
            published.symbols.push_back({.id = "local",
                                         .ownership = {.owner_unit = published.unit.id, .context = "cpp-debug"},
                                         .module = "main",
                                         .name = "local",
                                         .kind = std::string{"function"},
                                         .owner = "visit"});
            const std::map<std::string, semantic::source_anchor> anchors{{"visit", {"source-file", "visit-anchor"}},
                                                                         {"local", {"source-file", "local-anchor"}}};
            ::test::scratch scratch{"semantic-unbound"};
            semantic::store store{scratch.dir};
            view.units.clear();
            const auto first = store.publish_records(view, {published}, anchors);
            auto query = [&](const std::string& id, const std::string& name) {
                return store.load(id)->query(
                    {.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{name}}, .include = {"declarations"}});
            };
            const auto native = query(first, "visit");
            const auto sdk = query(first, "node");
            expect((native.items.size() == 1u && sdk.items.size() == 1u && sdk.symbols.size() == 1u) >> fatal);
            auto node = std::ranges::find(native.types, "node", &semantic::records::type::id);
            expect(node != native.types.end() && node->declaration == sdk.items.front().symbol_id && !sdk.symbols.front().native_key);
            const auto second = store.publish_records(view, {published}, anchors);
            expect(query(second, "visit").items.front().symbol_id == native.items.front().symbol_id &&
                   query(second, "node").items.front().symbol_id == sdk.items.front().symbol_id)
                << "identical records keep every symbol ID";
            auto edited = published;
            edited.unit.direct_inputs = {{semantic::input_kind::file, "main.cpp", "edited"}};
            const auto changed = store.publish_records(view, {edited}, anchors);
            expect(query(changed, "visit").items.front().symbol_id == native.items.front().symbol_id &&
                   query(changed, "node").items.front().symbol_id != sdk.items.front().symbol_id)
                << "an unbound declaration does not keep its ID across changed inputs";
            const auto local = query(first, "local").items.front().symbol_id;
            semantic::shard consumer;
            consumer.unit =
                semantic::make_unit({{"app", "reader", "native-artifact", "evaluate", semantic::unit_kind::artifact, "program"}, "options"}, "native");
            consumer.identities = store.load(first)->pieces.front()->identities();
            consumer.identities.push_back({semantic::native_identity{"app", "program", "native-artifact", "native", "entry"}, "entry"});
            consumer.symbols.push_back(
                {.id = "entry", .ownership = {.owner_unit = consumer.unit.id, .context = "native"}, .name = "entry", .kind = std::string{"function"}});
            consumer.relations.push_back({.id = "emits-local",
                                          .kind = std::string{"emits"},
                                          .ownership = {.owner_unit = consumer.unit.id, .context = "native"},
                                          .from = local,
                                          .to = "entry",
                                          .resolution = std::string{"resolved"},
                                          .from_context = "cpp-debug",
                                          .to_context = "native"});
            const auto combined = store.publish_records(store.describe(first), {consumer}, {});
            const auto relation = store.load(combined)->query(
                {.action = std::string{"relations"}, .selector = semantic::selection{.entity = semantic::entity{local, "cpp-debug"}}});
            expect(relation.relations.size() == 1u && relation.relations.front().from == local);
            const auto retained = store.load(combined);
            const auto imported = std::ranges::find(retained->pieces, consumer.unit.id, [](const auto& shard) { return shard->unit.id; });
            expect((imported != retained->pieces.end()) >> fatal);
            expect(std::ranges::count_if((*imported)->identities(), [](const auto& binding) { return binding.source.has_value(); }) == 2u);
        };
        "semantic selectors separate symbol search from persistent entity queries"_test = [] {
            auto selected = rfl::json::read<semantic::selection>(R"({"name":{"text":"visit"}})");
            expect(selected.has_value() >> fatal);
            expect(nothrow([&] { semantic::validate(*selected); }));
            selected->entity = semantic::entity{.symbol_id = "visit"};
            expect(throws<std::invalid_argument>([&] { semantic::validate(*selected); }));
            auto schemas = semantic::schemas().to_object().value();
            expect(schemas.get("request").has_value() && schemas.get("result").has_value() && schemas.get("view").has_value());
        };
        "provider descriptions and published coverage distinguish diagnostics from symbol facts"_test = [] {
            const auto providers = language::providers::describe();
            auto ruff = std::ranges::find(providers, "ruff", &language::description::id);
            auto clice = std::ranges::find(providers, "clice", &language::description::id);
            expect((ruff != providers.end() && clice != providers.end()) >> fatal);
            expect(ruff->facts == std::vector<semantic::fact_kind>{std::string{"diagnostics"}} && ruff->negotiated);
            expect(std::ranges::contains(clice->facts, semantic::fact_kind::make<"types">()) && clice->dependency_reads &&
                   clice->unit_kind == semantic::unit_kind::group);
            expect(clice->operations == std::vector<std::string>{"evaluate", "resolve"});
            const auto external = std::ranges::find(providers, "external", &language::description::id);
            expect((external != providers.end()) >> fatal);
            expect(external->operations == std::vector<std::string>{"evaluate"});
            language::config<"external"> registered;
            registered.registration.id = "workspace-helper";
            registered.registration.facts = {std::string{"symbols"}};
            registered.registration.ports = {{"facts", "semantic_shard", "output", true, true}};
            registered.argv = {"workspace-helper"};
            language::server<"external">::validate(registered);
            registered.registration.operations.push_back("resolve");
            expect(throws<std::invalid_argument>([&] { language::server<"external">::validate(registered); }))
                << "configured helpers advertise only the implemented evaluation protocol";
            expect(!clice->configuration_schema.to_object().value().empty());
            expect(std::ranges::any_of(clice->ports, [](const auto& port) { return port.type == "compilation_database" && port.required && port.multiple; }));
            ::test::scratch scratch{"diagnostic-coverage"};
            semantic::store store{scratch.dir};
            auto id =
                store.publish({.snapshots = {{"app", "main", "v1", "source-1"}},
                               .configuration = "python",
                               .coverage = {.requested = {.facts = {std::string{"diagnostics"}}}, .available = {{.facts = {std::string{"diagnostics"}}}}}},
                              {});
            auto view = store.load(id);
            expect(view->query({.view = id, .action = std::string{"diagnostics"}}).status == "complete");
            auto symbols = view->query({.view = id, .action = std::string{"symbols"}});
            expect((symbols.status == "partial" && symbols.items.empty() && symbols.coverage.missing.size() == 1u) >> fatal);
            expect(symbols.coverage.missing.front().scope.facts == std::vector<semantic::fact_kind>{std::string{"symbols"}});
            auto references = view->query({.view = id, .action = std::string{"references"}});
            expect(references.status == "partial" && references.references.empty());

            auto part = [](std::string project, std::string name, std::string context, semantic::fact_kind fact) {
                semantic::shard out;
                out.unit.recipe = {{project, "analysis", "configured", "index", semantic::unit_kind::module, name}, "options"};
                out.unit.id = semantic::unit_identifier(out.unit.recipe.unit);
                out.unit.context = context;
                out.unit.modules = {name};
                out.unit.coverage.available = {{.modules = {name}, .files = {name + ".cpp"}, .contexts = {context}, .facts = {fact}}};
                return out;
            };
            auto first = part("app", "one", "source-context", std::string{"symbols"});
            auto second = part("app", "two", "source-context", std::string{"symbols"});
            auto foreign = part("library", "foreign", "native-context", std::string{"symbols"});
            auto binding = part("app", "binding", "binding-context", std::string{"relations"});
            semantic::view mixed{.snapshots = {{"app", "main", "v1", "source-1"}, {"library", "release", "v2", "source-2"}}, .configuration = "mixed"};
            const auto whole = store.publish_records(mixed, {first, second, foreign, binding}, {});
            expect(store.load(whole)->query({.action = std::string{"symbols"}}).status == "complete")
                << "finite source modules and the foreign native context cover their joint symbol domain";
            expect(store.load(whole)
                       ->query({.action = std::string{"symbols"},
                                .scope = {.modules = {"one", "foreign"}, .files = {"one.cpp", "foreign.cpp"}, .projects = {"app", "library"}}})
                       .status == "complete");
            expect(store.load(whole)->query({.action = std::string{"symbols"}, .scope = {.contexts = {"binding-context"}}}).status == "partial")
                << "an explicit relations-only context still lacks symbol coverage";
            auto deferred = store.describe(whole);
            deferred.coverage.missing = {
                {.scope =
                     {.modules = {"three"}, .files = {"three.cpp"}, .contexts = {"source-context"}, .facts = {std::string{"symbols"}}, .projects = {"app"}},
                 .reason = "semantic unit budget exhausted",
                 .deferred = true}};
            const auto pending = store.publish(deferred, {});
            expect(store.load(pending)->query({.action = std::string{"symbols"}}).status == "partial");
            auto finished = store.describe(pending);
            finished.coverage.missing.clear();
            const auto complete = store.publish_records(finished, {part("app", "three", "source-context", std::string{"symbols"})}, {});
            expect(store.load(complete)->query({.action = std::string{"symbols"}}).status == "complete");
        };
        "dependency manifests contain selectors but never registry configuration or commands"_test = [] {
            auto manifest = builds::dependency_manifest(
                R"({"name":"tenant","dependencies":["fmt",{"name":"zlib","features":["core"],"default-features":false}],"features":{"tests":{"description":"tests","dependencies":["catch2"]}},"default-features":["tests"],"command":"untrusted","overrides":[{"name":"fmt","version":"10.0.0"}]})");
            const auto encoded = rfl::json::write(manifest);
            expect(encoded.contains("fmt") && encoded.contains("catch2") && !encoded.contains("untrusted") && !encoded.contains("tenant"));
            for (const auto* text : {R"({"vcpkg-configuration":{}})", R"({"dependencies":["../escape"]})",
                                     R"json({"dependencies":[{"name":"fmt","platform":"$(command)"}]})json", R"({"builtin-baseline":"not-a-hash"})"})
                expect(throws<std::invalid_argument>([&] { builds::dependency_manifest(text); }));
        };
        "dependency filling is described only for supported provider inputs"_test = [] {
            auto c = configured();
            auto& options = fixture::options<"vcpkg">(c.builders.at("deps"));
            auto plan = builds::prepare(c, {.operation = "build"});
            expect(plan.steps.front().dependencies.has_value());
            expect(plan.steps.front().dependencies->triplet == "x64-linux");
            options.overlay_ports = {"ports"};
            expect(!builds::prepare(c, {.operation = "build"}).steps.front().dependencies);
            options.overlay_ports.clear();
            options.extra = {"--dry-run"};
            expect(!builds::prepare(c, {.operation = "build"}).steps.front().dependencies);
        };
        "vcpkg installation feeds the CMake configuration on a worker"_test = [] {
            auto c = configured();
            auto& v = fixture::options<"vcpkg">(c.builders.at("deps"));
            v.features = {"tls"};
            v.default_features = false;
            v.buildtrees_dir = "build/trees";
            auto plan = builds::prepare(c, {.operation = "build"});
            expect(plan.steps.size() == 3u);
            auto install = builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {{"vcpkg", ::test::absolute("/opt/tools/vcpkg")}}, 3, {},
                                          ::test::absolute("/shared/dependencies"));
            expect(install.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/shared/dependencies/vcpkg") + ",readwrite"));
            expect(install.env.at("VCPKG_FEATURE_FLAGS") == "binarycaching");
            auto consumer =
                builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1, {}, "/private/dependencies", "/published/cache,stable");
            expect(consumer.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/published/cache`,stable/vcpkg") + ",read;files," +
                                                               ::test::absolute("/private/dependencies/vcpkg") + ",readwrite"));
            for (const auto* writable : {"/published", "/published/sub/..", "/published/vcpkg/private"})
                expect(throws<std::invalid_argument>(
                    [&] { builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1, {}, writable, "/published"); }));
            expect(throws<std::invalid_argument>(
                [&] { builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1, {}, "/published", "/published/vcpkg/nested"); }));
            auto escaped = builds::locate(plan.steps.front(), ::test::absolute("/work/other"), {}, 1, {}, ::test::absolute("/shared/cache,semi;tick`"));
            expect(escaped.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/shared/cache`,semi`;tick``/vcpkg") + ",readwrite"));
            auto literal =
                builds::locate(plan.steps.front(), ::test::absolute("/work/other"), {}, 1, {}, ::test::absolute("/shared/${tree}/${jobs}/${tool:cc}"));
            expect(literal.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/shared/${tree}/${jobs}/${tool:cc}/vcpkg") + ",readwrite"));
            auto argument = plan.steps.front();
            argument.argv.push_back("${cache:vcpkg}");
            expect(throws<std::invalid_argument>([&] { builds::locate(argument, ::test::absolute("/tree"), {}, 1, {}, ::test::absolute("/shared")); }));
            auto environment = plan.steps.front();
            environment.env["OTHER"] = "${cache:vcpkg}";
            expect(throws<std::invalid_argument>([&] { builds::locate(environment, ::test::absolute("/tree"), {}, 1, {}, ::test::absolute("/shared")); }));
            expect(throws<std::invalid_argument>([&] { builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1); }));
            expect(install.argv.front() == ::test::absolute("/opt/tools/vcpkg") && install.env.at("VCPKG_MAX_CONCURRENCY") == "3");
            expect(std::ranges::contains(install.argv, ("--vcpkg-root=" + ::test::absolute("/opt/vcpkg"))));
            expect(std::ranges::contains(install.argv, ("--x-install-root=" + ::test::absolute("/work/project/build/installed"))));
            expect(std::ranges::contains(install.argv, "--x-feature=tls") && std::ranges::contains(install.argv, "--x-no-default-features"));
            expect(std::ranges::contains(plan.steps[1].argv, "-DVCPKG_INSTALLED_DIR=${tree}/build/installed"));
            expect(plan.steps.front().outputs.size() == 1u);
            v.extra = {"--triplet=other"};
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            v.extra.clear();
            for (const auto* option : {"--binarysource=clear", "--binarysource=files,/other,readwrite", "--binarycaching", "--no-binarycaching",
                                       "--feature-flags=-binarycaching", "--editable"}) {
                v.extra = {option};
                expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            }
            v.extra.clear();
            for (const auto* argument : {"--binarysource=clear", "@${tree}/options.rsp", "--BINARYSOURCE=clear", "--x-binarysource=clear"}) {
                c.tasks.at("dependencies").parameters.args = {argument};
                expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "build"}); }));
            }
            c.tasks.at("dependencies").parameters.args.clear();
            v.env["VCPKG_FEATURE_FLAGS"] = "-binarycaching";
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            v.env.clear();
            v.install_dir.clear();
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };
        "byte pressure reclaims unrooted candidates across bounded scan batches"_test = [] {
            using namespace std::chrono;
            ::test::scratch scratch{"semantic-reclaim-batches"};
            auto now = system_clock::time_point{seconds{100}};
            semantic::retention limits{.lifetime = seconds{1}, .bytes = 0};
            auto clock = [&] { return now; };
            constexpr auto count = 260u;
            std::vector<semantic::shard> shards;
            for (auto at = 0u; at < count; ++at) {
                const auto name = "batch-" + std::to_string(at);
                semantic::shard shard{.unit =
                                          semantic::make_unit({{"app", "cpp", "clice", "evaluate", semantic::unit_kind::file, name + ".cpp"}, "cpp"}, "cpp")};
                shard.unit.direct_inputs = {{semantic::input_kind::artifact, "library.jar", "jar-object"}};
                shard.symbols.push_back({.id = name, .ownership = {.owner_unit = shard.unit.id, .context = "cpp"}, .name = name});
                shards.push_back(std::move(shard));
            }
            semantic::store store{scratch.dir, limits, clock};
            const auto view = store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}}, std::move(shards), {});
            expect(store.describe(view).units.size() == count);
            expect(store.candidates("app", "cpp", {{"app", {}}}).size() == count) << "one snapshot caches one candidate per unit";
            now += seconds{2};
            store.sweep();
            expect(store.candidates("app", "cpp", {{"app", {}}}).empty())
                << "unrooted candidates are reclaimed in bounded batches regardless of the scan cursor";
            expect(std::filesystem::is_empty(scratch.dir / "shards") && std::filesystem::is_empty(scratch.dir / "indexes"));
        };
        "clice initialization and executable mapping use the prepared project"_test = [] {
            language::stage c{.server = language::with<"clice">{.options = {.args = {"--mode", "pipe"}, .cache_dir = "build/clice", .append = {"-std=c++23"}}},
                              .build_configuration = "native",
                              .inputs = {{"compilation_database", {language::build_input{"native", "configure", "compilation_database"}}}}};
            auto plan = language::prepare(c, {{"native", configured()}});
            auto json = rfl::json::write(plan.server.initialization);
            expect(json.contains("build/debug/compile_commands.json") && json.contains("-std=c++23"));
            auto local = language::locate(plan.server, ::test::absolute("/work/project"), {{"clice", ::test::absolute("/opt/clice/bin/clice")}}, 2);
            expect(local.argv == std::vector<std::string>{::test::absolute("/opt/clice/bin/clice"), "serve", "--mode", "pipe"});
            expect(local.commands.at("index") == std::vector<std::string>{::test::absolute("/opt/clice/bin/clice"), "index", "--json", "--jobs", "2"});
            expect(local.commands.at("query") == std::vector<std::string>{::test::absolute("/opt/clice/bin/clice"), "query"});
            auto initialized = rfl::json::write(local.initialization);
            expect(initialized.contains("build/debug/compile_commands.json") && initialized.contains("-std=c++23") &&
                   initialized.contains("\"max_stateless_worker_count\":2"));
            auto unbound = c;
            unbound.inputs.clear();
            expect(throws<std::invalid_argument>([&] { language::prepare(unbound, {{"native", configured()}}); }));
            c.build_configuration.clear();
            expect(throws<std::invalid_argument>([&] { language::prepare(c, {{"native", configured()}}); }));
        };
    };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"tooling">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
