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
#include "llvm/Transforms/Utils/Local.h"

#include "revng/Support/SameOnAllPaths.h"

// Only LLVM is used in this file: the analysis and the rewrite can be compiled
// on their own and run on functions kept from a run.

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
  struct Found {
    std::optional<Place> Where;

    /// For the result of an allocation: its size
    llvm::Value *Size = nullptr;

    /// Whether this is a difference that amounts to the entry value plus a
    /// constant, which LLVM does not see (an allocation of a constant size)
    bool IsFolded = false;

    llvm::SmallVector<llvm::BasicBlock *, 1> Loops;
  };

  using Level = llvm::DenseMap<llvm::Value *, Found>;

private:
  llvm::Value *Entry = nullptr;
  Level Known;

  /// What was found while a phi was taken to have the top that enters its
  /// loop, innermost last: kept if the phi turns out to have it
  std::vector<Level> Assumed;

  /// The phis being worked out that nothing is assumed about yet, and those
  /// of them met since the value being looked at was started: what depends
  /// on one is not known yet
  llvm::SmallPtrSet<llvm::PHINode *, 8> Open;
  llvm::SmallPtrSet<llvm::PHINode *, 8> Met;

  unsigned Work = 0;

public:
  /// \p Entry is the value the stack pointer has when the function is entered
  explicit StackTops(llvm::Value *Entry) : Entry(Entry) {}

  /// \return where \p V stands, if it is the entry value, such a value plus
  ///         a constant, the result of an allocation below such a value, or
  ///         a join of such values that agree on the top
  std::optional<Place> of(llvm::Value *V) { return find(V).Where; }

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

  /// \return the allocations met so far, in the order of the instructions of
  ///         \p F
  std::vector<Allocation> allocations(llvm::Function &F) const {
    std::vector<Allocation> Result;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      auto It = Known.find(&I);
      if (It != Known.end() and It->second.Size != nullptr)
        Result.push_back({ &I,
                           It->second.Size,
                           It->second.Where->Top,
                           It->second.Loops });
    }
    return Result;
  }

  /// \return the differences met so far that amount to the entry value plus
  ///         a constant, with that constant and the loops they step through
  std::vector<Allocation> folded(llvm::Function &F) const {
    std::vector<Allocation> Result;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      auto It = Known.find(&I);
      if (It != Known.end() and It->second.IsFolded)
        Result
          .push_back({ &I, nullptr, It->second.Where->Top, It->second.Loops });
    }
    return Result;
  }

  /// \return the joins met so far whose values are not all in the frame
  std::vector<llvm::PHINode *> joins(llvm::Function &F) const {
    std::vector<llvm::PHINode *> Result;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I);
      if (Phi == nullptr)
        continue;
      auto It = Known.find(Phi);
      if (It != Known.end() and It->second.Where.has_value()
          and It->second.Where->Floor == Phi)
        Result.push_back(Phi);
    }
    return Result;
  }

