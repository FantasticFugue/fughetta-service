export module artifact;
import evaluation;
import coroutine;
import toolchain;
import durability;
import enumeration;
import constants;
import channels;
import std;
import reflection;
import content;
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
        struct options {
            std::string executable = "llvm-readobj";
            std::string symbolizer = "llvm-symbolizer";
            std::string llvm_dis = "llvm-dis";
            std::string objdump = "llvm-objdump";
            std::string module;
            builds::environment env;
        };
        static constexpr std::array<std::string_view, 5> facts{"symbols", "definitions", "references", "relations", "diagnostics"};
        static constexpr std::string_view update_granularity = "artifact";
        static auto tools(const options& o) -> std::vector<std::string> { return {o.executable, o.symbolizer, o.llvm_dis, o.objdump}; }
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable, o.symbolizer}); }
        static auto executes_project_code(const options& o) -> bool { return builds::in_project(tools(o)); }
        static void validate(const options& o) {
            builds::tools(tools(o));
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
            program result{.provider = "native-artifact",
                           .argv = {o.executable, "--elf-output-style=JSON", "--file-headers", "--symbols", "--dyn-symbols", "${tree}/" + artifact->path},
                           .env = o.env,
                           .commands = {{"symbolize", {o.symbolizer, "--output-style=JSON", "--inlines", "--no-demangle", "--obj=${tree}/" + artifact->path}},
                                        {"instructions", {o.objdump, "--disassemble", "--no-show-raw-insn", "${tree}/" + artifact->path}}}};
            if (ir) {
                settings.emplace("ir", ir->path);
                if (std::filesystem::path{ir->path}.extension() == ".bc")
                    result.commands.emplace("ir", std::vector<std::string>{o.llvm_dis, "${tree}/" + ir->path, "-o", "-"});
            }
            result.initialization = rfl::to_generic(settings);
            return result;
        }
    };
    static_assert(domain_reader<server<"native-artifact">>);
} // namespace language

export namespace native_artifact {
    struct entry {
        std::string name, kind, binding, section;
        std::uint64_t address = 0, size = 0;
        bool imported = false, exported = false;
        std::string object_file;
    };
    struct call {
        std::string from, to;
        std::uint64_t instruction = 0;
        std::string artifact;
    };
    struct registration {
        std::string klass, name, descriptor, function, table;
    };
    struct object {
        std::string format, arch;
        std::vector<entry> symbols;
        std::vector<call> calls;
        std::vector<registration> registrations;
        std::vector<std::string> unresolved;
    };
    inline auto read(std::string_view text) -> object {
        struct named {
            std::string Name;
            std::uint64_t Value = 0;
        };
        struct other {
            std::uint64_t Value = 0;
        };
        struct symbol {
            named Name;
            std::uint64_t Value = 0, Size = 0;
            named Binding, Type;
            other Other;
            named Section;
        };
        struct row {
            symbol Symbol;
        };
        struct summary {
            std::string Format, Arch;
        };
        struct header {
            std::string Type;
        };
        struct file {
            summary FileSummary;
            header ElfHeader;
            std::vector<row> Symbols, DynamicSymbols;
        };
        auto decoded = rfl::json::read<std::vector<file>>(text);
        if (!decoded)
            throw std::invalid_argument("cannot decode LLVM object report: " + std::string{decoded.error().what()});
        if (decoded->size() != 1)
            throw std::invalid_argument("native artifact reader requires one LLVM object report");
        const auto& input = decoded->front();
        if (!input.FileSummary.Format.starts_with("elf"))
            throw std::invalid_argument("native artifact report must describe an ELF object");
        if (!input.ElfHeader.Type.ends_with("(0x1)") && !input.ElfHeader.Type.ends_with("(0x2)") && !input.ElfHeader.Type.ends_with("(0x3)"))
            throw std::invalid_argument("native artifact reader requires a relocatable object, linked executable or shared object");
        object out{input.FileSummary.Format, input.FileSummary.Arch};
        std::map<std::tuple<std::string, std::string, std::uint64_t>, std::size_t> positions;
        auto append = [&](const auto& rows, bool dynamic) {
            std::string object_file;
            for (const auto& row : rows) {
                const auto& s = row.Symbol;
                if (s.Type.Name == "File") {
                    object_file = s.Name.Name;
                    continue;
                }
                if (s.Name.Name.empty() || s.Type.Name == "Section")
                    continue;
                const bool imported = s.Section.Value == 0;
                const bool visible = (s.Other.Value & 3) == 0 || (s.Other.Value & 3) == 3;
                const bool exported = dynamic && !imported && visible && (s.Binding.Name == "Global" || s.Binding.Name == "Weak");
                auto [same, inserted] = positions.emplace(std::tuple{s.Name.Name, s.Section.Name, s.Value}, out.symbols.size());
                if (!inserted) {
                    out.symbols[same->second].exported |= exported;
                    continue;
                }
                out.symbols.push_back({s.Name.Name, s.Type.Name == "Function" || s.Type.Name == "GNU_IFunc" ? "function" : "variable", s.Binding.Name,
                                       s.Section.Name, s.Value, s.Size, imported, exported, object_file});
            }
        };
        append(input.Symbols, false);
        append(input.DynamicSymbols, true);
        return out;
    }

