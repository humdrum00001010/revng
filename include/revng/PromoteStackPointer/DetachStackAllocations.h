#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>
#include <utility>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"

#include "revng/Support/StackTops.h"

// Only LLVM is used in this file: the rewrite can be compiled on its own and
// run on functions kept from a run.

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