private:
  const Found *lookup(llvm::Value *V) const {
    for (auto It = Assumed.rbegin(); It != Assumed.rend(); ++It)
      if (auto Old = It->find(V); Old != It->end())
        return &Old->second;
    if (auto Old = Known.find(V); Old != Known.end())
      return &Old->second;
    return nullptr;
  }

  void store(llvm::Value *V, Found New) {
    if (Assumed.empty())
      Known[V] = std::move(New);
    else
      Assumed.back()[V] = std::move(New);
  }

  Found find(llvm::Value *V) {
    constexpr unsigned MostWork = 100000;

    if (const Found *Old = lookup(V))
      return *Old;

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(V); Open.contains(Phi)) {
      Met.insert(Phi);
      return {};
    }

    if (++Work > MostWork)
      return {};

    // What depends on a phi nothing is assumed about yet is found again
    // once something is
    auto Before = std::move(Met);
    Met.clear();
    Found New = compute(V);
    bool IsSettled = Met.empty();
    Met.insert(Before.begin(), Before.end());
    if (IsSettled)
      store(V, New);
    return New;
  }

  Found compute(llvm::Value *V) {
    using namespace llvm;
    using namespace llvm::PatternMatch;

    if (V == Entry)
      return { Place{}, nullptr, false, {} };

    auto [Base, Offset] = skipConstantSums(V);
    if (Base != V) {
      std::optional<Place> Inner = find(Base).Where;
      if (not Inner.has_value())
        return {};
      Inner->Top += Offset;
      if (Inner->Floor != nullptr)
        Inner->FromFloor += Offset;
      return { Inner, nullptr, false, {} };
    }

    Value *A = nullptr;
    Value *B = nullptr;
    if (match(V, m_Sub(m_Value(A), m_Value(B)))) {
      Found Result;
      std::optional<ValueDifference> Both = differenceOf(A, B, &Result.Loops);
      if (not Both.has_value())
        return {};
      std::optional<Place> From = find(Both->Minuend).Where;
      if (not From.has_value())
        return {};

      From->Top += Both->Constant;
      if (auto *Size = dyn_cast<ConstantInt>(Both->Subtrahend)) {
        // A constant: more frame
        if (Size->getBitWidth() > 64)
          return {};
        From->Top -= Size->getSExtValue();
        if (From->Floor != nullptr)
          From->FromFloor += Both->Constant - Size->getSExtValue();
        else
          Result.IsFolded = true;
        Result.Where = From;
        return Result;
      }

      // Not a constant: from here down is a block of its own, and the top
      // of the frame stays where it was
      Result.Where = Place{ From->Top, V, 0 };
      Result.Size = Both->Subtrahend;
      return Result;
    }

    if (auto *Phi = dyn_cast<PHINode>(V))
      return join(Phi);

    return {};
  }

  /// The paths agree on the top, or nothing is known. What comes around a
  /// loop is taken to agree with what enters it, and then has to.
  Found join(llvm::PHINode *Phi) {
    using namespace llvm;

    Open.insert(Phi);
    std::optional<int64_t> Top;
    bool AllInFrame = true;
    bool Agree = true;
    SmallVector<Value *, 4> Around;
    for (Value *Incoming : Phi->incoming_values()) {
      if (Incoming == Phi)
        continue;

      bool WasMet = Met.contains(Phi);
      Met.erase(Phi);
      std::optional<Place> One = find(Incoming).Where;
      bool ComesAround = Met.contains(Phi);
      if (WasMet)
        Met.insert(Phi);
      if (ComesAround) {
        Around.push_back(Incoming);
        continue;
      }

      Agree = Agree and One.has_value()
              and (not Top.has_value() or *Top == One->Top);
      if (not Agree)
        break;
      Top = One->Top;
      AllInFrame = AllInFrame and One->Floor == nullptr;
    }
    Open.erase(Phi);
    Met.erase(Phi);

    if (not Agree or not Top.has_value())
      return {};

    // First as a place of the frame, if all that enters is: an allocation on
    // the way around makes it a floor
    for (bool AsFloor : { not AllInFrame, true }) {
      if (Around.empty())
        break;

      Place Taken{ *Top, AsFloor ? Phi : nullptr, 0 };
      Assumed.emplace_back();
      Assumed.back()[Phi] = Found{ Taken, nullptr, false, {} };
      bool Holds = true;
      bool StaysInFrame = true;
      for (Value *Incoming : Around) {
        std::optional<Place> One = find(Incoming).Where;
        Holds = Holds and One.has_value() and One->Top == *Top;
        StaysInFrame = StaysInFrame and One.has_value()
                       and One->Floor == nullptr;
      }

      Level Under = std::move(Assumed.back());
      Assumed.pop_back();
      if (not Holds)
        return {};

      if (not AsFloor and not StaysInFrame) {
        AllInFrame = false;
        continue;
      }

      // It held: so does what was found under it
      Under.erase(Phi);
      for (auto &[Value, What] : Under)
        store(Value, std::move(What));
      AllInFrame = not AsFloor;
      break;
    }

    return { Place{ *Top, AllInFrame ? nullptr : Phi, 0 }, nullptr, false, {} };
  }
};

/// What detachStackAllocations did
struct DetachedStackAllocations {
  unsigned Blocks = 0;
  unsigned Folded = 0;
  unsigned ToFrame = 0;
  unsigned IntoBlocks = 0;
  unsigned Sizes = 0;
  unsigned Loops = 0;

