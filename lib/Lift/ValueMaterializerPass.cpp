//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "revng/Support/Debug.h"
#include "revng/Support/IRHelperRegistry.h"
#include "revng/Support/IRHelpers.h"
#include "revng/ValueMaterializer/DataFlowRangeAnalysis.h"
#include "revng/ValueMaterializer/ValueMaterializer.h"

#include "JumpTargetManager.h"
#include "ValueMaterializerPass.h"

// This name is not present after `lift`.
IRHelper<> AVIMarker("revng_avi");

using namespace llvm;

cl::list<uint64_t> DumpValueMaterializerAt("dump-vm-at", cl::ZeroOrMore);
cl::opt<bool> DumpValueMaterializer("dump-all-vm");

using SDMO = StaticDataMemoryOracle;

SDMO::StaticDataMemoryOracle(const DataLayout &DL,
                             JumpTargetManager &JTM,
                             const MetaAddress::Features &Features) :
  JTM(JTM), Features(Features) {
  // Read the value using the endianness of the destination architecture,
  // since, if there's a mismatch, in the stack we will also have a byteswap
  // instruction
  IsLittleEndian = DL.isLittleEndian();
}

MaterializedValue StaticDataMemoryOracle::load(uint64_t LoadAddress,
                                               unsigned LoadSize) {
  auto Address = MetaAddress::fromGeneric(LoadAddress, Features);
  return JTM.readFromPointer(Address, LoadSize, IsLittleEndian);
}

bool StaticDataMemoryOracle::isCode(uint64_t Address) const {
  MetaAddress PC = JTM.fromPC(Address);
  return PC.isValid() and JTM.isExecutableAddress(PC);
}

