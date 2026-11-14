#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include "revng/Model/FunctionTags.h"
#include "revng/Model/Register.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/OpaqueFunctionsPool.h"
#include "revng/Support/PipeFixes.h"

namespace revng {
class IRBuilder;
} // namespace revng

class OpaqueRegisterUser {
private:
  llvm::Module *M;
  OpaqueFunctionsPool<std::string> Clobberers;
  OpaqueFunctionsPool<std::string> Writers;
  OpaqueFunctionsPool<std::string> Readers;
  llvm::SmallVector<llvm::Instruction *, 16> Created;

  /// The pools above are keyed by the name of the function. Filling them when
  /// this object is constructed takes a visit of all the functions of the
  /// module, and one of these objects is constructed for each function being
  /// analyzed. When this is set a pool learns of a function when it is asked
  /// for it: the function a pool filled up front would return for a name is
  /// the function of the module with that name, if it carries the tag.
  const bool Lazy = pipefix::enabled(pipefix::LazyOpaqueFunctionLookup);

public:
  OpaqueRegisterUser(llvm::Module *M) :
    M(M), Clobberers(M, false), Writers(M, false), Readers(M, false) {
    using namespace llvm;

    Clobberers.setMemoryEffects(MemoryEffects::readOnly());
    Clobberers.addFnAttribute(Attribute::NoUnwind);
    Clobberers.addFnAttribute(Attribute::WillReturn);
    Clobberers.setTags({ &FunctionTags::ClobbererFunction });
    if (not Lazy)
      Clobberers.initializeFromName(FunctionTags::ClobbererFunction);

    Writers.setMemoryEffects(MemoryEffects::readOnly());
    Writers.addFnAttribute(Attribute::NoUnwind);
    Writers.addFnAttribute(Attribute::WillReturn);
    Writers.setTags({ &FunctionTags::WriterFunction });
    if (not Lazy)
      Writers.initializeFromName(FunctionTags::WriterFunction);

    Readers.setMemoryEffects(MemoryEffects::inaccessibleMemOnly());
    Readers.addFnAttribute(Attribute::NoUnwind);
    Readers.addFnAttribute(Attribute::WillReturn);
    Readers.setTags({ &FunctionTags::ReaderFunction });
    if (not Lazy)
      Readers.initializeFromName(FunctionTags::ReaderFunction);
  }

public:
  void clobber(revng::IRBuilder &Builder, llvm::GlobalVariable *CSV) {
    writeImpl(Builder, CSV, "clobber_", Clobberers);
  }

  void clobber(revng::IRBuilder &Builder, model::Register::Values Value) {
    for (const model::Register::CSV &CSV : model::Register::getCSVs(Value))
      if (auto *Variable = M->getGlobalVariable(CSV.Name))
        clobber(Builder, Variable);
  }

  llvm::StoreInst *write(revng::IRBuilder &Builder, llvm::GlobalVariable *CSV) {
    return writeImpl(Builder, CSV, "write_", Writers);
  }

  llvm::Instruction *read(revng::IRBuilder &Builder,
                          llvm::GlobalVariable *CSV) {
    auto *CSVTy = CSV->getValueType();
    std::string Name = "reader_" + CSV->getName().str();
    recordIfPresent(Readers, FunctionTags::ReaderFunction, Name);
    llvm::Function *Reader = Readers.get(Name,
                                         Builder.getVoidTy(),
                                         { CSVTy },
                                         Name);

    auto *Load = Builder.CreateLoad(CSVTy, CSV);
    auto *OpaqueCall = Builder.CreateCall(Reader, Load);

    Created.push_back(OpaqueCall);
    Created.push_back(Load);

    return OpaqueCall;
  }

  void purgeCreated() {
    for (llvm::Instruction *I : Created)
      I->eraseFromParent();
    Created.clear();
  }

private:
  /// Make \p Pool aware of the function named \p Name, if the module has one
  /// carrying \p Tag: what initializeFromName does for all of them
  void recordIfPresent(OpaqueFunctionsPool<std::string> &Pool,
                       const FunctionTags::Tag &Tag,
                       const std::string &Name) {
    if (not Lazy)
      return;

    llvm::Function *F = M->getFunction(Name);
    if (F != nullptr and Tag.isTagOf(F))
      Pool.record(Name, F);
  }

  llvm::StoreInst *writeImpl(revng::IRBuilder &Builder,
                             llvm::GlobalVariable *CSV,
                             llvm::StringRef Prefix,
                             OpaqueFunctionsPool<std::string> &Pool) {
    auto *CSVTy = CSV->getValueType();
    std::string Name = Prefix.str() + CSV->getName().str();
    recordIfPresent(Pool,
                    &Pool == &Clobberers ? FunctionTags::ClobbererFunction :
                                           FunctionTags::WriterFunction,
                    Name);
    llvm::Function *Writer = Pool.get(Name, CSVTy, {}, Name);

    auto *OpaqueCall = Builder.CreateCall(Writer);
    auto *Store = Builder.CreateStore(OpaqueCall, CSV);

    Created.push_back(Store);
    Created.push_back(OpaqueCall);

    return Store;
  }
};
