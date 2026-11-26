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
    /// not one such thing), if any, and how far this value is from it. Not
    /// above zero for a stack pointer: an address at or above the result of
    /// an allocation lies in what was allocated.
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

  /// What all that reaches a join is, if it is one thing: the entry value
  /// plus a constant (no floor), or one floor at one distance
  using Common = std::optional<std::pair<llvm::Value *, int64_t>>;

private:
  /// The values the stack pointer has when the function is entered (one
  /// register, read as a number or as a pointer)
  llvm::SmallPtrSet<llvm::Value *, 2> Entries;

  /// Whether a value is the value a cast is applied to, and a pointer plus a
  /// constant offset a sum: so it is where the stack pointer is a pointer
  bool ThroughCasts = false;

  /// Whether the values asked about are heights: stack pointers the entry
  /// value has been subtracted from, in the form LLVM leaves them in once the
  /// entry value has cancelled out. A number is then what the entry value
  /// plus that number is otherwise.
  bool AreHeights = false;

  /// The top of each value asked about and of the joins on its way, if known
  llvm::DenseMap<llvm::Value *, std::optional<int64_t>> Tops;

  /// The differences on the way of a value whose top is known
  llvm::DenseMap<llvm::Value *, Carved> Differences;

  /// For the joins asked about, see Common
  llvm::DenseMap<llvm::PHINode *, Common> Reaching;

public:
  /// \p Entry is the value the stack pointer has when the function is entered
  explicit StackTops(llvm::Value *Entry, bool ThroughCasts = false) :
    ThroughCasts(ThroughCasts) {
    addEntry(Entry);
  }

  /// For heights, see AreHeights: there is no entry value
  static StackTops ofHeights(bool ThroughCasts = false) {
    StackTops Result(nullptr, ThroughCasts);
    Result.AreHeights = true;
    return Result;
  }

  /// \p Entry is the stack pointer at the entry too: the register read once
  /// more, or as another type
  void addEntry(llvm::Value *Entry) {
    if (Entry != nullptr)
      Entries.insert(skip(Entry).first);
  }

  /// \return where \p V stands, if it is the entry value, such a value plus
  ///         a constant, the result of an allocation below such a value, or
  ///         a join of such values that agree on the top
  std::optional<Place> of(llvm::Value *V) {
    using namespace llvm;
    constexpr unsigned MostSteps = 1024;

    std::optional<int64_t> Top = topOf(V);
    if (not Top.has_value())
      return std::nullopt;

    // The last value under it that is not the entry value plus a constant
    int64_t FromFloor = 0;
    for (unsigned Step = 0; Step < MostSteps; ++Step) {
      auto [Base, Offset] = skip(V);
      FromFloor += Offset;
      if (Entries.contains(Base))
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

      // A join of one thing is that thing
      Common One = reaching(Phi);
      if (not One.has_value())
        return Place{ *Top, Phi, FromFloor };
      if (One->first == nullptr)
        return Place{ *Top, nullptr, 0 };
      return Place{ *Top, One->first, FromFloor + One->second };
    }

    return std::nullopt;
  }

  /// \return the size of the stack \p Size amounts to, if it is the entry
  ///         value minus a stack pointer whose top is known, or a join of
  ///         such sizes that agree
  std::optional<int64_t> sizeOf(llvm::Value *Size) {
    std::optional<int64_t> Height = difference(Size, false, 0);
    if (not Height.has_value())
      return std::nullopt;
    return -*Height;
  }

  /// \return the top of the frame \p Height amounts to, if it is a stack
  ///         pointer whose top is known minus the entry value, or a join of
  ///         such heights that agree
  std::optional<int64_t> heightOf(llvm::Value *Height) {
    return difference(Height, true, 0);
  }

  /// \return the top of the frame the height \p Height amounts to, see
  ///         ofHeights: the number it is, without the blocks carved out
  std::optional<int64_t> ofHeight(llvm::Value *Height) {
    if (not AreHeights)
      return std::nullopt;
    return topOf(Height);
  }

  /// \return how much lies in the frame above the stack pointer \p Size is
  ///         the size of, as sizeOf takes it, before a block allocated at
  ///         run time begins: what the function pushed since. The least of
  ///         the ways the place is reached; nothing if no block lies above
  ///         it whichever way it is reached.
  std::optional<int64_t> roomAboveSize(llvm::Value *Size, unsigned Depth = 0) {
    using namespace llvm;
    constexpr unsigned MostDepth = 8;

    auto [Base, Offset] = skip(Size);
    if (Depth > MostDepth or isa<ConstantInt>(Base))
      return std::nullopt;

    if (auto *Operator = dyn_cast<BinaryOperator>(Base);
        Operator != nullptr and Operator->getOpcode() == Instruction::Sub) {
      auto [Origin, Above] = skip(Operator->getOperand(0));
      if (not Entries.contains(Origin))
        return std::nullopt;

      // (the stack pointer is what was subtracted, lowered by the constants
      // around the difference)
      unsigned Steps = 0;
      SmallPtrSet<PHINode *, 4> OnTheWay;
      std::optional<int64_t> Room = roomAbove(Operator->getOperand(1),
                                              OnTheWay,
                                              Steps);
      if (not Room.has_value())
        return std::nullopt;
      return *Room + Above + Offset;
    }

    std::optional<int64_t> Least;
    if (auto *Phi = dyn_cast<PHINode>(Base)) {
      for (Value *Incoming : Phi->incoming_values()) {
        if (Incoming == Phi)
          continue;
        std::optional<int64_t> One = roomAboveSize(Incoming, Depth + 1);
        if (One.has_value() and (not Least.has_value() or *One < *Least))
          Least = One;
      }
    }
    if (not Least.has_value())
      return std::nullopt;
    return *Least + Offset;
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

  /// \return the joins met so far whose top is known and that are not one
  ///         thing whichever way they are reached: floors of their own
  std::vector<llvm::PHINode *> joins(llvm::Function &F) {
    std::vector<llvm::PHINode *> Result;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I);
      if (Phi == nullptr or Phi->getNumIncomingValues() < 2)
        continue;
      auto It = Tops.find(Phi);
      if (It != Tops.end() and It->second.has_value()
          and not reaching(Phi).has_value())
        Result.push_back(Phi);
    }
    return Result;
  }

  /// \return \p V without what stands between it and the value it is a sum
  ///         with a constant of, and that constant
  std::pair<llvm::Value *, int64_t> skip(llvm::Value *V) const {
    return skipConstantSums(V, ThroughCasts);
  }

