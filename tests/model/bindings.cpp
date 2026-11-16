import std;
import boost.ut;
import support;
import jni;
import pinvoke;
import reflection;

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
        return rfl::get<semantic::records::binding_detail>(found->detail->variant());
    }
    const auto cases = [] {
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
            imported.extensions = {{"clr.pinvoke.library", rfl::Generic{std::string{"native"}}},
                                   {"clr.pinvoke.entry_point", rfl::Generic{std::string{"add_numbers"}}},
                                   {"clr.pinvoke.convention", rfl::Generic{std::string{"cdecl"}}}};
            fsharp.add(imported, "src/Native.fs");
            auto missing = imported;
            missing.id = missing.name = "missing";
            missing.extensions["clr.pinvoke.entry_point"] = rfl::Generic{std::string{"absent"}};
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
