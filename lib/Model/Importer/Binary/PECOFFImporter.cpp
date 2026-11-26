//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/MicrosoftDemangle.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"

#include "revng/ABI/DefaultFunctionPrototype.h"
#include "revng/Model/Binary.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Model/Importer/Binary/BinaryDescriptor.h"
#include "revng/Model/Importer/Binary/BinaryImporterHelper.h"
#include "revng/Model/Importer/Binary/Options.h"
#include "revng/Model/Importer/DebugInfo/PDBImporter.h"
#include "revng/Model/Pass/AllPasses.h"
#include "revng/Model/RawBinaryView.h"
#include "revng/Support/Debug.h"
#include "revng/Support/Error.h"
#include "revng/Support/MetaAddress.h"
#include "revng/Support/OverflowSafeInt.h"

#include "FindMissingTypes.h"
#include "Importers.h"

using namespace llvm;
using namespace llvm::object;

static Logger Log("pecoff-importer");

// Force using a specific PDB.
static llvm::cl::opt<std::string> UsePDB("use-pdb",
                                         llvm::cl::desc("Path to the PDB."),
                                         llvm::cl::cat(MainCategory));

using PELDDTree = std::map<std::string, std::vector<std::string>>;

class PECOFFImporter : public BinaryImporterHelper {
private:
  TupleTree<model::Binary> &Model;
  const COFFBinary &TheBinary;
  RawBinaryView File;
  llvm::StringRef BinaryPath;
  MetaAddress ImageBase = MetaAddress::invalid();

public:
  PECOFFImporter(TupleTree<model::Binary> &Model,
                 const COFFBinary &TheBinary,
                 uint64_t BaseAddress) :
    BinaryImporterHelper(Model, BaseAddress, Log),
    Model(Model),
    TheBinary(TheBinary),
    File(*Model, toArrayRef(TheBinary.ObjectFile.getData())) {
    revng_log(Log, "Creating binary importer helper");
  }

  Error import(const ImporterOptions &Options);

private:
  Error parseSectionsHeaders();

  /// Map the headers as the loader does: read-only, at the image base, over
  /// SizeOfHeaders rounded up to SectionAlignment.
  void parseHeadersSegment(uint64_t SizeOfHeaders, uint64_t SectionAlignment);

  /// Parse static symbols from the file.
  void parseSymbols();

  /// Register the targets of the export address table as functions.
  void parseExports();

  /// Read the NUL-terminated string at \p RVA.
  std::optional<llvm::StringRef> readString(uint64_t RVA) const;

  /// Parse dynamic symbols from the file.
  void parseImportedSymbols();
  using ImportedSymbolRange = iterator_range<imported_symbol_iterator>;
  void recordImportedFunctions(llvm::StringRef Library,
                               ImportedSymbolRange Range,
                               uint32_t ImportAddressTableEntry);
  /// Parse delay dynamic symbols from the file.
  void parseDelayImportedSymbols();

  using DelayDirectoryRef = const DelayImportDirectoryEntryRef;
  using DelayTable = llvm::object::delay_import_directory_table_entry;
  void recordDelayImportedFunctionsByAddress(const DelayTable &Table);
  void recordDelayImportedFunctions(DelayDirectoryRef &I,
                                    ImportedSymbolRange Range,
                                    uint64_t AddressTable);

  /// Parse the base relocation table, which describes the absolute pointers
  /// in the image
  void parseBaseRelocations();

  /// Give the imported C++ functions that have no prototype the one their
  /// decorated name describes.
  void importPrototypesFromDecoratedNames();
};