/// The values \p ToTrack can have if it is read from a table of addresses of
/// code, whatever the index is: all the addresses of code the table starts
/// with. The table of a `switch` is among them from its first entry to its
/// last, since what follows it in the program is not an address of code
/// (the bytes of another table, padding, instructions); if another table of
/// addresses follows, its entries are there too, which adds targets the jump
/// never takes and loses none.
///
/// This is for the jumps whose index the analysis of values cannot bound:
/// the index was checked before a call the lifter cannot see through, or the
/// block of the jump is also reached from bytes that are not code.
static std::optional<MaterializedValues>
tableTargets(Value *ToTrack, StaticDataMemoryOracle &MO) {
  using namespace llvm;

  // From the value back to what is read from memory: casts, and operations
  // with a constant
  SmallVector<Instruction *, 8> Steps;
  Value *V = ToTrack;
  LoadInst *Load = nullptr;
  for (unsigned Depth = 0; Depth < 8 and Load == nullptr; ++Depth) {
    if ((Load = dyn_cast<LoadInst>(V)))
      break;

    auto *I = dyn_cast<Instruction>(V);
    if (I == nullptr)
      return std::nullopt;

    if (isa<ZExtInst>(I) or isa<TruncInst>(I)) {
      V = I->getOperand(0);
    } else if (auto *Operator = dyn_cast<BinaryOperator>(I);
               Operator != nullptr
               and (Operator->getOpcode() == Instruction::Add
                    or Operator->getOpcode() == Instruction::Or)) {
      if (isa<ConstantInt>(Operator->getOperand(1)))
        V = Operator->getOperand(0);
      else if (isa<ConstantInt>(Operator->getOperand(0)))
        V = Operator->getOperand(1);
      else
        return std::nullopt;
    } else {
      return std::nullopt;
    }
    Steps.push_back(I);
  }

  if (Load == nullptr or not Load->getType()->isIntegerTy())
    return std::nullopt;
  unsigned EntrySize = Load->getType()->getIntegerBitWidth() / 8;
  if (EntrySize != 4 and EntrySize != 8)
    return std::nullopt;

  // The address: a constant plus an index times the size of an entry
  Value *Address = Load->getPointerOperand();
  if (auto *ToPointer = dyn_cast<IntToPtrInst>(Address))
    Address = ToPointer->getOperand(0);
  else
    return std::nullopt;

  uint64_t Base = 0;
  bool HasIndex = false;
  for (unsigned Depth = 0; Depth < 8 and not HasIndex; ++Depth) {
    if (isa<ZExtInst>(Address) or isa<TruncInst>(Address)) {
      Address = cast<Instruction>(Address)->getOperand(0);
      continue;
    }

    auto *Operator = dyn_cast<BinaryOperator>(Address);
    if (Operator == nullptr)
      return std::nullopt;
    auto *Constant = dyn_cast<ConstantInt>(Operator->getOperand(1));
    switch (Operator->getOpcode()) {
    case Instruction::Add:
      if (Constant != nullptr) {
        Base += Constant->getZExtValue();
        Address = Operator->getOperand(0);
      } else if (auto *Left = dyn_cast<ConstantInt>(Operator->getOperand(0))) {
        Base += Left->getZExtValue();
        Address = Operator->getOperand(1);
      } else {
        return std::nullopt;
      }
      break;

    case Instruction::Shl:
      if (Constant == nullptr
          or (uint64_t(1) << Constant->getLimitedValue(63)) != EntrySize)
        return std::nullopt;
      HasIndex = true;
      break;

    case Instruction::Mul:
      if (Constant == nullptr or Constant->getLimitedValue() != EntrySize)
        return std::nullopt;
      HasIndex = true;
      break;

    default:
      return std::nullopt;
    }
  }

  if (not HasIndex or Base == 0)
    return std::nullopt;

  constexpr unsigned MostEntries = 4096;
  MaterializedValues Result;
  for (unsigned Index = 0; Index < MostEntries; ++Index) {
    MaterializedValue Entry = MO.load(Base + uint64_t(Index) * EntrySize,
                                      EntrySize);
    if (not Entry.isValid() or Entry.hasSymbol()
        or not MO.isCode(Entry.value().getLimitedValue()))
      break;

    // What the value is when this entry is what was read
    APInt Value = Entry.value().zextOrTrunc(EntrySize * 8);
    for (Instruction *Step : llvm::reverse(Steps)) {
      unsigned Bits = Step->getType()->getIntegerBitWidth();
      if (isa<ZExtInst>(Step) or isa<TruncInst>(Step)) {
        Value = Value.zextOrTrunc(Bits);
        continue;
      }

      auto *Left = dyn_cast<ConstantInt>(Step->getOperand(0));
      auto *Right = dyn_cast<ConstantInt>(Step->getOperand(1));
      const APInt &Other = (Right != nullptr ? Right : Left)->getValue();
      if (Step->getOpcode() == Instruction::Add)
        Value = Value + Other;
      else
        Value = Value | Other;
    }

    Result.push_back(MaterializedValue::fromConstant(Value));
  }

  if (Result.empty())
    return std::nullopt;
  return Result;
}

static void demoteOrToAdd(Function &F) {
  using namespace llvm;

  auto &DL = F.getParent()->getDataLayout();
  std::set<Instruction *> ToReplace;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (I.getOpcode() == Instruction::Or) {

        Value *LHS = I.getOperand(0);
        Value *RHS = I.getOperand(1);
        const APInt &LHSZeros = computeKnownBits(LHS, DL).Zero;
        const APInt &RHSZeros = computeKnownBits(RHS, DL).Zero;

        if ((~RHSZeros & ~LHSZeros).isNullValue())
          ToReplace.insert(&I);
      }
    }
  }

  for (Instruction *I : ToReplace) {
    I->replaceAllUsesWith(BinaryOperator::Create(Instruction::Add,
                                                 I->getOperand(0),
                                                 I->getOperand(1),
                                                 Twine(),
                                                 I));
    eraseFromParent(I);
  }
}

