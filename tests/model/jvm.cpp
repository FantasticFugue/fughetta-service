import std;
import boost.ut;
import support;
import jvm;
import jni;
import artifact;
import knowledge;
import language;
import reflection;
import native;
import coroutine;
import channels;
import kotlin;
import fixture;
import reuse;
import delivery;
import projectmodel;

namespace {
    using namespace boost::ut;
    std::filesystem::path class_file, archive, header, changed_archive;
    std::array<std::filesystem::path, 15> kotlin_files;
    auto load(const std::filesystem::path& path) -> std::string {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, {}};
    }
    const auto kotlin_cases = [] {
        "Kotlin source edits follow real JVM and JNI bindings through native execution"_test = [] {
            test::scratch scratch{"kotlin-jni-records"};
            const auto inline_object = native_artifact::read(load(kotlin_files[13]));
            auto optimized = native_artifact::records(inline_object, "app", "options", "native", "native", "inline.so", {});
            const auto source_text = load(kotlin_files[14]);
            const auto split = source_text.find("extern");
            std::vector<channels::definition_body> bodies;
            for (const auto& [name, begin, end] :
                 std::vector<std::tuple<std::string, std::uint64_t, std::uint64_t>>{{"inline_leaf", 0, split}, {"optimized", split, source_text.size()}}) {
                semantic::native_identity identity{"app", "source", "clice", "cpp", name};
                bodies.push_back({"source-context",
                                  {identity, name, {}},
                                  {.source = semantic::place{"inline.cpp", begin, end}},
                                  {"inline.cpp", begin, end},
                                  std::string{"function"}});
            }
            expect(std::ranges::any_of(inline_object.locations, [](const auto& location) { return location.frames.size() > 1; }))
                << "an inlined call has an inline frame";
            native_artifact::associate(optimized, inline_object.locations, bodies, {{"inline.cpp", source_text}}, kotlin_files[14].parent_path());
            expect(std::ranges::any_of(optimized.relations, [](const auto& row) {
                return row.from == "inline_leaf" && row.detail && row.detail->variant().visit([](const auto& detail) {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(detail)>, semantic::records::emission_detail>)
                        return detail.inline_depth.value_or(0) > 0;
                    else
                        return false;
                });
            }));
            expect(std::ranges::any_of(optimized.relations, [](const auto& row) { return row.from == "optimized"; }));
            semantic::store store{scratch.dir};
            const auto native_object = native_artifact::read(load(kotlin_files[6]));
            expect(std::ranges::any_of(native_object.symbols, [](const auto& symbol) { return symbol.name == "registered_twice" && !symbol.exported; }));
            expect(native_object.registrations.size() == 1_u && native_object.unresolved.empty());
            const auto relocatable = native_artifact::read(load(kotlin_files[8]));
            expect(std::ranges::any_of(relocatable.symbols, [](const auto& symbol) { return symbol.name == "registered_twice" && !symbol.imported; }));
            std::array<semantic::view_id, 2> versions;
            std::string run_id, run_context;
            for (std::size_t version = 0; version < 2; ++version) {
                auto source = rfl::json::read<semantic::shard>(load(kotlin_files[2 + version])).value();
                source.unit.id = semantic::unit_identifier(source.unit.recipe.unit);
                auto own = [&](auto& rows) {
                    for (auto& row : rows)
                        row.ownership.owner_unit = source.unit.id;
                };
                own(source.symbols);
                own(source.occurrences);
                own(source.references);
                own(source.relations);
                own(source.scopes);
                own(source.typed_ranges);
                own(source.diagnostics);
                source.unit.channel_definitions = language::server<"kotlin">::channels();
                const auto source_path = "src/main/kotlin/sample/Sample.kt";
                source.unit.direct_inputs = {{semantic::input_kind::file, source_path, "source-" + std::to_string(version)}};
                std::map<std::string, semantic::source_anchor> anchors;
                for (const auto& occurrence : source.occurrences)
                    if (occurrence.location.source && std::ranges::contains(occurrence.roles, std::string{"definition"}))
                        anchors.try_emplace(occurrence.symbol,
                                            semantic::source_anchor{source_path, "anchor:" + std::to_string(occurrence.location.source->begin)});
                semantic::view view{.snapshots = {{"app", "main", "v" + std::to_string(version), "source-" + std::to_string(version)}},
                                    .configuration = "kotlin"};
                view.computation =
                    language::computation{.builds = {{"kotlin", fixture::commands({"gradle", "jar"}, {}, {"gradle", "verifyNative"})}}, .project = "app"};
                view.computation->profile.stages["source"].build_configuration = "kotlin";
                const auto source_view = store.load(store.publish_records(view, {source}, anchors));
                const auto bodies =
                    channels::definition_bodies({source_view->pieces.front()->materialized().get()}, {std::string{"function"}, std::string{"method"}});
                expect(!bodies.empty() >> fatal);
                const auto objects = jvm::classes(load(kotlin_files[version]));
                semantic::recipe recipe{{"app", "jvm", "jvm-artifact", "evaluate", semantic::unit_kind::artifact, "sample.jar"}, "options"};
                auto compiled = jvm::records(objects, recipe, "sample", "jar-" + std::to_string(version));
                compiled.unit.channel_definitions = language::server<"jvm-artifact">::channels();
                jvm::associate(compiled, objects, bodies, {{source_path, load(kotlin_files[4 + version])}});
                expect(std::ranges::any_of(compiled.relations, [&](const auto& relation) {
                    return relation.kind == "emits" && relation.ownership.origin.location && relation.ownership.origin.location->source &&
                           relation.ownership.origin.location->source->path == source_path;
                }));
                auto native = native_artifact::records(native_object, "app", "options", "native", "native", "libnative.so", {});
                native.unit.channel_definitions = language::server<"native-artifact">::channels();
                const auto core_call = std::ranges::find(native_object.calls, "registered_twice", &native_artifact::call::from);
                expect((core_call != native_object.calls.end()) >> fatal);
                expect(std::ranges::any_of(native.relations, [&](const auto& relation) {
                    if (relation.kind != "calls")
                        return false;
                    const auto from = std::ranges::find(native.symbols, relation.from, &semantic::records::symbol::id);
                    const auto to = std::ranges::find(native.symbols, relation.to, &semantic::records::symbol::id);
                    return static_cast<bool>(from != native.symbols.end() && to != native.symbols.end() && from->name == "registered_twice" &&
                                             to->name == core_call->to);
                })) << "LLVM call evidence connects the registered native entry to its implementation";
                const auto root = kotlin_files[11].parent_path().parent_path();
                const auto model = projectmodel::read(load(kotlin_files[11]));
                std::string library_module;
                for (const auto& unit : model.units)
                    for (const auto& ref : unit.references)
                        ref.visit([&](const auto& value) {
                            if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, projectmodel::artifact_reference>)
                                if (value.object.name == "library.jar")
                                    library_module = "object:" + value.object.hash;
                        });
                expect((!library_module.empty()) >> fatal) << "the separately built Kotlin library is a real model CAS dependency";
                std::map<std::string, std::string> dependencies;
                for (const auto& object : projectmodel::objects(model))
                    dependencies.emplace(object, object);
                (void)delivery::capture(root / "objects", dependencies, scratch.dir / "exports");
                const auto captured = delivery::capture(root, {{"archive", kotlin_files[version].filename().generic_string()}, {"model", "model/model.json"}},
                                                        scratch.dir / "exports");
                const auto jar = std::ranges::find(captured, "archive", &builds::exported_file::name);
                const auto model_object = std::ranges::find(captured, "model", &builds::exported_file::name);
                expect((jar != captured.end() && model_object != captured.end()) >> fatal);
                std::vector<semantic::shard> produced;
                native::context runtime;
                auto read_library = [&]() -> coroutine::task<> {
                    const auto java = std::getenv("TEST_JAVA");
                    if (!java)
                        throw std::runtime_error("Kotlin metadata flow requires the configured Java tool");
                    language::server<"jvm-artifact">::options options{
                        .module = "sample", .java_release = 25, .metadata_helper = "kotlin-helper", .source_provider = "registered-kotlin"};
                    const auto program =
                        language::locate(language::server<"jvm-artifact">::launch(
                                             options, {{.type = "jvm_artifact", .path = "sample.jar"}, {.type = "project_model", .path = "model/model.json"}}),
                                         root, {{"java", java}, {"kotlin-helper", kotlin_files[12].string()}}, 1, scratch.dir);
                    jvm::reader adapter{runtime.get_scheduler(),
                                        {.cwd = root, .env = builds::environment{{"JAVA_TOOL_OPTIONS", "-Djava.io.tmpdir=" + root.string()}}},
                                        {.slot = "slot", .project = "app", .provider = "jvm-artifact", .options = "options", .stage = "jvm"},
                                        program.commands,
                                        {.execution = "runtime"},
                                        language::document_loader{.documents = {{source_path, load(kotlin_files[4 + version])}}}};
                    std::set<std::string> allowed_objects;
                    for (const auto& entry : std::filesystem::directory_iterator(scratch.dir / "exports"))
                        if (entry.is_regular_file())
                            allowed_objects.insert(entry.path().filename().string());
                    adapter.cache(scratch.dir);
                    adapter.objects({scratch.dir / "exports", std::move(allowed_objects)});
                    auto opened = co_await adapter.open(program.initialization);
                    language::installation installed{.snapshot = "snapshot-" + std::to_string(version),
                                                     .source_files = {{source_path, "source-" + std::to_string(version)}},
                                                     .model = {{"sample.jar", jar->digest.str()}, {"model/model.json", model_object->digest.str()}}};
                    for (const auto& source : source_view->pieces)
                        installed.source_facts["source_facts"].push_back(
                            std::make_shared<semantic::shard_object>(source->materialized(), nullptr, std::filesystem::path{}));
                    auto current = co_await adapter.update(opened.epoch, std::move(installed));
                    semantic::extent demand{.modules = {"sample", library_module},
                                            .facts = {std::string{"symbols"}, std::string{"types"}, std::string{"relations"}}};
                    const auto units = co_await adapter.units(current.epoch, demand);
                    expect(!units.empty() >> fatal);
                    for (const auto& unit : units) {
                        auto evaluated = co_await adapter.evaluate({.execution = {"request", "computation", "jvm"},
                                                                    .slot_id = "slot",
                                                                    .unit = unit,
                                                                    .epoch = current.epoch,
                                                                    .inputs = current.inputs,
                                                                    .demand = demand});
                        for (auto& part : evaluated.shards) {
                            part.unit.channel_definitions = language::server<"jvm-artifact">::channels();
                            produced.push_back(std::move(part));
                        }
                    }
                    co_await adapter.close();
                };
                cores::run(runtime, read_library());
                produced.push_back(native);
                const auto combined = store.load(store.publish_records(source_view->description, std::move(produced), {}));
                const auto library = combined->query(
                    {.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"library.Box.decorate", library_module}}});
                expect((library.items.size() == 1_u) >> fatal);
                expect(library.items.front().domain == "kotlin");
                const semantic::entity declaration{library.items.front().symbol_id, library.items.front().context};
                const auto uses = combined->query({.action = std::string{"references"}, .selector = semantic::selection{.entity = declaration}});
                const auto source_use =
                    std::ranges::any_of(uses.references, [&](const auto& use) { return use.location.source && use.location.source->path == source_path; });
                expect(source_use) << "the actual source reference resolves to its independently decoded library declaration";
                const auto generated =
                    combined->query({.action = std::string{"relations"}, .selector = semantic::selection{.entity = declaration}, .relations = {"emits"}});
                expect(generated.relations.size() == 3_u) << "library declaration -> original, overload and default JVM members";
                std::vector<const semantic::shard*> inputs;
                for (const auto& shard : combined->pieces)
                    inputs.push_back(shard->materialized().get());
                semantic::recipe binding{{"app", "jni", "jni", "evaluate", semantic::unit_kind::group, "imports"}, "options"};
                auto bound = jni::resolve(binding, {{{"sample", {"native"}}}}, inputs);
                expect(bound.relations.size() == 1_u && bound.diagnostics.empty());
                expect(rfl::json::write(bound.relations).contains("jni_dynamic"));
                expect(std::ranges::any_of(native.relations, [](const auto& relation) { return relation.kind == "calls"; }));
                versions[version] = store.publish_records(combined->description, {bound}, {});
                const auto ready = store.load(versions[version]);
                const auto entry =
                    ready->query({.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"registered_twice", "native"}}});
                expect((entry.items.size() == 1_u) >> fatal);
                const auto impact =
                    ready->query({.action = std::string{"impact"},
                                  .selector = semantic::selection{.entity = semantic::entity{entry.items.front().symbol_id, entry.items.front().context}},
                                  .changes = {"body"},
                                  .depth = 4});
                const auto run = std::ranges::find_if(impact.items, [](const auto& item) { return item.domain == "kotlin" && item.name == "run"; });
                expect((run != impact.items.end()) >> fatal);
                expect(std::ranges::any_of(impact.impact, [&](const auto& path) {
                    return path.target.symbol_id == run->symbol_id && path.category == "behavior" && path.steps.size() == 3;
                })) << "native export -> JNI binding -> JVM caller -> Kotlin source";
                const auto core =
                    ready->query({.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{core_call->to, "native"}}});
                expect((core.items.size() == 1_u) >> fatal);
                const auto core_impact =
                    ready->query({.action = std::string{"impact"},
                                  .selector = semantic::selection{.entity = semantic::entity{core.items.front().symbol_id, core.items.front().context}},
                                  .changes = {"body"},
                                  .depth = 5});
                expect(std::ranges::any_of(core_impact.impact, [&](const auto& path) {
                    return path.target.symbol_id == run->symbol_id && path.category == "behavior" && path.steps.size() == 4;
                })) << "native implementation -> registered entry -> JNI -> JVM caller -> Kotlin source";
                run_id = run->symbol_id;
                run_context = run->context;
            }
            const auto compared = semantic::compare_views(
                *store.load(versions[0]), *store.load(versions[1]),
                {.action = std::string{"impact"}, .selector = semantic::selection{.entity = semantic::entity{run_id, run_context}}, .changes = {"body"}});
            expect(!compared.comparison->changes.empty());
            expect(std::ranges::any_of(compared.comparison->reanalysis, [](const auto& unit) {
                return unit.unit.provider == "registered-kotlin" && std::ranges::any_of(unit.tasks, [](const auto& task) { return task.operation == "test"; });
            }));
        };
    };
    const auto cases = [] {
        "compiled JVM artifacts publish typed declarations and queryable bytecode calls"_test = [] {
            const auto single = jvm::classes(load(class_file));
            const auto packed = jvm::classes(load(archive));
            expect((single.size() == 1_u && packed.size() == 2_u) >> fatal);
            expect(single.front().name == "sample/Sample" && packed.front().source == "Sample.java");
            semantic::recipe recipe{{"app", "jvm", "jvm-artifact", "evaluate", semantic::unit_kind::artifact, "sample.jar"}, "options"};
            const auto first = jvm::records(single, recipe, "sample", "object-one");
            auto shard = jvm::records(packed, recipe, "sample", "object-two");
            shard.unit.channel_definitions = language::server<"jvm-artifact">::channels();
            expect(std::ranges::all_of(first.identities, [&](const auto& identity) {
                return std::ranges::any_of(shard.identities,
                                           [&](const auto& other) { return identity.symbol == other.symbol && identity.declaration == other.declaration; });
            })) << "artifact content changes do not rename JVM declarations";
            expect(shard.unit.coverage.unresolved_count > 0_u) << "invokedynamic retains an unresolved call instead of inventing a target";
            const auto klass = std::ranges::find(shard.symbols, "sample/Sample", &semantic::records::symbol::name);
            expect((klass != shard.symbols.end() && klass->type.has_value()) >> fatal);
            const auto generic = std::ranges::find(shard.types, *klass->type, &semantic::records::type::id);
            expect((generic != shard.types.end()) >> fatal);
            expect(generic->type_parameters.size() == 1_u && generic->constraints.size() == 2_u);
            const auto parameter = std::ranges::find(shard.types, generic->type_parameters.front(), &semantic::records::type::id);
            expect((parameter != shard.types.end()) >> fatal);
            expect(parameter->binder == klass->id && parameter->constraints.size() == 2_u);
            expect(std::ranges::any_of(shard.types,
                                       [](const auto& row) { return row.lower_bound.has_value() && std::ranges::contains(row.qualifiers, "wildcard"); }));
            const auto annotations = rfl::json::write(klass->extensions.at("jvm.annotations"));
            const auto decoded_annotations = rfl::from_generic<std::vector<jvm::annotation>>(klass->extensions.at("jvm.annotations")).value();
            const auto details_annotation = std::ranges::find(decoded_annotations, "Lsample/Details;", &jvm::annotation::descriptor);
            expect((details_annotation != decoded_annotations.end()) >> fatal);
            const auto text_value = rfl::from_generic<std::map<std::string, rfl::Generic>>(details_annotation->values.at("text")).value();
            expect(rfl::from_generic<std::string>(text_value.at("value")).value() == std::string{"class\0", 6} + "😀");
            expect(annotations.contains("Lsample/Details;") && annotations.contains("Lsample/Hidden;"));
            expect(annotations.contains("\"visible\":true") && annotations.contains("\"visible\":false"));
            expect(annotations.contains("\"value\":-7") && annotations.contains("\"value\":9223372036854775806"));
            expect(annotations.contains("\"elements\"") && annotations.contains("Lsample/Nested;") && annotations.contains("[Ljava/lang/String;"));
            expect(annotations.contains("\"value\":\"1.5\"") && annotations.contains("\"value\":\"2.25\""));
            const auto run = std::ranges::find(shard.symbols, "run", &semantic::records::symbol::name);
            expect((run != shard.symbols.end()) >> fatal);
            expect(run->type && run->type->ends_with(":(I)I"));
            expect(std::ranges::any_of(shard.symbols, [](const auto& s) { return s.name == "nativeEntry" && std::ranges::contains(s.modifiers, "native"); }));
            const auto twice = std::ranges::find(shard.symbols, "twice", &semantic::records::symbol::name);
            expect((twice != shard.symbols.end()) >> fatal);
            expect(rfl::json::write(twice->extensions.at("jvm.annotations")).contains("method"));
            const auto field = std::ranges::find(shard.symbols, "field", &semantic::records::symbol::name);
            expect((field != shard.symbols.end()) >> fatal);
            expect(rfl::json::write(field->extensions.at("jvm.annotations")).contains("field"));
            expect(std::ranges::count(shard.relations, twice->id, &semantic::records::relation::to) == 3);
            test::scratch scratch{"jvm-records"};
            semantic::store store{scratch.dir};
            const auto id = store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "jvm"}, {shard}, {});
            const auto view = store.load(id);
            const auto class_query =
                view->query({.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"sample/Sample"}}, .include = {"type"}});
            expect(class_query.items.size() == 1_u);
            expect(std::ranges::any_of(class_query.types,
                                       [](const auto& row) { return row.kind == "parameter" && row.name == "T" && row.constraints.size() == 2; }));
            const auto selected = view->query(
                {.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"twice"}}, .include = {"declarations", "type"}});
            expect((selected.items.size() == 1_u) >> fatal);
            const auto references = view->query(
                {.action = std::string{"references"}, .selector = semantic::selection{.entity = semantic::entity{selected.items.front().symbol_id}}});
            expect(references.references.size() == 3_u);
            native_artifact::object library{.format = "elf", .arch = "x86_64"};
            const auto declarations = load(header);
            auto declared = [&](const std::string& name) {
                auto at = declarations.find(name);
                if (at == std::string::npos)
                    return false;
                at += name.size();
                while (at < declarations.size() && std::isspace(static_cast<unsigned char>(declarations[at])))
                    ++at;
                return at < declarations.size() && declarations[at] == '(';
            };
            for (const auto& symbol : shard.symbols)
                if (std::ranges::contains(symbol.modifiers, "native")) {
                    const auto names = jni::names(symbol);
                    expect((names.size() == 2_u) >> fatal);
                    const auto name = declared(names.front()) ? names.front() : names.back();
                    expect(declared(name)) << "JNI encoding must match javac's generated header";
                    library.symbols.push_back({.name = name, .kind = "function", .binding = "Global", .exported = true});
                }
            auto native = native_artifact::records(library, "app", "options", "native", "native", "libsample.so", {});
            native.unit.channel_definitions = language::server<"native-artifact">::channels();
            const auto combined = store.publish_records(store.describe(id), {native}, {});
            const auto ready = store.load(combined);
            std::vector<const semantic::shard*> input;
            for (const auto& object : ready->pieces)
                input.push_back(object->materialized().get());
            semantic::recipe binding{{"app", "jni", "jni", "evaluate", semantic::unit_kind::group, "imports"}, "options"};
            auto bound = jni::resolve(binding, {{{"sample", {"native"}}}}, input);
            expect(bound.relations.size() == 3_u && bound.diagnostics.empty());
            const auto details = rfl::json::write(bound.relations);
            expect(details.contains("\"class\"") && details.contains("\"receiver\"") && details.contains("_003bb"));
            for (const auto& type : {"JNIEnv*", "jclass", "jobject", "jstring", "jthrowable", "jintArray", "jobjectArray"})
                expect(details.contains(std::string{"\"native_type\":\""} + type + "\""));
            expect(details.contains("\"array_dimensions\":2") && details.contains("\"element_descriptor\":\"I\""));
            const auto bindings = store.publish_records(store.describe(combined), {bound}, {});
            expect(store.load(bindings)->query({.action = std::string{"relations"}, .relations = {"binds"}}).relations.size() == 3_u);
            native_artifact::object short_export{
                .format = "elf",
                .arch = "x86_64",
                .symbols = {{.name = "Java_sample_Sample_nativeEntry", .kind = "function", .binding = "Global", .exported = true}}};
            auto extra = native_artifact::records(short_export, "app", "options", "extra", "extra", "libextra.so", {});
            auto extended = store.load(store.publish_records(store.describe(combined), {extra}, {}));
            std::vector<const semantic::shard*> alternatives;
            for (const auto& object : extended->pieces)
                alternatives.push_back(object->materialized().get());
            auto preferred = jni::resolve(binding, {{{"sample", {"native", "extra"}}}}, alternatives);
            expect(preferred.relations.size() == 3_u && preferred.diagnostics.empty());
            const auto preferred_details = rfl::json::write(preferred.relations);
            expect(!preferred_details.contains("nativeEntry__J") && !preferred_details.contains("nativeEntry__I"))
                << "short JNI export precedes descriptor-qualified exports";
            auto duplicate = native_artifact::records(short_export, "app", "options", "duplicate", "duplicate", "libduplicate.so", {});
            auto conflicting = store.load(store.publish_records(extended->description, {duplicate}, {}));
            alternatives.clear();
            for (const auto& object : conflicting->pieces)
                alternatives.push_back(object->materialized().get());
            const auto ambiguous = jni::resolve(binding, {{{"sample", {"native", "extra", "duplicate"}}}}, alternatives);
            expect(std::ranges::count_if(ambiguous.relations, [](const auto& relation) { return relation.resolution == "candidate"; }) == 4);
            expect(ambiguous.unit.coverage.unresolved_count == 2_u);
            const auto stage = language::server<"jvm-artifact">::launch({.module = "sample"}, {{.type = "jvm_artifact", .path = "sample.jar"}});
            expect(language::locate(stage, scratch.dir, {}, 1, scratch.dir).argv.empty()) << "an in-process reader has no tool executable";
            native::context runtime;
            auto evaluate = [&]() -> coroutine::task<> {
                const auto tree = scratch.dir / "tree", objects = scratch.dir / "exports";
                std::filesystem::create_directories(tree);
                std::filesystem::copy_file(archive, tree / "sample.jar");
                const auto first_object = delivery::capture(tree, {{"archive", "sample.jar"}}, objects).front().digest.str();
                std::filesystem::copy_file(changed_archive, tree / "sample.jar", std::filesystem::copy_options::overwrite_existing);
                const auto second_object = delivery::capture(tree, {{"archive", "sample.jar"}}, objects).front().digest.str();
                jvm::reader adapter{runtime.get_scheduler(),
                                    {.cwd = tree},
                                    {.slot = "slot", .project = "app", .provider = "jvm-artifact", .options = "options", .stage = "jvm"},
                                    {},
                                    {.execution = "runtime"}};
                adapter.cache(scratch.dir);
                adapter.objects({objects, {first_object, second_object}});
                auto opened = co_await adapter.open(stage.initialization);
                std::vector<semantic::candidate> previous;
                std::array<semantic::view_id, 2> member_views;
                for (std::size_t version = 0; version < 2; ++version) {
                    const auto digest = version ? second_object : first_object;
                    auto installed =
                        co_await adapter.update(adapter.state().epoch, {.snapshot = "snapshot-" + std::to_string(version), .model = {{"sample.jar", digest}}});
                    auto units = co_await adapter.units(installed.epoch, {});
                    expect((units.size() == 3_u) >> fatal);
                    auto measured = co_await adapter.current_inputs(previous);
                    semantic::source_vector sources;
                    reuse::channel_values channels;
                    const auto validators = language::server<"jvm-artifact">::input_validators();
                    reuse::environment environment{sources, measured, channels, "app", validators};
                    std::vector<semantic::candidate> approved;
                    for (const auto& candidate : previous)
                        if (reuse::accept(candidate, installed, {}, environment))
                            approved.push_back(candidate);
                    std::vector<semantic::shard> produced;
                    std::size_t reused = 0;
                    for (const auto& unit : units) {
                        auto evaluated = co_await adapter.evaluate({.execution = {"request", "computation", "jvm"},
                                                                    .slot_id = "slot",
                                                                    .unit = unit,
                                                                    .epoch = installed.epoch,
                                                                    .inputs = installed.inputs},
                                                                   approved);
                        if (evaluated.reused)
                            ++reused;
                        for (auto& part : evaluated.shards) {
                            part.unit.channel_definitions = language::server<"jvm-artifact">::channels();
                            produced.push_back(std::move(part));
                        }
                    }
                    expect(reused == (version ? std::size_t{1} : std::size_t{0}))
                        << "the unchanged class reuses its contribution despite changed ZIP layout and another class";
                    auto description = version ? store.describe(member_views[0]) : semantic::view{};
                    description.id.clear();
                    description.snapshots = {{"app", "main", "members-" + std::to_string(version), "source"}};
                    description.configuration = "jvm-members";
                    member_views[version] = store.publish_records(std::move(description), std::move(produced), {});
                    const auto indexed = store.load(member_views[version]);
                    const auto query = indexed->query(
                        {.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"run"}}, .include = {"definitions"}});
                    expect((query.items.size() == 1_u) >> fatal);
                    const auto definition = std::ranges::find_if(query.occurrences, [&](const auto& occurrence) {
                        return occurrence.symbol == query.items.front().symbol_id && std::ranges::contains(occurrence.roles, std::string{"definition"});
                    });
                    expect((definition != query.occurrences.end() && definition->location.artifact.has_value()) >> fatal);
                    expect(definition->location.artifact->artifact == digest) << "reused class locations resolve through this view's captured container";
                    expect(load(objects / first_object) == load(archive)) << "rewriting the mutable build output preserves the captured old artifact";
                    const auto target = indexed->query({.action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"adjust"}}});
                    expect((target.items.size() == 1_u) >> fatal);
                    const auto calls = indexed->query(
                        {.action = std::string{"references"}, .selector = semantic::selection{.entity = semantic::entity{target.items.front().symbol_id}}});
                    expect(calls.references.size() == 1_u && calls.references.front().dispatch == std::string{"direct"})
                        << "cross-class calls resolve to the defining class's stable identity";
                    previous.clear();
                    for (const auto& [unit, ids] : indexed->description.units) {
                        const auto metadata = store.object_unit(ids.front());
                        previous.push_back({.id = unit, .recipe = metadata.recipe, .shards = ids, .units = {metadata}});
                    }
                }
                expect(store.artifact_roots().contains(first_object) && store.artifact_roots().contains(second_object));
                auto old = store.load(member_views[0]);
                store.retire(member_views[0]);
                expect(store.artifact_roots().contains(first_object)) << "an in-flight old view pins its immutable container after retirement";
                for (const auto& digest : {first_object, second_object})
                    std::filesystem::last_write_time(objects / digest, std::filesystem::file_time_type::clock::now() - std::chrono::hours{24});
                expect(delivery::collect(objects, std::chrono::seconds{0}, 0, {}, store.artifact_roots()).empty());
                expect(std::filesystem::exists(objects / first_object) && std::filesystem::exists(objects / second_object));
                old.reset();
                expect(!store.artifact_roots().contains(first_object));
                expect(delivery::collect(objects, std::chrono::seconds{0}, 0, {}, store.artifact_roots()).empty());
                expect(!std::filesystem::exists(objects / first_object) && std::filesystem::exists(objects / second_object))
                    << "retired candidate metadata does not pin obsolete containers permanently";
                co_await adapter.close();
                auto resolver = language::make_adapter(runtime.get_scheduler(), {.cwd = scratch.dir},
                                                       {.slot = "slot", .project = "app", .provider = "jni", .options = "options", .stage = "jni"}, {},
                                                       {.execution = "runtime"});
                native_artifact::object spare_object{
                    .format = "elf",
                    .arch = "x86_64",
                    .symbols = {{.name = "idle_function", .kind = "function", .binding = "Global", .size = 8, .exported = true}}};
                auto spare = native_artifact::records(spare_object, "app", "options", "spare", "spare", "libspare.so", {});
                spare.unit.channel_definitions = language::server<"native-artifact">::channels();
                const auto original_channels = channels::outputs(spare.unit.channel_definitions, {&spare});
                spare.symbols.front().extensions["native.size"] = std::int64_t{16};
                const auto resized_channels = channels::outputs(spare.unit.channel_definitions, {&spare});
                for (const auto& output : original_channels) {
                    const auto changed = std::ranges::find(resized_channels, output.name, &semantic::channel::name);
                    if (output.name == "exports")
                        expect(changed->value == output.value);
                    if (output.name == "linkage")
                        expect(changed->value != output.value);
                }
                const auto scoped = store.load(store.publish_records(ready->description, {spare}, {}));
                auto command = language::server<"jni">::launch({{{"sample", {"native", "spare"}}}}, {});
                auto started = co_await resolver->open(command.initialization);
                language::installation facts{.snapshot = "snapshot"};
                for (const auto& object : scoped->pieces)
                    facts.source_facts[object->unit.recipe.unit.provider == "jvm-artifact" ? "managed" : "native"].push_back(
                        std::make_shared<semantic::shard_object>(object->materialized(), nullptr, std::filesystem::path{}));
                auto current = co_await resolver->update(started.epoch, std::move(facts));
                auto groups = co_await resolver->units(current.epoch, {});
                expect((groups.size() == 3_u) >> fatal);
                const auto native_part =
                    std::ranges::find_if(scoped->pieces, [](const auto& part) { return std::ranges::contains(part->unit.modules, "native"); });
                expect((native_part != scoped->pieces.end()) >> fatal);
                const auto native_group =
                    std::ranges::find_if(groups, [&](const auto& unit) { return rfl::json::read<jni::group>(unit.name)->native == (*native_part)->unit.id; });
                expect((native_group != groups.end()) >> fatal);
                auto resolved = co_await resolver->evaluate({.execution = {"request", "computation", "jni"},
                                                             .slot_id = "slot",
                                                             .unit = *native_group,
                                                             .epoch = current.epoch,
                                                             .inputs = current.inputs});
                expect((resolved.shards.size() == 1_u) >> fatal);
                expect(resolved.shards.front().relations.size() == 3_u && resolved.shards.front().unit.cacheable);
                const auto& contribution = resolved.shards.front().unit;
                semantic::candidate candidate{.recipe = contribution.recipe, .units = {contribution}};
                semantic::source_vector sources;
                semantic::input_vector measured{{"app", current.inputs}};
                reuse::channel_values channels;
                reuse::environment environment{sources, measured, channels, "app"};
                std::vector<const semantic::unit*> managed, libraries;
                for (const auto& part : scoped->pieces)
                    (part->unit.recipe.unit.provider == "jvm-artifact" ? managed : libraries).push_back(&part->unit);
                auto refresh = [&] {
                    for (const auto& read : contribution.observed_dependencies)
                        if (read.kind == semantic::dependency_kind::record_set) {
                            const auto selection = read.records.value_or(semantic::record_selection{});
                            environment.collections[{read.key, *read.channel, selection}] =
                                channels::collection_value(read.key == "managed" ? managed : libraries, *read.channel, selection);
                        }
                };
                refresh();
                expect(reuse::accept(candidate, current, {}, environment));
                const auto spare_part = std::ranges::find_if(libraries, [](const auto* unit) { return std::ranges::contains(unit->modules, "spare"); });
                expect((spare_part != libraries.end()) >> fatal);
                auto changed_spare = **spare_part;
                for (auto& output : changed_spare.output_channels)
                    if (output.name == "linkage")
                        output.value = "changed-addresses-and-size";
                *spare_part = &changed_spare;
                refresh();
                expect(reuse::accept(candidate, current, {}, environment)) << "another mapped library's addresses do not replace this library's contribution";
                const auto extra_part =
                    std::ranges::find_if(extended->pieces, [](const auto& part) { return std::ranges::contains(part->unit.modules, "extra"); });
                expect((extra_part != extended->pieces.end()) >> fatal);
                auto unrelated = (*extra_part)->unit;
                libraries.push_back(&unrelated);
                refresh();
                expect(reuse::accept(candidate, current, {}, environment)) << "exports outside this class loader's mapped modules preserve its JNI group";
                unrelated.modules = libraries.front()->modules;
                refresh();
                expect(!reuse::accept(candidate, current, {}, environment)) << "new exports in a mapped module must be reconsidered";
                co_await resolver->close();
            };
            cores::run(runtime, evaluate());
        };
    };
} // namespace
int main(int argc, char** argv) {
    if (argc == 17 && std::string_view{argv[1]} == "--kotlin") {
        for (std::size_t i = 0; i < kotlin_files.size(); ++i)
            kotlin_files[i] = argv[i + 2];
        boost::ut::suite<"kotlin_jni_flow">{kotlin_cases};
        return boost::ut::cfg<>.run() ? 1 : 0;
    }
    if (argc != 5)
        return 2;
    class_file = argv[1];
    archive = argv[2];
    header = argv[3];
    changed_archive = argv[4];
    boost::ut::suite<"jvm_artifacts">{cases};
    return boost::ut::cfg<>.run() ? 1 : 0;
}