private:
  /// \return how many bytes above the stack pointer \p Pointer are in the
  ///         frame before a block begins, the least of the ways it is
  ///         reached; nothing if no block lies above it. Not above zero
  ///         where the place is in a block, and zero where too much would
  ///         have to be looked at.
  std::optional<int64_t> roomAbove(llvm::Value *Pointer,
                                   llvm::SmallPtrSetImpl<llvm::PHINode *> &Way,
                                   unsigned &Steps) {
    using namespace llvm;
    constexpr unsigned MostSteps = 4096;

    std::optional<Place> Where = of(Pointer);
    if (not Where.has_value() or Where->Floor == nullptr)
      return std::nullopt;

    auto *Join = dyn_cast<PHINode>(Where->Floor);
    if (Join == nullptr)
      return -Where->FromFloor;

    // A join of its own: what is above it depends on the way it is reached
    if (++Steps > MostSteps)
      return 0;
    if (not Way.insert(Join).second)
      return std::nullopt;

    std::optional<int64_t> Least;
    for (Value *Incoming : Join->incoming_values()) {
      if (Incoming == Join)
        continue;
      std::optional<int64_t> One = roomAbove(Incoming, Way, Steps);
      if (One.has_value() and (not Least.has_value() or *One < *Least))
        Least = One;
    }
    Way.erase(Join);

    if (not Least.has_value())
      return std::nullopt;
    return *Least - Where->FromFloor;
  }

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

  /// \return what \p V amounts to if it is a stack pointer whose top is
  ///         known minus the entry value (\p IsHeight) or the entry value
  ///         minus such a stack pointer, plus constants, or a join of such
  ///         that agree: in both cases as a height (the top)
  std::optional<int64_t>
  difference(llvm::Value *V, bool IsHeight, unsigned Depth) {
    using namespace llvm;
    constexpr unsigned MostDepth = 8;

    if (auto *Constant = dyn_cast<ConstantInt>(V)) {
      if (Constant->getBitWidth() > 64)
        return std::nullopt;
      return IsHeight ? Constant->getSExtValue() : -Constant->getSExtValue();
    }
    if (Depth > MostDepth)
      return std::nullopt;

    // (a sum with a constant outside the difference: for a size, the
    // constant is of the size)
    auto [Base, Offset] = skip(V);
    int64_t Outside = IsHeight ? Offset : -Offset;

    if (auto *Operator = dyn_cast<BinaryOperator>(Base);
        Operator != nullptr and Operator->getOpcode() == Instruction::Sub) {
      Value *Pointer = Operator->getOperand(IsHeight ? 0 : 1);
      auto [Origin, Above] = skip(Operator->getOperand(IsHeight ? 1 : 0));
      if (not Entries.contains(Origin))
        return std::nullopt;
      std::optional<Place> Where = of(Pointer);
      if (not Where.has_value())
        return std::nullopt;
      return Where->Top - Above + Outside;
    }

    if (auto *Phi = dyn_cast<PHINode>(Base)) {
      std::optional<int64_t> Agreed;
      for (Value *Incoming : Phi->incoming_values()) {
        if (Incoming == Phi)
          continue;
        std::optional<int64_t> One = difference(Incoming, IsHeight, Depth + 1);
        if (not One.has_value() or (Agreed.has_value() and *Agreed != *One))
          return std::nullopt;
        Agreed = One;
      }
      if (not Agreed.has_value())
        return std::nullopt;
      return *Agreed + Outside;
    }

    return std::nullopt;
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
    std::optional<ValueDifference> Both = differenceOf(A,
                                                       B,
                                                       &Result.Loops,
                                                       ThroughCasts);
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

      auto [Base, Offset] = skip(Next);
      Sum += Offset;

      std::optional<int64_t> Leaf;
      auto *Number = dyn_cast<ConstantInt>(Base);
      if (Entries.contains(Base)) {
        Leaf = 0;
      } else if (AreHeights and Number != nullptr
                 and Number->getBitWidth() <= 64) {
        Leaf = Number->getSExtValue();
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

  /// \return what all that reaches \p Root, a join whose top is known, is,
  ///         if it is one thing: no value and 0 if it is the entry value
  ///         plus a constant whichever way the join is reached, an
  ///         allocation and what the join is that allocation plus if every
  ///         way leads to that allocation with that distance. Nothing if the
  ///         ways differ: the join is then a floor of its own.
  Common reaching(llvm::PHINode *Root) {
    using namespace llvm;
    constexpr unsigned MostVisited = 4096;

    if (auto It = Reaching.find(Root); It != Reaching.end())
      return It->second;

    SmallDenseMap<PHINode *, int64_t, 8> Joins;
    SmallVector<std::pair<Value *, int64_t>, 16> Queue;
    Queue.emplace_back(Root, 0);
    Common Result;
    bool IsOne = true;
    unsigned Visited = 0;
    while (IsOne and not Queue.empty()) {
      auto [Next, Sum] = Queue.pop_back_val();
      IsOne = ++Visited <= MostVisited;

      auto [Base, Offset] = skip(Next);
      Sum += Offset;

      std::pair<Value *, int64_t> Leaf{ nullptr, 0 };
      if (Entries.contains(Base)) {
        // (the top says which constant)
      } else if (auto *Phi = dyn_cast<PHINode>(Base)) {
        auto [It, IsNew] = Joins.try_emplace(Phi, Sum);
        IsOne = IsOne and It->second == Sum;
        if (IsNew)
          for (Value *Incoming : Phi->incoming_values())
            if (Incoming != Phi)
              Queue.emplace_back(Incoming, Sum);
        continue;
      } else if (auto It = Differences.find(Base); It != Differences.end()) {
        if (It->second.Size == nullptr) {
          Queue.emplace_back(It->second.From, Sum + It->second.Constant);
          continue;
        }
        Leaf = { Base, Sum };
      } else {
        IsOne = false;
      }

      IsOne = IsOne and (not Result.has_value() or *Result == Leaf);
      Result = Leaf;
    }

    if (not IsOne)
      Result = std::nullopt;

    // The joins on the way are that one thing too, each at its distance; if
    // the ways differ, only the root is known to be a floor of its own
    if (Result.has_value())
      for (const auto &[Phi, Sum] : Joins)
        Reaching[Phi] = Common({ Result->first, Result->second - Sum });
    Reaching[Root] = Result;
    return Result;
  }
};
