// fughetta-llvm reads one native artifact in process with LLVM's libraries and prints one JSON object
// with the shape of llvm::object: the symbol tables of an ELF, Mach-O or COFF file, the
// inline frames of every line-table address inside its functions (DWARF, or the PDB of a COFF image),
// and the direct calls and JNI registrations of an optional LLVM IR or bitcode file.
//
//   fughetta-llvm <artifact> [--ir <file>] [--lines]

#include <llvm/BinaryFormat/COFF.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/BinaryFormat/MachO.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/DebugInfo/DWARF/DWARFDie.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/DebugInfo/CodeView/SymbolDeserializer.h>
#include <llvm/DebugInfo/CodeView/SymbolRecord.h>
#include <llvm/DebugInfo/MSF/MappedBlockStream.h>
#include <llvm/DebugInfo/PDB/IPDBSession.h>
#include <llvm/DebugInfo/PDB/Native/DbiModuleList.h>
#include <llvm/DebugInfo/PDB/Native/DbiStream.h>
#include <llvm/DebugInfo/PDB/Native/ModuleDebugStream.h>
#include <llvm/DebugInfo/PDB/Native/NativeSession.h>
#include <llvm/DebugInfo/PDB/Native/PDBFile.h>
#include <llvm/DebugInfo/PDB/Native/PublicsStream.h>
#include <llvm/DebugInfo/PDB/Native/SymbolStream.h>
#include <llvm/DebugInfo/PDB/PDBContext.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/MachO.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
    using llvm::object::SectionedAddress;
    using llvm::object::SymbolRef;

    struct entry {
        std::string name, kind, binding, section;
        std::uint64_t address = 0, size = 0;
        bool imported = false, exported = false;
        std::string object_file;
        std::uint64_t section_index = SectionedAddress::UndefSection;
    };
    struct call {
        std::string from, to;
        std::uint64_t instruction = 0;
    };
    struct registration {
        std::string klass, name, descriptor, function, table;
    };
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

    [[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
    template <class T> auto checked(llvm::Expected<T> value, const std::string& what) -> T {
        if (!value)
            fail(what + ": " + llvm::toString(value.takeError()));
        return std::forward<T>(*value);
    }
    void checked(llvm::Error error, const std::string& what) {
        if (error)
            fail(what + ": " + llvm::toString(std::move(error)));
    }

    // Symbols with the same name, section and address are one symbol: the static and dynamic tables of
    // a linked ELF file list an exported function twice.
    class table {
    public:
        explicit table(object& out) : out_(out) {}
        void add(entry value) {
            auto [same, inserted] = positions_.emplace(std::tuple{value.name, value.section, value.address}, out_.symbols.size());
            if (inserted)
                out_.symbols.push_back(std::move(value));
            else
                out_.symbols[same->second].exported |= value.exported;
        }

    private:
        object& out_;
        std::map<std::tuple<std::string, std::string, std::uint64_t>, std::size_t> positions_;
    };

    auto section_of(const llvm::object::ObjectFile& file, const SymbolRef& symbol) -> std::pair<std::string, std::uint64_t> {
        auto section = checked(symbol.getSection(), "cannot read a symbol's section");
        if (section == file.section_end())
            return {{}, SectionedAddress::UndefSection};
        return {checked(section->getName(), "cannot read a section name").str(), section->getIndex()};
    }

    auto section_at(const llvm::object::ObjectFile& file, std::uint64_t address) -> std::pair<std::string, std::uint64_t> {
        for (const auto& section : file.sections())
            if (section.isText() || section.isData() || section.isBSS())
                if (address >= section.getAddress() && address - section.getAddress() < section.getSize())
                    return {checked(section.getName(), "cannot read a section name").str(), section.getIndex()};
        return {{}, SectionedAddress::UndefSection};
    }

    auto kind_of(SymbolRef::Type type) -> std::string { return type == SymbolRef::ST_Function ? "function" : "variable"; }

    void elf(const llvm::object::ELFObjectFileBase& file, object& out) {
        const auto type = file.getEType();
        if (type != llvm::ELF::ET_REL && type != llvm::ELF::ET_EXEC && type != llvm::ELF::ET_DYN)
            fail("native artifact reader requires a relocatable object, linked executable or shared object");
        table symbols{out};
        auto append = [&](auto range, bool dynamic) {
            std::string object_file;
            for (const llvm::object::ELFSymbolRef symbol : range) {
                const auto kind = checked(symbol.getType(), "cannot read a symbol type");
                auto name = checked(symbol.getName(), "cannot read a symbol name").str();
                if (kind == SymbolRef::ST_File) {
                    object_file = name;
                    continue;
                }
                const auto flags = checked(symbol.getFlags(), "cannot read symbol flags");
                // Format specific symbols include the $x and $d mapping symbols of ARM, AArch64 and RISC-V.
                if (name.empty() || kind == SymbolRef::ST_Debug || (flags & SymbolRef::SF_FormatSpecific))
                    continue;
                // A linker writes versioned names such as `f@GLIBC_2.2.5` into the static table; the dynamic one keeps
                // versions apart from names.
                name = name.substr(0, name.find('@'));
                const auto binding = symbol.getBinding();
                const bool imported = flags & SymbolRef::SF_Undefined;
                const auto visibility = symbol.getOther() & 3;
                const bool visible = visibility == llvm::ELF::STV_DEFAULT || visibility == llvm::ELF::STV_PROTECTED;
                const bool global = binding == llvm::ELF::STB_GLOBAL || binding == llvm::ELF::STB_GNU_UNIQUE;
                const bool weak = binding == llvm::ELF::STB_WEAK;
                auto [section, index] = section_of(file, symbol);
                symbols.add({.name = std::move(name),
                             .kind = symbol.getELFType() == llvm::ELF::STT_GNU_IFUNC ? "function" : kind_of(kind),
                             .binding = weak     ? "Weak"
                                        : global ? "Global"
                                                 : "Local",
                             .section = std::move(section),
                             .address = checked(symbol.getAddress(), "cannot read a symbol address"),
                             .size = symbol.getSize(),
                             .imported = imported,
                             .exported = dynamic && !imported && visible && (global || weak),
                             .object_file = object_file,
                             .section_index = index});
            }
        };
        append(file.symbols(), false);
        append(file.getDynamicSymbolIterators(), true);
    }

    // Mach-O prefixes every C name with an underscore; the linkage name the other domains bind to is the C name.
    void macho(const llvm::object::MachOObjectFile& file, object& out) {
        const bool linked = file.getHeader().filetype != llvm::MachO::MH_OBJECT;
        table symbols{out};
        for (const auto& [symbol, size] : llvm::object::computeSymbolSizes(file)) {
            const auto flags = checked(symbol.getFlags(), "cannot read symbol flags");
            const auto kind = checked(symbol.getType(), "cannot read a symbol type");
            auto name = checked(symbol.getName(), "cannot read a symbol name");
            const bool global = flags & SymbolRef::SF_Global;
            // Local names starting with l or L are assembler temporaries such as section start labels.
            if ((flags & SymbolRef::SF_FormatSpecific) || kind == SymbolRef::ST_Debug || kind == SymbolRef::ST_File || name.empty() ||
                (!global && (name.starts_with("l") || name.starts_with("L"))))
                continue;
            name.consume_front("_");
            const bool imported = flags & SymbolRef::SF_Undefined;
            auto [section, index] = section_of(file, symbol);
            symbols.add({.name = name.str(),
                         .kind = kind_of(kind),
                         .binding = (flags & SymbolRef::SF_Weak) ? "Weak"
                                    : global                     ? "Global"
                                                                 : "Local",
                         .section = std::move(section),
                         .address = checked(symbol.getAddress(), "cannot read a symbol address"),
                         .size = size,
                         .imported = imported,
                         .exported = linked && !imported && global && !(flags & SymbolRef::SF_Hidden),
                         .section_index = index});
        }
    }

    // A COFF object has a symbol table. A linked image usually has none: its exports and imports are in
    // its directories, and its other symbols in the PDB the debug directory names.
    auto pdb_path(const llvm::object::COFFObjectFile& file, llvm::StringRef artifact) -> std::optional<std::string> {
        const llvm::codeview::DebugInfo* info = nullptr;
        llvm::StringRef named;
        checked(file.getDebugPDBInfo(info, named), "cannot read the debug directory");
        std::vector<std::string> candidates;
        if (!named.empty()) {
            candidates.push_back(named.str());
            candidates.push_back((llvm::sys::path::parent_path(artifact) + "/" + llvm::sys::path::filename(named, llvm::sys::path::Style::windows)).str());
        }
        llvm::SmallString<256> beside{artifact};
        llvm::sys::path::replace_extension(beside, ".pdb");
        candidates.push_back(beside.str().str());
        for (const auto& candidate : candidates)
            if (llvm::sys::fs::is_regular_file(candidate))
                return candidate;
        return std::nullopt;
    }

    auto coff(const llvm::object::COFFObjectFile& file, llvm::StringRef artifact, object& out) -> std::unique_ptr<llvm::pdb::IPDBSession> {
        table symbols{out};
        if (!file.getPE32Header() && !file.getPE32PlusHeader()) {
            std::string object_file;
            for (const auto& [symbol, size] : llvm::object::computeSymbolSizes(file)) {
                const auto kind = checked(symbol.getType(), "cannot read a symbol type");
                auto name = checked(symbol.getName(), "cannot read a symbol name").str();
                if (kind == SymbolRef::ST_File) {
                    object_file = name;
                    continue;
                }
                // Absolute symbols such as @feat.00 are markers for the linker, not code or data.
                if (name.empty() || kind == SymbolRef::ST_Debug || file.getCOFFSymbol(symbol).getSectionNumber() == llvm::COFF::IMAGE_SYM_ABSOLUTE)
                    continue;
                const auto flags = checked(symbol.getFlags(), "cannot read symbol flags");
                auto [section, index] = section_of(file, symbol);
                symbols.add({.name = std::move(name),
                             .kind = kind_of(kind),
                             .binding = (flags & SymbolRef::SF_Weak)     ? "Weak"
                                        : (flags & SymbolRef::SF_Global) ? "Global"
                                                                         : "Local",
                             .section = std::move(section),
                             .address = checked(symbol.getAddress(), "cannot read a symbol address"),
                             .size = size,
                             .imported = static_cast<bool>(flags & SymbolRef::SF_Undefined),
                             .object_file = object_file,
                             .section_index = index});
            }
            return nullptr;
        }
        const auto base = file.getImageBase();
        auto at = [&](std::string name, std::uint64_t address, std::string kind, std::string binding, std::uint64_t size, bool exported,
                      std::string object_file = {}) {
            auto [section, index] = section_at(file, address);
            symbols.add({.name = std::move(name),
                         .kind = std::move(kind),
                         .binding = std::move(binding),
                         .section = std::move(section),
                         .address = address,
                         .size = size,
                         .exported = exported,
                         .object_file = std::move(object_file),
                         .section_index = index});
        };
        std::unique_ptr<llvm::pdb::IPDBSession> session;
        std::map<std::uint64_t, std::uint64_t> sizes;
        if (const auto path = pdb_path(file, artifact)) {
            checked(llvm::pdb::NativeSession::createFromPdbPath(*path, session), "cannot load " + *path);
            // The native session enumerates neither functions nor publics, so read the streams themselves.
            auto& native = static_cast<llvm::pdb::NativeSession&>(*session);
            auto& pdb = native.getPDBFile();
            auto address_of = [&](std::uint32_t segment, std::uint32_t offset) -> std::optional<std::uint64_t> {
                if (segment == 0 || segment > file.getNumberOfSections())
                    return std::nullopt;
                auto section = file.getSection(segment);
                if (!section) {
                    llvm::consumeError(section.takeError());
                    return std::nullopt;
                }
                if (offset >= (*section)->VirtualSize)
                    return std::nullopt;
                return base + static_cast<std::uint64_t>((*section)->VirtualAddress) + offset;
            };
            struct procedure {
                std::uint64_t address = 0;
                std::string name, object_file;
                std::uint64_t size = 0;
                bool global = false;
            };
            // Identical code folding gives several procedures one address.
            std::vector<procedure> procedures;
            const auto& modules = checked(pdb.getPDBDbiStream(), "cannot read the PDB DBI stream").modules();
            for (std::uint32_t i = 0; i < modules.getModuleCount(); ++i) {
                const auto descriptor = modules.getModuleDescriptor(i);
                if (descriptor.getModuleStreamIndex() == llvm::pdb::kInvalidStreamIndex)
                    continue;
                llvm::pdb::ModuleDebugStreamRef module{descriptor,
                                                       checked(pdb.safelyCreateIndexedStream(descriptor.getModuleStreamIndex()), "cannot read a PDB module")};
                checked(module.reload(), "cannot read a PDB module");
                for (const auto& record : module.symbols(nullptr)) {
                    const auto kind = record.kind();
                    if (kind != llvm::codeview::S_GPROC32 && kind != llvm::codeview::S_LPROC32 && kind != llvm::codeview::S_GPROC32_ID &&
                        kind != llvm::codeview::S_LPROC32_ID)
                        continue;
                    const auto proc =
                        checked(llvm::codeview::SymbolDeserializer::deserializeAs<llvm::codeview::ProcSym>(record), "cannot read a PDB procedure");
                    if (const auto address = address_of(proc.Segment, proc.CodeOffset))
                        procedures.push_back({*address, proc.Name.str(), descriptor.getObjFileName().str(), proc.CodeSize,
                                              kind == llvm::codeview::S_GPROC32 || kind == llvm::codeview::S_GPROC32_ID});
                }
            }
            for (const auto& procedure : procedures)
                sizes[procedure.address] = procedure.size;
            std::set<std::uint64_t> named;
            const auto& records = checked(pdb.getPDBSymbolStream(), "cannot read the PDB symbol stream");
            for (const auto offset : checked(pdb.getPDBPublicsStream(), "cannot read the PDB publics stream").getPublicsTable()) {
                const auto symbol = checked(llvm::codeview::SymbolDeserializer::deserializeAs<llvm::codeview::PublicSym32>(records.readRecord(offset)),
                                            "cannot read a PDB public");
                const auto address = address_of(symbol.Segment, symbol.Offset);
                if (!address)
                    continue;
                const bool code =
                    (symbol.Flags & (llvm::codeview::PublicSymFlags::Code | llvm::codeview::PublicSymFlags::Function)) != llvm::codeview::PublicSymFlags::None;
                const auto size = sizes.find(*address);
                at(symbol.Name.str(), *address, code ? "function" : "variable", "Global", size == sizes.end() ? 0 : size->second, false);
                named.insert(*address);
            }
            // Static functions have no public symbol.
            for (const auto& procedure : procedures)
                if (!named.contains(procedure.address))
                    at(procedure.name, procedure.address, "function", procedure.global ? "Global" : "Local", procedure.size, false,
                       procedure.global ? std::string{} : procedure.object_file);
        }
        for (const auto& exported : file.export_directories()) {
            bool forwarder = false;
            checked(exported.isForwarder(forwarder), "cannot read an export");
            llvm::StringRef name;
            checked(exported.getSymbolName(name), "cannot read an export name");
            if (forwarder || name.empty())
                continue;
            std::uint32_t rva = 0;
            checked(exported.getExportRVA(rva), "cannot read an export address");
            const auto [section, _] = section_at(file, base + rva);
            const auto size = sizes.find(base + rva);
            at(name.str(), base + rva, section == ".text" ? "function" : "variable", "Global", size == sizes.end() ? 0 : size->second, true);
        }
        for (const auto& directory : file.import_directories())
            for (const auto& imported : directory.imported_symbols()) {
                bool ordinal = false;
                checked(imported.isOrdinal(ordinal), "cannot read an import ordinal");
                if (ordinal)
                    continue;
                llvm::StringRef name;
                checked(imported.getSymbolName(name), "cannot read an import name");
                if (!name.empty())
                    symbols.add({.name = name.str(), .kind = "function", .binding = "Global", .imported = true});
            }
        return session;
    }

    // Debug information beside a linked Mach-O file.
    auto dsym(llvm::StringRef artifact) -> std::optional<llvm::object::OwningBinary<llvm::object::ObjectFile>> {
        const auto path = (artifact + ".dSYM/Contents/Resources/DWARF/" + llvm::sys::path::filename(artifact)).str();
        if (!llvm::sys::fs::is_regular_file(path))
            return std::nullopt;
        return checked(llvm::object::ObjectFile::createObjectFile(path), "cannot open " + path);
    }

    auto unsigned_attribute(llvm::DWARFDie die, llvm::dwarf::Attribute attribute) -> std::optional<std::uint64_t> {
        if (auto value = die.find(attribute))
            return value->getAsUnsignedConstant();
        return {};
    }

    auto named_child(llvm::DWARFDie parent, llvm::dwarf::Tag tag, llvm::StringRef name) -> llvm::DWARFDie {
        for (auto child : parent.children())
            if (child.getTag() == tag && llvm::StringRef(child.getShortName()) == name)
                return child;
        return {};
    }

    auto underlying_type(llvm::DWARFDie die) -> llvm::DWARFDie {
        using namespace llvm::dwarf;
        auto type = die.getAttributeValueAsReferencedDie(DW_AT_type);
        while (type && (type.getTag() == DW_TAG_typedef || type.getTag() == DW_TAG_const_type || type.getTag() == DW_TAG_volatile_type))
            type = type.getAttributeValueAsReferencedDie(DW_AT_type);
        return type;
    }

    auto in_detail_namespace(llvm::DWARFDie die, llvm::StringRef library) -> bool {
        auto detail = die.getParent();
        auto outer = detail.getParent();
        return detail.getTag() == llvm::dwarf::DW_TAG_namespace && llvm::StringRef(detail.getShortName()) == "detail" &&
               outer.getTag() == llvm::dwarf::DW_TAG_namespace && llvm::StringRef(outer.getShortName()) == library;
    }

    using layout = std::map<std::string, std::uint64_t>;

    auto structure_layout(llvm::DWARFDie die, std::initializer_list<llvm::StringRef> fields) -> std::optional<layout> {
        using namespace llvm::dwarf;
        auto size = unsigned_attribute(die, DW_AT_byte_size);
        if (!size || !*size)
            return {};
        layout result{{"size", *size}};
        for (auto name : fields) {
            auto offset = unsigned_attribute(named_child(die, DW_TAG_member, name), DW_AT_data_member_location);
            if (!offset || *offset >= *size)
                return {};
            result.emplace(name.str(), *offset);
        }
        return result;
    }

    auto bit_field(llvm::DWARFDie die, llvm::StringRef name, std::uint64_t width) -> std::optional<std::uint64_t> {
        using namespace llvm::dwarf;
        const auto field = named_child(die, DW_TAG_member, name);
        if (unsigned_attribute(field, DW_AT_bit_size) != width)
            return {};
        return unsigned_attribute(field, DW_AT_data_bit_offset);
    }

    using placed_member = std::pair<std::uint64_t, llvm::DWARFDie>;
    auto member_position(llvm::DWARFDie type, llvm::StringRef name) -> std::optional<placed_member> {
        using namespace llvm::dwarf;
        if (auto member = named_child(type, DW_TAG_member, name))
            if (const auto offset = unsigned_attribute(member, DW_AT_data_member_location))
                return placed_member{*offset, underlying_type(member)};
        for (auto base : type.children())
            if (base.getTag() == DW_TAG_inheritance)
                if (const auto offset = unsigned_attribute(base, DW_AT_data_member_location))
                    if (auto member = member_position(underlying_type(base), name)) {
                        member->first += *offset;
                        return member;
                    }
        return {};
    }

    auto argument_layout(llvm::DWARFDie argument, bool pybind) -> std::optional<layout> {
        using namespace llvm::dwarf;
        const auto size = unsigned_attribute(argument, DW_AT_byte_size);
        const auto value = member_position(argument, "value");
        if (!size || !value)
            return {};
        auto offset = value->first;
        if (pybind) {
            const auto ptr = member_position(value->second, "m_ptr");
            if (!ptr || ptr->second.getTag() != DW_TAG_pointer_type)
                return {};
            offset += ptr->first;
        } else if (value->second.getTag() != DW_TAG_pointer_type)
            return {};
        layout result{{"size", *size}, {"default", offset}};
        if (const auto name = member_position(argument, "name"); name && name->second.getTag() == DW_TAG_pointer_type)
            result.emplace("name", name->first);
        if (pybind) {
            if (const auto convert = bit_field(argument, "convert", 1))
                result.emplace("convert_bit", *convert);
        } else if (const auto flag = member_position(argument, "flag"); flag && unsigned_attribute(flag->second, DW_AT_byte_size) == 4)
            result.emplace("flags", flag->first);
        return result;
    }

    auto pybind_function(llvm::DWARFDie die) -> std::optional<layout> {
        using namespace llvm::dwarf;
        auto result = structure_layout(die, {"impl", "data", "next", "scope", "nargs", "nargs_pos", "nargs_pos_only"});
        if (!result)
            return {};
        for (const auto* name : {"has_args", "has_kwargs"})
            if (const auto bit = bit_field(die, name, 1))
                result->emplace(name, *bit);
        const auto vector = member_position(die, "args");
        const auto impl = vector ? member_position(vector->second, "_M_impl") : std::nullopt;
        const auto start = impl ? member_position(impl->second, "_M_start") : std::nullopt;
        const auto finish = impl ? member_position(impl->second, "_M_finish") : std::nullopt;
        if (start && finish && start->second.getTag() == DW_TAG_pointer_type && finish->second == start->second) {
            result->emplace("arguments_begin", vector->first + impl->first + start->first);
            result->emplace("arguments_end", vector->first + impl->first + finish->first);
        }
        return result;
    }

    auto pybind_instance(llvm::DWARFDie die) -> std::optional<layout> {
        using namespace llvm::dwarf;
        const auto size = unsigned_attribute(die, DW_AT_byte_size);
        const auto simple = bit_field(die, "simple_layout", 1);
        const auto registered = bit_field(die, "simple_instance_registered", 1);
        if (!size || !simple || !registered)
            return {};
        for (const auto field : die.children()) {
            if (field.getTag() != DW_TAG_member || field.getShortName())
                continue;
            const auto storage = underlying_type(field);
            if (storage.getTag() != DW_TAG_union_type)
                continue;
            const auto outer = unsigned_attribute(field, DW_AT_data_member_location);
            const auto inner = unsigned_attribute(named_child(storage, DW_TAG_member, "simple_value_holder"), DW_AT_data_member_location);
            if (outer && inner)
                return layout{{"size", *size}, {"value", *outer + *inner}, {"simple_bit", *simple}, {"registered_bit", *registered}};
        }
        return {};
    }

    auto nanobind_instance(llvm::DWARFDie die) -> std::optional<layout> {
        using namespace llvm::dwarf;
        auto result = structure_layout(die, {"offset", "state"});
        if (!result)
            return {};
        const auto status = underlying_type(named_child(die, DW_TAG_member, "state"));
        const auto ready = bit_field(status, "state", 2);
        const auto direct = bit_field(status, "direct", 1);
        if (!ready || !direct)
            return {};
        result->emplace("state_bit", result->at("state") * 8 + *ready);
        result->emplace("direct_bit", result->at("state") * 8 + *direct);
        return result;
    }

    auto declaration_of(llvm::DWARFDie die) -> llvm::DWARFDie {
        using namespace llvm::dwarf;
        while (true) {
            auto next = die.getAttributeValueAsReferencedDie(DW_AT_abstract_origin);
            if (!next)
                next = die.getAttributeValueAsReferencedDie(DW_AT_specification);
            if (!next || next == die)
                return die;
            die = next;
        }
    }

    using declaration_key = std::pair<const llvm::DWARFUnit*, std::uint64_t>;
    auto key_for(llvm::DWARFDie die) -> declaration_key {
        die = declaration_of(die);
        return {die.getDwarfUnit(), die.getOffset()};
    }

    enum class capture_kind { function, member, callable };
    enum class argument_domain { unknown, integer, floating, boolean, text };

    struct argument_shape {
        argument_domain domain = argument_domain::unknown;
        std::uint64_t bits = 0;
        bool is_signed = false;
        auto operator<=>(const argument_shape&) const = default;
    };

    auto function_arguments(llvm::DWARFDie function) -> std::vector<std::optional<declaration_key>> {
        using namespace llvm::dwarf;
        std::vector<std::optional<declaration_key>> arguments;
        for (const auto parameter : function.children()) {
            if (parameter.getTag() == DW_TAG_unspecified_parameters)
                return {};
            if (parameter.getTag() != DW_TAG_formal_parameter || parameter.find(DW_AT_artificial))
                continue;
            auto argument = underlying_type(parameter);
            while (argument.getTag() == DW_TAG_reference_type || argument.getTag() == DW_TAG_rvalue_reference_type)
                argument = underlying_type(argument);
            arguments.push_back(argument ? std::optional{key_for(argument)} : std::nullopt);
        }
        return arguments;
    }

    auto builtin_caster(llvm::DWARFDie caster, llvm::DWARFDie argument, bool pybind) -> argument_shape {
        using namespace llvm::dwarf;
        const auto file = caster.getDeclFile(llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath);
        const auto encoding = unsigned_attribute(argument, DW_AT_encoding);
        if (argument.getTag() == DW_TAG_base_type && file.ends_with(pybind ? "/pybind11/cast.h" : "/nanobind/nb_cast.h")) {
            const auto size = unsigned_attribute(argument, DW_AT_byte_size);
            if (encoding == DW_ATE_float)
                return {argument_domain::floating, size.value_or(0) * 8};
            if (encoding == DW_ATE_boolean)
                return {argument_domain::boolean};
            if (size && *size > 0 && *size <= 8 && (encoding == DW_ATE_signed || encoding == DW_ATE_unsigned))
                return {argument_domain::integer, *size * 8, encoding == DW_ATE_signed};
        }
        const llvm::StringRef name(argument.getShortName());
        if (!name.starts_with("basic_string<char,") || !file.ends_with(pybind ? "/pybind11/cast.h" : "/nanobind/stl/string.h"))
            return {};
        auto scope = argument.getParent();
        while (scope.getTag() == DW_TAG_namespace && scope.getParent().getTag() == DW_TAG_namespace)
            scope = scope.getParent();
        if (scope.getTag() != DW_TAG_namespace || llvm::StringRef(scope.getShortName()) != "std")
            return {};
        if (!pybind)
            return {argument_domain::text};
        for (const auto child : caster.children())
            if (child.getTag() == DW_TAG_inheritance) {
                const auto base = underlying_type(child);
                if (llvm::StringRef(base.getShortName()).starts_with("string_caster<") && in_detail_namespace(base, "pybind11"))
                    return {argument_domain::text};
            }
        return {};
    }

    auto receiver_type_name(llvm::DWARFDie type) -> std::string {
        using namespace llvm::dwarf;
        std::string result;
        for (auto scope = type; scope; scope = scope.getParent()) {
            const auto tag = scope.getTag();
            if (tag == DW_TAG_compile_unit || tag == DW_TAG_type_unit)
                break;
            if (tag != DW_TAG_namespace && tag != DW_TAG_class_type && tag != DW_TAG_structure_type && tag != DW_TAG_union_type)
                return {};
            const llvm::StringRef name(scope.getShortName());
            if (name.empty() && tag != DW_TAG_namespace)
                return {};
            auto part = name.empty() ? std::string{"(anonymous namespace)"} : name.str();
            result = result.empty() ? std::move(part) : part + "::" + result;
        }
        return result;
    }

    struct binding_capture {
        capture_kind kind;
        declaration_key declaration{};
        std::string receiver;
        std::vector<std::optional<declaration_key>> arguments;
        auto operator==(const binding_capture&) const -> bool = default;
    };

    auto python_capture(llvm::DWARFDie thunk, llvm::StringRef field, std::uint8_t pointer_size, bool method_adapter, bool member_pointer_layout)
        -> std::optional<binding_capture> {
        using namespace llvm::dwarf;
        const auto declaration = declaration_of(thunk);
        const auto capture = named_child(declaration.getParent().getParent(), DW_TAG_structure_type, "capture");
        const auto payload = named_child(capture, DW_TAG_member, field);
        const auto type = underlying_type(payload);
        if (unsigned_attribute(payload, DW_AT_data_member_location) != 0)
            return {};
        const auto size = unsigned_attribute(capture, DW_AT_byte_size);
        if (size == pointer_size && type.getTag() == DW_TAG_pointer_type && underlying_type(type).getTag() == DW_TAG_subroutine_type)
            return binding_capture{capture_kind::function, {}, {}, function_arguments(underlying_type(type))};
        if (type.getTag() != DW_TAG_class_type && type.getTag() != DW_TAG_structure_type)
            return {};
        if (!method_adapter) {
            // A named functor can dispatch through its own captured state.
            if (type.getShortName())
                return {};
            llvm::DWARFDie call;
            for (auto child : type.children()) {
                if (child.getTag() != DW_TAG_subprogram || llvm::StringRef(child.getShortName()) != "operator()")
                    continue;
                if (call)
                    return {};
                call = child;
            }
            if (call)
                return binding_capture{capture_kind::callable, key_for(call)};
            return {};
        }
        if (!member_pointer_layout || size != pointer_size * 2u)
            return {};
        // Both framework method adapters close over the original member pointer.
        // The standard Itanium representation stores its code pointer or virtual offset first.
        for (const auto member : type.children()) {
            if (member.getTag() != DW_TAG_member)
                continue;
            const auto slot = underlying_type(member);
            if (slot.getTag() == DW_TAG_ptr_to_member_type && underlying_type(slot).getTag() == DW_TAG_subroutine_type &&
                unsigned_attribute(member, DW_AT_data_member_location) == 0) {
                const auto owner = slot.getAttributeValueAsReferencedDie(DW_AT_containing_type);
                return binding_capture{capture_kind::member, {}, receiver_type_name(owner), function_arguments(underlying_type(slot))};
            }
        }
        return {};
    }

    auto encloses_inline_body(llvm::DWARFDie function, llvm::DWARFDie body) -> bool {
        auto ranges = function.getAddressRanges();
        auto inlined = body.getAddressRanges();
        if (!ranges || !inlined) {
            if (!ranges)
                llvm::consumeError(ranges.takeError());
            if (!inlined)
                llvm::consumeError(inlined.takeError());
            return false;
        }
        auto emitted = *inlined | std::views::filter([](const auto& range) { return range.LowPC < range.HighPC; });
        return !emitted.empty() && std::ranges::all_of(emitted, [&](const auto& region) {
            return std::ranges::any_of(*ranges, [&](const auto& outer) {
                return outer.SectionIndex == region.SectionIndex && outer.LowPC <= region.LowPC && region.HighPC <= outer.HighPC;
            });
        });
    }

    void python_layout(const std::string& artifact, llvm::raw_ostream& output) {
        using namespace llvm::dwarf;
        auto binary = checked(llvm::object::ObjectFile::createObjectFile(artifact), "cannot open " + artifact);
        const auto& file = *binary.getBinary();
        const auto separate = llvm::isa<llvm::object::MachOObjectFile>(file) ? dsym(artifact) : std::nullopt;
        auto context = llvm::DWARFContext::create(separate ? *separate->getBinary() : file);
        std::map<std::string, std::set<layout>> records;
        std::map<std::uint64_t, std::optional<binding_capture>> captures;
        std::map<declaration_key, std::set<std::uint64_t>> implementations, inline_owners;
        std::map<std::pair<bool, declaration_key>, std::set<argument_shape>> casters;
        std::map<std::uint64_t, bool> pybind_captures;
        std::map<std::uint64_t, std::set<std::string>> functions;
        std::map<std::string, std::set<std::pair<std::optional<std::uint64_t>, std::string>>> type_info;
        if (const auto* image = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(&file)) {
            object symbols;
            elf(*image, symbols);
            for (const auto& symbol : symbols.symbols)
                if (symbol.kind == "function" && !symbol.imported && !symbol.section.empty())
                    functions[symbol.address].insert(symbol.name);
                else if ((symbol.imported || !symbol.section.empty()) && symbol.name.starts_with("_ZTI")) {
                    const auto name = llvm::demangle(symbol.name);
                    constexpr std::string_view prefix = "typeinfo for ";
                    if (name.starts_with(prefix))
                        type_info[name.substr(prefix.size())].emplace(symbol.imported ? std::nullopt : std::optional{symbol.address}, symbol.name.substr(4));
                }
        }
        std::map<std::uint64_t, std::string> adjustment_thunks;
        std::map<std::string, std::set<std::uint64_t>> thunk_bodies;
        for (const auto& [address, names] : functions)
            if (names.size() == 1 && (names.begin()->starts_with("_ZTh") || names.begin()->starts_with("_ZTv") || names.begin()->starts_with("_ZTc"))) {
                const auto name = llvm::demangle(*names.begin());
                for (const std::string_view prefix : {"non-virtual thunk to ", "virtual thunk to ", "covariant return thunk to "})
                    if (name.starts_with(prefix)) {
                        auto body = name.substr(prefix.size());
                        thunk_bodies.try_emplace(body);
                        adjustment_thunks.emplace(address, std::move(body));
                        break;
                    }
            }
        if (!adjustment_thunks.empty())
            for (const auto& [address, names] : functions)
                if (names.size() == 1 && !adjustment_thunks.contains(address))
                    if (const auto body = thunk_bodies.find(llvm::demangle(*names.begin())); body != thunk_bodies.end())
                        body->second.insert(address);
        const bool standard_member_pointer = llvm::isa<llvm::object::ELFObjectFileBase>(file) &&
                                             file.getArch() == llvm::Triple::x86_64;
        for (const auto& unit : context->compile_units()) {
            const auto visit = [&](this const auto& visit, llvm::DWARFDie die, llvm::DWARFDie container) -> void {
                if (die.getTag() == DW_TAG_structure_type || die.getTag() == DW_TAG_class_type) {
                    const llvm::StringRef name(die.getShortName());
                    std::optional<layout> record;
                    if (name == "function_record_PyObject" && in_detail_namespace(die, "pybind11"))
                        record = structure_layout(die, {"cpp_func_rec"});
                    else if (name == "function_record" && in_detail_namespace(die, "pybind11"))
                        record = pybind_function(die);
                    else if (name == "nb_func" && in_detail_namespace(die, "nanobind"))
                        record = structure_layout(die, {"vectorcall", "scope"});
                    else if (name == "func_data" && in_detail_namespace(die, "nanobind"))
                        record = structure_layout(die, {"impl", "capture", "nargs", "nargs_pos", "flags", "args"});
                    else if (name == "argument_record" && in_detail_namespace(die, "pybind11"))
                        record = argument_layout(die, true);
                    else if (name == "arg_data" && in_detail_namespace(die, "nanobind"))
                        record = argument_layout(die, false);
                    else if (name == "nb_bound_method" && in_detail_namespace(die, "nanobind"))
                        record = structure_layout(die, {"vectorcall", "func", "self"});
                    else if (standard_member_pointer && name == "instance" && in_detail_namespace(die, "pybind11"))
                        record = pybind_instance(die);
                    else if (standard_member_pointer && name == "nb_inst" && in_detail_namespace(die, "nanobind"))
                        record = nanobind_instance(die);
                    else if (standard_member_pointer && name == "_object" && die.getParent().getTag() == DW_TAG_compile_unit)
                        record = structure_layout(die, {"ob_type"});
                    if (record)
                        records[name.str()].insert(std::move(*record));
                    if (name.starts_with("type_caster<")) {
                        const bool pybind = in_detail_namespace(die, "pybind11");
                        if (pybind || in_detail_namespace(die, "nanobind"))
                            for (const auto child : die.children())
                                if (child.getTag() == DW_TAG_template_type_parameter) {
                                    if (const auto argument = underlying_type(child))
                                        casters[{pybind, key_for(argument)}].insert(builtin_caster(die, argument, pybind));
                                    break;
                                }
                    }
                } else if (die.getTag() == DW_TAG_enumeration_type && llvm::StringRef(die.getShortName()) == "func_flags" &&
                           in_detail_namespace(die, "nanobind")) {
                    const auto args = unsigned_attribute(named_child(die, DW_TAG_enumerator, "has_var_args"), DW_AT_const_value);
                    const auto kwargs = unsigned_attribute(named_child(die, DW_TAG_enumerator, "has_var_kwargs"), DW_AT_const_value);
                    if (args && kwargs)
                        records["func_flags"].insert(layout{{"variadic", *args | *kwargs}});
                } else if (die.getTag() == DW_TAG_enumeration_type && llvm::StringRef(die.getShortName()) == "cast_flags" &&
                           in_detail_namespace(die, "nanobind")) {
                    if (const auto convert = unsigned_attribute(named_child(die, DW_TAG_enumerator, "convert"), DW_AT_const_value))
                        records["cast_flags"].insert(layout{{"convert", *convert}});
                } else if (die.getTag() == DW_TAG_subprogram) {
                    const auto pc = die.find(DW_AT_low_pc);
                    const auto address = pc ? pc->getAsAddress() : std::nullopt;
                    container = die;
                    if (address && functions.contains(*address) && llvm::StringRef(die.getShortName()) == "operator()")
                        implementations[key_for(die)].insert(*address);
                    const llvm::StringRef linkage(address ? die.getLinkageName() : nullptr);
                    if (address && (linkage.contains("cpp_function10initialize") || linkage.contains("func_create"))) {
                        const auto name = llvm::demangle(linkage.str());
                        const bool pybind = name.contains("pybind11::cpp_function::initialize<");
                        const bool nanobind = name.contains("nanobind::detail::func_create<");
                        if ((pybind || nanobind) && (name.contains("::__invoke(") || name.contains("::_FUN("))) {
                            const auto method_adapter = name.contains("pybind11::cpp_function::initialize<pybind11::cpp_function::cpp_function<") ||
                                                        (nanobind && name.contains("nanobind::cpp_function_def<"));
                            const auto capture =
                                python_capture(die, pybind ? "f" : "func", unit->getAddressByteSize(), method_adapter, standard_member_pointer);
                            auto [entry, inserted] = captures.try_emplace(*address, capture);
                            if (!inserted && entry->second != capture)
                                entry->second.reset();
                            if (capture && !capture->arguments.empty())
                                pybind_captures.try_emplace(*address, pybind);
                        }
                    }
                } else if (die.getTag() == DW_TAG_inlined_subroutine && container && llvm::StringRef(die.getShortName()) == "operator()") {
                    if (const auto pc = container.find(DW_AT_low_pc))
                        if (const auto address = pc->getAsAddress(); address && encloses_inline_body(container, die))
                            inline_owners[key_for(die)].insert(*address);
                }
                for (auto child : die.children())
                    visit(child, container);
            };
            visit(unit->getUnitDIE(false), {});
        }
        llvm::json::OStream json(output);
        json.object([&] {
            json.attributeObject("records", [&] {
                for (const auto& [name, options] : records)
                    if (options.size() == 1)
                        json.attributeObject(name, [&] {
                            for (const auto& [field, value] : *options.begin())
                                json.attribute(field, value);
                        });
            });
            json.attributeObject("captures", [&] {
                for (const auto& [address, capture] : captures)
                    if (capture && capture->kind != capture_kind::callable)
                        json.attribute(std::to_string(address), capture->kind == capture_kind::member ? "member" : "function");
            });
            json.attributeObject("member_type_info", [&] {
                for (const auto& [address, capture] : captures) {
                    if (!capture || capture->kind != capture_kind::member)
                        continue;
                    if (const auto info = type_info.find(capture->receiver); info != type_info.end() && info->second.size() == 1) {
                        const auto& [offset, encoding] = *info->second.begin();
                        json.attributeObject(std::to_string(address), [&] {
                            if (offset)
                                json.attribute("offset", *offset);
                            else
                                json.attribute("offset", nullptr);
                            json.attribute("encoding", encoding);
                        });
                    }
                }
            });
            json.attributeObject("argument_shapes", [&] {
                for (const auto& [address, capture] : captures) {
                    if (!capture || capture->arguments.empty())
                        continue;
                    const auto pybind = pybind_captures.at(address);
                    json.attributeArray(std::to_string(address), [&] {
                        for (const auto& argument : capture->arguments) {
                            const auto found = argument ? casters.find({pybind, *argument}) : casters.end();
                            if (found == casters.end() || found->second.size() != 1 || found->second.begin()->domain == argument_domain::unknown) {
                                json.value(nullptr);
                                continue;
                            }
                            const auto& shape = *found->second.begin();
                            json.object([&] {
                                const auto domain = shape.domain;
                                json.attribute("domain", domain == argument_domain::integer ? "integer" :
                                                         domain == argument_domain::floating ? "floating" :
                                                         domain == argument_domain::boolean ? "boolean" : "text");
                                if (shape.bits)
                                    json.attribute("bits", shape.bits);
                                if (domain == argument_domain::integer)
                                    json.attribute("signed", shape.is_signed);
                            });
                        }
                    });
                }
            });
            json.attributeObject("callables", [&] {
                for (const auto& [address, capture] : captures) {
                    if (!capture || capture->kind != capture_kind::callable)
                        continue;
                    const auto inlined = inline_owners.find(capture->declaration);
                    if (inlined != inline_owners.end() && inlined->second.contains(address))
                        json.attribute(std::to_string(address), address);
                    else if (const auto found = implementations.find(capture->declaration); found != implementations.end() && found->second.size() == 1)
                        json.attribute(std::to_string(address), *found->second.begin());
                }
            });
            json.attributeObject("functions", [&] {
                for (const auto& [address, names] : functions)
                    if (names.size() == 1)
                        json.attribute(std::to_string(address), *names.begin());
            });
            json.attributeObject("adjustment_thunks", [&] {
                for (const auto& [address, body] : adjustment_thunks) {
                    if (const auto& sites = thunk_bodies.at(body); sites.size() == 1)
                        json.attribute(std::to_string(address), *sites.begin());
                    else
                        json.attribute(std::to_string(address), nullptr);
                }
            });
        });
        output << '\n';
    }

    // Every address the line table lists inside a defined function, and the function's entry, with its inline frames.
    void lines(llvm::DIContext& context, object& out, bool pdb) {
        const llvm::DILineInfoSpecifier spec{llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath, llvm::DILineInfoSpecifier::FunctionNameKind::None};
        // Section-relative object addresses and linked aliases leave an entry address naming no single function.
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> entries;
        for (const auto& symbol : out.symbols)
            if (symbol.kind == "function" && !symbol.imported)
                ++entries[{symbol.section_index, symbol.address}];
        std::set<std::pair<std::uint64_t, std::uint64_t>> addresses;
        for (const auto& symbol : out.symbols) {
            if (symbol.kind != "function" || symbol.imported)
                continue;
            if (entries.at({symbol.section_index, symbol.address}) == 1)
                addresses.emplace(symbol.address, symbol.section_index);
            if (symbol.size)
                for (const auto& [address, _] : context.getLineInfoForAddressRange({symbol.address, symbol.section_index}, symbol.size, spec))
                    addresses.emplace(address, symbol.section_index);
        }
        for (const auto& [address, section] : addresses) {
            location row{.address = address};
            if (pdb) {
                const auto value = context.getLineInfoForAddress({address, section}, spec);
                if (value && value->FileName != llvm::DILineInfo::BadString)
                    out.locations.push_back({address, {{value->FileName, value->Line, value->Column}}});
                continue;
            }
            const auto inlined = context.getInliningInfoForAddress({address, section}, spec);
            for (std::uint32_t index = 0; index < inlined.getNumberOfFrames(); ++index) {
                const auto& value = inlined.getFrame(index);
                if (value.FileName != llvm::DILineInfo::BadString)
                    row.frames.push_back({value.FileName, value.Line, value.Column});
            }
            if (!row.frames.empty())
                out.locations.push_back(std::move(row));
        }
    }

    // The string a constant initializer points at.
    auto text(const llvm::Value* value) -> std::optional<std::string> {
        value = value->stripPointerCasts();
        if (const auto* expression = llvm::dyn_cast<llvm::ConstantExpr>(value); expression && expression->getOpcode() == llvm::Instruction::GetElementPtr)
            value = expression->getOperand(0)->stripPointerCasts();
        const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value);
        if (!global || !global->hasInitializer())
            return std::nullopt;
        const auto* data = llvm::dyn_cast<llvm::ConstantDataSequential>(global->getInitializer());
        if (!data || !data->isCString())
            return std::nullopt;
        return data->getAsCString().str();
    }

    auto global_of(const llvm::Value* value) -> const llvm::GlobalVariable* {
        value = value->stripPointerCasts();
        if (const auto* expression = llvm::dyn_cast<llvm::ConstantExpr>(value); expression && expression->getOpcode() == llvm::Instruction::GetElementPtr)
            value = expression->getOperand(0)->stripPointerCasts();
        return llvm::dyn_cast<llvm::GlobalVariable>(value);
    }

    // JNI's function table indexes FindClass at 6 and RegisterNatives at 215; the C interface calls through
    // that table where C++ calls JNIEnv's inline members.
    constexpr std::uint64_t find_class_slot = 6, register_natives_slot = 215;

    void ir(const std::string& path, object& out) {
        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        auto module = llvm::parseIRFile(path, error, context);
        if (!module)
            fail("cannot read LLVM IR: " + error.getMessage().str());
        // A JNINativeMethod initializer is three pointers: a name string, a descriptor string and a function.
        std::map<const llvm::GlobalVariable*, std::vector<registration>> tables;
        for (const auto& global : module->globals()) {
            if (!global.hasInitializer())
                continue;
            std::vector<const llvm::Constant*> methods;
            if (const auto* array = llvm::dyn_cast<llvm::ConstantArray>(global.getInitializer()))
                for (const auto& element : array->operands())
                    methods.push_back(llvm::cast<llvm::Constant>(element));
            else
                methods.push_back(global.getInitializer());
            for (const auto* method : methods) {
                const auto* triple = llvm::dyn_cast<llvm::ConstantStruct>(method);
                if (!triple || triple->getNumOperands() != 3)
                    continue;
                const auto name = text(triple->getOperand(0)), descriptor = text(triple->getOperand(1));
                const auto* function = llvm::dyn_cast<llvm::GlobalValue>(triple->getOperand(2)->stripPointerCasts());
                if (!name || !descriptor || !function || !descriptor->starts_with('(') || !descriptor->contains(')'))
                    continue;
                tables[&global].push_back({{}, *name, *descriptor, function->getName().str(), global.getName().str()});
            }
        }
        for (const auto& function : *module) {
            if (function.isDeclaration())
                continue;
            const auto caller = function.getName().str();
            // Class names held by values and stored in memory, and the JNI table slots pointers hold.
            std::map<const llvm::Value*, std::string> values, memory;
            std::map<const llvm::Value*, std::uint64_t> slots;
            std::uint64_t index = 0;
            for (const auto& block : function)
                for (const auto& instruction : block) {
                    ++index;
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                        if (const auto found = values.find(store->getValueOperand()); found != values.end())
                            memory[store->getPointerOperand()] = found->second;
                        else
                            memory.erase(store->getPointerOperand());
                    } else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                        if (const auto found = memory.find(load->getPointerOperand()); found != memory.end())
                            values[load] = found->second;
                        if (const auto found = slots.find(load->getPointerOperand()); found != slots.end())
                            slots[load] = found->second;
                    } else if (const auto* element = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
                        for (const auto& operand : element->indices())
                            if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(operand))
                                if (constant->getZExtValue() == find_class_slot || constant->getZExtValue() == register_natives_slot)
                                    slots[element] = constant->getZExtValue();
                    }
                    const auto* site = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!site)
                        continue;
                    const auto* callee = site->getCalledFunction();
                    const auto name = callee ? callee->getName() : llvm::StringRef{};
                    const auto slot = slots.find(site->getCalledOperand());
                    const auto arguments = site->arg_size();
                    if ((name.contains("FindClass") || (slot != slots.end() && slot->second == find_class_slot)) && arguments)
                        if (const auto klass = text(site->getArgOperand(arguments - 1)))
                            values[site] = *klass;
                    if ((name.contains("RegisterNatives") || (slot != slots.end() && slot->second == register_natives_slot)) && arguments >= 3) {
                        const auto table = tables.find(global_of(site->getArgOperand(arguments - 2)));
                        if (table != tables.end()) {
                            const auto klass = values.find(site->getArgOperand(arguments - 3));
                            const auto* count = llvm::dyn_cast<llvm::ConstantInt>(site->getArgOperand(arguments - 1));
                            if (klass == values.end())
                                out.unresolved.push_back("RegisterNatives class could not be resolved for " + table->first->getName().str());
                            else if (!count || count->getZExtValue() > table->second.size())
                                out.unresolved.push_back("RegisterNatives method count could not be resolved for " + table->first->getName().str());
                            else
                                for (std::uint64_t method = 0; method < count->getZExtValue(); ++method) {
                                    auto registered = table->second[method];
                                    registered.klass = klass->second;
                                    out.registrations.push_back(std::move(registered));
                                }
                        }
                    }
                    if (callee && !name.starts_with("llvm."))
                        out.calls.push_back({caller, name.str(), index});
                }
        }
    }

    void write(const object& value, llvm::raw_ostream& output) {
        llvm::json::OStream json{output};
        json.object([&] {
            json.attribute("format", value.format);
            json.attribute("arch", value.arch);
            json.attributeArray("symbols", [&] {
                for (const auto& symbol : value.symbols)
                    json.object([&] {
                        json.attribute("name", symbol.name);
                        json.attribute("kind", symbol.kind);
                        json.attribute("binding", symbol.binding);
                        json.attribute("section", symbol.section);
                        json.attribute("address", symbol.address);
                        json.attribute("size", symbol.size);
                        json.attribute("imported", symbol.imported);
                        json.attribute("exported", symbol.exported);
                        json.attribute("object_file", symbol.object_file);
                    });
            });
            json.attributeArray("calls", [&] {
                for (const auto& row : value.calls)
                    json.object([&] {
                        json.attribute("from", row.from);
                        json.attribute("to", row.to);
                        json.attribute("instruction", row.instruction);
                        json.attribute("artifact", "");
                    });
            });
            json.attributeArray("registrations", [&] {
                for (const auto& row : value.registrations)
                    json.object([&] {
                        json.attribute("klass", row.klass);
                        json.attribute("name", row.name);
                        json.attribute("descriptor", row.descriptor);
                        json.attribute("function", row.function);
                        json.attribute("table", row.table);
                    });
            });
            json.attributeArray("unresolved", [&] {
                for (const auto& reason : value.unresolved)
                    json.value(reason);
            });
            json.attributeArray("locations", [&] {
                for (const auto& row : value.locations)
                    json.object([&] {
                        json.attribute("address", row.address);
                        json.attributeArray("frames", [&] {
                            for (const auto& frame : row.frames)
                                json.object([&] {
                                    json.attribute("file", frame.file);
                                    json.attribute("line", frame.line);
                                    json.attribute("column", frame.column);
                                });
                        });
                    });
            });
        });
        output << '\n';
    }

    auto read(const std::string& artifact, const std::optional<std::string>& ir_path, bool with_lines) -> object {
        auto binary = checked(llvm::object::ObjectFile::createObjectFile(artifact), "cannot open " + artifact);
        const auto& file = *binary.getBinary();
        object out{.format = file.getFileFormatName().str(), .arch = llvm::Triple::getArchTypeName(file.getArch()).str()};
        std::unique_ptr<llvm::pdb::IPDBSession> session;
        if (const auto* value = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(&file))
            elf(*value, out);
        else if (const auto* value = llvm::dyn_cast<llvm::object::MachOObjectFile>(&file))
            macho(*value, out);
        else if (const auto* value = llvm::dyn_cast<llvm::object::COFFObjectFile>(&file))
            session = coff(*value, artifact, out);
        else
            fail("native artifact reader requires an ELF, Mach-O or COFF file");
        if (with_lines) {
            std::unique_ptr<llvm::DIContext> context;
            std::optional<llvm::object::OwningBinary<llvm::object::ObjectFile>> separate;
            const bool pdb = session != nullptr;
            if (session) {
                auto& reader = *session;
                context = std::make_unique<llvm::pdb::PDBContext>(llvm::cast<llvm::object::COFFObjectFile>(file), std::move(session));
                // The session reads section contributions on its first symbol lookup, at the load address the
                // context sets; line lookups find nothing before them.
                reader.findSymbolByRVA(0, llvm::pdb::PDB_SymType::None);
            } else if (llvm::isa<llvm::object::MachOObjectFile>(file) && (separate = dsym(artifact)))
                context = llvm::DWARFContext::create(*separate->getBinary());
            else
                context = llvm::DWARFContext::create(file);
            lines(*context, out, pdb);
        }
        if (ir_path)
            ir(*ir_path, out);
        return out;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        llvm::cl::opt<std::string> artifact(llvm::cl::Positional, llvm::cl::Required, llvm::cl::desc("<artifact>"));
        llvm::cl::opt<std::string> ir_path("ir", llvm::cl::desc("LLVM IR or bitcode file"), llvm::cl::value_desc("file"));
        llvm::cl::opt<bool> with_lines("lines", llvm::cl::desc("Include source line information"));
        llvm::cl::opt<bool> python("python-layout", llvm::cl::desc("Read Python binding records from DWARF"));
        llvm::cl::ParseCommandLineOptions(argc, argv, "Fughetta native artifact reader\n");
        if (python) {
            python_layout(artifact.getValue(), llvm::outs());
            return 0;
        }
        std::optional<std::string> ir;
        if (ir_path.getNumOccurrences())
            ir = ir_path.getValue();
        write(read(artifact.getValue(), ir, with_lines.getValue()), llvm::outs());
        llvm::outs().flush();
        return 0;
    } catch (const std::exception& error) {
        llvm::errs() << error.what() << '\n';
        return 1;
    }
}