    // LLVM's textual representation supplies target-independent call and
    // registration facts. It is an explicit build output, never reconstructed
    // from source text or from the host's loaded libraries.
    namespace ir {
        inline auto unquote(std::string_view value) -> std::string {
            if (value.starts_with('@') || value.starts_with('%'))
                value.remove_prefix(1);
            if (value.starts_with("c\""))
                value.remove_prefix(1);
            if (!value.starts_with('"'))
                return std::string{value};
            value.remove_prefix(1);
            value.remove_suffix(1);
            std::string result;
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\' && i + 2 < value.size()) {
                    unsigned byte = 0;
                    const auto parsed = std::from_chars(value.data() + i + 1, value.data() + i + 3, byte, 16);
                    if (parsed.ec == std::errc{}) {
                        result += static_cast<char>(byte);
                        i += 2;
                        continue;
                    }
                }
                result += value[i];
            }
            return result;
        }
        inline auto tokens(std::string_view line) -> std::vector<std::string> {
            std::vector<std::string> result;
            for (std::size_t i = 0; i < line.size();) {
                if (std::isspace(static_cast<unsigned char>(line[i]))) {
                    ++i;
                    continue;
                }
                if (line[i] == ';')
                    break;
                const auto first = i;
                if (line[i] == '@' || line[i] == '%' || (line[i] == 'c' && i + 1 < line.size() && line[i + 1] == '"'))
                    ++i;
                if (i < line.size() && line[i] == '"') {
                    ++i;
                    while (i < line.size() && line[i] != '"') {
                        if (line[i] == '\\' && i + 2 < line.size())
                            i += 3;
                        else
                            ++i;
                    }
                    if (i < line.size())
                        ++i;
                } else if (i != first || std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_' || line[i] == '.') {
                    while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || std::string_view{"_.$-"}.contains(line[i])))
                        ++i;
                } else
                    ++i;
                result.emplace_back(line.substr(first, i - first));
            }
            return result;
        }
        inline auto reference(const std::vector<std::string>& values) -> std::string {
            for (auto it = values.rbegin(); it != values.rend(); ++it)
                if (it->starts_with('@') || it->starts_with('%'))
                    return *it;
            return {};
        }
        inline auto arguments(const std::vector<std::string>& row, std::size_t open) -> std::vector<std::vector<std::string>> {
            std::vector<std::vector<std::string>> result(1);
            unsigned depth = 0;
            for (auto i = open + 1; i < row.size(); ++i) {
                if (row[i] == ")" && !depth)
                    break;
                if (row[i] == "," && !depth)
                    result.emplace_back();
                else {
                    if (row[i] == "(" || row[i] == "[" || row[i] == "{")
                        ++depth;
                    if (row[i] == ")" || row[i] == "]" || row[i] == "}")
                        --depth;
                    result.back().push_back(row[i]);
                }
            }
            return result;
        }
        inline void append(object& out, std::string_view text, std::string_view artifact = {}) {
            std::vector<std::vector<std::string>> lines;
            for (std::size_t first = 0; first < text.size();) {
                auto last = text.find('\n', first);
                if (last == std::string_view::npos)
                    last = text.size();
                lines.push_back(tokens(text.substr(first, last - first)));
                first = last + 1;
            }
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].empty() || !lines[i].front().starts_with('@'))
                    continue;
                int depth = 0;
                auto count = [&](const auto& row) {
                    for (const auto& token : row) {
                        if (token == "[" || token == "{" || token == "(")
                            ++depth;
                        if (token == "]" || token == "}" || token == ")")
                            --depth;
                    }
                };
                count(lines[i]);
                for (auto next = i + 1; depth > 0 && next < lines.size(); ++next) {
                    count(lines[next]);
                    lines[i].insert(lines[i].end(), lines[next].begin(), lines[next].end());
                    lines[next].clear();
                }
            }
            std::map<std::string, std::string> strings;
            std::map<std::string, std::vector<registration>> tables;
            for (const auto& row : lines) {
                if (row.size() < 3 || !row.front().starts_with('@') || row[1] != "=")
                    continue;
                for (const auto& token : row)
                    if (token.starts_with("c\"")) {
                        auto value = unquote(token);
                        if (!value.empty() && value.back() == '\0')
                            value.pop_back();
                        strings.emplace(row.front(), std::move(value));
                    }
            }
            // A JNINativeMethod initializer has three pointer constants. Only
            // triples whose first two values are actual string globals qualify.
            for (const auto& row : lines) {
                if (row.size() < 3 || !row.front().starts_with('@') || row[1] != "=")
                    continue;
                for (std::size_t i = 2; i < row.size(); ++i) {
                    if (row[i] != "{")
                        continue;
                    std::vector<std::string> refs;
                    auto end = i + 1;
                    for (; end < row.size() && row[end] != "}" && row[end] != "{"; ++end)
                        if (row[end].starts_with('@'))
                            refs.push_back(row[end]);
                    if (end == row.size() || row[end] != "}" || refs.size() != 3 || !strings.contains(refs[0]) || !strings.contains(refs[1]))
                        continue;
                    const auto& descriptor = strings.at(refs[1]);
                    if (!descriptor.starts_with('(') || !descriptor.contains(')'))
                        continue;
                    tables[row.front()].push_back({{}, strings.at(refs[0]), descriptor, unquote(refs[2]), unquote(row.front())});
                }
            }
            std::string function;
            std::map<std::string, std::string> values, memory;
            std::map<std::string, unsigned> slots;
            std::uint64_t instruction = 0;
            for (const auto& row : lines) {
                if (row.empty())
                    continue;
                if (row.front() == "define") {
                    function.clear();
                    values.clear();
                    memory.clear();
                    slots.clear();
                    instruction = 0;
                    for (const auto& token : row)
                        if (token.starts_with('@')) {
                            function = unquote(token);
                            break;
                        }
                    continue;
                }
                if (row.front() == "}") {
                    function.clear();
                    continue;
                }
                if (function.empty())
                    continue;
                ++instruction;
                const auto lhs = row.size() > 2 && row[1] == "=" ? row.front() : std::string{};
                const auto operation = lhs.empty() ? 0u : 2u;
                if (operation >= row.size())
                    continue;
                if (row[operation] == "store") {
                    // store has no parentheses; split its two typed operands.
                    const auto comma = std::ranges::find(row, ",");
                    if (comma != row.end()) {
                        std::vector<std::string> from(row.begin() + operation + 1, comma), to(comma + 1, row.end());
                        const auto source = reference(from), target = reference(to);
                        if (values.contains(source))
                            memory[target] = values.at(source);
                        else
                            memory.erase(target);
                    }
                } else if (row[operation] == "load") {
                    const auto source = reference(row);
                    if (memory.contains(source))
                        values[lhs] = memory.at(source);
                    if (slots.contains(source))
                        slots[lhs] = slots.at(source);
                } else if (row[operation] == "getelementptr") {
                    // JNI's published function-table indexes: FindClass and
                    // RegisterNatives. This also handles the C interface where
                    // clang emits an indirect call instead of a C++ wrapper.
                    for (std::size_t i = operation + 1; i + 1 < row.size(); ++i)
                        if ((row[i] == "i32" || row[i] == "i64") && (row[i + 1] == "6" || row[i + 1] == "215"))
                            slots[lhs] = row[i + 1] == "6" ? 6 : 215;
                }
                auto call_at = std::ranges::find(row, "call");
                if (call_at == row.end())
                    call_at = std::ranges::find(row, "invoke");
                if (call_at == row.end())
                    continue;
                auto at = static_cast<std::size_t>(call_at - row.begin()) + 1;
                for (; at + 1 < row.size(); ++at)
                    if ((row[at].starts_with('@') || row[at].starts_with('%')) && row[at + 1] == "(")
                        break;
                if (at + 1 >= row.size())
                    continue;
                const auto callee = row[at];
                const auto name = unquote(callee);
                const auto args = arguments(row, at + 1);
                const bool find_class = name.contains("FindClass") || (slots.contains(callee) && slots.at(callee) == 6);
                const bool register_natives = name.contains("RegisterNatives") || (slots.contains(callee) && slots.at(callee) == 215);
                if (find_class && !args.empty()) {
                    const auto path = reference(args.back());
                    if (strings.contains(path))
                        values[lhs] = strings.at(path);
                }
                if (register_natives && args.size() >= 3) {
                    const auto table = reference(args[args.size() - 2]);
                    const auto klass = reference(args[args.size() - 3]);
                    if (tables.contains(table)) {
                        if (!values.contains(klass))
                            out.unresolved.push_back("RegisterNatives class could not be resolved for " + unquote(table));
                        else {
                            std::uint64_t count = 0;
                            const auto operand = args.back().empty() ? std::string{} : args.back().back();
                            const auto parsed = std::from_chars(operand.data(), operand.data() + operand.size(), count);
                            if (parsed.ec != std::errc{} || count > tables.at(table).size())
                                out.unresolved.push_back("RegisterNatives method count could not be resolved for " + unquote(table));
                            else
                                for (std::size_t i = 0; i < count; ++i) {
                                    auto method = tables.at(table)[i];
                                    method.klass = values.at(klass);
                                    out.registrations.push_back(std::move(method));
                                }
                        }
                    }
                }
                if (callee.starts_with('@') && !name.starts_with("llvm."))
                    out.calls.push_back({function, name, instruction, std::string{artifact}});
            }
        }
    } // namespace ir

    struct frame {
        std::string FileName, FunctionName;
        std::uint64_t Line = 0, Column = 0;
    };
    struct location {
        std::string Address;
        std::vector<frame> Symbol;
    };
    inline auto instruction_addresses(std::string_view text) -> std::vector<std::string> {
        std::set<std::uint64_t> addresses;
        while (!text.empty()) {
            const auto end = text.find('\n');
            auto line = text.substr(0, end);
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string_view::npos) {
                line.remove_prefix(first);
                const auto colon = line.find(':');
                if (colon != std::string_view::npos) {
                    std::uint64_t address;
                    const auto parsed = std::from_chars(line.data(), line.data() + colon, address, 16);
                    if (parsed.ec == std::errc{} && parsed.ptr == line.data() + colon)
                        addresses.insert(address);
                }
            }
            if (end == std::string_view::npos)
                break;
            text.remove_prefix(end + 1);
        }
        std::vector<std::string> out;
        for (auto address : addresses)
            out.push_back(std::format("0x{:x}", address));
        return out;
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
            std::uint64_t address = 0;
            auto value = std::string_view{location.Address};
            if (value.starts_with("0x"))
                value.remove_prefix(2);
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), address, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || location.Symbol.empty())
                continue;
            auto interval = addresses.upper_bound(address);
            if (interval == addresses.begin() || !(--interval)->second)
                continue;
            const auto [symbol, occurrence] = *interval->second;
            for (std::size_t frame_index = 0; frame_index < location.Symbol.size(); ++frame_index) {
                const auto& frame = location.Symbol[frame_index];
                auto path = std::filesystem::path{frame.FileName};
                if (path.is_absolute())
                    path = path.lexically_relative(std::filesystem::absolute(root));
                const auto file = documents.find(path.lexically_normal().generic_string());
                if (file == documents.end() || !frame.Line)
                    continue;
                const auto& text = file->second;
                const auto& offsets = lines.at(file->first);
                if (frame.Line > offsets.size() || !seen.emplace(symbol->id, file->first, frame.Line, frame.Column).second)
                    continue;
                const auto [line_begin, line_scalar] = offsets[frame.Line - 1];
                auto begin = line_begin;
                const auto newline = text.find('\n', begin);
                auto end = newline == std::string::npos ? text.size() : newline;
                if (frame.Column) {
                    begin += frame.Column - 1;
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
                        if (!frame.Column && selected && (selected->symbol != source.identity.symbol || source_context != source.context))
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
                                                                                          .line = frame.Line,
                                                                                          .inline_depth = location.Symbol.size() - frame_index - 1},
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
            std::map<std::string, std::string> executables{{"reader", command_.argv.front()}};
            if (commands_.contains("ir"))
                executables.emplace("ir", commands_.at("ir").front());
            if (!installed_.source_facts.empty())
                executables.emplace("symbolize", commands_.at("symbolize").front());
            runtime_.execution = toolchain::select(runtime_, executables, command_.env.value_or(builds::environment{}));
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
            auto object = native_artifact::read(co_await run(command_.argv));
            if (const auto ir = settings_.find("ir"); ir != settings_.end()) {
                std::string text;
                if (commands_.contains("ir"))
                    text = co_await run(commands_.at("ir"));
                else {
                    std::ifstream input{root_ / ir->second, std::ios::binary};
                    if (!input)
                        throw std::runtime_error("cannot open LLVM IR build output");
                    text.assign(std::istreambuf_iterator<char>{input}, {});
                    if (text.size() > constants::semantic_object_bytes)
                        throw std::length_error("LLVM IR exceeds semantic object budget");
                }
                native_artifact::ir::append(object, text, ir->second);
            }
            auto shard =
                native_artifact::records(object, state_.project, state_.options, state_.stage, settings_.at("module"), settings_.at("artifact"), inputs_);
            shard.unit.cacheable = !runtime_.execution.empty();
            shard.unit.observed_dependencies = consumed_;
            if (!installed_.source_facts.empty()) {
                auto selected = source_documents_;
                std::erase_if(selected, [&](const auto& path) { return !installed_.source_files.contains(path); });
                auto documents = co_await loader_.read(std::move(selected));
                auto addresses = instruction_addresses(co_await run(commands_.at("instructions")));
                std::map<std::uint64_t, std::size_t> address_counts;
                for (const auto& symbol : object.symbols)
                    if (symbol.kind == "function" && !symbol.imported)
                        ++address_counts[symbol.address];
                for (const auto& symbol : object.symbols)
                    // Section-relative ET_REL addresses (and linked aliases)
                    // need a unique function before an address can name an
                    // unambiguous debug endpoint.
                    if (symbol.kind == "function" && !symbol.imported && address_counts.at(symbol.address) == 1)
                        addresses.push_back(std::format("0x{:x}", symbol.address));
                std::ranges::sort(addresses);
                auto repeated = std::ranges::unique(addresses);
                addresses.erase(repeated.begin(), repeated.end());
                if (!addresses.empty()) {
                    struct response_file {
                        std::filesystem::path path;
                        ~response_file() {
                            std::error_code ignored;
                            durability::remove_tree(path, ignored);
                        }
                    } response{root_.parent_path() / ("symbolize-" + state_.id + ".rsp")};
                    {
                        std::ofstream output{response.path};
                        for (const auto& address : addresses)
                            output << address << '\n';
                        output.close();
                        if (!output)
                            throw std::runtime_error("cannot write symbolizer address input");
                    }
                    auto argv = commands_.at("symbolize");
                    argv.push_back("@" + std::filesystem::absolute(response.path).string());
                    auto locations = rfl::json::read<std::vector<native_artifact::location>>(co_await run(std::move(argv))).value();
                    native_artifact::associate(shard, locations, definitions_, documents, root_);
                }
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
