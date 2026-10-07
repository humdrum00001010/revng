#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>

#include "llvm/ADT/BitVector.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

#include "revng/ADT/Queue.h"

namespace DetectUninlinableHelpers {

/// \return `true` if `Pointer` addresses a `GlobalVariable` marked as
///         `constant`.
inline bool isPointerToConstantGlobal(const llvm::Value *Pointer) {
  const llvm::Value *Stripped = Pointer->stripPointerCasts();
  const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Stripped);
  return Global != nullptr and Global->isConstant();
}

/// Compute the set of *critical formal arguments* of `Helper`.
///
/// An operand of an instruction is *critical* when one of the following holds:
/// - it is the condition of a `switch` instruction;
/// - it is the pointer or an index operand of a `getelementptr` instruction.
///
/// A *critical argument* of `Helper` is a formal parameter that flows into a
/// critical operand. The function performs a backward dataflow walk from every
/// critical operand and classifies as critical every formal parameter reached
/// during the walk.
///
/// \return One of three possible values:
/// - `std::nullopt`: the helper cannot be inlined at any call site (the
///   backward walk reaches runtime memory, a local allocation, or a
///   nontrivial phi).
/// - empty `BitVector`: the helper can always be inlined.
/// - non-empty `BitVector`: the set of formal-parameter indices that must be
///   `isa<Constant>` at the call site for the helper to be inlinable.
inline std::optional<llvm::BitVector>
computeCriticalArgumentsFor(const llvm::Function &Helper) {
  // Keep this analysis shared by the build-time policy producer and the
  // runtime consumer: helper preparation can inline callees after the policy
  // metadata has been computed.
  if (Helper.isDeclaration())
    return std::nullopt;

  OnceQueue<const llvm::Value *> Worklist;
  for (const llvm::BasicBlock &BB : Helper) {
    for (const llvm::Instruction &I : BB) {
      if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
        Worklist.insert(GEP->getPointerOperand());
        for (const auto &Index : GEP->indices())
          Worklist.insert(Index);
      }
    }

    const auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(BB.getTerminator());
    if (Switch != nullptr)
      Worklist.insert(Switch->getCondition());
  }

  llvm::BitVector Critical(Helper.arg_size(), false);
  while (not Worklist.empty()) {
    const llvm::Value *V = Worklist.pop();
    if (const auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
      Critical.set(Arg->getArgNo());
    } else if (llvm::isa<llvm::AllocaInst>(V)) {
      // A local address cannot become constant at the caller, even when all
      // of the GEP's indices are constants.
      return std::nullopt;
    } else if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(V)) {
      // Walking a loop's induction cycle only finds its constant initial
      // value and increment, but does not make the induction value constant.
      // A phi with one common incoming value can still be specialized.
      const llvm::Value *Common = Phi->hasConstantValue();
      if (Common == nullptr)
        return std::nullopt;
      Worklist.insert(Common);
    } else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(V)) {
      // A runtime-memory load cannot become constant by specializing formal
      // arguments. Keep the original helper call opaque in this case.
      if (not isPointerToConstantGlobal(Load->getPointerOperand()))
        return std::nullopt;
    } else if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
      for (const llvm::Use &U : I->operands())
        Worklist.insert(U.get());
    }
  }

  return Critical;
}

} // namespace DetectUninlinableHelpers
