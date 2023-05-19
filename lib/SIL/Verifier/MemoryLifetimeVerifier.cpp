//===--- MemoryLifetimeVerifier.cpp ---------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-memory-lifetime-verifier"
#include "swift/SIL/MemoryLocations.h"
#include "swift/SIL/BitDataflow.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;

llvm::cl::opt<bool> DontAbortOnMemoryLifetimeErrors(
    "dont-abort-on-memory-lifetime-errors",
    llvm::cl::desc("Don't abort compilation if the memory lifetime checker "
                   "detects an error."));

namespace {

/// A utility for verifying memory lifetime.
///
/// The MemoryLifetime utility checks the lifetime of memory locations.
/// This is limited to memory locations which can be handled by
/// `MemoryLocations`.
class MemoryLifetimeVerifier {

  using Bits = MemoryLocations::Bits;
  using Location = MemoryLocations::Location;
  using BlockState = BitDataflow::BlockState;

  SILFunction *function;
  AliasAnalysis *aliasAnalysis;
  MemoryLocations locations;

  /// alloc_stack memory locations which are used for store_borrow.
  Bits storeBorrowLocations;

  /// Returns true if the enum location \p locIdx can be proven to hold a
  /// hold a trivial value (e non-payload case) at \p atInst.
  bool isEnumTrivialAt(int locIdx, SILInstruction *atInst);

  /// Returns true if an instruction in the range between \p start and \p end
  /// stores a trivial enum case into the enum location \p loc.
  bool storesTrivialEnum(int locIdx,
                         SILBasicBlock::reverse_iterator start,
                         SILBasicBlock::reverse_iterator end);

  /// Returns true if \p block contains a `switch_enum` or `switch_enum_addr`
  /// and \p succ is a a successor block for a enum case with no payload or a
  /// trivial payload.
  bool isTrivialEnumSuccessor(SILBasicBlock *block, SILBasicBlock *succ,
                              int locIdx);

  /// Issue an error for a memory location.
  void reportError(const Twine &complaint, int locationIdx,
                   SILInstruction *where);

  /// Issue an error if any bit in \p wrongBits is set.
  void require(const Bits &wrongBits, const Twine &complaint,
               SILInstruction *where, bool excludeTrivialEnums = false);

  /// Require that all the subLocation bits of the location, associated with
  /// \p addr, are clear in \p bits.
  void requireBitsClear(const Bits &bits, SILValue addr, SILInstruction *where);

  /// Require that all the subLocation bits of the location, associated with
  /// \p addr, are set in \p bits.
  void requireBitsSet(const Bits &bits, SILValue addr, SILInstruction *where);

  void requireBitsSetForArgument(const Bits &bits, SILValue addr, SILInstruction *applyInst);

  bool isStoreBorrowLocation(SILValue addr) {
    auto *loc = locations.getLocation(addr);
    return loc && storeBorrowLocations.anyCommon(loc->subLocations);
  }

  /// Require that the location of addr is not an alloc_stack used for a
  /// store_borrow.
  void requireNoStoreBorrowLocation(SILValue addr, SILInstruction *where);

  /// Register the destination address of a store_borrow as borrowed location.
  void registerStoreBorrowLocation(SILValue addr);

  /// Registers all store_borrow instructions in a block.
  void registerStoreBorrowsInBlock(SILBasicBlock *block);

  /// Handles locations of the predecessor's terminator, which are only valid
  /// in \p block.
  /// Example: @out results of try_apply. They are only valid in the
  /// normal-block, but not in the throw-block.
  void setBitsOfPredecessor(Bits &genSet, Bits &killSet, SILBasicBlock *block);

  /// Initializes the data flow bits sets in the block states for all blocks.
  void initDataflow(BitDataflow &dataFlow);

  /// Initializes the data flow bits sets in the block state for a single block.
  void initDataflowInBlock(SILBasicBlock *block, BlockState &state);

  /// Helper function to set bits for function arguments and returns.
  void setFuncOperandBits(BlockState &state, Operand &op,
                          SILArgumentConvention convention,
                          bool isTryApply);

  /// Perform all checks in the function after the data flow has been computed.
  void checkFunction(BitDataflow &dataFlow);

  /// Check all instructions in \p block, starting with \p bits as entry set.
  void checkBlock(SILBasicBlock *block, Bits &bits);

  /// Check a function argument against the current live \p bits at the function
  /// call.
  void checkFuncArgument(Bits &bits, Operand &argumentOp,
                          SILArgumentConvention argumentConvention,
                          SILInstruction *applyInst);

  // Utility functions for setting and clearing gen- and kill-bits.