Error PECOFFImporter::parseSectionsHeaders() {
  using namespace model;

  revng_assert(Model->Architecture() != Architecture::Invalid);

  uint64_t PointerSize = Architecture::getPointerSize(Model->Architecture());
  bool IsLittleEndian = Architecture::isLittleEndian(Model->Architecture());

  if ((PointerSize != 4 and PointerSize != 8) or not IsLittleEndian) {
    return revng::createError("Only 32/64-bit little endian COFF files are "
                              "supported");
  }

  const object::pe32_header *PE32Header = TheBinary.ObjectFile.getPE32Header();

  // Identify ImageBase
  uint64_t SizeOfHeaders = 0;
  uint64_t SectionAlignment = 0;
  if (PE32Header) {
    // TODO: ImageBase should aligned to 4kb pages, should we check that?
    ImageBase = fromGeneric(PE32Header->ImageBase);
    SizeOfHeaders = PE32Header->SizeOfHeaders;
    SectionAlignment = PE32Header->SectionAlignment;
  } else {
    const pe32plus_header *PE32PlusHeader = TheBinary.ObjectFile
                                              .getPE32PlusHeader();
    if (not PE32PlusHeader)
      return revng::createError("Invalid PE Header");

    // PE32+ Header
    ImageBase = fromGeneric(PE32PlusHeader->ImageBase);
    SizeOfHeaders = PE32PlusHeader->SizeOfHeaders;
    SectionAlignment = PE32PlusHeader->SectionAlignment;
  }

  // Read sections
  for (const SectionRef &SecRef : TheBinary.ObjectFile.sections()) {
    unsigned Id = TheBinary.ObjectFile.getSectionID(SecRef);
    Expected<const object::coff_section *> MaybeSection = TheBinary.ObjectFile
                                                            .getSection(Id);
    if (auto Error = MaybeSection.takeError()) {
      // TODO: emit a diagnostic message for the user.
      std::string Message = toString(std::move(Error));
      revng_log(Log, "Error in section with ID " << Id << ": " << Message);
      continue;
    }
    const object::coff_section *CoffRef = *MaybeSection;

    MetaAddress Start = ImageBase + u64(CoffRef->VirtualAddress);
    Segment Segment(Start);
    Segment.VirtualSize() = u64(CoffRef->VirtualSize);
    Segment.Binary() = TheBinary.Reference;
    Segment.StartOffset() = CoffRef->PointerToRawData;

    // VirtualSize might be larger than SizeOfRawData (extra data at the end of
    // the section) or vice-versa (data mapped in memory but not present in
    // memory, e.g., .bss)
    Segment.FileSize() = CoffRef->SizeOfRawData;

    // Since it is possible that the file size is greater than VirtualSize
    // because SizeOfRawData is rounded, but VirtualSize is not, we work it
    // around here by using maximum of these two values for the VirtSize.
    if (Segment.FileSize() > Segment.VirtualSize())
      Segment.VirtualSize() = Segment.FileSize();

    Segment.IsReadable() = CoffRef->Characteristics & COFF::IMAGE_SCN_MEM_READ;
    Segment.IsWriteable() = CoffRef->Characteristics
                            & COFF::IMAGE_SCN_MEM_WRITE;
    Segment.IsExecutable() = CoffRef->Characteristics
                             & COFF::IMAGE_SCN_MEM_EXECUTE;

    // TODO: replace the following with `populateSegmentTypeStruct`, when
    // symbol table and dynamic symbol table parsing is finalized
    auto &&[Struct, Type] = Model->makeStructDefinition(Segment.VirtualSize());
    Struct.IsSingleton() = true;
    Struct.CanContainCode() = Segment.IsExecutable();
    Segment.Type() = std::move(Type);

    Segment.verify(true);
    Model->Segments().insert(std::move(Segment));
  }

  parseHeadersSegment(SizeOfHeaders, SectionAlignment);

  processSegments();

  // Identify EntryPoint
  MetaAddress EntryPoint;
  if (PE32Header) {
    if (PE32Header->AddressOfEntryPoint != 0) {
      EntryPoint = toPC(ImageBase + u64(PE32Header->AddressOfEntryPoint));
    }
  } else {
    const pe32plus_header *PE32PlusHeader = TheBinary.ObjectFile
                                              .getPE32PlusHeader();
    revng_assert(PE32PlusHeader);

    // PE32+ Header
    if (PE32PlusHeader->AddressOfEntryPoint != 0) {
      EntryPoint = toPC(ImageBase + u64(PE32PlusHeader->AddressOfEntryPoint));
    }
  }

  if (EntryPoint.isValid())
    setEntryPoint(EntryPoint);

  return Error::success();
}

void PECOFFImporter::parseHeadersSegment(uint64_t SizeOfHeaders,
                                         uint64_t SectionAlignment) {
  if (SizeOfHeaders == 0) {
    revng_log(Log, "SizeOfHeaders is 0, not mapping the headers.");
    return;
  }

  uint64_t Size = alignTo(SizeOfHeaders,
                          std::max<uint64_t>(SectionAlignment, 1));
  MetaAddress End = ImageBase + Size;
  if (End.isInvalid()) {
    revng_log(Log, "The headers do not fit in the address space. Ignoring.");
    return;
  }

  for (const model::Segment &Segment : Model->Segments()) {
    if (Segment.StartAddress().addressLowerThan(End)) {
      revng_log(Log,
                "The headers, from " << ImageBase.toString() << " to "
                                     << End.toString()
                                     << ", overlap the segment at "
                                     << Segment.StartAddress().toString()
                                     << ". Not mapping them.");
      return;
    }
  }

  uint64_t FileSize = TheBinary.ObjectFile.getData().size();

  model::Segment Headers(ImageBase);
  Headers.VirtualSize() = Size;
  Headers.Binary() = TheBinary.Reference;
  Headers.StartOffset() = 0;
  Headers.FileSize() = std::min(SizeOfHeaders, FileSize);
  Headers.IsReadable() = true;

  auto &&[Struct, Type] = Model->makeStructDefinition(Size);
  Struct.IsSingleton() = true;
  Headers.Type() = std::move(Type);

  Model->Segments().insert(std::move(Headers));
}

void PECOFFImporter::parseSymbols() {
  for (auto Sym : TheBinary.ObjectFile.symbols()) {
    COFFSymbolRef Symbol = TheBinary.ObjectFile.getCOFFSymbol(Sym);

    if (!Symbol.isFunctionDefinition())
      continue;

    Expected<StringRef> MaybeName = TheBinary.ObjectFile.getSymbolName(Symbol);
    if (auto Error = MaybeName.takeError()) {
      std::string Message = toString(std::move(Error));
      revng_log(Log, "Found static symbol without a name: " << Message);
      continue;
    }

    // Relocate the symbol.
    MetaAddress Address = toPC(ImageBase + Symbol.getValue());
    if (Model->Functions().contains(Address))
      continue;

    if (auto *Function = registerFunctionEntry(Address))
      Function->Name() = *MaybeName;
  }
}

