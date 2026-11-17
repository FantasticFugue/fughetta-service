export module artifact;
import evaluation;
import coroutine;
import toolchain;
import enumeration;
import channels;
import std;
import reflection;
import records;
export import provider;

export namespace native_artifact {
    class reader;
}

export namespace language {
    // Object files, shared libraries and executables: ELF, Mach-O and COFF with their C ABI symbol names.
    template <> struct domain<"native"> {
        static constexpr std::string_view name = "native", artifact = "native-artifact", linkage = "native.linkage_name";
    };
    template <> struct server<"native-artifact"> : reads<domain<"native">, optional_input<"ir", "llvm_ir">> {
        static auto channels() -> std::vector<semantic::channel_definition> {
            return {{.name = "declarations", .projections = {std::string{"symbols"}, std::string{"types"}}},
                    {.name = "exports",
                     .projections = {std::string{"symbols"}},
                     .symbol_extensions = std::vector<std::string>{std::string{domain::linkage}, "native.jni_registrations"}},
                    {.name = "relations", .projections = {std::string{"relations"}}},
                    linkage_channel()};
        }
        using adapter = native_artifact::reader;
        // fughetta-native reads the artifact, its debug information and the LLVM IR or bitcode with LLVM's libraries.
        struct options {
            std::string host = "fughetta-native";
            std::string module;
            builds::environment env;
        };
        static constexpr std::array<std::string_view, 5> facts{"symbols", "definitions", "references", "relations", "diagnostics"};
        static constexpr std::string_view update_granularity = "artifact";
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.host}); }
        static auto executes_project_code(const options& o) -> bool { return builds::in_project({o.host}); }
        static void validate(const options& o) {
            (void)requirements(o);
            if (o.module.empty())
                throw std::invalid_argument("native artifact module is required");
            builds::validate_environment(o.env);
        }
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program {
            const builds::context* artifact = nullptr;
            const builds::context* ir = nullptr;
            for (const auto& input : inputs) {
                auto*& selected = input.type == "native-artifact" ? artifact : ir;
                if ((input.type != "native-artifact" && input.type != "llvm_ir") || selected)
                    throw std::invalid_argument("native artifact stage requires one artifact and at most one LLVM IR input");
                selected = &input;
            }
            if (!artifact)
                throw std::invalid_argument("native artifact stage requires one native-artifact producer");
            std::map<std::string, std::string> settings{{"artifact", artifact->path}, {"module", o.module}};
            program result{.provider = "native-artifact", .argv = {o.host, "${tree}/" + artifact->path}, .env = o.env};
            if (ir) {
                settings.emplace("ir", ir->path);
                result.argv.insert(result.argv.end(), {"--ir", "${tree}/" + ir->path});
            }
            result.initialization = rfl::to_generic(settings);
            return result;
        }
    };
    static_assert(domain_reader<server<"native-artifact">>);
} // namespace language

export namespace native_artifact {
    // The object fughetta-native prints.
    struct entry {
        std::string name, kind, binding, section;
        std::uint64_t address = 0, size = 0;
        bool imported = false, exported = false;
        std::string object_file;
    };
    struct call {
        std::string from, to;
        std::uint64_t instruction = 0;
        // The LLVM IR the call is in; the host leaves it empty and the reader names the build output.
        std::string artifact;
    };
    struct registration {
        std::string klass, name, descriptor, function, table;
    };
    // Inline frames of one address, innermost first.
    struct frame {
        std::string file;
        std::uint64_t line = 0, column = 0;
    };
    struct location {
        std::uint64_t address = 0;
        std::vector<frame> frames;
    };
    struct object {
        std::string format, arch;
        std::vector<entry> symbols;
        std::vector<call> calls;
        std::vector<registration> registrations;
        std::vector<std::string> unresolved;
        std::vector<location> locations;
    };
    inline auto read(std::string_view text) -> object {
        auto decoded = rfl::json::read<object>(text);
        if (!decoded)
            throw std::invalid_argument("cannot decode the native artifact report: " + std::string{decoded.error().what()});
        return std::move(*decoded);
    }