  void genBits(BitDataflow::BlockState &blockState, SILValue addr) {
    locations.genBits(blockState.genSet, blockState.killSet, addr);
  }

  void killBits(BitDataflow::BlockState &blockState, SILValue addr) {
    locations.killBits(blockState.genSet, blockState.killSet, addr);
  }

public:
  MemoryLifetimeVerifier(SILFunction *function, SILPassManager *passManager) :
    function(function),
    aliasAnalysis(passManager ? passManager->getAnalysis<AliasAnalysis>(function)
                              : nullptr),
    locations(/*handleNonTrivialProjections*/ true,
              /*handleTrivialLocations*/ true) {}

  /// The main entry point to verify the lifetime of all memory locations in
  /// the function.
  void verify();
};

bool MemoryLifetimeVerifier::isEnumTrivialAt(int locIdx,
                                             SILInstruction *atInst) {
  const Location *rootLoc = locations.getRootLocation(locIdx);
  SILBasicBlock *rootBlock = rootLoc->representativeValue->getParentBlock();
  SILBasicBlock *startBlock = atInst->getParent();
  
  // Start at atInst an walk up the control flow.
  BasicBlockWorklist worklist(startBlock);
  while (SILBasicBlock *block = worklist.pop()) {
    auto start = (block == atInst->getParent() ? atInst->getReverseIterator()
                                               : block->rbegin());
    if (storesTrivialEnum(locIdx, start, block->rend())) {
      // Stop at trivial stores to the enum.
      continue;
    }
    if (block == rootBlock) {
      // We reached the block where the memory location is defined. So we cannot
      // prove that the enum contains a non-payload case.
      return false;
    }
    assert(block != function->getEntryBlock());
    for (SILBasicBlock *pred : block->getPredecessorBlocks()) {
      // Stop walking to the predecessor if block is a non-payload successor
      // of a switch_enum/switch_enum_addr.
      if (!isTrivialEnumSuccessor(pred, block, locIdx))
        worklist.pushIfNotVisited(pred);
    }
  }
  return true;
}

static bool isTrivialEnumElem(EnumElementDecl *elem, SILType enumType,
                              SILFunction *function) {
  return !elem->hasAssociatedValues() ||
        enumType.getEnumElementType(elem, function).isTrivial(*function);
}

static bool injectsNoPayloadCase(InjectEnumAddrInst *IEAI) {
  if (!IEAI->getElement()->hasAssociatedValues())
    return true;
  SILType enumType = IEAI->getOperand()->getType();
  SILFunction *function = IEAI->getFunction();
  SILType elemType = enumType.getEnumElementType(IEAI->getElement(), function);
  // Handle empty types (e.g. the empty tuple) as no-payload.
  return elemType.isEmpty(*function);
}

bool MemoryLifetimeVerifier::storesTrivialEnum(int locIdx,
                        SILBasicBlock::reverse_iterator start,
                        SILBasicBlock::reverse_iterator end) {
  for (SILInstruction &inst : make_range(start, end)) {
    if (auto *IEI = dyn_cast<InjectEnumAddrInst>(&inst)) {
      const Location *loc = locations.getLocation(IEI->getOperand());
      if (loc && loc->isSubLocation(locIdx))
        return isTrivialEnumElem(IEI->getElement(), IEI->getOperand()->getType(),
                                 function);
    }
    if (auto *SI = dyn_cast<StoreInst>(&inst)) {
      const Location *loc = locations.getLocation(SI->getDest());
      if (loc && loc->isSubLocation(locIdx) &&
          SI->getSrc()->getType().isOrHasEnum()) {
        return SI->getOwnershipQualifier() == StoreOwnershipQualifier::Trivial;
      }
    }
  }
  return false;
}

bool MemoryLifetimeVerifier::isTrivialEnumSuccessor(SILBasicBlock *block,
                                        SILBasicBlock *succ, int locIdx) {
  TermInst *term = block->getTerminator();
  NullablePtr<EnumElementDecl> elem;
  SILType enumTy;
  if (auto *switchEnum = dyn_cast<SwitchEnumInst>(term)) {
    elem = switchEnum->getUniqueCaseForDestination(succ);
    enumTy = switchEnum->getOperand()->getType();
  } else if (auto *switchEnumAddr = dyn_cast<SwitchEnumAddrInst>(term)) {
    elem = switchEnumAddr->getUniqueCaseForDestination(succ);
    enumTy = switchEnumAddr->getOperand()->getType();
  } else {
    return false;
  }
  // The conservative default (if we cannot figure out the element) is to
  // assume that it's a trivial element.
  if (!elem)
    return true;
  return isTrivialEnumElem(elem.get(), enumTy, function);
}

void MemoryLifetimeVerifier::reportError(const Twine &complaint,
                                     int locationIdx, SILInstruction *where) {
  llvm::errs() << "SIL memory lifetime failure in @" << function->getName()
               << ": " << complaint << '\n';
  if (locationIdx >= 0) {
    llvm::errs() << "memory location: "
                 << locations.getLocation(locationIdx)->representativeValue;
  }
  llvm::errs() << "at instruction: " << *where << '\n';

  if (DontAbortOnMemoryLifetimeErrors)
    return;

  llvm::errs() << "in function:\n";
  function->print(llvm::errs());
  abort();
}

void MemoryLifetimeVerifier::require(const Bits &wrongBits,
                                const Twine &complaint, SILInstruction *where,
                                bool excludeTrivialEnums) {
  for (int errorLocIdx = wrongBits.find_first(); errorLocIdx >= 0;
       errorLocIdx = wrongBits.find_next(errorLocIdx)) {
    if (!excludeTrivialEnums || !isEnumTrivialAt(errorLocIdx, where))
      reportError(complaint, errorLocIdx, where);
  }
}

void MemoryLifetimeVerifier::requireBitsClear(const Bits &bits, SILValue addr,
                                             SILInstruction *where) {
  if (auto *loc = locations.getLocation(addr)) {
    require(bits & loc->subLocations, "memory is initialized, but shouldn't be",
            where, /*excludeTrivialEnums*/ true);
  }
}

void MemoryLifetimeVerifier::requireBitsSet(const Bits &bits, SILValue addr,
                                           SILInstruction *where) {
  if (auto *loc = locations.getLocation(addr)) {
    require(~bits & loc->subLocations,
            "memory is not initialized, but should be", where);
  }
}

void MemoryLifetimeVerifier::requireBitsSetForArgument(const Bits &bits, SILValue addr,
                                           SILInstruction *applyInst) {
  // Optimizations can rely on alias analysis to know that an in-argument (or
  // parts of it) is not actually read.
  // We have to do the same in the verifier: if alias analysis says that an in-
  // argument is not read, there is no need that the memory location is initialized.

  // Not all calls to the verifier provide the alias analysis.
  if (!aliasAnalysis)
    return;

  if (auto *loc = locations.getLocation(addr)) {
    Bits missingBits = ~bits & loc->subLocations;
    for (int errorLocIdx = missingBits.find_first(); errorLocIdx >= 0;
         errorLocIdx = missingBits.find_next(errorLocIdx)) {
      auto *errorLoc = locations.getLocation(errorLocIdx);
      if (aliasAnalysis->mayReadFromMemory(applyInst, errorLoc->representativeValue)) {
        reportError("memory is not initialized, but should be",
                    errorLocIdx, applyInst);
      }
    }
  }
}

void MemoryLifetimeVerifier::requireNoStoreBorrowLocation(
    SILValue addr, SILInstruction *where) {
  if (isa<StoreBorrowInst>(addr)) {
    reportError("store-borrow location cannot be written",
                locations.getLocationIdx(addr), where);
  }
}

void MemoryLifetimeVerifier::registerStoreBorrowLocation(SILValue addr) {
  if (auto *loc = locations.getLocation(addr)) {
    storeBorrowLocations.resize(locations.getNumLocations());
    storeBorrowLocations |= loc->subLocations;
  }
}

void MemoryLifetimeVerifier::registerStoreBorrowsInBlock(SILBasicBlock *block) {
  for (SILInstruction &inst : *block) {
    if (auto *sbi = dyn_cast<StoreBorrowInst>(&inst))
      registerStoreBorrowLocation(sbi->getDest());
  }
}

void MemoryLifetimeVerifier::initDataflow(BitDataflow &dataFlow) {
  // Initialize the entry and exit sets to all-bits-set. Except for the function
  // entry.
  for (auto bs : dataFlow) {
    if (&bs.block == function->getEntryBlock()) {
      bs.data.entrySet.reset();
      for (SILArgument *arg : function->getArguments()) {
        SILFunctionArgument *funcArg = cast<SILFunctionArgument>(arg);
        if (funcArg->getArgumentConvention() !=
              SILArgumentConvention::Indirect_Out) {
          locations.setBits(bs.data.entrySet, arg);
        }
      }
    } else {
      bs.data.entrySet.set();
    }
    bs.data.exitSet.set();

    // Anything weird can happen in unreachable blocks. So just ignore them.
    // Note: while solving the dataflow, unreachable blocks are implicitly
    // ignored, because their entry/exit sets are all-ones and their gen/kill
    // sets are all-zeroes.
    if (bs.data.reachableFromEntry)
      initDataflowInBlock(&bs.block, bs.data);
  }
}

void MemoryLifetimeVerifier::initDataflowInBlock(SILBasicBlock *block,
                                                 BlockState &state) {
  // Initialize the genSet with special cases, like the @out results of an
  // try_apply in the predecessor block.
  setBitsOfPredecessor(state.genSet, state.killSet, block);

  for (SILInstruction &I : *block) {
    switch (I.getKind()) {
      case SILInstructionKind::LoadInst: {
        auto *LI = cast<LoadInst>(&I);
        switch (LI->getOwnershipQualifier()) {
          case LoadOwnershipQualifier::Take:
            killBits(state, LI->getOperand());
            break;
          default:
            break;
        }
        break;
      }
      case SILInstructionKind::StoreInst:
        genBits(state, cast<StoreInst>(&I)->getDest());
        break;
      case SILInstructionKind::StoreBorrowInst: {
        SILValue destAddr = cast<StoreBorrowInst>(&I)->getDest();
        genBits(state, destAddr);
        registerStoreBorrowLocation(destAddr);
        break;
      }
      case SILInstructionKind::CopyAddrInst: {
        auto *CAI = cast<CopyAddrInst>(&I);
        if (CAI->isTakeOfSrc())
          killBits(state, CAI->getSrc());
        genBits(state, CAI->getDest());
        break;
      }
      case SILInstructionKind::MarkUnresolvedMoveAddrInst: {
        auto *MMAI = cast<MarkUnresolvedMoveAddrInst>(&I);
        // We do not treat the move addr inst as invalidating its src since we
        // are going to prove that we do not inappropriately reuse the memory
        // later.
        genBits(state, MMAI->getDest());
        break;
      }
      case SILInstructionKind::InjectEnumAddrInst: {
        auto *IEAI = cast<InjectEnumAddrInst>(&I);
        int enumIdx = locations.getLocationIdx(IEAI->getOperand());
        if (enumIdx >= 0 && injectsNoPayloadCase(IEAI)) {
          // This is a bit tricky: an injected no-payload case means that the
          // "full" enum is initialized. So, for the purpose of dataflow, we
          // treat it like a full initialization of the payload data.
          genBits(state, IEAI->getOperand());
        }
        break;
      }
      case SILInstructionKind::EndBorrowInst: {
        auto *ebi = cast<EndBorrowInst>(&I);
        if (auto *sbi = dyn_cast<StoreBorrowInst>(ebi->getOperand())) {
          killBits(state, sbi->getDest());
        }
        break;
      }
      case SILInstructionKind::DestroyAddrInst:
      case SILInstructionKind::DeallocStackInst:
        killBits(state, I.getOperand(0));
        break;
      case SILInstructionKind::UncheckedRefCastAddrInst:
      case SILInstructionKind::UnconditionalCheckedCastAddrInst: {
        SILValue src = I.getOperand(CopyLikeInstruction::Src);
        SILValue dest = I.getOperand(CopyLikeInstruction::Dest);
        killBits(state, src);
        genBits(state, dest);
        break;
      }
      case SILInstructionKind::PartialApplyInst:
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst: {
        ApplySite AS(&I);
        for (Operand &op : I.getAllOperands()) {
          if (AS.isArgumentOperand(op)) {
            setFuncOperandBits(state, op, AS.getArgumentOperandConvention(op),
                              isa<TryApplyInst>(&I));
          }
        }
        break;
      }
      case SILInstructionKind::BeginApplyInst: {
        auto *BAI = cast<BeginApplyInst>(&I);
        auto yieldedValues = BAI->getYieldedValues();
        for (auto index : indices(yieldedValues)) {
          auto fnType = BAI->getSubstCalleeType();
          SILArgumentConvention argConv(
              fnType->getYields()[index].getConvention());
          if (argConv.isIndirectConvention()) {
            genBits(state, yieldedValues[index]);
          }
        }
        break;
      }
      case SILInstructionKind::EndApplyInst:
      case SILInstructionKind::AbortApplyInst: {
        auto *BAI = [&]() {
          if (auto *EAI = dyn_cast<EndApplyInst>(&I)) {
            return EAI->getBeginApply();
          }
          auto *AAI = dyn_cast<AbortApplyInst>(&I);
          return AAI->getBeginApply();
        }();
        auto yieldedValues = BAI->getYieldedValues();
        for (auto index : indices(yieldedValues)) {
          auto fnType = BAI->getSubstCalleeType();
          SILArgumentConvention argConv(
              fnType->getYields()[index].getConvention());
          if (argConv.isIndirectConvention()) {
            killBits(state, yieldedValues[index]);
          }
        }
        break;
      }
      case SILInstructionKind::YieldInst: {
        auto *YI = cast<YieldInst>(&I);
        for (Operand &op : YI->getAllOperands()) {
          setFuncOperandBits(state, op, YI->getArgumentConventionForOperand(op),
                             /*isTryApply=*/ false);
        }
        break;
      }
      default:
        break;
    }
  }
}

void MemoryLifetimeVerifier::setBitsOfPredecessor(Bits &getSet, Bits &killSet,
                                                  SILBasicBlock *block) {
  SILBasicBlock *pred = block->getSinglePredecessorBlock();
  if (!pred)
    return;

  TermInst *term = pred->getTerminator();
  if (auto *tai = dyn_cast<TryApplyInst>(term)) {
    // @out results of try_apply are only valid in the normal-block, but not in
    // the throw-block.
    if (tai->getNormalBB() != block)
      return;

    FullApplySite FAS(tai);
    for (Operand &op : tai->getAllOperands()) {
      if (FAS.isArgumentOperand(op) &&
          FAS.getArgumentConvention(op) == SILArgumentConvention::Indirect_Out) {
        locations.genBits(getSet, killSet, op.get());
      }
    }
  } else if (auto *castInst = dyn_cast<CheckedCastAddrBranchInst>(term)) {
    switch (castInst->getConsumptionKind()) {
    case CastConsumptionKind::TakeAlways:
      locations.killBits(getSet, killSet, castInst->getSrc());
      break;
    case CastConsumptionKind::TakeOnSuccess:
      if (castInst->getSuccessBB() == block)
        locations.killBits(getSet, killSet, castInst->getSrc());
      break;
    case CastConsumptionKind::CopyOnSuccess:
      break;
    case CastConsumptionKind::BorrowAlways:
      llvm_unreachable("checked_cast_addr_br cannot have BorrowAlways");
    }
    if (castInst->getSuccessBB() == block)
      locations.genBits(getSet, killSet, castInst->getDest());
  }
}

void MemoryLifetimeVerifier::setFuncOperandBits(BlockState &state, Operand &op,
                                        SILArgumentConvention convention,
                                        bool isTryApply) {
  switch (convention) {
    case SILArgumentConvention::Indirect_In:
      killBits(state, op.get());
      break;
    case SILArgumentConvention::Indirect_Out:
      // try_apply is special, because an @out result is only initialized
      // in the normal-block, but not in the throw-block.
      // We handle the @out result of try_apply in setBitsOfPredecessor.
      if (!isTryApply)
        genBits(state, op.get());
      break;
    case SILArgumentConvention::Indirect_In_Guaranteed:
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_InoutAliasable:
    case SILArgumentConvention::Direct_Owned:
    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Guaranteed:
    case SILArgumentConvention::Pack_Inout:
    case SILArgumentConvention::Pack_Out:
    case SILArgumentConvention::Pack_Guaranteed:
    case SILArgumentConvention::Pack_Owned:
      break;
  }
}

void MemoryLifetimeVerifier::checkFunction(BitDataflow &dataFlow) {

  // Collect the bits which we require to be set at function exits.
  Bits expectedReturnBits(locations.getNumLocations());
  Bits expectedThrowBits(locations.getNumLocations());
  for (SILArgument *arg : function->getArguments()) {
    SILFunctionArgument *funcArg = cast<SILFunctionArgument>(arg);
    switch (funcArg->getArgumentConvention()) {
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_In_Guaranteed:
      locations.setBits(expectedReturnBits, funcArg);
      locations.setBits(expectedThrowBits, funcArg);
      break;
    case SILArgumentConvention::Indirect_Out:
      locations.setBits(expectedReturnBits, funcArg);
      break;
    default:
      break;
    }
  }

  const Bits &nonTrivialLocations = locations.getNonTrivialLocations();
  Bits bits(locations.getNumLocations());
  for (auto bs : dataFlow) {
    if (!bs.data.reachableFromEntry || !bs.data.exitReachable())
      continue;

    // Check all instructions in the block.
    bits = bs.data.entrySet;
    checkBlock(&bs.block, bits);

    // Check if there is a mismatch in location lifetime at the merge point.
    for (SILBasicBlock *pred : bs.block.getPredecessorBlocks()) {
      BlockState &predState = dataFlow[pred];
      if (predState.reachableFromEntry) {
        require((bs.data.entrySet ^ predState.exitSet) & nonTrivialLocations,
          "lifetime mismatch in predecessors", pred->getTerminator(),
          /*excludeTrivialEnums*/ true);
      }
    }

    // Check the bits at function exit.
    TermInst *term = bs.block.getTerminator();
    assert(bits == bs.data.exitSet || isa<TryApplyInst>(term));
    switch (term->getKind()) {
      case SILInstructionKind::ReturnInst:
      case SILInstructionKind::UnwindInst:
        require(expectedReturnBits & ~bs.data.exitSet,
          "indirect argument is not alive at function return", term);
        require(bs.data.exitSet & ~expectedReturnBits & nonTrivialLocations,
                "memory is initialized at function return but shouldn't be",
                term,
                /*excludeTrivialEnums*/ true);
        break;
      case SILInstructionKind::ThrowInst:
        require(expectedThrowBits & ~bs.data.exitSet,
          "indirect argument is not alive at throw", term);
        require(bs.data.exitSet & ~expectedThrowBits & nonTrivialLocations,
                "memory is initialized at throw but shouldn't be", term,
                /*excludeTrivialEnums*/ true);
        break;
      default:
        break;
    }
  }
}

void MemoryLifetimeVerifier::checkBlock(SILBasicBlock *block, Bits &bits) {
  setBitsOfPredecessor(bits, bits, block);
  const Bits &nonTrivialLocations = locations.getNonTrivialLocations();

  for (SILInstruction &I : *block) {
    switch (I.getKind()) {
      case SILInstructionKind::LoadInst: {
        auto *LI = cast<LoadInst>(&I);
        requireBitsSet(bits, LI->getOperand(), &I);
        switch (LI->getOwnershipQualifier()) {
          case LoadOwnershipQualifier::Take:
            locations.clearBits(bits, LI->getOperand());
            requireNoStoreBorrowLocation(LI->getOperand(), &I);
            break;
          case LoadOwnershipQualifier::Copy:
          case LoadOwnershipQualifier::Trivial:
            break;
          case LoadOwnershipQualifier::Unqualified:
            llvm_unreachable("unqualified load shouldn't be in ownership SIL");
        }
        break;
      }
      case SILInstructionKind::StoreInst: {
        auto *SI = cast<StoreInst>(&I);
        switch (SI->getOwnershipQualifier()) {
          case StoreOwnershipQualifier::Init:
            requireBitsClear(bits & nonTrivialLocations, SI->getDest(), &I);
            locations.setBits(bits, SI->getDest());
            break;
          case StoreOwnershipQualifier::Assign:
            requireBitsSet(bits | ~nonTrivialLocations, SI->getDest(), &I);
            break;
          case StoreOwnershipQualifier::Trivial:
            locations.setBits(bits, SI->getDest());
            break;
          case StoreOwnershipQualifier::Unqualified:
            llvm_unreachable("unqualified store shouldn't be in ownership SIL");
        }
        requireNoStoreBorrowLocation(SI->getDest(), &I);
        break;
      }
      case SILInstructionKind::StoreBorrowInst: {
        SILValue destAddr = cast<StoreBorrowInst>(&I)->getDest();
        locations.setBits(bits, destAddr);
        break;
      }
      case SILInstructionKind::CopyAddrInst: {
        auto *CAI = cast<CopyAddrInst>(&I);
        requireBitsSet(bits, CAI->getSrc(), &I);
        if (CAI->isTakeOfSrc()) {
          locations.clearBits(bits, CAI->getSrc());
          requireNoStoreBorrowLocation(CAI->getSrc(), &I);
        }
        if (CAI->isInitializationOfDest()) {
          requireBitsClear(bits & nonTrivialLocations, CAI->getDest(), &I);
        } else {
          requireBitsSet(bits | ~nonTrivialLocations, CAI->getDest(), &I);
        }
        locations.setBits(bits, CAI->getDest());
        requireNoStoreBorrowLocation(CAI->getDest(), &I);
        break;
      }
      case SILInstructionKind::InjectEnumAddrInst: {
        auto *IEAI = cast<InjectEnumAddrInst>(&I);
        int enumIdx = locations.getLocationIdx(IEAI->getOperand());
        if (enumIdx >= 0 && injectsNoPayloadCase(IEAI)) {
          // Again, an injected no-payload case is treated like a "full"
          // initialization. See initDataflowInBlock().
          requireBitsClear(bits & nonTrivialLocations, IEAI->getOperand(), &I);
          locations.setBits(bits, IEAI->getOperand());
        }
        requireNoStoreBorrowLocation(IEAI->getOperand(), &I);
        break;
      }
      case SILInstructionKind::InitExistentialAddrInst:
      case SILInstructionKind::InitEnumDataAddrInst: {
        SILValue addr = I.getOperand(0);
        requireBitsClear(bits & nonTrivialLocations, addr, &I);
        requireNoStoreBorrowLocation(addr, &I);
        break;
      }
      case SILInstructionKind::OpenExistentialAddrInst:
      case SILInstructionKind::SelectEnumAddrInst:
      case SILInstructionKind::ExistentialMetatypeInst:
      case SILInstructionKind::ValueMetatypeInst:
      case SILInstructionKind::IsUniqueInst:
      case SILInstructionKind::FixLifetimeInst:
        requireBitsSet(bits, I.getOperand(0), &I);
        break;
      case SILInstructionKind::DebugValueInst:
        // We don't want to check `debug_value` instructions that
        // are used to mark variable declarations (e.g. its SSA value is
        // an alloc_stack), which don't have any `op_deref` in its
        // di-expression, because that memory doesn't need to be initialized
        // when `debug_value` is referencing it.
        if (cast<DebugValueInst>(&I)->hasAddrVal() &&
            cast<DebugValueInst>(&I)->exprStartsWithDeref())
          requireBitsSet(bits, I.getOperand(0), &I);
        break;
      case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
        // Note that despite the name, unchecked_take_enum_data_addr does _not_
        // "take" the payload of the Swift.Optional enum. This is a terrible
        // hack in SIL.
        SILValue enumAddr = cast<UncheckedTakeEnumDataAddrInst>(&I)->getOperand();
        int enumIdx = locations.getLocationIdx(enumAddr);
        if (enumIdx >= 0)
          requireBitsSet(bits, enumAddr, &I);
        requireNoStoreBorrowLocation(enumAddr, &I);
        break;
      }
      case SILInstructionKind::DestroyAddrInst: {
        SILValue opVal = cast<DestroyAddrInst>(&I)->getOperand();
        requireBitsSet(bits | ~nonTrivialLocations, opVal, &I);
        locations.clearBits(bits, opVal);
        requireNoStoreBorrowLocation(opVal, &I);
        break;
      }
      case SILInstructionKind::EndBorrowInst: {
        auto *ebi = cast<EndBorrowInst>(&I);
        if (auto *sbi = dyn_cast<StoreBorrowInst>(ebi->getOperand())) {
          requireBitsSet(bits, sbi->getDest(), &I);
          locations.clearBits(bits, sbi->getDest());
        } else if (auto *lbi = dyn_cast<LoadBorrowInst>(ebi->getOperand())) {
          requireBitsSet(bits, lbi->getOperand(), &I);
        }
        break;
      }
      case SILInstructionKind::UncheckedRefCastAddrInst:
      case SILInstructionKind::UnconditionalCheckedCastAddrInst: {
        SILValue src = I.getOperand(CopyLikeInstruction::Src);
        SILValue dest = I.getOperand(CopyLikeInstruction::Dest);
        requireBitsSet(bits, src, &I);
        locations.clearBits(bits, src);
        requireBitsClear(bits & nonTrivialLocations, dest, &I);
        locations.setBits(bits, dest);
        requireNoStoreBorrowLocation(dest, &I);
        break;
      }
      case SILInstructionKind::CheckedCastAddrBranchInst: {
        auto *castInst = cast<CheckedCastAddrBranchInst>(&I);
        requireBitsSet(bits, castInst->getSrc(), &I);
        requireBitsClear(bits & nonTrivialLocations, castInst->getDest(), &I);
        break;
      }
      case SILInstructionKind::PartialApplyInst:
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst: {
        ApplySite AS(&I);
        for (Operand &op : I.getAllOperands()) {
          if (AS.isArgumentOperand(op))
            checkFuncArgument(bits, op, AS.getArgumentOperandConvention(op), &I);
        }
        break;
      }
      case SILInstructionKind::BeginApplyInst: {
        auto *BAI = cast<BeginApplyInst>(&I);
        auto yieldedValues = BAI->getYieldedValues();
        for (auto index : indices(yieldedValues)) {
          auto fnType = BAI->getSubstCalleeType();
          SILArgumentConvention argConv(
              fnType->getYields()[index].getConvention());
          if (argConv.isIndirectConvention()) {
            requireBitsClear(bits, yieldedValues[index], &I);
            locations.setBits(bits, yieldedValues[index]);
          }
        }
        break;
      }
      case SILInstructionKind::EndApplyInst:
      case SILInstructionKind::AbortApplyInst: {
        auto *BAI = [&]() {
          if (auto *EAI = dyn_cast<EndApplyInst>(&I)) {
            return EAI->getBeginApply();
          }
          auto *AAI = dyn_cast<AbortApplyInst>(&I);
          return AAI->getBeginApply();
        }();
        auto yieldedValues = BAI->getYieldedValues();
        for (auto index : indices(yieldedValues)) {
          auto fnType = BAI->getSubstCalleeType();
          SILArgumentConvention argConv(
              fnType->getYields()[index].getConvention());
          if (argConv.isIndirectConvention()) {
            if (argConv.isInoutConvention() ||
                argConv.isGuaranteedConvention()) {
              requireBitsSet(bits | ~nonTrivialLocations, yieldedValues[index],
                             &I);
            } else if (argConv.isOwnedConvention()) {
              requireBitsClear(bits & nonTrivialLocations, yieldedValues[index],
                               &I);
            }
            locations.clearBits(bits, yieldedValues[index]);
          }
        }
        break;
      }
      case SILInstructionKind::YieldInst: {
        auto *YI = cast<YieldInst>(&I);
        for (Operand &op : YI->getAllOperands()) {
          checkFuncArgument(bits, op, YI->getArgumentConventionForOperand(op),
                             &I);
        }
        break;
      }
      case SILInstructionKind::DeallocStackInst: {
        SILValue opVal = cast<DeallocStackInst>(&I)->getOperand();
        requireBitsClear(bits & nonTrivialLocations, opVal, &I);
        // Needed to clear any bits of trivial locations (which are not required
        // to be zero).
        locations.clearBits(bits, opVal);
        break;
      }
      default:
        break;
    }
  }
}