  bool changed() const {
    return Blocks + Folded + ToFrame + IntoBlocks + Sizes + Loops != 0;
  }
};

/// Give each block \p F allocates on its stack at run time an existence of
/// its own, and make what lies below it places of the frame again.
///
/// The calls to \p SizeMarker take the stack pointer at the entry, \p Entry,
/// minus the stack pointer at a call. Where that is not a constant, the
/// stack pointer is followed back. Then:
/// - the result of an allocation becomes the result of a call to
///   \p Allocator, which takes the size and returns the block;
/// - a value at a known distance d from a floor (an allocation, or a join of
///   stack pointers that agree on the top) becomes the entry value plus the
///   top if d < 0, which is what the function pushed after it, and stays the
///   floor plus d otherwise, which is inside what was allocated;
/// - the argument of the marker becomes the constant the top says;
/// - a loop that only stepped through the pages of the block is left at
///   once.
///
/// Where the tops of the paths that join do not agree nothing is done: what
/// one of the callees on the way removes is not known, and the size stays
/// unknown there.
inline DetachedStackAllocations
detachStackAllocations(llvm::Function &F,
                       llvm::Instruction *Entry,
                       llvm::Function *SizeMarker,
                       llvm::FunctionCallee Allocator) {
  using namespace llvm;

  DetachedStackAllocations Result;
  Type *Integer = Entry->getType();
  StackTops Tops(Entry);

  // The sizes first: everything is asked while the function is intact
  SmallVector<std::pair<CallBase *, int64_t>, 16> Sizes;
  for (Instruction &I : instructions(F)) {
    auto *Call = dyn_cast<CallBase>(&I);
    if (Call == nullptr or Call->getCalledFunction() != SizeMarker)
      continue;
    Value *Size = Call->getArgOperand(0);
    if (isa<ConstantInt>(Size))
      continue;
    if (std::optional<int64_t> Known = Tops.sizeOf(Size))
      Sizes.emplace_back(Call, *Known);
  }

  if (Sizes.empty())
    return Result;

  std::vector<StackTops::Allocation> Allocations = Tops.allocations(F);
  std::vector<StackTops::Allocation> Folded = Tops.folded(F);

  // The values that stand at a known distance from a floor: those computed
  // from one by sums with constants
  SmallPtrSet<Value *, 8> Floors;
  for (const StackTops::Allocation &A : Allocations)
    Floors.insert(A.Result);
  for (PHINode *Phi : Tops.joins(F))
    Floors.insert(Phi);

  struct Change {
    Instruction *Old = nullptr;
    StackTops::Place Where;
  };
  SmallVector<Change, 32> Changes;
  {
    SmallVector<Value *, 32> Queue(Floors.begin(), Floors.end());
    SmallPtrSet<Value *, 32> Visited;
    while (not Queue.empty()) {
      Value *Next = Queue.pop_back_val();
      if (not Visited.insert(Next).second)
        continue;
      for (User *U : Next->users()) {
        auto *I = dyn_cast<Instruction>(U);
        if (I == nullptr or Floors.contains(I)
            or skipConstantSums(I).first == I)
          continue;
        std::optional<StackTops::Place> Where = Tops.of(I);
        if (not Where.has_value() or Where->Floor == nullptr
            or not Floors.contains(Where->Floor))
          continue;
        Changes.push_back({ I, *Where });
        Queue.push_back(I);
      }
    }

    // (In the order of the instructions: what is done does not depend on
    // where things are in memory)
    DenseMap<Instruction *, unsigned> Order;
    for (Instruction &I : instructions(F))
      Order[&I] = Order.size();
    llvm::sort(Changes, [&Order](const Change &Left, const Change &Right) {
      return Order.lookup(Left.Old) < Order.lookup(Right.Old);
    });
  }

  auto Before = [](Instruction *I) -> Instruction * {
    return isa<PHINode>(I) ? I->getParent()->getFirstNonPHI() : I;
  };

  auto InFrame = [&](Instruction *Old, int64_t Top) -> Value * {
    auto *New = BinaryOperator::CreateAdd(Entry,
                                          ConstantInt::getSigned(Integer, Top),
                                          "",
                                          Before(Old));
    New->setDebugLoc(Old->getDebugLoc());
    return New;
  };

  // The blocks
  DenseMap<Value *, Value *> BlockOf;
  for (const StackTops::Allocation &A : Allocations) {
    auto *Block = CallInst::Create(Allocator, { A.Size }, "", Before(A.Result));
    Block->setDebugLoc(A.Result->getDebugLoc());
    BlockOf[A.Result] = Block;
    ++Result.Blocks;
  }

  SmallVector<Instruction *, 32> Replaced;
  for (const Change &C : Changes) {
    Value *New = nullptr;
    if (C.Where.FromFloor < 0) {
      New = InFrame(C.Old, C.Where.Top);
      ++Result.ToFrame;
    } else {
      Value *Floor = C.Where.Floor;
      if (auto It = BlockOf.find(Floor); It != BlockOf.end())
        Floor = It->second;
      New = Floor;
      if (C.Where.FromFloor != 0) {
        auto *Offset = ConstantInt::getSigned(Integer, C.Where.FromFloor);
        auto *Sum = BinaryOperator::CreateAdd(Floor, Offset, "", Before(C.Old));
        Sum->setDebugLoc(C.Old->getDebugLoc());
        New = Sum;
      }
      ++Result.IntoBlocks;
    }
    C.Old->replaceAllUsesWith(New);
    Replaced.push_back(C.Old);
  }

  for (const StackTops::Allocation &A : Allocations) {
    A.Result->replaceAllUsesWith(BlockOf[A.Result]);
    Replaced.push_back(A.Result);
  }

  // The places of the frame LLVM does not see as such
  for (const StackTops::Allocation &A : Folded) {
    A.Result->replaceAllUsesWith(InFrame(A.Result, A.Top));
    Replaced.push_back(A.Result);
    ++Result.Folded;
  }

  for (auto &[Call, Size] : Sizes) {
    Call->setArgOperand(0, ConstantInt::getSigned(Integer, Size));
    ++Result.Sizes;
  }

  // The loops that stepped through the pages: collect them before what
  // they computed is deleted
  SmallVector<BasicBlock *, 4> Loops;
  for (const auto *List : { &Allocations, &Folded })
    for (const StackTops::Allocation &A : *List)
      for (BasicBlock *Loop : A.Loops)
        if (not llvm::is_contained(Loops, Loop))
          Loops.push_back(Loop);

  for (Instruction *Old : Replaced)
    RecursivelyDeleteTriviallyDeadInstructions(Old);

  // A loop of one block that writes nothing, calls nothing, and computes
  // nothing that is used after it is left at once. (It ends in the function
  // as it was: it counted the pages of the block down.)
  for (BasicBlock *Loop : Loops) {
    auto *Branch = dyn_cast<BranchInst>(Loop->getTerminator());
    if (Branch == nullptr or not Branch->isConditional())
      continue;
    BasicBlock *Exit = nullptr;
    if (Branch->getSuccessor(0) == Loop and Branch->getSuccessor(1) != Loop)
      Exit = Branch->getSuccessor(1);
    else if (Branch->getSuccessor(1) == Loop
             and Branch->getSuccessor(0) != Loop)
      Exit = Branch->getSuccessor(0);
    if (Exit == nullptr)
      continue;

    bool IsIdle = true;
    for (Instruction &I : *Loop) {
      IsIdle = IsIdle and (&I == Branch or not I.mayHaveSideEffects());
      for (User *U : I.users()) {
        auto *UserI = dyn_cast<Instruction>(U);
        IsIdle = IsIdle and UserI != nullptr and UserI->getParent() == Loop;
      }
    }
    if (not IsIdle)
      continue;

    for (PHINode &Phi : Loop->phis())
      Phi.removeIncomingValue(Loop, false);
    auto *Leave = BranchInst::Create(Exit, Branch);
    Leave->setDebugLoc(Branch->getDebugLoc());
    Value *Condition = Branch->getCondition();
    Branch->eraseFromParent();
    if (auto *ConditionI = dyn_cast<Instruction>(Condition))
      RecursivelyDeleteTriviallyDeadInstructions(ConditionI);
    ++Result.Loops;
  }

  return Result;
}
