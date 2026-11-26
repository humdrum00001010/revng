#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>
#include <utility>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

/// Values as a value plus a constant, or as a constant alone (no value).
/// Where the paths of a function join with the same sum, computed on each of
/// them, LLVM is left with a phi of those: it is that sum.
class SameOnAllPaths {
public:
  using Result = std::pair<llvm::Value *, int64_t>;

private:
  /// What each value was found to be: each is looked at once. A phi that is
  /// met again while it is being worked out stands for itself.
  llvm::DenseMap<llvm::Value *, Result> Known;

  /// Whether a value is the value a cast is applied to: not where what is
  /// found has to replace the value
  bool ThroughCasts = true;

public:
  SameOnAllPaths(bool ThroughCasts = true) : ThroughCasts(ThroughCasts) {}

  Result of(llvm::Value *V) {
    using namespace llvm;

    if (auto *Constant = dyn_cast<ConstantInt>(V))
      return { nullptr, Constant->getSExtValue() };

    if (not isa<Instruction>(V))
      return { V, 0 };

    auto It = Known.find(V);
    if (It != Known.end()) {
      // A phi that stood for another one while that one was being worked
      // out is what that one turned out to be
      Result Found = It->second;
      for (unsigned Hops = 0; Hops < 8; ++Hops) {
        if (Found.first == nullptr or Found.first == V
            or not isa<PHINode>(Found.first))
          break;
        auto Next = Known.find(Found.first);
        if (Next == Known.end() or Next->second.first == Found.first)
          break;
        Found = { Next->second.first, Next->second.second + Found.second };
      }
      return Found;
    }

    Known[V] = Result{ V, 0 };
    Result New = compute(V);
    Known[V] = New;
    return New;
  }

  /// Make the argument \p Index of \p Call the constant it amounts to, if it
  /// is one LLVM does not see
  void fold(llvm::CallBase *Call, unsigned Index) {
    using namespace llvm;
    Value *Argument = Call->getArgOperand(Index);
    if (isa<ConstantInt>(Argument) or not Argument->getType()->isIntegerTy())
      return;

    auto &&[Base, Offset] = of(Argument);
    if (Base != nullptr)
      return;

    unsigned Bits = Argument->getType()->getIntegerBitWidth();
    APInt Folded(64, uint64_t(Offset), true);
    Call->setArgOperand(Index,
                        ConstantInt::get(Argument->getType(),
                                         Folded.sextOrTrunc(Bits)));
  }

private:
  Result compute(llvm::Value *V) {
    using namespace llvm;

    if (ThroughCasts
        and (isa<TruncInst>(V) or isa<ZExtInst>(V) or isa<PtrToIntInst>(V)
             or isa<IntToPtrInst>(V) or isa<BitCastInst>(V)))
      return of(cast<Instruction>(V)->getOperand(0));

    if (auto *Operator = dyn_cast<BinaryOperator>(V)) {
      bool IsAdd = Operator->getOpcode() == Instruction::Add;
      if (IsAdd or Operator->getOpcode() == Instruction::Sub) {
        Result Left = of(Operator->getOperand(0));
        Result Right = of(Operator->getOperand(1));
        if (Right.first == nullptr)
          return { Left.first,
                   IsAdd ? Left.second + Right.second :
                           Left.second - Right.second };
        if (IsAdd and Left.first == nullptr)
          return { Right.first, Left.second + Right.second };
        if (not IsAdd and Left.first == Right.first)
          return { nullptr, Left.second - Right.second };
      }
      return { V, 0 };
    }

    if (auto *Phi = dyn_cast<PHINode>(V)) {
      if (std::optional<Result> Found = throughPhis(Phi))
        return *Found;

      // Not one sum for all that reaches it: it can still be another phi
      // plus a constant
      std::optional<Result> Common;
      for (Value *Incoming : Phi->incoming_values()) {
        if (Incoming == Phi)
          continue;
        Result New = of(Incoming);
        if (New.first == Phi and New.second == 0)
          continue;
        if (Common.has_value() and *Common != New)
          return { V, 0 };
        Common = New;
      }
      if (Common.has_value())
        return *Common;
    }

    return { V, 0 };
  }

  /// What \p Root amounts to if everything that reaches it, through the phis
  /// and the sums with a constant on the way, amounts to the same: the phis
  /// of a loop refer to each other, and none of them can be worked out before
  /// the others. They are worked out together: each is the root minus a
  /// constant, a phi reached twice has to be reached with the same constant
  /// (going around a loop changes nothing), and what is not a phi has to be
  /// one value plus one constant.
  std::optional<Result> throughPhis(llvm::PHINode *Root) {
    using namespace llvm;
    constexpr unsigned MostVisited = 4096;

    // For each phi, what the root is that phi plus
    SmallDenseMap<PHINode *, int64_t, 8> Offsets;
    SmallVector<std::pair<Value *, int64_t>, 16> Queue;
    std::optional<Result> Leaf;
    Offsets[Root] = 0;
    for (Value *Incoming : Root->incoming_values())
      Queue.emplace_back(Incoming, 0);

    unsigned Visited = 0;
    while (not Queue.empty()) {
      auto [Next, Offset] = Queue.pop_back_val();
      if (++Visited > MostVisited)
        return std::nullopt;

      while (true) {
        if (ThroughCasts
            and (isa<TruncInst>(Next) or isa<ZExtInst>(Next)
                 or isa<PtrToIntInst>(Next) or isa<IntToPtrInst>(Next)
                 or isa<BitCastInst>(Next))) {
          Next = cast<Instruction>(Next)->getOperand(0);
          continue;
        }

        auto *Operator = dyn_cast<BinaryOperator>(Next);
        if (Operator == nullptr)
          break;
        bool IsAdd = Operator->getOpcode() == Instruction::Add;
        bool IsSub = Operator->getOpcode() == Instruction::Sub;
        auto *Right = dyn_cast<ConstantInt>(Operator->getOperand(1));
        auto *Left = dyn_cast<ConstantInt>(Operator->getOperand(0));
        if ((IsAdd or IsSub) and Right != nullptr
            and Right->getBitWidth() <= 64) {
          Offset += IsAdd ? Right->getSExtValue() : -Right->getSExtValue();
          Next = Operator->getOperand(0);
        } else if (IsAdd and Left != nullptr and Left->getBitWidth() <= 64) {
          Offset += Left->getSExtValue();
          Next = Operator->getOperand(1);
        } else {
          break;
        }
      }

      if (auto *Phi = dyn_cast<PHINode>(Next)) {
        auto [It, IsNew] = Offsets.try_emplace(Phi, Offset);
        if (not IsNew and It->second != Offset)
          return std::nullopt;
        if (IsNew)
          for (Value *Incoming : Phi->incoming_values())
            Queue.emplace_back(Incoming, Offset);
        continue;
      }

      Result Found = of(Next);
      Result New{ Found.first, Found.second + Offset };
      if (Leaf.has_value() and *Leaf != New)
        return std::nullopt;
      Leaf = New;
    }

    if (not Leaf.has_value() or isa_and_nonnull<PHINode>(Leaf->first))
      return std::nullopt;

    // Each phi on the way is known now
    for (const auto &[Phi, Offset] : Offsets)
      Known[Phi] = Result{ Leaf->first, Leaf->second - Offset };
    return *Leaf;
  }
};
