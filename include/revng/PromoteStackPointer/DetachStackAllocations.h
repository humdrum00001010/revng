#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
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
#include "llvm/IR/Metadata.h"
#include "llvm/Transforms/Utils/Local.h"

#include "revng/Support/StackTops.h"

// Only LLVM is used in this file: the rewrite can be compiled on its own and
// run on functions kept from a run.

/// The metadata a call that marks the size of the stack at a call site gets
/// where a block allocated at run time lies above the stack pointer: how
/// many bytes above the stack pointer are in the frame before the block
/// begins. Arguments that would lie further up are in the block, not in the
/// frame.
inline constexpr const char *StackRoomMetadata = "revng.stack.room";

/// \return what StackRoomMetadata says on \p Marker, if it is there
inline std::optional<int64_t> stackRoomOf(const llvm::Instruction &Marker) {
  using namespace llvm;
  MDNode *Node = Marker.getMetadata(StackRoomMetadata);
  if (Node == nullptr or Node->getNumOperands() != 1)
    return std::nullopt;
  auto *Room = mdconst::dyn_extract<ConstantInt>(Node->getOperand(0));
  if (Room == nullptr or Room->getBitWidth() > 64)
    return std::nullopt;
  return Room->getSExtValue();
}

/// What detachStackAllocations did
struct DetachedStackAllocations {
  unsigned Blocks = 0;
  unsigned Folded = 0;
  unsigned ToFrame = 0;
  unsigned IntoBlocks = 0;
  unsigned Sizes = 0;
  unsigned Loops = 0;

  /// Why the function was left as it was although something was found, if it
  /// was
  const char *Refused = nullptr;

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
///   stack pointers that agree on the top and are not one thing) becomes the
///   entry value plus the top if d < 0, which is what the function pushed
///   after it, and the floor plus d otherwise, which is inside what was
///   allocated;
/// - the argument of the marker becomes the constant the top says, and the
///   marker is told how much of the frame lies above the stack pointer
///   before a block begins, see StackRoomMetadata;
/// - a loop that only stepped through the pages of the block is left at
///   once.
///
/// What is renamed are places: a value keeps naming the same word of the
/// frame or of a block, though no longer the same number, since the blocks
/// are no longer between the parts of the frame. So nothing is done to a
/// function that could tell the difference:
/// - one that computes an address from one below a floor with something that
///   is not a constant (such an address can be on either side of the floor);
/// - one that goes up from a join of its own, where one of the ways into the
///   join comes from below a floor that is then crossed.
/// A size whose top the paths do not agree on stays as it is: what one of
/// the callees on the way removes is not known.
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
  struct Site {
    CallBase *Call = nullptr;
    int64_t Size = 0;
    std::optional<int64_t> Room;
  };
  SmallVector<Site, 16> Sizes;
  for (Instruction &I : instructions(F)) {
    auto *Call = dyn_cast<CallBase>(&I);
    if (Call == nullptr or Call->getCalledFunction() != SizeMarker)
      continue;
    Value *Size = Call->getArgOperand(0);
    if (isa<ConstantInt>(Size))
      continue;
    if (std::optional<int64_t> Known = Tops.sizeOf(Size))
      Sizes.push_back({ Call, *Known, Tops.roomAboveSize(Size) });
  }

  if (Sizes.empty())
    return Result;

  // The values that stand at a known distance from a floor: those computed
  // from one by sums with constants, also through the joins that are one
  // thing whichever way they are reached. Asking about them can bring up an
  // allocation or a join no size depends on: it is a floor like the others,
  // and everything is looked at again with it.
  struct Change {
    Instruction *Old = nullptr;
    StackTops::Place Where;
  };
  constexpr unsigned MostRounds = 8;
  std::vector<StackTops::Allocation> Allocations;
  std::vector<StackTops::Allocation> Folded;
  SmallPtrSet<Value *, 8> Floors;
  SmallVector<Change, 32> Changes;
  bool IsSettled = false;
  for (unsigned Round = 0; not IsSettled; ++Round) {
    if (Round > MostRounds) {
      Result.Refused = "the floors do not settle";
      return Result;
    }

    Allocations = Tops.allocations(F);
    Folded = Tops.folded(F);
    std::vector<PHINode *> Joins = Tops.joins(F);

    Floors.clear();
    for (const StackTops::Allocation &A : Allocations)
      Floors.insert(A.Result);
    Floors.insert(Joins.begin(), Joins.end());

    Changes.clear();
    SmallVector<Value *, 32> Queue(Floors.begin(), Floors.end());
    SmallPtrSet<Value *, 32> Visited;
    while (not Queue.empty()) {
      Value *Next = Queue.pop_back_val();
      if (not Visited.insert(Next).second)
        continue;
      for (User *U : Next->users()) {
        auto *I = dyn_cast<Instruction>(U);
        if (I == nullptr or Floors.contains(I))
          continue;

        bool IsSum = Tops.skip(I).first != I;
        if (not IsSum and not isa<PHINode>(I))
          continue;

        std::optional<StackTops::Place> Where = Tops.of(I);
        if (not Where.has_value())
          continue;

        Queue.push_back(I);
        if (IsSum and Where->Floor != nullptr)
          Changes.push_back({ I, *Where });
      }
    }

    IsSettled = Tops.allocations(F).size() == Allocations.size()
                and Tops.folded(F).size() == Folded.size()
                and Tops.joins(F).size() == Joins.size();
  }