std::optional<llvm::StringRef> PECOFFImporter::readString(uint64_t RVA) const {
  MetaAddress Address = ImageBase + RVA;
  if (Address.isInvalid())
    return std::nullopt;

  auto MaybeData = File.getFromAddressOn(Address);
  if (not MaybeData)
    return std::nullopt;

  StringRef Data = toStringRef(*MaybeData);
  size_t End = Data.find('\0');
  if (End == StringRef::npos)
    return std::nullopt;

  return Data.take_front(End);
}

/// Add \p Names to the exported names of \p Function, naming it after the first
/// one if it has no name yet.
static void addExportedNames(model::Function &Function,
                             llvm::ArrayRef<llvm::StringRef> Names) {
  for (llvm::StringRef Name : Names) {
    if (Function.Name().empty())
      Function.Name() = Name.str();
    Function.ExportedNames().insert(Name.str());
  }
}

void PECOFFImporter::parseExports() {
  const COFFObjectFile &Object = TheBinary.ObjectFile;
  const export_directory_table_entry *Directory = Object.getExportTable();
  if (Directory == nullptr)
    return;

  // export_directories reads the export address table without checking that
  // it lies within the file
  uint64_t Count = Directory->AddressTableEntries;
  MetaAddress AddressesStart = ImageBase
                               + u64(Directory->ExportAddressTableRVA);
  if (not File.getTable(AddressesStart, Count, 4).has_value()) {
    revng_log(Log, "The export address table is not in the file. Ignoring.");
    return;
  }

  // Collect the names of each entry, which can have several, or none. LLVM
  // only provides the first one.
  std::map<uint64_t, std::vector<StringRef>> NamesByIndex;
  uint64_t NamesCount = Directory->NumberOfNamePointers;
  if (NamesCount > 0) {
    MetaAddress NamesStart = ImageBase + u64(Directory->NamePointerRVA);
    MetaAddress OrdinalsStart = ImageBase + u64(Directory->OrdinalTableRVA);
    auto Names = File.getTable(NamesStart, NamesCount, 4);
    auto Ordinals = File.getTable(OrdinalsStart, NamesCount, 2);
    if (Names.has_value() and Ordinals.has_value()) {
      for (uint64_t NameIndex = 0; NameIndex < NamesCount; ++NameIndex) {
        uint64_t Index = Ordinals->read(NameIndex);
        std::optional<StringRef> Name = readString(Names->read(NameIndex));
        if (Index >= Count or not Name.has_value() or Name->empty()) {
          revng_log(Log,
                    "Invalid export name number " << NameIndex
                                                  << ". Ignoring.");
          continue;
        }

        NamesByIndex[Index].push_back(*Name);
      }
    } else {
      revng_log(Log, "The export names are not in the file. Ignoring them.");
    }
  }

  uint64_t NextIndex = 0;
  for (const ExportDirectoryEntryRef &Entry : Object.export_directories()) {
    uint64_t Index = NextIndex++;
    uint32_t RVA = 0;
    if (auto Error = Entry.getExportRVA(RVA)) {
      revng_log(Log,
                "Cannot read export number " << Index << ": "
                                             << toString(std::move(Error))
                                             << ". Ignoring.");
      continue;
    }

    // Unused ordinal
    if (RVA == 0)
      continue;

    bool IsForwarder = false;
    if (auto Error = Entry.isForwarder(IsForwarder)) {
      revng_log(Log,
                "Cannot read export number " << Index << ": "
                                             << toString(std::move(Error))
                                             << ". Ignoring.");
      continue;
    }

    if (IsForwarder) {
      revng_log(Log,
                "Export number " << Index
                                 << " is forwarded to another DLL. Ignoring.");
      continue;
    }

    MetaAddress Address = ImageBase + u64(RVA);
    MetaAddress PC = Address.isValid() ? toPC(Address) : Address;
    if (PC.isInvalid()) {
      revng_log(Log, "Export number " << Index << " is invalid. Ignoring.");
      continue;
    }

    // Exported data is not executable and is rejected here
    model::Function *Function = registerFunctionEntry(PC);
    if (Function == nullptr)
      continue;

    auto NamesIterator = NamesByIndex.find(Index);
    if (NamesIterator != NamesByIndex.end())
      addExportedNames(*Function, NamesIterator->second);
  }
}

/// \return the name of the function \p Library exports as number
///         \p Ordinal, for a function that is imported by its number alone:
///         one that can be used as it is where the function is named in C
static std::string nameOfOrdinal(StringRef Library, uint16_t Ordinal) {
  std::string Result = Library.str();
  for (char &Character : Result)
    if (not isAlnum(Character))
      Character = '_';
  return Result + "_ordinal_" + std::to_string(Ordinal);
}

/// \return whether \p Symbol is imported by its number and not by a name
static bool isImportedByOrdinal(const ImportedSymbolRef &Symbol) {
  bool Result = false;
  if (Error E = Symbol.isOrdinal(Result)) {
    consumeError(std::move(E));
    return false;
  }
  return Result;
}