PreservedAnalyses ValueMaterializerPass::run(Function &F,
                                             FunctionAnalysisManager &FAM) {
  using namespace llvm;

  DataFlowRangeAnalysis DFRA(*F.getParent());

  llvm::EliminateUnreachableBlocks(F, nullptr, false);

  demoteOrToAdd(F);

  // Early exit in case nothing was marked
  Function *Marker = functionOrNull(AVIMarker.get(*F.getParent()));
  if (Marker == nullptr)
    return PreservedAnalyses::all();

  auto &LVI = FAM.getResult<LazyValueAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);

  // The function does not change while the markers are processed
  AVICandidatesCache Candidates;

  BasicBlock *Entry = &F.getEntryBlock();
  auto GetConstantArgument = [](CallBase *Call, unsigned Index) {
    return cast<ConstantInt>(Call->getArgOperand(Index))->getLimitedValue();
  };

  for (CallBase *Call : callersIn(Marker, &F)) {
    // Decode arguments
    revng_assert(Call->arg_size() >= 4);
    Value *ToTrack = Call->getArgOperand(0);
    uint64_t MaxPhiLike = GetConstantArgument(Call, 1);
    uint64_t MaxLoad = GetConstantArgument(Call, 2);

    auto Oracle = static_cast<Oracle::Values>(GetConstantArgument(Call, 3));
    revng_assert(Oracle < Oracle::Count);

    auto AddressAsString = extractFromConstantStringPtr(Call->getArgOperand(4));
    auto Address = MetaAddress::fromString(AddressAsString);
    revng_assert(Address.isValid());
    uint64_t CurrentAddress = Address.address();

    MaterializedValues Values;
    DataFlowGraph::Limits Limits(MaxPhiLike, MaxLoad);
    auto Results = ::ValueMaterializer::getValuesFor(Call,
                                                     ToTrack,
                                                     MO,
                                                     LVI,
                                                     DFRA,
                                                     DT,
                                                     Limits,
                                                     Oracle,
                                                     &Candidates);
    if (Results.values())
      Values = std::move(*Results.values());

    // No value at all, or values that are not addresses of code, for what
    // is read from a table of them and goes in the program counter (the
    // only values that are followed through loads with this oracle): the
    // table says what it can be
    if (Oracle == Oracle::AdvancedValueInfo and MaxLoad != 0) {
      bool IsOff = Values.empty();
      for (const MaterializedValue &V : Values)
        if (not V.hasSymbol()
            and not MO.isCode(V.value().trunc(64).getZExtValue()))
          IsOff = true;
      if (IsOff)
        if (auto FromTable = tableTargets(ToTrack, MO))
          Values = std::move(*FromTable);
    }

    if (DumpValueMaterializer
        or count(DumpValueMaterializerAt, CurrentAddress) > 0) {
      // User asked to dump information about this address
      dbg << "Values produced by ValueMaterializer for " << getName(ToTrack)
          << " at " << Address.toString() << ":\n";
      for (const MaterializedValue &V : Values) {
        dbg << "  ";
        V.dump(dbg);
        dbg << "\n";
      }

      dbg << "Dumping graphs\n";
      Results.dataFlowGraph().dump();
      AdvancedValueInfoMFI::dump(&Results.cfeg(), Results.mfiResult());
    }

    //
    // Create a revng.avi metadata containing the type of instruction and
    // all the possible values we identified
    //
    QuickMetadata QMD(getContext(&F));
    std::vector<Metadata *> ValuesMD;
    ValuesMD.reserve(Values.size());
    for (const MaterializedValue &V : Values) {
      // TODO: we are we ignoring those with symbols
      auto Offset = V.value();
      std::string SymbolName;
      if (V.hasSymbol())
        SymbolName = V.symbolName();

      ValuesMD.push_back(QMD.tuple({ QMD.get(SymbolName), QMD.get(Offset) }));
    }

    Call->setMetadata("revng.avi", QMD.tuple(ValuesMD));
  }

  return PreservedAnalyses::all();
}

llvm::Function *ValueMaterializerPass::createMarker(llvm::Module &M) {
  using namespace llvm;
  LLVMContext &C = M.getContext();
  auto *Type = FunctionType::get(FunctionType::getVoidTy(C), {}, true);
  auto *Marker = AVIMarker.getOrCreate(M, Type).function();
  Marker->setOnlyAccessesInaccessibleMemory();
  return Marker;
}