    inline void associate(semantic::shard& out, const std::vector<location>& locations, const std::vector<channels::definition_body>& definitions,
                          const std::map<std::string, std::string>& documents, const std::filesystem::path& root) {
        std::unordered_map<semantic::symbol_id, const semantic::records::occurrence*> positions;
        for (const auto& row : out.occurrences)
            positions.try_emplace(row.symbol, &row);
        using native_record = std::pair<const semantic::records::symbol*, const semantic::records::occurrence*>;
        std::map<std::uint64_t, std::vector<std::pair<bool, native_record>>> events;
        for (const auto& symbol : out.symbols) {
            if (symbol.kind != "function" || std::ranges::contains(symbol.modifiers, "import"))
                continue;
            const auto found = positions.find(symbol.id);
            if (found != positions.end() && found->second->location.artifact && found->second->location.artifact->offset) {
                const auto start = *found->second->location.artifact->offset;
                auto size = std::uint64_t{1};
                if (const auto value = symbol.extensions.find("native.size"); value != symbol.extensions.end())
                    size = std::max(std::uint64_t{1}, rfl::from_generic<std::uint64_t>(value->second).value());
                events[start].push_back({true, {&symbol, found->second}});
                if (size <= std::numeric_limits<std::uint64_t>::max() - start)
                    events[start + size].push_back({false, {&symbol, found->second}});
            }
        }
        std::map<std::uint64_t, std::optional<native_record>> addresses;
        std::set<native_record> active;
        for (const auto& [address, changes] : events) {
            for (const auto& [add, row] : changes) {
                if (add)
                    active.insert(row);
                else
                    active.erase(row);
            }
            addresses[address] = active.size() == 1 ? std::optional{*active.begin()} : std::nullopt;
        }
        std::unordered_set<semantic::symbol_id> retained;
        for (const auto& row : out.identities)
            retained.insert(row.symbol);
        std::set<std::string> emitted;
        for (const auto& row : out.relations)
            emitted.insert(row.id);
        std::map<std::string, std::vector<std::pair<std::size_t, std::uint64_t>>> lines;
        for (const auto& [path, text] : documents) {
            auto& offsets = lines[path];
            offsets.emplace_back(0, 0);
            std::uint64_t scalar = 0;
            for (std::size_t byte = 0; byte < text.size(); ++byte) {
                if ((static_cast<unsigned char>(text[byte]) & 0xc0) != 0x80)
                    ++scalar;
                if (text[byte] == '\n')
                    offsets.emplace_back(byte + 1, scalar);
            }
        }
        std::map<std::string, std::vector<const channels::definition_body*>> source_files;
        for (const auto& definition : definitions)
            source_files[definition.body.path].push_back(&definition);
        std::set<std::tuple<std::string, std::string, std::uint64_t, std::uint64_t>> seen;
        for (const auto& location : locations) {
            const auto address = location.address;
            auto interval = addresses.upper_bound(address);
            if (interval == addresses.begin() || !(--interval)->second)
                continue;
            const auto [symbol, occurrence] = *interval->second;
            for (std::size_t frame_index = 0; frame_index < location.frames.size(); ++frame_index) {
                const auto& frame = location.frames[frame_index];
                auto path = std::filesystem::path{frame.file};
                if (path.is_absolute())
                    path = path.lexically_relative(std::filesystem::absolute(root));
                const auto file = documents.find(path.lexically_normal().generic_string());
                if (file == documents.end() || !frame.line)
                    continue;
                const auto& text = file->second;
                const auto& offsets = lines.at(file->first);
                if (frame.line > offsets.size() || !seen.emplace(symbol->id, file->first, frame.line, frame.column).second)
                    continue;
                const auto [line_begin, line_scalar] = offsets[frame.line - 1];
                auto begin = line_begin;
                const auto newline = text.find('\n', begin);
                auto end = newline == std::string::npos ? text.size() : newline;
                if (frame.column) {
                    begin += frame.column - 1;
                    if (begin > end)
                        continue;
                    end = begin;
                }
                auto scalar = [&](std::size_t byte) {
                    auto result = line_scalar;
                    for (std::size_t i = line_begin; i < byte; ++i)
                        if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80)
                            ++result;
                    return result;
                };
                const auto first = scalar(begin), last = scalar(end);
                {
                    const auto& native = *symbol;
                    const semantic::identity_binding* selected = nullptr;
                    std::string source_context;
                    semantic::locator source_location;
                    std::uint64_t width = std::numeric_limits<std::uint64_t>::max();
                    bool ambiguous = false;
                    for (const auto* definition : source_files[file->first]) {
                        const auto& source = *definition;
                        if (source.body.end <= first || source.body.begin > last)
                            continue;
                        const auto length = source.body.end - source.body.begin;
                        if (!frame.column && selected && (selected->symbol != source.identity.symbol || source_context != source.context))
                            ambiguous = true;
                        else if (length < width) {
                            selected = &source.identity;
                            source_context = source.context;
                            source_location = source.location;
                            width = length;
                            ambiguous = false;
                        } else if (length == width && selected && (selected->symbol != source.identity.symbol || source_context != source.context))
                            ambiguous = true;
                    }
                    if (!selected || ambiguous)
                        continue;
                    const auto relation = out.unit.id + ":emits:" + native.id + ":" + source_context + ":" + selected->symbol;
                    if (!emitted.insert(relation).second)
                        continue;
                    if (retained.insert(selected->symbol).second)
                        out.identities.push_back(*selected);
                    auto origin = occurrence->ownership;
                    origin.origin.kind = std::string{"debug"};
                    origin.origin.location = source_location;
                    origin.origin.location->artifact = occurrence->location.artifact;
                    origin.origin.location->artifact->offset = address;
                    out.relations.push_back({.id = relation,
                                             .kind = std::string{"emits"},
                                             .ownership = std::move(origin),
                                             .from = selected->symbol,
                                             .to = native.id,
                                             .resolution = std::string{"resolved"},
                                             .detail = semantic::records::emission_detail{.file = file->first,
                                                                                          .line = frame.line,
                                                                                          .inline_depth = location.frames.size() - frame_index - 1},
                                             .from_context = source_context});
                }
            }
        }
    }

    inline auto records(const object& object, const std::string& project, const std::string& options, const std::string& stage, const std::string& module,
                        const std::string& artifact, const std::vector<semantic::input_object>& inputs) -> semantic::shard {
        semantic::shard out;
        semantic::recipe recipe{{project, stage, "native-artifact", "evaluate", semantic::unit_kind::artifact, artifact}, options};
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
        out.unit.direct_inputs = inputs;
        out.unit.coverage.available.push_back(
            {.contexts = {out.unit.context},
             .facts = {std::string{"symbols"}, std::string{"definitions"}, std::string{"references"}, std::string{"relations"}, std::string{"diagnostics"}}});
        auto owner = [&] { return semantic::ownership{out.unit.id, out.unit.context, {.provider = "native-artifact", .kind = std::string{"artifact"}}}; };
        auto linkage = [](const entry& symbol) { return symbol.binding == "Local" ? "local:" + symbol.object_file + ":" + symbol.name : symbol.name; };
        std::map<std::string, std::size_t> identities;
        for (const auto& symbol : object.symbols)
            ++identities[linkage(symbol)];
        for (std::size_t index = 0; index < object.symbols.size(); ++index) {
            const auto& s = object.symbols[index];
            const auto id = "native:" + std::to_string(index);
            auto key = linkage(s);
            if (identities.at(key) != 1) {
                out.unit.coverage.missing.push_back({.scope = {.modules = {module}, .contexts = {out.unit.context}, .facts = {std::string{"symbols"}}},
                                                     .reason = "native symbol has no unique linkage identity: " + s.name});
                continue;
            }
            semantic::native_identity identity{project, module, "native-artifact", "native", options + "/" + artifact + "/" + key};
            out.identities.push_back({identity, id});
            auto& row = out.symbols.emplace_back(semantic::records::symbol{.id = id,
                                                                           .ownership = owner(),
                                                                           .language = "native",
                                                                           .domain = "native",
                                                                           .module = module,
                                                                           .name = s.name,
                                                                           .qualified_name = module + "::" + s.name,
                                                                           .kind = s.kind,
                                                                           .visibility = s.exported ? "public" : "internal",
                                                                           .native_key = identity});
            if (s.imported)
                row.modifiers.push_back("import");
            if (s.exported)
                row.modifiers.push_back("export");
            if (s.binding == "Weak")
                row.modifiers.push_back("weak");
            row.extensions = {{"native.linkage_name", s.name}, {"native.format", object.format}, {"native.arch", object.arch},
                              {"native.section", s.section},   {"native.object", s.object_file}, {"native.size", static_cast<std::int64_t>(s.size)}};
            std::vector<registration> registered;
            for (const auto& method : object.registrations)
                if (method.function == s.name)
                    registered.push_back(method);
            if (!registered.empty())
                row.extensions.emplace("native.jni_registrations", rfl::to_generic(registered));
            out.occurrences.push_back({.id = out.unit.id + ":" + id,
                                       .ownership = owner(),
                                       .symbol = id,
                                       .location = {.artifact = semantic::artifact_location{artifact, module, s.name, s.address}},
                                       .roles = {std::string{s.imported ? "declaration" : "definition"}}});
        }
        std::map<std::string, std::vector<const semantic::records::symbol*>> symbols;
        for (const auto& symbol : out.symbols)
            symbols[symbol.name].push_back(&symbol);
        for (const auto& call : object.calls) {
            const auto from = symbols.find(call.from), to = symbols.find(call.to);
            if (from == symbols.end() || from->second.size() != 1)
                continue;
            const auto id = out.unit.id + ":call:" + call.from + ":" + std::to_string(call.instruction);
            semantic::records::reference reference{
                .id = id,
                .ownership = owner(),
                .from = from->second.front()->id,
                .location = {.artifact = semantic::artifact_location{call.artifact.empty() ? artifact : call.artifact, module, call.from, call.instruction}},
                .roles = {std::string{"call"}},
                .resolution = std::string{"unresolved"},
                .dispatch = std::string{"direct"}};
            if (to != symbols.end() && to->second.size() == 1) {
                reference.target = to->second.front()->id;
                reference.resolution = std::string{"resolved"};
                out.relations.push_back({.id = id,
                                         .kind = std::string{"calls"},
                                         .ownership = owner(),
                                         .from = *reference.from,
                                         .to = *reference.target,
                                         .reference = id,
                                         .resolution = std::string{"resolved"}});
            } else
                ++out.unit.coverage.unresolved_count;
            out.references.push_back(std::move(reference));
        }
        for (const auto& reason : object.unresolved) {
            ++out.unit.coverage.unresolved_count;
            out.unit.coverage.missing.push_back({.scope = {.modules = {module}, .facts = {std::string{"relations"}}}, .reason = reason});
        }
        return out;
    }
} // namespace native_artifact