void PECOFFImporter::recordImportedFunctions(StringRef Library,
                                             ImportedSymbolRange Range,
                                             uint32_t ImportAddressTableEntry) {

  // Index of entries within import table.
  int32_t Index = -1;
  for (const ImportedSymbolRef &I : Range) {
    ++Index;
    StringRef Sym;
    if (Error E = I.getSymbolName(Sym)) {
      std::string Message = toString(std::move(E));
      revng_log(Log, "Found an imported symbol without a name: " << Message);
      continue;
    }

    // TODO: We may face some old linkers that use ordinal bits only
    // so consider this info then.
    uint16_t Ordinal;
    if (Error E = I.getOrdinal(Ordinal)) {
      std::string Message = toString(std::move(E));
      revng_log(Log,
                "Found an imported symbol without an ordinal: " << Message);
      continue;
    }

    // Dynamic functions must have a name: one that is imported by its number
    // gets the name of the library and the number, so that the calls to it
    // are calls to one function and not jumps through a pointer nothing is
    // known about.
    std::string OrdinalName;
    if (Sym.empty() and isImportedByOrdinal(I)) {
      OrdinalName = nameOfOrdinal(Library, Ordinal);
      Sym = OrdinalName;
    }

    if (Sym.empty() or Model->ImportedDynamicFunctions().contains(Sym.str()))
      continue;

    // NOTE: This address will occur in the .text section as a target of a jump.
    // Once we have the address of the entry within .idata, we can access
    // the information about symbol.
    uint64_t PointerSize = getPointerSize(Model->Architecture());
    MetaAddress AddressOfImportEntry = ImageBase + u64(ImportAddressTableEntry)
                                       + u64(Index * PointerSize);

    // Lets make a Relocation.
    using namespace model::RelocationType;
    auto [RelocationType, Size] = formCOFFRelocation(Model->Architecture());
    model::Relocation NewRelocation(AddressOfImportEntry,
                                    RelocationType,
                                    Size,
                                    0);

    auto It = Model->ImportedDynamicFunctions().insert(Sym.str()).first;
    revng_assert(NewRelocation.verify(true));
    It->Relocations().insert(NewRelocation);
  }
}

void PECOFFImporter::parseImportedSymbols() {
  for (const ImportDirectoryEntryRef &I :
       TheBinary.ObjectFile.import_directories()) {
    StringRef Name;
    if (Error E = I.getName(Name)) {
      std::string Message = toString(std::move(E));
      revng_log(Log, "Found an imported library without a name: " << Message);
      continue;
    }

    // Let's find symbols of the dll associated with Name.

    // In PE/COFF you can have two tables for imported functions: the
    // ImportLookupTable (aka ILT) and the ImportAddressTable (aka IAT). On disk
    // they are supposed to be the same. At run-time the IAT will be patched
    // with the actual addresses.
    //
    // Exception: the IAT might be "bound", i.e., pre-filled. The loader checks
    // at run-time if the bound IAT is valid via a timestamp. If it isn't uses
    // the ILT. One could debate which one is more reliable, we choose to first
    // use the ILT, if absent, we use the IAT.

    // A table can be reported as absent in two distinct ways: the getter can
    // fail, or it can succeed and hand back a zero RVA.

    uint32_t ImportLookupTableEntry = 0;
    bool HasImportLookupTableEntry = true;
    if (Error E = I.getImportLookupTableRVA(ImportLookupTableEntry)) {
      // Consume eagerly: revng_log does not evaluate its argument unless the
      // logger is enabled, and an unconsumed Error aborts on destruction.
      std::string Message = toString(std::move(E));
      revng_log(Log,
                "No ImportLookupTableRVA found for an import: " << Message);
      HasImportLookupTableEntry = false;
    } else if (ImportLookupTableEntry == 0) {
      revng_log(Log, "The ImportLookupTableRVA of an import is zero");
      HasImportLookupTableEntry = false;
    }

    uint32_t ImportAddressTableEntry = 0;
    bool HasImportAddressTableEntry = true;
    if (Error E = I.getImportAddressTableRVA(ImportAddressTableEntry)) {
      std::string Message = toString(std::move(E));
      revng_log(Log,
                "No ImportAddressTableRVA found for an import: " << Message);
      HasImportAddressTableEntry = false;
    } else if (ImportAddressTableEntry == 0) {
      revng_log(Log, "The ImportAddressTableRVA of an import is zero");
      HasImportAddressTableEntry = false;
    }

    // The IAT is not optional: whichever table we enumerate the names from, its
    // RVA is the base we compute the address of each entry from.
    if (not HasImportAddressTableEntry) {
      revng_log(Log, "The IAT of " << Name.str() << " is missing, skipping");
      continue;
    }

    if (not Model->ImportedLibraries().insert(Name.str()).second) {
      revng_log(Log, "Library " << Name.str() << " already imported, skipping");
      continue;
    }

    // The import lookup table can be missing with certain older linkers, so
    // fall back to the import address table in that case.
    if (HasImportLookupTableEntry) {
      recordImportedFunctions(Name,
                              I.lookup_table_symbols(),
                              ImportAddressTableEntry);
    } else {
      recordImportedFunctions(Name,
                              I.imported_symbols(),
                              ImportAddressTableEntry);
    }
  }
}

