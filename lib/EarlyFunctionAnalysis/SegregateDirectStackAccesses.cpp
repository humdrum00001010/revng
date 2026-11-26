/// Segregate direct stack accesses from all other memory accesses through alias
/// information.
///
/// This provides a way to say that stack accesses do not interfere with any
/// other memory access. The accesses to the block of the thread, which is
/// neither on the stack nor reached in any other way than through its
/// register, are kept apart from both.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/PatternMatch.h"

#include "revng/EarlyFunctionAnalysis/SegregateDirectStackAccesses.h"
#include "revng/Support/Assert.h"
#include "revng/Support/IRBuilder.h"

using namespace llvm;
class SegregateDirectStackAccessesPassImpl;

using SDSAP = SegregateDirectStackAccessesPass;
using SDSAPI = SegregateDirectStackAccessesPassImpl;

class SegregateDirectStackAccessesPassImpl {
  LLVMContext *Context = nullptr;
  GlobalVariable *StackPointer = nullptr;
  ArrayRef<GlobalVariable *> ThreadPointers;
  std::vector<Instruction *> DirectStackAccesses;
  std::vector<Instruction *> ThreadBlockAccesses;
  std::vector<Instruction *> NotDirectStackAccesses;

public:
  SegregateDirectStackAccessesPassImpl(GlobalVariable *StackPointer,
                                       ArrayRef<GlobalVariable *>
                                         ThreadPointers) :
    StackPointer(StackPointer), ThreadPointers(ThreadPointers) {}
  void run(Function &, FunctionAnalysisManager &);

private:
  void segregateAccesses(Function &F);
  void decorateStackAccesses();

  /// \return whether \p Pointer is what a thread pointer holds, plus a
  ///         constant
  bool isInThreadBlock(Value *Pointer) const;
};

PreservedAnalyses SDSAP::run(Function &F, FunctionAnalysisManager &FAM) {
  SegregateDirectStackAccessesPassImpl SDASP(StackPointer, ThreadPointers);
  SDASP.run(F, FAM);
  return PreservedAnalyses::none();
}

void SDSAPI::run(Function &F, FunctionAnalysisManager &FAM) {
  Context = &(F.getContext());

  // Populate the two buckets with all load and store instruction of the
  // function, properly segregated.
  segregateAccesses(F);

  // Adorn IR with the alias information collected before.
  decorateStackAccesses();
}

