//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <string>

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

#include "revng/PromoteStackPointer/DetachStackAllocations.h"
#include "revng/PromoteStackPointer/DetachStackAllocationsPass.h"
#include "revng/PromoteStackPointer/Markers.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

using namespace llvm;

static Logger Log("detach-stack-allocations");

/// \return how many call sites of \p F have a stack whose size is not a
///         constant
static unsigned countUnknownSizes(Function &F, Function *SizeMarker) {
  unsigned Result = 0;
  for (Instruction &I : instructions(F))
    if (auto *Call = dyn_cast<CallBase>(&I))
      if (Call->getCalledFunction() == SizeMarker
          and not isa<ConstantInt>(Call->getArgOperand(0)))
        ++Result;
  return Result;
}

/// What was done to the function called \p Name (\p Length characters), and
/// how many of its call sites still have a stack of an unknown size. (A
/// function of its own, with the name as plain characters: what the pass did
/// to a function can be watched from a debugger that cannot call anything.)
[[gnu::noinline, gnu::used]] static void
noteDetached(const char *Name,
             size_t Length,
             const DetachedStackAllocations &Done,
             unsigned Before,
             unsigned After) {
  if (not Log.isEnabled())
    return;

  Log << std::string(Name, Length) << ": ";
  if (Done.Refused != nullptr)
    Log << "left as it was (" << Done.Refused << "); ";
  Log << Done.Blocks << " blocks, " << Done.Folded
      << " allocations of a constant size, " << Done.ToFrame
      << " values to places of the frame, " << Done.IntoBlocks
      << " into a block, " << Done.Loops
      << " loops left; call sites with a stack of unknown size: " << Before
      << " before, " << After << " now" << DoLog;
}

bool DetachStackAllocationsPass::runOnModule(Module &M) {
  auto *InitFunction = functionOrNull(UndefinedLocalSPMarker.get(M));
  auto *SizeMarker = functionOrNull(StackSizeAtCallSite.get(M));
  if (InitFunction == nullptr or SizeMarker == nullptr)
    return false;

  bool Result = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    // The stack pointer the function is entered with
    Instruction *Entry = nullptr;
    for (Instruction &I : F.getEntryBlock()) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (Call != nullptr and Call->getCalledFunction() == InitFunction) {
        Entry = Call;
        break;
      }
    }

    if (Entry == nullptr)
      continue;

    unsigned Before = countUnknownSizes(F, SizeMarker);
    if (Before == 0)
      continue;

    // What stands for an allocation: takes the size, returns the block. Two
    // calls give two blocks, and none of them touches what the function can
    // name.
    Type *Integer = Entry->getType();
    auto *Type = FunctionType::get(Integer, { Integer }, false);
    Function *Allocator = StackAllocation.getOrCreate(M, Type).function();
    Allocator->addFnAttr(Attribute::NoUnwind);
    Allocator->addFnAttr(Attribute::WillReturn);
    Allocator->addFnAttr(Attribute::NoMerge);
    Allocator->setOnlyAccessesInaccessibleMemory();

    DetachedStackAllocations Done = detachStackAllocations(F,
                                                           Entry,
                                                           SizeMarker,
                                                           Allocator);
    StringRef Name = F.getName();
    unsigned After = countUnknownSizes(F, SizeMarker);
    noteDetached(Name.data(), Name.size(), Done, Before, After);
    Result = Result or Done.changed();
  }

  // Not left behind where nothing was allocated
  if (auto *Allocator = functionOrNull(StackAllocation.get(M));
      Allocator != nullptr and Allocator->use_empty()) {
    Allocator->eraseFromParent();
  }

  return Result;
}

char DetachStackAllocationsPass::ID = 0;

using Register = RegisterPass<DetachStackAllocationsPass>;
static Register R("detach-stack-allocations", "Detach Stack Allocations Pass");