void PECOFFImporter::recordDelayImportedFunctions(DelayDirectoryRef &I,
                                                  ImportedSymbolRange Range,
                                                  uint64_t AddressTable) {
  auto Architecture = Model->Architecture();
  uint64_t PointerSize = model::Architecture::getPointerSize(Architecture);
  // Index of entries within import table.
  int32_t Index = -1;
  for (const ImportedSymbolRef &S : Range) {
    ++Index;
    StringRef Sym;
    if (Error E = S.getSymbolName(Sym)) {
      std::string Message = toString(std::move(E));
      revng_log(Log,
                "Found a delay imported symbol without a name: " << Message);
      continue;
    }

    // TODO: We may face some old linkers that use ordinal bits only
    // so consider this info then.
    uint16_t Ordinal;
    if (Error E = S.getOrdinal(Ordinal)) {
      std::string Message = toString(std::move(E));
      revng_log(Log,
                "Found a delay imported symbol without an ordinal: "
                  << Message);
      continue;
    }

    uint64_t Addr;
    if (Error E = I.getImportAddress(Index, Addr)) {
      std::string Message = toString(std::move(E));
      revng_log(Log,
                "Found a delay imported symbol without an address: "
                  << Message);
      continue;
    }

    // Dynamic functions must have a name: see recordImportedFunctions for
    // those that are imported by their number
    std::string OrdinalName;
    if (Sym.empty() and isImportedByOrdinal(S)) {
      StringRef Library;
      if (Error E = I.getName(Library)) {
        consumeError(std::move(E));
      } else {
        OrdinalName = nameOfOrdinal(Library, Ordinal);
        Sym = OrdinalName;
      }
    }

    if (Sym.empty() or Model->ImportedDynamicFunctions().contains(Sym.str()))
      continue;

    // The loader stores the address of the function in the entry of the delay
    // import address table: that entry is what the code goes through. What
    // `getImportAddress` returns is the content of the entry in the file,
    // which points to the code that binds the function at its first use.
    MetaAddress AddressOfDelayImportEntry = ImageBase
                                            + (AddressTable
                                               + Index * PointerSize);

    // Lets make Relocation.
    using namespace model::RelocationType;
    auto [RelocationType, Size] = formCOFFRelocation(Model->Architecture());
    model::Relocation NewRelocation(AddressOfDelayImportEntry,
                                    RelocationType,
                                    Size,
                                    0);
    auto NewIt = Model->ImportedDynamicFunctions().insert(Sym.str()).first;
    revng_assert(NewRelocation.verify(true));
    NewIt->Relocations().insert(NewRelocation);
  }
}

void PECOFFImporter::parseDelayImportedSymbols() {
  for (DelayDirectoryRef &I : TheBinary.ObjectFile.delay_import_directories()) {
    const delay_import_directory_table_entry *Table;
    if (Error E = I.getDelayImportTable(Table)) {
      std::string Message = toString(std::move(E));
      revng_log(Log, "No delay import table found for a dll: " << Message);
      continue;
    }

    // The fields of the table are RVAs if the first bit of the attributes is
    // set, virtual addresses otherwise: the format of older linkers, which
    // the reader of the object file does not know. Read here.
    if ((Table->Attributes & 1) == 0) {
      recordDelayImportedFunctionsByAddress(*Table);
      continue;
    }

    StringRef Name;
    if (Error E = I.getName(Name)) {
      std::string Message = toString(std::move(E));
      revng_log(Log, "No name of a delay imported dll: " << Message);
      continue;
    }

    if (not Model->ImportedLibraries().insert(Name.str()).second)
      continue;

    uint64_t AddressTable = Table->DelayImportAddressTable;
    recordDelayImportedFunctions(I, I.imported_symbols(), AddressTable);
  }
}

void PECOFFImporter::recordDelayImportedFunctionsByAddress(const DelayTable
                                                             &Table) {
  auto Architecture = Model->Architecture();
  uint64_t PointerSize = model::Architecture::getPointerSize(Architecture);
  uint64_t Base = ImageBase.address();
  constexpr uint64_t Word = 4;
  if (PointerSize != Word or Table.Name < Base
      or Table.DelayImportAddressTable < Base
      or Table.DelayImportNameTable < Base)
    return;

  std::optional<StringRef> Library = readString(Table.Name - Base);
  if (not Library.has_value()) {
    revng_log(Log, "No name of a delay imported dll");
    return;
  }

  if (not Model->ImportedLibraries().insert(Library->str()).second)
    return;

  constexpr uint64_t MostEntries = 1 << 16;
  for (uint64_t Index = 0; Index < MostEntries; ++Index) {
    MetaAddress NameEntry = fromGeneric(Table.DelayImportNameTable
                                        + Index * Word);
    std::optional<uint64_t> Thunk = File.readInteger(NameEntry, Word);
    if (not Thunk.has_value() or *Thunk == 0)
      break;

    // By its number, or by the address of its hint and its name
    std::string Symbol;
    constexpr uint64_t OrdinalFlag = uint64_t(1) << 31;
    if (*Thunk & OrdinalFlag) {
      Symbol = nameOfOrdinal(*Library, *Thunk & 0xFFFF);
    } else if (*Thunk >= Base) {
      if (std::optional<StringRef> Name = readString(*Thunk - Base + 2))
        Symbol = Name->str();
    }

    if (Symbol.empty() or Model->ImportedDynamicFunctions().contains(Symbol))
      continue;

    // The loader stores the address of the function in the entry of the
    // delay import address table: that entry is what the code goes through
    MetaAddress Entry = fromGeneric(Table.DelayImportAddressTable
                                    + Index * Word);
    using namespace model::RelocationType;
    auto [RelocationType, Size] = formCOFFRelocation(Model->Architecture());
    model::Relocation NewRelocation(Entry, RelocationType, Size, 0);
    auto NewIt = Model->ImportedDynamicFunctions().insert(Symbol).first;
    revng_assert(NewRelocation.verify(true));
    NewIt->Relocations().insert(NewRelocation);
  }
}