/// \return what \p Pointer is above the stack pointer the function is
///         entered with (one of \p EntryLoads), if it is that plus a constant,
///         however that is written: through casts, sums with constants,
///         pointer arithmetic, and the mask that keeps a sum computed with
///         the bits of a host pointer within those of an address
static std::optional<int64_t>
stackOffsetOf(Value *Pointer,
              const SmallPtrSetImpl<Value *> &EntryLoads,
              const DataLayout &DL,
              unsigned AddressBits) {
  int64_t Offset = 0;
  for (unsigned Depth = 0; Depth < 32; ++Depth) {
    if (EntryLoads.contains(Pointer))
      return Offset;

    if (isa<TruncInst>(Pointer) or isa<ZExtInst>(Pointer)
        or isa<PtrToIntInst>(Pointer) or isa<IntToPtrInst>(Pointer)
        or isa<BitCastInst>(Pointer)) {
      Pointer = cast<Instruction>(Pointer)->getOperand(0);
    } else if (auto *GEP = dyn_cast<GetElementPtrInst>(Pointer)) {
      APInt Constant(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
      if (not GEP->accumulateConstantOffset(DL, Constant))
        return std::nullopt;
      Offset += Constant.getSExtValue();
      Pointer = GEP->getPointerOperand();
    } else if (auto *Operator = dyn_cast<BinaryOperator>(Pointer)) {
      auto *Constant = dyn_cast<ConstantInt>(Operator->getOperand(1));
      if (Constant == nullptr or Constant->getBitWidth() > 64)
        return std::nullopt;

      switch (Operator->getOpcode()) {
      case Instruction::Add:
        Offset += Constant->getSExtValue();
        break;
      case Instruction::Sub:
        Offset -= Constant->getSExtValue();
        break;
      case Instruction::And:
        if (AddressBits >= 64
            or Constant->getZExtValue() != (uint64_t(1) << AddressBits) - 1)
          return std::nullopt;
        break;
      default:
        return std::nullopt;
      }
      Pointer = Operator->getOperand(0);
    } else {
      return std::nullopt;
    }
  }

  return std::nullopt;
}

bool SDSAPI::isInThreadBlock(Value *Pointer) const {
  using namespace PatternMatch;

  Value *Base = nullptr;
  if (match(Pointer, m_c_Add(m_Value(Base), m_ConstantInt())))
    Pointer = skipCasts(Base);

  auto *Load = dyn_cast<LoadInst>(Pointer);
  if (Load == nullptr)
    return false;

  auto *CSV = dyn_cast<GlobalVariable>(skipCasts(Load->getPointerOperand()));
  return CSV != nullptr and is_contained(ThreadPointers, CSV);
}

void SDSAPI::segregateAccesses(Function &F) {
  using namespace PatternMatch;

  Value *LoadSP = nullptr;
  bool Found = false;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (skipCasts(LI->getPointerOperand()) == StackPointer) {
          revng_assert(!Found);
          LoadSP = LI;
          Found = true;
          break;
        }
      }
    }

    if (Found)
      break;
  }

  // The stack pointer the function is entered with: what is read from the
  // register at the start, before anything is written to it
  SmallPtrSet<Value *, 4> EntryLoads;
  for (Instruction &I : F.getEntryBlock()) {
    if (auto *Store = dyn_cast<StoreInst>(&I)) {
      if (skipCasts(Store->getPointerOperand()) == StackPointer)
        break;
    } else if (auto *Load = dyn_cast<LoadInst>(&I)) {
      if (skipCasts(Load->getPointerOperand()) == StackPointer)
        EntryLoads.insert(Load);
    }
  }
  const DataLayout &DL = F.getParent()->getDataLayout();
  unsigned AddressBits = StackPointer->getValueType()->getIntegerBitWidth();

  // Modify the IR after alloca instructions, if they exist.
  auto It = F.getEntryBlock().begin();
  while (It->getOpcode() == Instruction::Alloca)
    It++;

  revng::IRBuilder Builder(&(*It));

  // Context: inttoptr instructions basically inhibits all optimizations. In
  // particular, when an integer is inttoptr'd twice with different destination
  // type, alias analysis messes up. Hence, we need to ensure that no inttoptr
  // exists when operating on a instruction that directly accesses the stack.
  // Note that this problem will be addressed by opaque pointers in the future.
  auto *I8PtrTy = Builder.getInt8PtrTy();
  auto *CE = ConstantExpr::getBitCast(StackPointer, I8PtrTy->getPointerTo());
  Value *SPI8Ptr = Builder.CreateLoad(I8PtrTy, CE);
  EntryLoads.insert(SPI8Ptr);

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      Value *Pointer = nullptr;

      // Differentiate accesses and add them onto their respective bucket.
      // Everything that is not a direct access on the stack is put onto the
      // bucket `NotDirectStackAccesses`. Load/store that access the CSVs will
      // have their alias info added later as well.
      if (auto *LI = dyn_cast<LoadInst>(&I))
        Pointer = skipCasts(LI->getPointerOperand());
      else if (auto *SI = dyn_cast<StoreInst>(&I))
        Pointer = skipCasts(SI->getPointerOperand());

      if (Pointer != nullptr) {
        Value *LoadPtr = nullptr;
        Value *LHS = nullptr;
        Value *BitCast = nullptr;
        ConstantInt *Offset = nullptr;
        Builder.SetInsertPoint(&I);

        // Do we have a inttoptr to a load i64, i64* LoadPtr as pointer operand
        // of the current instruction, where LoadPtr is SP? Change it with the
        // newly-created bitcasted load in order to prevent from using inttoptr.
        if (Pointer == LoadSP) {
          Type *DestTy = nullptr;
          if (isa<LoadInst>(&I))
            DestTy = I.getOperand(0)->getType();
          else
            DestTy = I.getOperand(0)->getType()->getPointerTo();

          BitCast = Builder.CreateBitCast(SPI8Ptr, DestTy);
          I.setOperand(isa<LoadInst>(&I) ? 0 : 1, BitCast);

          DirectStackAccesses.emplace_back(&I);
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(Pointer);
                   GEP != nullptr and GEP->hasAllConstantIndices()
                   and skipCasts(GEP->getPointerOperand()) == LoadSP) {
          // An access this pass has already rewritten, in a previous run
          DirectStackAccesses.emplace_back(&I);
        } else if (match(Pointer, m_c_Add(m_Value(LHS), m_ConstantInt(Offset)))
                   && skipCasts(LHS) == LoadSP) {
          // Do we have a inttoptr whose pointer operand is an instruction `add
          // i64 LHS, X` with X negative constant and LHS the stack pointer? If
          // so, canonicalize the i2p + add into a gep whose value is bitcasted
          // to the original type of SP.
          auto *GEP = Builder.CreateGEP(Builder.getInt8Ty(), SPI8Ptr, Offset);

          Type *DestTy = nullptr;
          if (isa<LoadInst>(&I))
            DestTy = I.getOperand(0)->getType();
          else
            DestTy = I.getOperand(0)->getType()->getPointerTo();

          BitCast = Builder.CreateBitCast(GEP, DestTy);
          I.setOperand(isa<LoadInst>(&I) ? 0 : 1, BitCast);

          DirectStackAccesses.emplace_back(&I);
        } else if (std::optional<int64_t>
                     Above = stackOffsetOf(Pointer,
                                           EntryLoads,
                                           DL,
                                           AddressBits);
                   Above.has_value()) {
          // The same address, written another way (after the optimizations
          // have been at it, or where the paths of the function that join
          // bring the same stack pointer): an access to the stack as well
          APInt Wrapped = APInt(64, uint64_t(*Above)).trunc(AddressBits);
          auto *Index = Builder.getInt64(Wrapped.getSExtValue());
          auto *GEP = Builder.CreateGEP(Builder.getInt8Ty(), SPI8Ptr, Index);

          Type *DestTy = nullptr;
          if (isa<LoadInst>(&I))
            DestTy = I.getOperand(0)->getType();
          else
            DestTy = I.getOperand(0)->getType()->getPointerTo();

          BitCast = Builder.CreateBitCast(GEP, DestTy);
          I.setOperand(isa<LoadInst>(&I) ? 0 : 1, BitCast);

          DirectStackAccesses.emplace_back(&I);
        } else if (isInThreadBlock(Pointer)) {
          ThreadBlockAccesses.emplace_back(&I);
        } else {
          NotDirectStackAccesses.emplace_back(&I);
        }
      }
    }
  }
}

