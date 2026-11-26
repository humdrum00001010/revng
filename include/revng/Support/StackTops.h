#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>
#include <utility>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"

#include "revng/Support/SameOnAllPaths.h"

// Only LLVM is used in this file: the analysis can be compiled on its own and
// run on functions kept from a run.

/// What the stack pointer is after a function has allocated on its stack at
/// run time.
///
/// It is two things. The top of what the function pushes (where the next
/// argument goes, where a call puts its return address) is as far from the
/// stack pointer at the entry as if nothing had been allocated: each push
/// lowers it by a word, each callee raises it by what it removes. The address
/// of the block is a value, like the result of a call. In the machine they
/// are one register, because the block is carved out of the stack; nothing in
/// the meaning of the function depends on that.
///
/// For the values a stack pointer is computed from (the entry value, sums
/// with constants, differences with what is not a constant, joins), this
/// tells where each stands: its top, and how far it is from the last value
/// that is not the entry value plus a constant.
class StackTops {
public:
  struct Place {
    /// The top of what the function has pushed, from the stack pointer at
    /// the entry
    int64_t Top = 0;

    /// The last value the stack pointer had that is not the entry value plus
    /// a constant (the result of an allocation, or a join of values that are
    /// not all in the frame), if any, and how far this value is from it. Not
    /// above zero for a stack pointer: an address at or above it lies in
    /// what was allocated.
    llvm::Value *Floor = nullptr;
    int64_t FromFloor = 0;
  };

  /// An allocation: the value of the stack pointer after it, which is the
  /// address of the block
  struct Allocation {
    llvm::Instruction *Result = nullptr;
    llvm::Value *Size = nullptr;

    /// The top of the frame where the block was carved out
    int64_t Top = 0;

    /// The loops the two values the result is the difference of step through
    /// together (the pages of the block, touched one by one)
    llvm::SmallVector<llvm::BasicBlock *, 1> Loops;
  };

private:
  /// What a difference was found to be: a block carved out below a place
  /// (its size is not a constant) or, with a constant, more frame
  struct Carved {
    llvm::Value *From = nullptr;
    llvm::Value *Size = nullptr;
    int64_t Constant = 0;
    llvm::SmallVector<llvm::BasicBlock *, 1> Loops;
  };

private:
  llvm::Value *Entry = nullptr;

  /// The top of each value asked about and of the joins on its way, if known
  llvm::DenseMap<llvm::Value *, std::optional<int64_t>> Tops;

  /// The differences on the way of a value whose top is known
  llvm::DenseMap<llvm::Value *, Carved> Differences;

  /// For a join whose top is known, whether all that reaches it is the entry
  /// value plus a constant
  llvm::DenseMap<llvm::PHINode *, bool> InFrame;

public:
  /// \p Entry is the value the stack pointer has when the function is entered
  explicit StackTops(llvm::Value *Entry) : Entry(Entry) {}

  /// \return where \p V stands, if it is the entry value, such a value plus
  ///         a constant, the result of an allocation below such a value, or
  ///         a join of such values that agree on the top
  std::optional<Place> of(llvm::Value *V) {
    using namespace llvm;

    std::optional<int64_t> Top = topOf(V);
    if (not Top.has_value())
      return std::nullopt;

    // The last value under it that is not the entry value plus a constant
    int64_t FromFloor = 0;
    while (true) {
      auto [Base, Offset] = skipConstantSums(V);
      FromFloor += Offset;
      if (Base == Entry)
        return Place{ *Top, nullptr, 0 };

      if (auto It = Differences.find(Base); It != Differences.end()) {
        if (It->second.Size != nullptr)
          return Place{ *Top, Base, FromFloor };
        FromFloor += It->second.Constant;
        V = It->second.From;
        continue;
      }

      auto *Phi = dyn_cast<PHINode>(Base);
      if (Phi == nullptr)
        return std::nullopt;
      if (isInFrame(Phi))
        return Place{ *Top, nullptr, 0 };
      return Place{ *Top, Phi, FromFloor };
    }
  }