void PECOFFImporter::parseBaseRelocations() {
  revng_log(Log, "Parsing base relocations");
  LoggerIndent Indent(Log);

  auto LogError = [](Error TheError) {
    std::string Message = toString(std::move(TheError));
    revng_log(Log, "Cannot read the base relocation table: " << Message);
  };

  for (const BaseRelocRef &Entry : TheBinary.ObjectFile.base_relocs()) {
    uint8_t Type = 0;
    if (Error E = Entry.getType(Type)) {
      LogError(std::move(E));
      return;
    }

    uint32_t RVA = 0;
    if (Error E = Entry.getRVA(RVA)) {
      LogError(std::move(E));
      return;
    }

    uint64_t Size = 0;
    switch (Type) {
    case COFF::IMAGE_REL_BASED_ABSOLUTE:
      // Padding
      continue;

    case COFF::IMAGE_REL_BASED_HIGHLOW:
      Size = 4;
      break;

    case COFF::IMAGE_REL_BASED_DIR64:
      Size = 8;
      break;

    default:
      revng_log(Log,
                "Ignoring base relocation of type "
                  << static_cast<unsigned>(Type) << " at RVA " << RVA);
      continue;
    }

    MetaAddress Address = ImageBase + u64(RVA);
    if (auto Target = File.readInteger(Address, Size)) {
      registerAbsoluteRelocation(Address, Size, *Target);
    } else {
      revng_log(Log,
                "Cannot read the target of the base relocation at "
                  << Address.toString() << ". Ignoring.");
    }
  }
}

/// \return the type of an argument or of a return value that a decorated name
///         gives \p Node as the type of, or an empty type if it is not a
///         scalar: the name does not tell how large a class is.
static model::UpcastableType
scalarFromDecoratedName(const ms_demangle::TypeNode &Node,
                        model::Architecture::Values Architecture) {
  using namespace ms_demangle;
  using model::PrimitiveType;

  switch (Node.kind()) {
  case NodeKind::PrimitiveType:
    switch (static_cast<const PrimitiveTypeNode &>(Node).PrimKind) {
    case PrimitiveKind::Void:
      return PrimitiveType::makeVoid();
    case PrimitiveKind::Bool:
    case PrimitiveKind::Uchar:
    case PrimitiveKind::Char8:
      return PrimitiveType::makeUnsigned(1);
    case PrimitiveKind::Char:
    case PrimitiveKind::Schar:
      return PrimitiveType::makeSigned(1);
    case PrimitiveKind::Ushort:
    case PrimitiveKind::Wchar:
    case PrimitiveKind::Char16:
      return PrimitiveType::makeUnsigned(2);
    case PrimitiveKind::Short:
      return PrimitiveType::makeSigned(2);
    case PrimitiveKind::Uint:
    case PrimitiveKind::Ulong:
    case PrimitiveKind::Char32:
      return PrimitiveType::makeUnsigned(4);
    case PrimitiveKind::Int:
    case PrimitiveKind::Long:
      return PrimitiveType::makeSigned(4);
    case PrimitiveKind::Uint64:
      return PrimitiveType::makeUnsigned(8);
    case PrimitiveKind::Int64:
      return PrimitiveType::makeSigned(8);
    case PrimitiveKind::Float:
      return PrimitiveType::makeFloat(4);
    case PrimitiveKind::Double:
    case PrimitiveKind::Ldouble:
      return PrimitiveType::makeFloat(8);
    case PrimitiveKind::Nullptr:
      return model::PointerType::make(PrimitiveType::makeVoid(), Architecture);
    }
    return {};

  case NodeKind::PointerType: {
    // Pointers and references. A pointer to a member can be larger than a
    // pointer.
    const auto &Pointer = static_cast<const PointerTypeNode &>(Node);
    if (Pointer.ClassParent != nullptr or Pointer.Pointee == nullptr)
      return {};

    // What it points to is kept if it is a scalar too
    auto Pointee = scalarFromDecoratedName(*Pointer.Pointee, Architecture);
    if (Pointee.isEmpty())
      Pointee = PrimitiveType::makeVoid();
    if (Pointer.Pointee->Quals & Q_Const)
      Pointee->IsConst() = true;
    return model::PointerType::make(std::move(Pointee), Architecture);
  }

  case NodeKind::TagType:
    if (static_cast<const TagTypeNode &>(Node).Tag == TagKind::Enum)
      return PrimitiveType::makeSigned(4);
    return {};

  default:
    return {};
  }
}

