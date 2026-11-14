#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Parser/Parser.h"

#include "revng/Clift/Clift.h"
#include "revng/Clift/CliftDialect.h"
#include "revng/PipeboxCommon/CliftSharedTypes.h"
#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/ObjectID.h"

namespace revng::pypeline {

namespace detail {

template<Kind TheKind>
class CliftMultiObjectContainerBase {
public:
  static constexpr Kind Kind = TheKind;
  static constexpr llvm::StringRef MimeType = "application/x.mlir.bc";

private:
  bool Disposable = false;
  std::unique_ptr<mlir::MLIRContext> Context;
  std::map<ObjectID, mlir::OwningOpRef<mlir::ModuleOp>> Modules;

  // The shared form (see CliftSharedTypes.h). Between beginSharedParts and
  // endSharedParts, serialize produces objects in the shared form and collects
  // the parts they need, which takeSharedParts hands out. deserialize reads
  // both forms; the parts of the objects in the shared form must have been
  // given with addSharedParts.
  std::unique_ptr<clift_shared::Writer> SharedWriter;
  std::map<std::string, std::string> AvailableParts;
  std::set<std::string> LoadedParts;
  clift_shared::Statistics SharedStatistics;

public:
  CliftMultiObjectContainerBase() : Context(clift::makeContext()) {}

public:
  std::set<ObjectID> objects() const {
    return std::views::keys(Modules) | revng::to<std::set<ObjectID>>();
  }

  void
  deserialize(const std::map<const ObjectID *, llvm::ArrayRef<char>> Data) {
    const mlir::ParserConfig Config(Context.get());
    clift_shared::Reader SharedReader(Context.get(),
                                      AvailableParts,
                                      LoadedParts,
                                      SharedStatistics);
    for (auto const &[Object, Buffer] : Data) {
      revng_assert(Object->kind() == Kind);
      llvm::StringRef String(Buffer.data(), Buffer.size());
      mlir::OwningOpRef<mlir::ModuleOp> NewModule;
      if (clift_shared::hasMagic(String, clift_shared::ObjectMagic))
        NewModule = SharedReader.readObject(String);
      else
        NewModule = mlir::parseSourceString<mlir::ModuleOp>(String, Config);
      revng_assert(NewModule);
      revng_assert(clift::isCliftModule(NewModule.get()));
      Modules[*Object] = std::move(NewModule);
    }
    AvailableParts.clear();
  }

  std::map<ObjectID, Buffer>
  serialize(const std::vector<const ObjectID *> Objects) const {
    std::map<ObjectID, Buffer> Result;
    for (const ObjectID *Object : Objects) {
      if (SharedWriter) {
        std::string Shared = SharedWriter->writeObject(*Modules.at(*Object));
        Result[*Object].data().append(Shared.begin(), Shared.end());
        continue;
      }
      llvm::raw_svector_ostream OS(Result[*Object].data());
      mlir::writeBytecodeToFile(*Modules.at(*Object), OS);
    }
    return Result;
  }

  void beginSharedParts() {
    SharedWriter = std::make_unique<clift_shared::Writer>(Context.get(),
                                                          SharedStatistics);
  }

  std::map<std::string, std::string> takeSharedParts() {
    revng_assert(SharedWriter);
    return std::exchange(SharedWriter->NewParts, {});
  }

  void endSharedParts() { SharedWriter.reset(); }

  void addSharedParts(std::map<std::string, std::string> &&Parts) {
    AvailableParts.merge(Parts);
  }

  std::map<std::string, uint64_t> sharedPartsStatistics() const {
    const auto &S = SharedStatistics;
    const auto &C = clift::bytecode::TheCounters;
    return { { "objects", S.Objects },
             { "object_bytes", S.ObjectBytes },
             { "roots", S.Roots },
             { "components", S.Components },
             { "parts_emitted", S.PartsEmitted },
             { "part_bytes_emitted", S.PartBytesEmitted },
             { "parts_loaded", S.PartsLoaded },
             { "parts_skipped", S.PartsSkipped },
             // For the whole process, not only this container
             { "definitions_read", C.DefinitionsRead.load() },
             { "definitions_written", C.DefinitionsWritten.load() },
             { "references_read", C.ReferencesRead.load() },
             { "references_written", C.ReferencesWritten.load() },
             { "definitions_stored", C.DefinitionsStored.load() },
             { "definitions_compared", C.DefinitionsCompared.load() },
             { "names_set", C.NamesSet.load() },
             { "names_unchanged", C.NamesUnchanged.load() },
             { "recursion_checks", C.RecursionChecks.load() },
             { "recursion_check_visits", C.RecursionCheckVisits.load() } };
  }