void SDSAPI::decorateStackAccesses() {
  MDBuilder MDB(*Context);

  MDNode *AliasDomain = MDB.createAliasScopeDomain("CSVAliasDomain");

  // Create three different scopes: one will be populated with memory
  // operations that directly access the stack; one with those that access the
  // block of the thread; the other one, with all the remaining kind of accesses
  // (to the CSVs, to the heap, etc.).
  auto *DSAScope = MDB.createAliasScope("DirectStackAccessScope", AliasDomain);
  auto *TBAScope = MDB.createAliasScope("ThreadBlockAccessScope", AliasDomain);
  auto *NDSAScope = MDB.createAliasScope("Not(DirectStackAccessScope)",
                                         AliasDomain);

  auto Set = [this](std::initializer_list<Metadata *> Scopes) {
    return MDNode::get(*Context, ArrayRef<Metadata *>(Scopes));
  };

  for (auto *I : DirectStackAccesses) {
    I->setMetadata(LLVMContext::MD_alias_scope, Set({ DSAScope }));
    I->setMetadata(LLVMContext::MD_noalias, Set({ NDSAScope, TBAScope }));
  }

  for (auto *I : ThreadBlockAccesses) {
    I->setMetadata(LLVMContext::MD_alias_scope, Set({ TBAScope }));
    I->setMetadata(LLVMContext::MD_noalias, Set({ DSAScope, NDSAScope }));
  }

  for (auto *I : NotDirectStackAccesses) {
    I->setMetadata(LLVMContext::MD_alias_scope, Set({ NDSAScope }));
    I->setMetadata(LLVMContext::MD_noalias, Set({ DSAScope, TBAScope }));
  }
}