/// \return the prototype the decorated name \p Name of a C++ function
///         describes, or an empty type if \p Name is not such a name or does
///         not tell what the function takes from the stack.
static model::UpcastableType prototypeFromDecoratedName(model::Binary &Binary,
                                                        llvm::StringRef Name) {
  using namespace ms_demangle;

  if (not Name.startswith("?"))
    return {};

  Demangler Demangler;
  StringView View(Name.data(), Name.size());
  SymbolNode *Symbol = Demangler.parse(View);
  if (Demangler.Error or Symbol == nullptr
      or Symbol->kind() != NodeKind::FunctionSymbol)
    return {};

  // Thunks and functions with a variable number of arguments are left alone
  const FunctionSignatureNode
    *Signature = static_cast<const FunctionSymbolNode *>(Symbol)->Signature;
  if (Signature == nullptr or Signature->kind() != NodeKind::FunctionSignature
      or Signature->IsVariadic)
    return {};

  model::ABI::Values ABI = model::ABI::Invalid;
  switch (Signature->CallConvention) {
  case ms_demangle::CallingConv::Cdecl:
    ABI = model::ABI::Microsoft_x86_cdecl;
    break;
  case ms_demangle::CallingConv::Stdcall:
    ABI = model::ABI::Microsoft_x86_stdcall;
    break;
  case ms_demangle::CallingConv::Thiscall:
    ABI = model::ABI::Microsoft_x86_thiscall;
    break;
  case ms_demangle::CallingConv::Fastcall:
    ABI = model::ABI::Microsoft_x86_fastcall;
    break;
  default:
    return {};
  }

  model::Architecture::Values Architecture = Binary.Architecture();
  auto Pointer = [Architecture]() {
    return model::PointerType::make(model::PrimitiveType::makeVoid(),
                                    Architecture);
  };

  // Collect the types first: nothing is added to the model for a name that
  // turns out not to tell enough
  SmallVector<model::UpcastableType, 8> Arguments;
  model::UpcastableType ReturnType;

  // A non-static member function takes `this` first
  const unsigned Member = FC_Public | FC_Protected | FC_Private;
  bool HasThis = (Signature->FunctionClass & Member) != 0
                 and (Signature->FunctionClass & FC_Static) == 0;
  if (HasThis)
    Arguments.push_back(Pointer());

  auto IsObject = [](const TypeNode &Type) {
    return Type.kind() == NodeKind::TagType
           and static_cast<const TagTypeNode &>(Type).Tag != TagKind::Enum;
  };

  const TypeNode *Returned = Signature->ReturnType;
  if (Returned == nullptr) {
    // A constructor returns `this`, a destructor returns nothing
    const IdentifierNode *Identifier = Symbol->Name->getUnqualifiedIdentifier();
    if (Identifier->kind() != NodeKind::StructorIdentifier)
      return {};
    auto *Structor = static_cast<const StructorIdentifierNode *>(Identifier);
    if (not Structor->IsDestructor)
      ReturnType = Pointer();
  } else if (IsObject(*Returned)) {
    // A member function returns an object through a pointer the caller passes
    // after `this` and gets back. For the other functions it depends on the
    // definition of the class.
    if (not HasThis)
      return {};
    Arguments.push_back(Pointer());
    ReturnType = Pointer();
  } else {
    ReturnType = scalarFromDecoratedName(*Returned, Architecture);
    if (ReturnType.isEmpty())
      return {};
    if (ReturnType->isVoidPrimitive())
      ReturnType = model::UpcastableType();
  }

  if (Signature->Params != nullptr) {
    const NodeArrayNode &List = *Signature->Params;
    for (size_t I = 0; I < List.Count; ++I) {
      const auto &Parameter = static_cast<const TypeNode &>(*List.Nodes[I]);
      auto Type = scalarFromDecoratedName(Parameter, Architecture);
      if (Type.isEmpty() or Type->isVoidPrimitive())
        return {};
      Arguments.push_back(std::move(Type));
    }
  }

  auto &&[Prototype, Type] = Binary.makeCABIFunctionDefinition();
  Prototype.ABI() = ABI;
  for (model::UpcastableType &Argument : Arguments)
    Prototype.addArgument(std::move(Argument));
  if (not ReturnType.isEmpty())
    Prototype.ReturnType() = std::move(ReturnType);

  return std::move(Type);
}

