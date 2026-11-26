/// Collect the function entry points from unused addresses.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <set>

#include "boost/icl/interval_set.hpp"
#include "boost/icl/right_open_interval.hpp"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "revng/EarlyFunctionAnalysis/CollectFunctionsFromUnusedAddresses.h"
#include "revng/EarlyFunctionAnalysis/ControlFlowGraphCache.h"
#include "revng/Lift/JumpTargetReason.h"
#include "revng/Lift/Lift.h"
#include "revng/Support/BlockType.h"
#include "revng/Support/NewPC.h"

using namespace llvm;

static Logger Log("functions-from-unused-addresses-collection");

class CFFUAImpl {
public:
  CFFUAImpl(llvm::Module &M, RootFunction &Root, model::Binary &Binary) :
    M(M), Root(Root), Binary(Binary) {}

  void run(ControlFlowGraphCache &MDCache) {
    for (const MetaAddress &Entry : candidates(MDCache))
      Binary.Functions()[Entry];
  }

  std::vector<MetaAddress> candidates(ControlFlowGraphCache &MDCache) {
    loadAllCFGs(MDCache);
    collectFunctionsFromUnusedAddresses();
    return std::move(Found);
  }

private:
  void loadAllCFGs(ControlFlowGraphCache &MDCache) {
    for (auto &Function : Binary.Functions()) {
      llvm::BasicBlock *Entry = Root.getBlockAt(Function.Entry());
      llvm::Instruction *Term = Entry->getTerminator();

      const efa::ControlFlowGraph &FM = MDCache.getControlFlowGraph(Function
                                                                      .Entry());
      for (const efa::BasicBlock &Block : FM.Blocks()) {
        auto Start = Block.ID().start();
        auto End = Block.End();
        revng_log(Log,
                  "Registering as used range [" << Start.toString() << ", "
                                                << End.toString() << ")");
        UsedRanges += interval::right_open(Start, End);
      }
    }
  }

  /// \return whether \p I, which computes with an address, is only ever used
  ///         to say where memory is read or written: the address is then
  ///         that of data, a table for instance, whatever segment it is in
  static bool isOnlyAnAddress(const llvm::Instruction *I, unsigned Depth = 0) {
    using namespace llvm;
    if (Depth > 6 or I->use_empty())
      return false;

    for (const User *U : I->users()) {
      if (auto *Load = dyn_cast<LoadInst>(U)) {
        if (Load->getPointerOperand() != I)
          return false;
      } else if (auto *Store = dyn_cast<StoreInst>(U)) {
        if (Store->getPointerOperand() != I or Store->getValueOperand() == I)
          return false;
      } else if (auto *Next = dyn_cast<Instruction>(U);
                 Next != nullptr
                 and (Next->getOpcode() == Instruction::Add
                      or isa<ZExtInst>(Next) or isa<IntToPtrInst>(Next)
                      or isa<GetElementPtrInst>(Next))) {
        if (not isOnlyAnAddress(Next, Depth + 1))
          return false;
      } else {
        return false;
      }
    }

    return true;
  }

  /// The addresses of code that the program only uses, plus something, as
  /// the place memory is accessed at: `[table + index]`. A table of a
  /// `switch` the lifter could not resolve is one, and it is not code.
  std::set<uint64_t> addressesOfData() {
    using namespace llvm;
    Function &Root = *M.getFunction("root");

    std::set<uint64_t> Candidates;
    for (BasicBlock &BB : Root) {
      if (getType(&BB) != BlockType::JumpTargetBlock)
        continue;
      uint32_t Reasons = JumpTargetReason::getReasons(&BB);
      if (hasReason(Reasons, JumpTargetReason::SimpleLiteral))
        Candidates.insert(getBasicBlockAddress(getJumpTargetBlock(&BB))
                            .address());
    }

    std::set<uint64_t> AsAddress;
    std::set<uint64_t> Otherwise;
    for (Instruction &I : instructions(Root)) {
      for (const Use &Operand : I.operands()) {
        auto *Constant = dyn_cast<ConstantInt>(Operand.get());
        if (Constant == nullptr or Constant->getBitWidth() > 64
            or not Candidates.contains(Constant->getZExtValue()))
          continue;

        bool IsSum = I.getOpcode() == Instruction::Add;
        if (IsSum and isOnlyAnAddress(&I))
          AsAddress.insert(Constant->getZExtValue());
        else
          Otherwise.insert(Constant->getZExtValue());
      }
    }

    std::set<uint64_t> Result;
    for (uint64_t Address : AsAddress)
      if (not Otherwise.contains(Address))
        Result.insert(Address);
    return Result;
  }