export namespace native_artifact {
    class reader : public language::reader_adapter {
    public:
        using language::reader_adapter::reader_adapter;

    protected:
        auto do_open(rfl::Generic initialization) -> coroutine::task<> override {
            settings_ = rfl::from_generic<std::map<std::string, std::string>>(initialization).value();
            co_await reader_adapter::do_open(std::move(initialization));
        }
        auto do_update(language::installation input, const std::vector<std::string>&) -> coroutine::task<> override {
            installed_ = std::move(input);
            runtime_.execution = toolchain::select(runtime_, {{"reader", command_.argv.front()}}, command_.env.value_or(builds::environment{}));
            auto runtime = std::ranges::find(state_.inputs, semantic::input_kind::runtime, &semantic::input_object::kind);
            if (runtime == state_.inputs.end())
                state_.inputs.push_back({semantic::input_kind::runtime, "toolchain", runtime_.execution});
            else
                runtime->object = runtime_.execution;
            inputs_.clear();
            consumed_.clear();
            source_documents_.clear();
            const auto artifact = installed_.model.find(settings_.at("artifact"));
            if (artifact == installed_.model.end())
                throw std::invalid_argument("native artifact has no measured build output");
            inputs_.push_back({semantic::input_kind::artifact, artifact->first, artifact->second});
            inputs_.push_back({semantic::input_kind::runtime, "toolchain", runtime_.execution});
            if (const auto ir = settings_.find("ir"); ir != settings_.end()) {
                const auto input = installed_.model.find(ir->second);
                if (input == installed_.model.end())
                    throw std::invalid_argument("LLVM IR has no measured build output");
                inputs_.push_back({semantic::input_kind::artifact, input->first, input->second});
            }
            for (const auto& [port, shards] : installed_.source_facts)
                for (const auto& object : shards) {
                    const auto& shard = *object->data;
                    auto dependency = channels::consume(state_.project, port, shard, "definition_bodies", semantic::dependency_kind::body);
                    semantic::input_object input{semantic::input_kind::channel, port + "/" + shard.unit.id + "/definition_bodies", dependency.value};
                    inputs_.push_back(input);
                    state_.inputs.push_back(std::move(input));
                    consumed_.push_back(std::move(dependency));
                }
            definitions_ = channels::definition_bodies(installed_.source_facts, "definition_bodies");
            for (const auto& binding : definitions_)
                source_documents_.insert(binding.body.path);
            if (!source_documents_.empty()) {
                std::map<std::string, std::string> selected;
                for (const auto& file : source_documents_)
                    if (const auto found = installed_.source_files.find(file); found != installed_.source_files.end()) {
                        selected.insert(*found);
                        consumed_.push_back({.kind = semantic::dependency_kind::source, .project = state_.project, .key = file, .value = found->second});
                    }
                consumed_.push_back(
                    enumeration::read(state_.project, "source_documents", selected, {.files = {source_documents_.begin(), source_documents_.end()}}));
            }
            std::ranges::sort(inputs_, {}, [](const auto& input) { return std::tie(input.kind, input.name, input.object); });
            inputs_.erase(std::unique(inputs_.begin(), inputs_.end()), inputs_.end());
            std::ranges::sort(consumed_, {}, [](const auto& read) { return std::tuple{read.kind, read.key, read.producer, read.channel, read.value}; });
            consumed_.erase(std::unique(consumed_.begin(), consumed_.end()), consumed_.end());
            co_return;
        }
        auto do_units(const semantic::extent&) -> coroutine::task<std::vector<semantic::unit_key>> override {
            co_return std::vector<semantic::unit_key>{
                {state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::artifact, settings_.at("artifact")}};
        }
        auto do_evaluate(const language::evaluation_request& request) -> coroutine::task<language::evaluated_unit> override {
            // Source definitions to map debug locations onto are the only reason to read the line tables.
            auto argv = command_.argv;
            if (!installed_.source_facts.empty())
                argv.push_back("--lines");
            auto object = native_artifact::read(co_await run(std::move(argv)));
            if (const auto ir = settings_.find("ir"); ir != settings_.end())
                for (auto& call : object.calls)
                    call.artifact = ir->second;
            auto shard =
                native_artifact::records(object, state_.project, state_.options, state_.stage, settings_.at("module"), settings_.at("artifact"), inputs_);
            shard.unit.cacheable = !runtime_.execution.empty();
            shard.unit.observed_dependencies = consumed_;
            if (!installed_.source_facts.empty()) {
                auto selected = source_documents_;
                std::erase_if(selected, [&](const auto& path) { return !installed_.source_files.contains(path); });
                native_artifact::associate(shard, object.locations, definitions_, co_await loader_.read(std::move(selected)), root_);
            }
            shard.unit.coverage.requested = request.demand;
            language::evaluated_unit result;
            result.shards.push_back(std::move(shard));
            co_return result;
        }

    private:
        std::map<std::string, std::string> settings_;
        std::vector<semantic::input_object> inputs_;
        std::vector<semantic::dependency> consumed_;
        std::vector<channels::definition_body> definitions_;
        std::set<std::string> source_documents_;
    };
} // namespace native_artifact