/// \return for the decorated name \p Name of a C function that removes its
///         arguments from the stack (`_Name@N`, where N is how many bytes
///         they take), the default prototype with those N bytes of arguments
///         on the stack, removed by the function; an empty type for any
///         other name. The name does not say what the arguments are, nor
///         what the function returns: that stays as it is for a function
///         nothing is known about.
static model::UpcastableType prototypeFromStdcallName(model::Binary &Binary,
                                                      llvm::StringRef Name) {
  if (not Name.consume_front("_"))
    return {};

  size_t At = Name.rfind('@');
  uint64_t Bytes = 0;
  if (At == llvm::StringRef::npos or At == 0
      or Name.substr(At + 1).getAsInteger(10, Bytes))
    return {};

  uint64_t Word = model::Architecture::getPointerSize(Binary.Architecture());
  constexpr uint64_t MostWords = 64;
  if (Bytes % Word != 0 or Bytes / Word > MostWords)
    return {};

  const model::TypeDefinition *Default = Binary.defaultPrototype();
  const auto *Raw = dyn_cast_or_null<model::RawFunctionDefinition>(Default);
  if (Raw == nullptr)
    return {};

  // Collect what is copied first: making a type can move the others
  auto Architecture = Raw->Architecture();
  auto Arguments = Raw->Arguments();
  auto ReturnValues = Raw->ReturnValues();
  auto PreservedRegisters = Raw->PreservedRegisters();
  uint64_t FinalStackOffset = Raw->FinalStackOffset() + Bytes;

  model::UpcastableType OnStack;
  if (Bytes != 0) {
    auto &&[Struct, StructType] = Binary.makeStructDefinition(Bytes);
    for (uint64_t Offset = 0; Offset < Bytes; Offset += Word)
      Struct.addField(Offset, model::PrimitiveType::makeGeneric(Word));
    OnStack = std::move(StructType);
  }

  auto &&[Prototype, Type] = Binary.makeRawFunctionDefinition();
  Prototype.Architecture() = Architecture;
  Prototype.Arguments() = std::move(Arguments);
  Prototype.ReturnValues() = std::move(ReturnValues);
  Prototype.PreservedRegisters() = std::move(PreservedRegisters);
  Prototype.FinalStackOffset() = FinalStackOffset;
  if (not OnStack.isEmpty())
    Prototype.StackArgumentsType() = std::move(OnStack);

  return std::move(Type);
}

void PECOFFImporter::importPrototypesFromDecoratedNames() {
  // The calling conventions these names can tell apart are those of x86
  if (Model->Architecture() != model::Architecture::x86)
    return;

  for (model::DynamicFunction &Function : Model->ImportedDynamicFunctions()) {
    if (not Function.Prototype().isEmpty())
      continue;

    auto Prototype = prototypeFromDecoratedName(*Model, Function.Name());
    if (Prototype.isEmpty())
      Prototype = prototypeFromStdcallName(*Model, Function.Name());
    if (Prototype.isEmpty())
      continue;

    revng_log(Log, "Prototype from the name of " << Function.Name());
    Function.Prototype() = std::move(Prototype);
  }
}

Error PECOFFImporter::import(const ImporterOptions &Options) {
  if (Error E = parseSectionsHeaders())
    return E;

  parseBaseRelocations();

  // Without base relocations, the image can only be loaded at its preferred
  // base
  auto Characteristics = TheBinary.ObjectFile.getCharacteristics();
  if (Characteristics & COFF::IMAGE_FILE_RELOCS_STRIPPED)
    Model->AddressSpaces()[0].IsPositionIndependent() = false;

  // Parse the symbol table.
  parseSymbols();

  // Parse the export table.
  parseExports();

  // Parse dynamic symbol table.
  parseImportedSymbols();

  // Parse delay dynamic symbol table (similar to ELF's symbols used for lazy
  // linking).
  parseDelayImportedSymbols();

  // Set default ABI
  if (Model->DefaultABI() == model::ABI::Invalid) {
    revng_assert(Model->Architecture() != model::Architecture::Invalid);
    if (auto ABI = model::ABI::getDefaultForPECOFF(Model->Architecture())) {
      Model->DefaultABI() = ABI.value();
    } else {
      auto ArchName = model::Architecture::getName(Model->Architecture()).str();
      return revng::createError("Unsupported architecture for PECOFF: "
                                + ArchName);
    }
  }

  // Create a default prototype.
  Model->DefaultPrototype() = abi::registerDefaultFunctionPrototype(*Model);

  if (Options.DebugInfo != DebugInfoLevel::No) {
    revng_log(Log, "Importing PDB");
    LoggerIndent Indent(Log);

    // Consider the --use-pdb argument
    if (not UsePDB.empty()) {
      auto ImportLogger = importLogger(TheBinary.canonicalPath());
      PDBImporter PDBI(Model, ImageBase, std::nullopt);
      PDBI.importPDB(UsePDB, Options);
    }

    // Identify dependencies
    std::optional<LDDTree>
      MaybeDependencies = identifyDependencies(TheBinary.ObjectFile,
                                               TheBinary.canonicalPath());

    if (MaybeDependencies.has_value()) {
      // Import debug info
      {
        auto ImportLogger = importLogger(TheBinary.canonicalPath());
        PDBImporter PDBI(Model, ImageBase, std::nullopt);
        revng_assert(MaybeDependencies->Root != nullptr);
        PDBI.import(MaybeDependencies->Root, TheBinary, Options);
      }

      // Now we try to find missing types in the dependencies
      findMissingTypes<COFFBinary, PDBImporter>(*MaybeDependencies,
                                                Options,
                                                Logger,
                                                Model);
    } else {
      revng_log(Log, "Couldn't find an appropriate rootfs");
    }
  }

  // An imported function without a prototype is assumed to leave its
  // arguments on the stack, which the member functions of C++ classes do not:
  // their names tell
  importPrototypesFromDecoratedNames();

  model::flattenPrimitiveTypedefs(Model);
  model::deduplicateCollidingNames(Model);
  return Error::success();
}

Error importPECOFF(TupleTree<model::Binary> &Model,
                   const COFFBinary &TheBinary,
                   const ImporterOptions &Options) {
  PECOFFImporter Importer(Model, TheBinary, Options.BaseAddress);
  return Importer.import(Options);
}