  void collectFunctionsFromUnusedAddresses() {
    using namespace llvm;
    Function &Root = *M.getFunction("root");
    std::set<uint64_t> Data = addressesOfData();

    for (BasicBlock &BB : Root) {
      if (getType(&BB) != BlockType::JumpTargetBlock)
        continue;

      MetaAddress Entry = getBasicBlockAddress(getJumpTargetBlock(&BB));
      if (Binary.Functions().tryGet(Entry) != nullptr)
        continue;

      uint32_t Reasons = JumpTargetReason::getReasons(&BB);
      bool IsSimpleLiteral = hasReason(Reasons,
                                       JumpTargetReason::SimpleLiteral);
      bool IsUnusedGlobalData = hasReason(Reasons,
                                          JumpTargetReason::UnusedGlobalData);
      bool IsDirectJump = hasReason(Reasons, JumpTargetReason::DirectJump);
      bool IsMemoryStore = hasReason(Reasons, JumpTargetReason::MemoryStore);
      bool IsPCStore = hasReason(Reasons, JumpTargetReason::PCStore);
      bool IsReturnAddress = hasReason(Reasons,
                                       JumpTargetReason::ReturnAddress);
      bool IsLoadAddress = hasReason(Reasons, JumpTargetReason::LoadAddress);
      bool IsPartOfOtherCFG = UsedRanges.find(Entry) != UsedRanges.end();

      revng_log(Log,
                Entry.toString()
                  << "\n"
                  << "  IsSimpleLiteral: " << IsSimpleLiteral << "\n"
                  << "  IsUnusedGlobalData: " << IsUnusedGlobalData << "\n"
                  << "  IsDirectJump: " << IsDirectJump << "\n"
                  << "  IsMemoryStore: " << IsMemoryStore << "\n"
                  << "  IsPCStore: " << IsPCStore << "\n"
                  << "  IsReturnAddress: " << IsReturnAddress << "\n"
                  << "  IsLoadAddress: " << IsLoadAddress << "\n"
                  << "  IsPartOfOtherCFG: " << IsPartOfOtherCFG << "\n");

      // Look for simple literals, unused global data or address stored in
      // memory that are never written directly to the PC. Also, ignore code for
      // which we already have a direct jump or is part of the CFG of another
      // function. Also, ignore addresses that are return addresses.
      bool Interesting = (IsSimpleLiteral or IsUnusedGlobalData
                          or (IsMemoryStore and not IsPCStore));
      bool Ignore = IsPartOfOtherCFG or IsDirectJump or IsReturnAddress;
      if (Interesting and not Ignore) {

        if (IsLoadAddress) {
          revng_log(Log,
                    "Ignoring " << Entry.toString()
                                << " since it's target of a memory operation");

        } else if (IsSimpleLiteral and not IsUnusedGlobalData
                   and not IsMemoryStore and Data.contains(Entry.address())) {
          revng_log(Log,
                    "Ignoring " << Entry.toString()
                                << " since it is only used to address memory");

        } else {
          if (IsMemoryStore and not IsUnusedGlobalData) {
            revng_log(Log,
                      "Creating function due to memory store at "
                        << Entry.toString());
          }

          Found.push_back(Entry);
          revng_log(Log,
                    "Found function from unused addresses: "
                      << BB.getName().str());
        }
      }
    }
  }

private:
  using interval_set = boost::icl::interval_set<MetaAddress>;
  using interval = boost::icl::interval<MetaAddress>;

private:
  llvm::Module &M;
  RootFunction &Root;
  model::Binary &Binary;
  interval_set UsedRanges;
  std::vector<MetaAddress> Found;
};

std::vector<MetaAddress> unusedCodeAddresses(llvm::Module &M,
                                             RootFunction &Root,
                                             model::Binary &Binary,
                                             ControlFlowGraphCache &FMC) {
  CFFUAImpl Impl(M, Root, Binary);
  return Impl.candidates(FMC);
}

void collectFunctionsFromUnusedAddresses(llvm::Module &M,
                                         RootFunction &Root,
                                         model::Binary &Binary,
                                         ControlFlowGraphCache &FMC) {
  CFFUAImpl Impl(M, Root, Binary);
  Impl.run(FMC);
}