  /// \return the size of the stack \p Size amounts to, if it is the entry
  ///         value minus a stack pointer whose top is known, or a join of
  ///         such sizes that agree
  std::optional<int64_t> sizeOf(llvm::Value *Size, unsigned Depth = 0) {
    using namespace llvm;
    using namespace llvm::PatternMatch;
    constexpr unsigned MostDepth = 8;

    if (auto *Constant = dyn_cast<ConstantInt>(Size))
      return Constant->getSExtValue();
    if (Depth > MostDepth)
      return std::nullopt;

    auto [Base, Offset] = skipConstantSums(Size);
    if (Base != Size) {
      std::optional<int64_t> Inner = sizeOf(Base, Depth + 1);
      if (not Inner.has_value())
        return std::nullopt;
      return *Inner + Offset;
    }

    Value *Pointer = nullptr;
    if (match(Size, m_Sub(m_Specific(Entry), m_Value(Pointer)))) {
      std::optional<Place> Where = of(Pointer);
      if (not Where.has_value())
        return std::nullopt;
      return -Where->Top;
    }

    if (auto *Phi = dyn_cast<PHINode>(Size)) {
      std::optional<int64_t> Common;
      for (Value *Incoming : Phi->incoming_values()) {
        if (Incoming == Phi)
          continue;
        std::optional<int64_t> One = sizeOf(Incoming, Depth + 1);
        if (not One.has_value() or (Common.has_value() and *Common != *One))
          return std::nullopt;
        Common = One;
      }
      return Common;
    }

    return std::nullopt;
  }

  /// \return the top of the frame \p Height amounts to, if it is a stack
  ///         pointer whose top is known minus the entry value, or a join of
  ///         such heights that agree
  std::optional<int64_t> heightOf(llvm::Value *Height, unsigned Depth = 0) {
    using namespace llvm;
    using namespace llvm::PatternMatch;
    constexpr unsigned MostDepth = 8;

    if (auto *Constant = dyn_cast<ConstantInt>(Height))
      return Constant->getSExtValue();
    if (Depth > MostDepth)
      return std::nullopt;

    auto [Base, Offset] = skipConstantSums(Height);
    if (Base != Height) {
      std::optional<int64_t> Inner = heightOf(Base, Depth + 1);
      if (not Inner.has_value())
        return std::nullopt;
      return *Inner + Offset;
    }

    Value *Pointer = nullptr;
    if (match(Height, m_Sub(m_Value(Pointer), m_Specific(Entry)))) {
      std::optional<Place> Where = of(Pointer);
      if (not Where.has_value())
        return std::nullopt;
      return Where->Top;
    }

    if (auto *Phi = dyn_cast<PHINode>(Height)) {
      std::optional<int64_t> Common;
      for (Value *Incoming : Phi->incoming_values()) {
        if (Incoming == Phi)
          continue;
        std::optional<int64_t> One = heightOf(Incoming, Depth + 1);
        if (not One.has_value() or (Common.has_value() and *Common != *One))
          return std::nullopt;
        Common = One;
      }
      return Common;
    }

    return std::nullopt;
  }

  /// \return the allocations met so far, in the order of the instructions of
  ///         \p F
  std::vector<Allocation> allocations(llvm::Function &F) const {
    return differences(F, true);
  }

  /// \return the differences met so far that amount to more frame (an
  ///         allocation of a constant size, through the loop that touches
  ///         its pages: LLVM does not see that it is the entry value plus a
  ///         constant)
  std::vector<Allocation> folded(llvm::Function &F) const {
    return differences(F, false);
  }

  /// \return the joins met so far whose top is known and that are not in
  ///         the frame on every path
  std::vector<llvm::PHINode *> joins(llvm::Function &F) {
    std::vector<llvm::PHINode *> Result;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I);
      if (Phi == nullptr)
        continue;
      auto It = Tops.find(Phi);
      if (It != Tops.end() and It->second.has_value() and not isInFrame(Phi))
        Result.push_back(Phi);
    }
    return Result;
  }

