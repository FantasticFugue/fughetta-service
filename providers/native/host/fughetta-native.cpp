// fughetta-native reads one native artifact in process with LLVM's libraries and prints one JSON object
// with the shape of native_artifact::object: the symbol tables of an ELF, Mach-O or COFF file, the
// inline frames of every line-table address inside its functions (DWARF, or the PDB of a COFF image),
// and the direct calls and JNI registrations of an optional LLVM IR or bitcode file.
//
//   fughetta-native <artifact> [--ir <file>] [--lines]

#include <llvm/BinaryFormat/COFF.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/BinaryFormat/MachO.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
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
#include <llvm/DebugInfo/PDB/PDB.h>
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
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
            checked(llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native, *path, session), "cannot load " + *path);
            // The native session enumerates neither functions nor publics, so read the streams themselves.
            auto& native = static_cast<llvm::pdb::NativeSession&>(*session);
            auto& pdb = native.getPDBFile();
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
                    procedures.push_back({base + native.getRVAFromSectOffset(proc.Segment, proc.CodeOffset), proc.Name.str(), descriptor.getObjFileName().str(),
                                          proc.CodeSize, kind == llvm::codeview::S_GPROC32 || kind == llvm::codeview::S_GPROC32_ID});
                }
            }
            for (const auto& procedure : procedures)
                sizes[procedure.address] = procedure.size;
            std::set<std::uint64_t> named;
            const auto& records = checked(pdb.getPDBSymbolStream(), "cannot read the PDB symbol stream");
            for (const auto offset : checked(pdb.getPDBPublicsStream(), "cannot read the PDB publics stream").getPublicsTable()) {
                const auto symbol = checked(llvm::codeview::SymbolDeserializer::deserializeAs<llvm::codeview::PublicSym32>(records.readRecord(offset)),
                                            "cannot read a PDB public");
                const auto address = base + native.getRVAFromSectOffset(symbol.Segment, symbol.Offset);
                const bool code =
                    (symbol.Flags & (llvm::codeview::PublicSymFlags::Code | llvm::codeview::PublicSymFlags::Function)) != llvm::codeview::PublicSymFlags::None;
                const auto size = sizes.find(address);
                at(symbol.Name.str(), address, code ? "function" : "variable", "Global", size == sizes.end() ? 0 : size->second, false);
                named.insert(address);
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

    // Every address the line table lists inside a defined function, and the function's entry, with its inline frames.
    void lines(llvm::DIContext& context, object& out) {
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
            const auto inlined = context.getInliningInfoForAddress({address, section}, spec);
            location row{.address = address};
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
            lines(*context, out);
        }
        if (ir_path)
            ir(*ir_path, out);
        return out;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        std::optional<std::string> artifact, ir_path;
        bool with_lines = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--ir" && index + 1 < argc)
                ir_path = argv[++index];
            else if (argument == "--lines")
                with_lines = true;
            else if (!artifact && !argument.starts_with("--"))
                artifact = argument;
            else
                fail("usage: fughetta-native <artifact> [--ir <file>] [--lines]");
        }
        if (!artifact)
            fail("usage: fughetta-native <artifact> [--ir <file>] [--lines]");
        write(read(*artifact, ir_path, with_lines), llvm::outs());
        llvm::outs().flush();
        return 0;
    } catch (const std::exception& error) {
        llvm::errs() << error.what() << '\n';
        return 1;
    }
}