  bool verify() const {
    bool Result = true;
    for (auto const &[_, Module] : Modules) {
      mlir::LogicalResult ModuleResult = Module.get().verify();
      Result &= ModuleResult.succeeded();
    }
    return Result;
  }

  void setIsDisposable() { Disposable = true; }

  void disposeIfPossible() {
    if (not Disposable)
      return;

    Modules.clear();
    SharedWriter.reset();
    AvailableParts.clear();
    LoadedParts.clear();
    Context = clift::makeContext();
    Disposable = false;
  }

public:
  mlir::MLIRContext *getContext() const { return Context.get(); }
  mlir::ModuleOp getModule(const ObjectID &ID) const { return *Modules.at(ID); }
  mlir::ModuleOp getModule(const ObjectID &ID) { return *Modules.at(ID); }

  void assign(const ObjectID &ID,
              mlir::OwningOpRef<mlir::ModuleOp> &&NewModule) {
    revng_assert(&*Context == NewModule->getContext());
    Modules[ID] = std::move(NewModule);
  }
};

} // namespace detail

class CliftFunctionContainer
  : public detail::CliftMultiObjectContainerBase<Kinds::Function> {
public:
  static constexpr llvm::StringRef Name = "CliftFunctionContainer";
  static constexpr llvm::StringRef Compression = "zstd;level=3";
};

class CliftSingleTypeContainer
  : public detail::CliftMultiObjectContainerBase<Kinds::TypeDefinition> {
public:
  static constexpr llvm::StringRef Name = "CliftSingleTypeContainer";
  static constexpr llvm::StringRef Compression = "zstd;level=3";
};

class CliftModuleContainer {
public:
  static constexpr llvm::StringRef Name = "CliftModuleContainer";
  static constexpr Kind Kind = Kinds::Binary;
  static constexpr llvm::StringRef MimeType = "application/x.mlir.bc";
  static constexpr llvm::StringRef Compression = "zstd;level=3";

private:
  bool Disposable = false;
  std::unique_ptr<mlir::MLIRContext> Context;
  mlir::OwningOpRef<mlir::ModuleOp> Module;

public:
  CliftModuleContainer() :
    Context(clift::makeContext()), Module(clift::makeModule(Context.get())) {}

public:
  std::set<ObjectID> objects() const {
    if (Module.get() and Module.get().getBodyRegion().empty())
      return std::set<ObjectID>{};
    else
      return std::set{ ObjectID() };
  }

  void
  deserialize(const std::map<const ObjectID *, llvm::ArrayRef<char>> Data) {
    if (Data.size() == 0)
      return;

    revng_assert(Data.size() == 1);
    for (const auto &[Object, Buffer] : Data) {
      const mlir::ParserConfig Config(&*Context);
      llvm::StringRef String(Buffer.data(), Buffer.size());
      Module = mlir::parseSourceString<mlir::ModuleOp>(String, Config);
      revng_assert(Module);
      revng_assert(clift::isCliftModule(Module.get()));
    }
  }

  std::map<ObjectID, Buffer>
  serialize(const std::vector<const ObjectID *> Objects) const {
    if (Objects.size() == 0)
      return {};

    revng_assert(Objects.size() == 1 and Objects[0]->kind() == Kind);
    std::map<ObjectID, Buffer> Result;
    for (const ObjectID *Object : Objects) {
      llvm::raw_svector_ostream OS(Result[*Object].data());
      mlir::writeBytecodeToFile(Module.get(), OS);
    }
    return Result;
  }

  bool verify() const { return Module.get().verify().succeeded(); }

  void setIsDisposable() { Disposable = true; }

  void disposeIfPossible() {
    if (not Disposable)
      return;

    Module = {};
    Context = clift::makeContext();
    Disposable = false;
  }

public:
  mlir::MLIRContext *getContext() const { return Context.get(); }
  mlir::ModuleOp getModule() const { return Module.get(); }
  mlir::ModuleOp getModule() { return Module.get(); }

  void assign(const ObjectID &ID,
              mlir::OwningOpRef<mlir::ModuleOp> &&NewModule) {
    revng_assert(&*Context == NewModule->getContext());
    Module = std::move(NewModule);
  }
};

} // namespace revng::pypeline