  {
    // (In the order of the instructions: what is done does not depend on
    // where things are in memory)
    DenseMap<Instruction *, unsigned> Order;
    for (Instruction &I : instructions(F))
      Order[&I] = Order.size();
    llvm::sort(Changes, [&Order](const Change &Left, const Change &Right) {
      return Order.lookup(Left.Old) < Order.lookup(Right.Old);
    });
  }

  // An address computed from one below a floor with something that is not a
  // constant can be on either side of the floor, and so can what an address
  // below a floor is subtracted from: nothing says which word such a value
  // names once the block is no longer next to the frame
  auto IsMovedBySomething = [&](Instruction *Below) {
    for (User *U : Below->users()) {
      auto *Operator = dyn_cast<BinaryOperator>(U);
      if (Operator == nullptr
          or (Operator->getOpcode() != Instruction::Add
              and Operator->getOpcode() != Instruction::Sub))
        continue;

      // A sum with a constant is a place of its own
      if (Tops.skip(Operator).first != Operator)
        continue;

      // The entry value minus it is a size; a block carved out below it
      // starts a new floor
      bool IsSub = Operator->getOpcode() == Instruction::Sub;
      bool IsSize = IsSub and Operator->getOperand(1) == Below
                    and Tops.skip(Operator->getOperand(0)).first == Entry;
      bool IsCarved = IsSub and Operator->getOperand(0) == Below
                      and Operator->getOperand(1) != Below
                      and Tops.of(Operator).has_value();
      if (not IsSize and not IsCarved)
        return true;
    }
    return false;
  };

  // Going up by \p Distance from the join \p Join: is the sum what it names,
  // once all that enters the join has its new name? Not if one of the ways
  // into the join comes from below a floor the sum is at or above.
  std::map<std::pair<PHINode *, int64_t>, bool> Crossing;
  auto Crosses = [&](auto &&Crosses, PHINode *Join, int64_t Distance) -> bool {
    constexpr unsigned MostAsked = 256;
    if (Crossing.size() > MostAsked)
      return true;
    auto [It, IsNew] = Crossing.try_emplace({ Join, Distance }, false);
    if (not IsNew)
      return It->second;

    bool IsCrossed = false;
    for (Value *Incoming : Join->incoming_values()) {
      if (Incoming == Join)
        continue;
      std::optional<StackTops::Place> Where = Tops.of(Incoming);
      if (not Where.has_value()) {
        IsCrossed = true;
      } else if (Where->Floor != nullptr) {
        auto *Under = dyn_cast<PHINode>(Where->Floor);
        if (Where->FromFloor < 0)
          IsCrossed = IsCrossed or Where->FromFloor + Distance >= 0;
        else if (Under != nullptr and Under != Join)
          IsCrossed = IsCrossed
                      or Crosses(Crosses, Under, Where->FromFloor + Distance);
      }
    }
    Crossing[{ Join, Distance }] = IsCrossed;
    return IsCrossed;
  };

  for (const Change &C : Changes) {
    auto *Join = dyn_cast<PHINode>(C.Where.Floor);
    if (C.Where.FromFloor < 0 and IsMovedBySomething(C.Old)) {
      Result.Refused = "an address below a floor is moved by something that "
                       "is not a constant";
      return Result;
    }
    if (C.Where.FromFloor > 0 and Join != nullptr
        and Crosses(Crosses, Join, C.Where.FromFloor)) {
      Result.Refused = "going up from a join crosses a floor";
      return Result;
    }
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

  LLVMContext &Context = F.getContext();
  for (const Site &S : Sizes) {
    S.Call->setArgOperand(0, ConstantInt::getSigned(Integer, S.Size));
    if (S.Room.has_value()) {
      auto *Room = ConstantInt::getSigned(Type::getInt64Ty(Context), *S.Room);
      S.Call->setMetadata(StackRoomMetadata,
                          MDNode::get(Context, ConstantAsMetadata::get(Room)));
    }
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