void MemoryLifetimeVerifier::checkFuncArgument(Bits &bits, Operand &argumentOp,
                         SILArgumentConvention argumentConvention,
                         SILInstruction *applyInst) {
  if (argumentConvention != SILArgumentConvention::Indirect_In_Guaranteed)
    requireNoStoreBorrowLocation(argumentOp.get(), applyInst);
  
  switch (argumentConvention) {
    case SILArgumentConvention::Indirect_In:
      requireBitsSetForArgument(bits, argumentOp.get(), applyInst);
      locations.clearBits(bits, argumentOp.get());
      break;
    case SILArgumentConvention::Indirect_Out:
      requireBitsClear(bits & locations.getNonTrivialLocations(),
                       argumentOp.get(), applyInst);
      locations.setBits(bits, argumentOp.get());
      break;
    case SILArgumentConvention::Indirect_In_Guaranteed:
    case SILArgumentConvention::Indirect_Inout:
      requireBitsSetForArgument(bits, argumentOp.get(), applyInst);
      break;
    case SILArgumentConvention::Indirect_InoutAliasable:
      // We don't require any locations to be initialized for a partial_apply
      // which takes an inout_aliasable argument. This is used for implicit
      // closures (e.g. for the Bool '||' and '&&' operator arguments). Such
      // closures capture the whole "self". When this is done in an initializer
      // it can happen that not all fields of "self" are initialized, yet.
      if (!isa<PartialApplyInst>(applyInst))
        requireBitsSetForArgument(bits, argumentOp.get(), applyInst);
      break;
    case SILArgumentConvention::Direct_Owned:
    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Guaranteed:
    case SILArgumentConvention::Pack_Inout:
    case SILArgumentConvention::Pack_Out:
    case SILArgumentConvention::Pack_Guaranteed:
    case SILArgumentConvention::Pack_Owned:
      break;
  }
}

void MemoryLifetimeVerifier::verify() {
  // First step: handle memory locations which (potentially) span multiple
  // blocks.
  locations.analyzeLocations(function);
  if (locations.getNumLocations() > 0) {
    BitDataflow dataFlow(function, locations.getNumLocations());
    dataFlow.entryReachabilityAnalysis();
    dataFlow.exitReachableAnalysis();
    initDataflow(dataFlow);
    dataFlow.solveForwardWithIntersect();
    checkFunction(dataFlow);
  }
  // Second step: handle single-block locations.
  locations.handleSingleBlockLocations([this](SILBasicBlock *block) {
    storeBorrowLocations.clear();
    Bits bits(locations.getNumLocations());
    registerStoreBorrowsInBlock(block);
    checkBlock(block, bits);
  });
}

} // anonymous namespace

void SILFunction::verifyMemoryLifetime(SILPassManager *passManager) {
  MemoryLifetimeVerifier verifier(this, passManager);
  verifier.verify();
}
