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

public:
  OpaqueRegisterUser(llvm::Module *M) :
    M(M), Clobberers(M, false), Writers(M, false), Readers(M, false) {
    using namespace llvm;

    Clobberers.setMemoryEffects(MemoryEffects::readOnly());
    Clobberers.addFnAttribute(Attribute::NoUnwind);
    Clobberers.addFnAttribute(Attribute::WillReturn);
    Clobberers.setTags({ &FunctionTags::ClobbererFunction });

    Writers.setMemoryEffects(MemoryEffects::readOnly());
    Writers.addFnAttribute(Attribute::NoUnwind);
    Writers.addFnAttribute(Attribute::WillReturn);
    Writers.setTags({ &FunctionTags::WriterFunction });

    Readers.setMemoryEffects(MemoryEffects::inaccessibleMemOnly());
    Readers.addFnAttribute(Attribute::NoUnwind);
    Readers.addFnAttribute(Attribute::WillReturn);
    Readers.setTags({ &FunctionTags::ReaderFunction });
  }

public:
  void clobber(revng::IRBuilder &Builder, llvm::GlobalVariable *CSV) {
    using FunctionTags::ClobbererFunction;
    writeImpl(Builder, CSV, "clobber_", Clobberers, ClobbererFunction);
  }

  void clobber(revng::IRBuilder &Builder, model::Register::Values Value) {
    for (const model::Register::CSV &CSV : model::Register::getCSVs(Value))
      if (auto *Variable = M->getGlobalVariable(CSV.Name))
        clobber(Builder, Variable);
  }

  llvm::StoreInst *write(revng::IRBuilder &Builder, llvm::GlobalVariable *CSV) {
    using FunctionTags::WriterFunction;
    return writeImpl(Builder, CSV, "write_", Writers, WriterFunction);
  }

  llvm::Instruction *read(revng::IRBuilder &Builder,
                          llvm::GlobalVariable *CSV) {
    auto *CSVTy = CSV->getValueType();
    std::string Name = "reader_" + CSV->getName().str();
    llvm::Function *Reader = get(Readers,
                                 FunctionTags::ReaderFunction,
                                 Name,
                                 Builder.getVoidTy(),
                                 { CSVTy });

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
  /// \return the function of \p Pool named \p Name, creating it if needed.
  ///
  /// The module might already have it, created by another instance of this
  /// class: the pools are not filled with the functions of the module when
  /// they are created, since that takes a visit of all of them.
  llvm::Function *get(OpaqueFunctionsPool<std::string> &Pool,
                      const FunctionTags::Tag &Tag,
                      const std::string &Name,
                      llvm::Type *ReturnType,
                      llvm::ArrayRef<llvm::Type *> Arguments) {
    if (not Pool.contains(Name)) {
      llvm::Function *F = M->getFunction(Name);
      if (F != nullptr and Tag.isTagOf(F))
        Pool.record(Name, F);
    }

    return Pool.get(Name, ReturnType, Arguments, Name);
  }

  llvm::StoreInst *writeImpl(revng::IRBuilder &Builder,
                             llvm::GlobalVariable *CSV,
                             llvm::StringRef Prefix,
                             OpaqueFunctionsPool<std::string> &Pool,
                             const FunctionTags::Tag &Tag) {
    auto *CSVTy = CSV->getValueType();
    std::string Name = Prefix.str() + CSV->getName().str();
    llvm::Function *Writer = get(Pool, Tag, Name, CSVTy, {});

    auto *OpaqueCall = Builder.CreateCall(Writer);
    auto *Store = Builder.CreateStore(OpaqueCall, CSV);

    Created.push_back(Store);
    Created.push_back(OpaqueCall);

    return Store;
  }
};