private:
  std::vector<Allocation> differences(llvm::Function &F, bool Blocks) const {
    std::vector<Allocation> Result;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      auto It = Differences.find(&I);
      if (It == Differences.end() or (It->second.Size != nullptr) != Blocks)
        continue;
      auto Top = Tops.find(&I);
      if (Top == Tops.end() or not Top->second.has_value())
        continue;
      Result.push_back({ &I, It->second.Size, *Top->second, It->second.Loops });
    }
    return Result;
  }

  /// \return what the difference \p V is, if it is one this analysis follows:
  ///         a place minus something
  std::optional<Carved> carvedBy(llvm::Value *V) const {
    using namespace llvm;
    using namespace llvm::PatternMatch;

    Value *A = nullptr;
    Value *B = nullptr;
    if (not match(V, m_Sub(m_Value(A), m_Value(B))))
      return std::nullopt;

    Carved Result;
    std::optional<ValueDifference> Both = differenceOf(A, B, &Result.Loops);
    if (not Both.has_value())
      return std::nullopt;

    Result.From = Both->Minuend;
    Result.Constant = Both->Constant;
    if (auto *Size = dyn_cast<ConstantInt>(Both->Subtrahend)) {
      if (Size->getBitWidth() > 64)
        return std::nullopt;
      Result.Constant -= Size->getSExtValue();
    } else {
      Result.Size = Both->Subtrahend;
    }
    return Result;
  }

  /// The top of \p Root: the joins it is computed from are worked out
  /// together, since those of a loop refer to each other. Each value on the
  /// way is the root's top minus the constants met between the two; a join
  /// reached twice has to be reached with the same (going around a loop
  /// brings the top back to where it was), and all that enters has to be the
  /// entry value. What a block was carved out of is on the way like a sum
  /// with a constant: the top is not lowered by the block.
  std::optional<int64_t> topOf(llvm::Value *Root) {
    using namespace llvm;
    constexpr unsigned MostVisited = 4096;

    if (auto It = Tops.find(Root); It != Tops.end())
      return It->second;

    // For each value on the way, what the root's top is its top plus
    SmallDenseMap<Value *, int64_t, 16> Above;
    SmallDenseMap<Value *, Carved, 4> Carvings;
    SmallVector<std::pair<Value *, int64_t>, 16> Queue;
    std::optional<int64_t> Top;
    bool IsKnown = true;
    unsigned Visited = 0;
    Queue.emplace_back(Root, 0);
    while (IsKnown and not Queue.empty()) {
      auto [Next, Sum] = Queue.pop_back_val();
      IsKnown = ++Visited <= MostVisited;

      auto [Base, Offset] = skipConstantSums(Next);
      Sum += Offset;

      std::optional<int64_t> Leaf;
      if (Base == Entry) {
        Leaf = 0;
      } else if (auto It = Tops.find(Base); It != Tops.end()) {
        IsKnown = IsKnown and It->second.has_value();
        Leaf = It->second;
      }

      if (Leaf.has_value()) {
        // The root's top, as this way of reaching the entry value has it
        IsKnown = IsKnown and (not Top.has_value() or *Top == *Leaf + Sum);
        Top = *Leaf + Sum;
        continue;
      }

      if (not IsKnown)
        break;

      auto [It, IsNew] = Above.try_emplace(Base, Sum);
      if (not IsNew) {
        IsKnown = It->second == Sum;
        continue;
      }

      if (auto *Phi = dyn_cast<PHINode>(Base)) {
        for (Value *Incoming : Phi->incoming_values())
          if (Incoming != Phi)
            Queue.emplace_back(Incoming, Sum);
      } else if (std::optional<Carved> Difference = carvedBy(Base)) {
        Queue.emplace_back(Difference->From, Sum + Difference->Constant);
        Carvings[Base] = std::move(*Difference);
      } else {
        IsKnown = false;
      }
    }

    if (not IsKnown or not Top.has_value()) {
      Tops[Root] = std::nullopt;
      return std::nullopt;
    }

    for (const auto &[Value, Sum] : Above)
      Tops[Value] = *Top - Sum;
    for (auto &[Value, Difference] : Carvings)
      Differences[Value] = std::move(Difference);
    Tops[Root] = *Top;
    return *Top;
  }

  /// \return whether \p Root, a join whose top is known, is the entry value
  ///         plus a constant whichever way it is reached: no block was
  ///         carved out on any of them
  bool isInFrame(llvm::PHINode *Root) {
    using namespace llvm;

    if (auto It = InFrame.find(Root); It != InFrame.end())
      return It->second;

    SmallPtrSet<PHINode *, 8> Joins;
    SmallVector<Value *, 16> Queue{ Root };
    bool Result = true;
    while (Result and not Queue.empty()) {
      Value *Base = skipConstantSums(Queue.pop_back_val()).first;
      if (Base == Entry)
        continue;

      if (auto *Phi = dyn_cast<PHINode>(Base)) {
        if (auto It = InFrame.find(Phi); It != InFrame.end()) {
          Result = It->second;
        } else if (Joins.insert(Phi).second) {
          for (Value *Incoming : Phi->incoming_values())
            if (Incoming != Phi)
              Queue.push_back(Incoming);
        }
        continue;
      }

      auto It = Differences.find(Base);
      Result = It != Differences.end() and It->second.Size == nullptr;
      if (Result)
        Queue.push_back(It->second.From);
    }

    // All the joins on the way are in the frame if the root is; if it is not,
    // only the root is known not to be
    if (Result)
      for (PHINode *Phi : Joins)
        InFrame[Phi] = true;
    InFrame[Root] = Result;
    return Result;
  }
};
