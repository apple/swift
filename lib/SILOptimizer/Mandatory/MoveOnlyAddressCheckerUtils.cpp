//===--- MoveOnlyAddressCheckerUtils.cpp ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Move Only Checking of Addresses
/// -------------------------------
///
/// In this file, we implement move checking of addresses. This allows for the
/// compiler to perform move checking of address only lets, vars, inout args,
/// and mutating self.
///
/// Move Address Checking in Swift
/// ------------------------------
///
/// In order to not have to rewrite all of SILGen to avoid copies, Swift has
/// taken an approach where SILGen marks moveonly addresses with a special
/// marker instruction and emits copies when it attempts to access move only
/// addresses. Then this algorithm fixed up SILGen's output by analyzing the
/// memory uses of a marked memory root location recursively using AccessPath
/// based analyses and then attempting to transform those uses based off of the
/// marked kind into one of a few variants of "simple move only address form"
/// (see below for more information). If the pass is unable to reason that it
/// can safely transform the uses into said form, we emit a diagnostic stating
/// the error to the user. If we emit said diagnostic, we then bail early. If we
/// do not emit a diagnostic, we then transform the IR so that the move only
/// address uses are in said form. This then guarantees that despite SILGen
/// emitting move only types with copies, in the end, our move only types are
/// never copied. As an additional check, once the pass has run we emit an extra
/// diagnostic if we find any copies of move only types so that the user can be
/// sure that any accepted program does not copy move only types.
///
/// Simple Move Only Address Form
/// -----------------------------
///
/// We define a memory location to be in "simple move only address" form (SMOA
/// form for ease of typing) to mean that along any path from an init of the
/// address to a consume of the address, all uses are guaranteed to be semantic
/// "borrow uses" instead of semantic "copy uses". Additionally, SMOA does not
/// consider destroy_addr to be a true consuming use since it will rewrite
/// destroy_addr as necessary so the consuming uses are defined by consuming
/// uses modulo destroy_addr.
///
/// An example of a memory location in "simple move only address form" is the
/// following:
///
/// ```
/// // Memory is defined
/// %0 = alloc_stack $Type
///
/// // Initial initialization.
/// store %input to [init] %0 : $Type
///
/// // Sequence of borrow uses.
/// %1 = load_borrow %0 : $Type
/// apply %f(%1) : $@convention(thin) (@guaranteed Type) -> ()
/// end_borrow %1
/// apply %f2(%0) : $@convention(thin) (@in_guaranteed Type) -> ()
///
/// // Assign is ok since we are just consuming the value.
/// store %input2 to [assign] %0 : $*Type
///
/// // More borrow uses.
/// %3 = load_borrow %0 : $*Type
/// apply %f(%3) : $@convention(thin) (@guaranteed Type) -> ()
/// end_borrow %1
/// apply %f2(%0) : $@convention(thin) (@in_guaranteed Type) -> ()
///
/// // Final destroy
/// destroy_addr %0 : $Type
/// ```
///
/// An example of an instruction not in SMOA form is:
///
/// ```
/// // Memory is defined
/// %0 = alloc_stack $Type
///
/// // Initial initialization.
/// store %input to [init] %0 : $*Type
///
/// // Perform a load + copy of %0 to pass as an argument to %f.
/// %1 = load [copy] %0 : $*Type
/// apply %f(%1) : $@convention(thin) (@guaranteed Type) -> ()
/// destroy_value %1 : $Type
///
/// // Initialize other variable.
/// %otherVar = alloc_stack $Type
/// copy_addr %0 to [initialization] %otherVar : $*Type
/// ...
///
/// // Final destroy that is not part of the use set.
/// destroy_addr %0 : $*Type
/// ```
///
/// The variants of SMOA form can be classified by the specific mark_must_check
/// kind put on the the checker mark instruction and are as follows:
///
/// 1. no_consume_or_assign. This means that the address can only be consumed by
/// destroy_addr and otherwise is only read from. This simulates guaranteed
/// semantics.
///
/// 2. consumable_and_assignable. This means that the address can be consumed
/// (e.x.: take/pass to a +1 function) or assigned to. Additionally, the value
/// is supposed to have its lifetime end along all program paths locally in the
/// function. This simulates a local var's semantics.
///
/// 3. assignable_but_not_consumable. This means that the address can be
/// assigned over, but cannot be taken from. It additionally must have a valid
/// value in it and the end of its lifetime. This simulates accesses to class
/// fields, globals, and escaping mutable captures where we want the user to be
/// able to update the value, but allowing for escapes of the value would break
/// memory safety. In all cases where this is used, the mark_must_check is used
/// as the initial def of the value lifetime. Example:
///
/// 4. initable_but_not_consumable. This means that the address can only be
/// initialized once but cannot be taken from or assigned over. It is assumed
/// that the initial def will always be the mark_must_check and that the value
/// will be uninitialized at that point. Example:
///
/// Algorithm Stages In Detail
/// --------------------------
///
/// To implement this, our algorithm works in 4 stages: a use classification
/// stage, a dataflow stage, and then depending on success/failure one of two
/// transform stages.
///
/// Use Classification Stage
/// ~~~~~~~~~~~~~~~~~~~~~~~~
///
/// Here we use an AccessPath based analysis to transitively visit all uses of
/// our marked address and classify a use as one of the following kinds of uses:
///
/// * init - store [init], copy_addr [init] %dest.
/// * destroy - destroy_addr.
/// * pureTake - load [take], copy_addr [take] %src.
/// * copyTransformableToTake - certain load [copy], certain copy_addr ![take]
/// %src of a temporary %dest.
/// * reinit - store [assign], copy_addr ![init] %dest
/// * borrow - load_borror, a load [copy] without consuming uses.
/// * livenessOnly - a read only use of the address.
///
/// We classify these by adding them to several disjoint SetVectors which track
/// membership.
///
/// When we classify an instruction as copyTransformableToTake, we perform some
/// extra preprocessing to determine if we can actually transform this copy to a
/// take. This means that we:
///
/// 1. For loads, we perform object move only checking. If we find a need for
/// multiple copies, we emit an error. If we find no extra copies needed, we
/// classify the load [copy] as a take if it has any last consuming uses and a
/// borrow if it only has destroy_addr consuming uses.
///
/// 2. For copy_addr, we pattern match if a copy_addr is initializing a "simple
/// temporary" (an alloc_stack with only one use that initializes it, a
/// copy_addr [init] in the same block). In this case, if the copy_addr only has
/// destroy_addr consuming uses, we treat it as a borrow... otherwise, we treat
/// it as a take. If we find any extra initializations, we fail the visitor so
/// we emit a "I don't understand this error" so that users report this case and
/// we can extend it as appropriate.
///
/// If we fail in either case, if we emit an error, we bail early with success
/// so we can assume invariants later in the dataflow stages that make the
/// dataflow easier.
///
/// Dataflow Stage
/// ~~~~~~~~~~~~~~
///
/// To perform our dataflow, we take our classified uses and initialize field
/// sensitive pruned liveness with the data. We then use field sensitive pruned
/// liveness and our check kinds to determine if all of our copy uses that could
/// not be changed into borrows are on the liveness boundary of the memory. If
/// they are within the liveness boundary, then we know a copy is needed and we
/// emit an error to the user. Otherwise, we know that we can change them
/// semantically into a take.
///
/// Success Transformation
/// ~~~~~~~~~~~~~~~~~~~~~~
///
/// Now that we know that we can change our address into "simple move only
/// address form", we transform the IR in the following way:
///
/// 1. Any load [copy] that are classified as borrows are changed to
///    load_borrow.
/// 2. Any load [copy] that are classified as takes are changed to load [take].
/// 3. Any copy_addr [init] temporary allocation are eliminated with their
///    destroy_addr. All uses are placed on the source address.
/// 4. Any destroy_addr that is paired with a copyTransformableToTake is
///    eliminated.
///
/// Fail Transformation
/// ~~~~~~~~~~~~~~~~~~~
///
/// If we emit any diagnostics, we loop through the function one last time after
/// we are done processing and convert all load [copy]/copy_addr of move only
/// types into their explicit forms. We take a little more compile time, but we
/// are going to fail anyways at this point, so it is ok to do so since we will
/// fail before attempting to codegen into LLVM IR.
///
/// Final Black Box Checks on Success
/// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
///
/// Finally since we want to be able to guarantee to users 100% that the
/// compiler will reject programs even if the checker gives a false success for
/// some reason due to human compiler writer error, we do a last pass over the
/// IR and emit an error diagnostic on any copies of move only types that we
/// see. The error states to the user that this is a compiler bug and to file a
/// bug report. Since it is a completely separate, simple implementation, this
/// gives the user of our implementation the confidence to know that the
/// compiler even in the face of complexity in the checker will emit correct
/// code.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/AST/AccessScope.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#include "MoveOnlyAddressCheckerUtils.h"
#include "MoveOnlyBorrowToDestructureUtils.h"
#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyObjectCheckerUtils.h"
#include "MoveOnlyTypeUtils.h"
#include "MoveOnlyUtils.h"

#include <utility>

using namespace swift;
using namespace swift::siloptimizer;

#pragma clang optimize off

llvm::cl::opt<bool> DisableMoveOnlyAddressCheckerLifetimeExtension(
    "move-only-address-checker-disable-lifetime-extension",
    llvm::cl::init(false),
    llvm::cl::desc("Disable the lifetime extension of non-consumed fields of "
                   "move-only values."));

//===----------------------------------------------------------------------===//
//                              MARK: Utilities
//===----------------------------------------------------------------------===//

struct RAIILLVMDebug {
  StringRef str;

  RAIILLVMDebug(StringRef str) : str(str) {
    LLVM_DEBUG(llvm::dbgs() << "===>>> Starting " << str << '\n');
  }

  RAIILLVMDebug(StringRef str, SILInstruction *u) : str(str) {
    LLVM_DEBUG(llvm::dbgs() << "===>>> Starting " << str << ":" << *u);
  }

  ~RAIILLVMDebug() {
    LLVM_DEBUG(llvm::dbgs() << "===<<< Completed " << str << '\n');
  }
};

static void insertDebugValueBefore(SILInstruction *insertPt,
                                   DebugVarCarryingInst debugVar,
                                   llvm::function_ref<SILValue ()> operand) {
  if (!debugVar) {
    return;
  }
  auto varInfo = debugVar.getVarInfo();
  if (!varInfo) {
    return;
  }
  SILBuilderWithScope debugInfoBuilder(insertPt);
  debugInfoBuilder.setCurrentDebugScope(debugVar->getDebugScope());
  debugInfoBuilder.createDebugValue(debugVar->getLoc(), operand(),
                                    *varInfo, false, true);
}

static void convertMemoryReinitToInitForm(SILInstruction *memInst,
                                          DebugVarCarryingInst debugVar) {
  SILValue dest;
  switch (memInst->getKind()) {
  default:
    llvm_unreachable("unsupported?!");

  case SILInstructionKind::CopyAddrInst: {
    auto *cai = cast<CopyAddrInst>(memInst);
    cai->setIsInitializationOfDest(IsInitialization_t::IsInitialization);
    dest = cai->getDest();
    break;
  }
  case SILInstructionKind::StoreInst: {
    auto *si = cast<StoreInst>(memInst);
    si->setOwnershipQualifier(StoreOwnershipQualifier::Init);
    dest = si->getDest();
    break;
  }
  }

  // Insert a new debug_value instruction after the reinitialization, so that
  // the debugger knows that the variable is in a usable form again.
  insertDebugValueBefore(memInst->getNextInstruction(), debugVar,
                       [&]{ return debugVar.getOperandForDebugValueClone(); });
}

/// Is this a reinit instruction that we know how to convert into its init form.
static bool isReinitToInitConvertibleInst(SILInstruction *memInst) {
  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *cai = cast<CopyAddrInst>(memInst);
    return !cai->isInitializationOfDest();
  }
  case SILInstructionKind::StoreInst: {
    auto *si = cast<StoreInst>(memInst);
    return si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign;
  }
  }
}

/// Returns true if \p value a function argument from an inout argument or a
/// value extracted from a closure captured box that we did not convert to an
/// address.
///
/// These are cases where we want to treat the end of the function as a liveness
/// use to ensure that we reinitialize \p value before the end of the function
/// if we consume \p value in the function body.
static bool isInOutDefThatNeedsEndOfFunctionLiveness(MarkMustCheckInst *markedAddr) {
  SILValue operand = markedAddr->getOperand();

  // Check for inout types of arguments that are marked with consumable and
  // assignable.
  if (markedAddr->getCheckKind() ==
      MarkMustCheckInst::CheckKind::ConsumableAndAssignable) {
    if (auto *fArg = dyn_cast<SILFunctionArgument>(operand)) {
      switch (fArg->getArgumentConvention()) {
      case SILArgumentConvention::Indirect_In:
      case SILArgumentConvention::Indirect_Out:
      case SILArgumentConvention::Indirect_In_Guaranteed:
      case SILArgumentConvention::Direct_Guaranteed:
      case SILArgumentConvention::Direct_Owned:
      case SILArgumentConvention::Direct_Unowned:
      case SILArgumentConvention::Pack_Guaranteed:
      case SILArgumentConvention::Pack_Owned:
      case SILArgumentConvention::Pack_Out:
        return false;
      case SILArgumentConvention::Indirect_Inout:
      case SILArgumentConvention::Indirect_InoutAliasable:
      case SILArgumentConvention::Pack_Inout:
        LLVM_DEBUG(llvm::dbgs() << "Found inout arg: " << *fArg);
        return true;
      }
    }
  }

  return false;
}

static bool isCopyableValue(SILValue value) {
  if (value->getType().isMoveOnly())
    return false;
  if (auto *m = dyn_cast<MoveOnlyWrapperToCopyableAddrInst>(value))
    return false;
  return true;
}

//===----------------------------------------------------------------------===//
//                   MARK: Find Candidate Mark Must Checks
//===----------------------------------------------------------------------===//

void swift::siloptimizer::searchForCandidateAddressMarkMustChecks(
    SILFunction *fn,
    llvm::SmallSetVector<MarkMustCheckInst *, 32> &moveIntroducersToProcess,
    DiagnosticEmitter &diagnosticEmitter) {
  for (auto &block : *fn) {
    for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
      auto *mmci = dyn_cast<MarkMustCheckInst>(&*ii);
      ++ii;

      if (!mmci || !mmci->hasMoveCheckerKind() || !mmci->getType().isAddress())
        continue;

      moveIntroducersToProcess.insert(mmci);
    }
  }
}

//===----------------------------------------------------------------------===//
//                              MARK: Use State
//===----------------------------------------------------------------------===//

namespace {

struct UseState {
  MarkMustCheckInst *address;

  using InstToBitMap =
      llvm::SmallMapVector<SILInstruction *, SmallBitVector, 4>;

  llvm::Optional<unsigned> cachedNumSubelements;

  /// The blocks that consume fields of the value.
  ///
  /// A map from blocks to a bit vector recording which fields were destroyed
  /// in each.
  llvm::SmallMapVector<SILBasicBlock *, SmallBitVector, 8> consumingBlocks;

  /// A map from destroy_addr to the part of the type that it destroys.
  llvm::SmallMapVector<SILInstruction *, TypeTreeLeafTypeRange, 4> destroys;

  /// A map from a liveness requiring use to the part of the type that it
  /// requires liveness for.
  InstToBitMap livenessUses;

  /// A map from a load [copy] or load [take] that we determined must be
  /// converted to a load_borrow to the part of the type tree that it needs to
  /// borrow.
  ///
  /// NOTE: This does not include actual load_borrow which are treated
  /// just as liveness uses.
  ///
  /// NOTE: load_borrow that we actually copy, we canonicalize early to a load
  /// [copy] + begin_borrow so that we do not need to convert load_borrow to a
  /// normal load when rewriting.
  llvm::SmallMapVector<SILInstruction *, TypeTreeLeafTypeRange, 4> borrows;

  /// A copy_addr, load [copy], or load [take] that we determine is semantically
  /// truly a take mapped to the part of the type tree that it needs to use.
  ///
  /// DISCUSSION: A copy_addr [init] or load [copy] are considered actually
  /// takes if they are not destroyed with a destroy_addr/destroy_value. We
  /// consider them to be takes since after the transform they must be a take.
  ///
  /// Importantly, these we know are never copied and are only consumed once.
  llvm::SmallMapVector<SILInstruction *, TypeTreeLeafTypeRange, 4> takeInsts;

  /// A map from a copy_addr, load [copy], or load [take] that we determine
  /// semantically are true copies to the part of the type tree they must copy.
  ///
  /// DISCUSSION: One of these instructions being a true copy means that their
  /// result or destination is used in a way that some sort of extra copy is
  /// needed. Example:
  ///
  /// %0 = load [take] %addr
  /// %1 = copy_value %0
  /// consume(%0)
  /// consume(%1)
  ///
  /// Notice how the load [take] above semantically requires a copy since it was
  /// consumed twice even though SILGen emitted it as a load [take].
  ///
  /// We represent these separately from \p takeInsts since:
  ///
  /// 1.
  llvm::SmallMapVector<SILInstruction *, TypeTreeLeafTypeRange, 4> copyInsts;

  /// A map from an instruction that initializes memory to the description of
  /// the part of the type tree that it initializes.
  InstToBitMap initInsts;

  SmallFrozenMultiMap<SILInstruction *, SILValue, 8> initToValueMultiMap;

  /// memInstMustReinitialize insts. Contains both insts like copy_addr/store
  /// [assign] that are reinits that we will convert to inits and true reinits.
  InstToBitMap reinitInsts;

  SmallFrozenMultiMap<SILInstruction *, SILValue, 8> reinitToValueMultiMap;

  /// The set of drop_deinits of this mark_must_check
  SmallSetVector<SILInstruction *, 2> dropDeinitInsts;

  /// A "inout terminator use" is an implicit liveness use of the entire value
  /// placed on a terminator. We use this both so we add liveness for the
  /// terminator user and so that we can use the set to quickly identify later
  /// while emitting diagnostics that a liveness use is a terminator user and
  /// emit a specific diagnostic message.
  SmallSetVector<SILInstruction *, 2> implicitEndOfLifetimeLivenessUses;

  /// We add debug_values to liveness late after we diagnose, but before we
  /// hoist destroys to ensure that we do not hoist destroys out of access
  /// scopes.
  DebugValueInst *debugValue = nullptr;

  SILFunction *getFunction() const { return address->getFunction(); }

  /// The number of fields in the exploded type.
  unsigned getNumSubelements() {
    if (!cachedNumSubelements) {
      cachedNumSubelements = TypeSubElementCount(address);
    }
    return *cachedNumSubelements;
  }

  SmallBitVector &getOrCreateAffectedBits(SILInstruction *inst,
                                          InstToBitMap &map) {
    auto iter = map.find(inst);
    if (iter == map.end()) {
      iter = map.insert({inst, SmallBitVector(getNumSubelements())}).first;
    }
    return iter->second;
  }

  void setAffectedBits(SILInstruction *inst, SmallBitVector const &bits,
                       InstToBitMap &map) {
    getOrCreateAffectedBits(inst, map) |= bits;
  }

  void setAffectedBits(SILInstruction *inst, TypeTreeLeafTypeRange range,
                       InstToBitMap &map) {
    range.setBits(getOrCreateAffectedBits(inst, map));
  }

  void recordLivenessUse(SILInstruction *inst, SmallBitVector const &bits) {
    setAffectedBits(inst, bits, livenessUses);
  }

  void recordLivenessUse(SILInstruction *inst, TypeTreeLeafTypeRange range) {
    setAffectedBits(inst, range, livenessUses);
  }

  void recordReinitUse(SILInstruction *inst, SILValue value,
                       TypeTreeLeafTypeRange range) {
    reinitToValueMultiMap.insert(inst, value);
    setAffectedBits(inst, range, reinitInsts);
  }

  void recordInitUse(SILInstruction *inst, SILValue value,
                     TypeTreeLeafTypeRange range) {
    initToValueMultiMap.insert(inst, value);
    setAffectedBits(inst, range, initInsts);
  }

  /// Returns true if this is an instruction that is used by the pass to ensure
  /// that we reinit said argument if we consumed it in a region of code.
  ///
  /// Example:
  ///
  /// 1. In the case of an inout argument, this will contain the terminator
  /// instruction.
  /// 2. In the case of a ref_element_addr or a global, this will contain the
  /// end_access.
  bool isImplicitEndOfLifetimeLivenessUses(SILInstruction *inst) const {
    return implicitEndOfLifetimeLivenessUses.count(inst);
  }

  /// Returns true if the given instruction is within the same block as a reinit
  /// and precedes a reinit instruction in that block.
  bool precedesReinitInSameBlock(SILInstruction *inst) const {
    SILBasicBlock *block = inst->getParent();
    SmallSetVector<SILInstruction *, 8> sameBlockReinits;

    // First, search for all reinits that are within the same block.
    for (auto &reinit : reinitInsts) {
      if (reinit.first->getParent() != block)
        continue;
      sameBlockReinits.insert(reinit.first);
    }

    if (sameBlockReinits.empty())
      return false;

    // Walk down from the given instruction to see if we encounter a reinit.
    for (auto ii = std::next(inst->getIterator()); ii != block->end(); ++ii) {
      if (sameBlockReinits.contains(&*ii))
        return true;
    }

    return false;
  }

  void clear() {
    address = nullptr;
    cachedNumSubelements = llvm::None;
    consumingBlocks.clear();
    destroys.clear();
    livenessUses.clear();
    borrows.clear();
    copyInsts.clear();
    takeInsts.clear();
    initInsts.clear();
    initToValueMultiMap.reset();
    reinitInsts.clear();
    reinitToValueMultiMap.reset();
    dropDeinitInsts.clear();
    implicitEndOfLifetimeLivenessUses.clear();
    debugValue = nullptr;
  }

  void dump() {
    llvm::dbgs() << "AddressUseState!\n";
    llvm::dbgs() << "Destroys:\n";
    for (auto pair : destroys) {
      llvm::dbgs() << *pair.first;
    }
    llvm::dbgs() << "LivenessUses:\n";
    for (auto pair : livenessUses) {
      llvm::dbgs() << *pair.first;
    }
    llvm::dbgs() << "Borrows:\n";
    for (auto pair : borrows) {
      llvm::dbgs() << *pair.first;
    }
    llvm::dbgs() << "Takes:\n";
    for (auto pair : takeInsts) {
      llvm::dbgs() << *pair.first;
    }
    llvm::dbgs() << "Copies:\n";
    for (auto pair : copyInsts) {
      llvm::dbgs() << *pair.first;
    }
    llvm::dbgs() << "Inits:\n";
    for (auto pair : initInsts) {
      llvm::dbgs() << *pair.first;
    }
    llvm::dbgs() << "Reinits:\n";
    for (auto pair : reinitInsts) {
      llvm::dbgs() << *pair.first;
    }
    llvm::dbgs() << "DropDeinits:\n";
    for (auto *inst : dropDeinitInsts) {
      llvm::dbgs() << *inst;
    }
    llvm::dbgs() << "Implicit End Of Lifetime Liveness Users:\n";
    for (auto *inst : implicitEndOfLifetimeLivenessUses) {
      llvm::dbgs() << *inst;
    }
    llvm::dbgs() << "Debug Value User:\n";
    if (debugValue) {
      llvm::dbgs() << *debugValue;
    }
  }

  void freezeMultiMaps() {
    initToValueMultiMap.setFrozen();
    reinitToValueMultiMap.setFrozen();
  }

  SmallBitVector &getOrCreateConsumingBlock(SILBasicBlock *block) {
    auto iter = consumingBlocks.find(block);
    if (iter == consumingBlocks.end()) {
      iter =
          consumingBlocks.insert({block, SmallBitVector(getNumSubelements())})
              .first;
    }
    return iter->second;
  }

  void recordConsumingBlock(SILBasicBlock *block, TypeTreeLeafTypeRange range) {
    auto &consumingBits = getOrCreateConsumingBlock(block);
    range.setBits(consumingBits);
  }

  void recordConsumingBlock(SILBasicBlock *block, SmallBitVector &bits) {
    auto &consumingBits = getOrCreateConsumingBlock(block);
    consumingBits |= bits;
  }

  void
  initializeLiveness(FieldSensitiveMultiDefPrunedLiveRange &prunedLiveness);

  void initializeImplicitEndOfLifetimeLivenessUses() {
    if (isInOutDefThatNeedsEndOfFunctionLiveness(address)) {
      SmallVector<SILBasicBlock *, 8> exitBlocks;
      address->getFunction()->findExitingBlocks(exitBlocks);
      for (auto *block : exitBlocks) {
        LLVM_DEBUG(llvm::dbgs() << "    Adding term as liveness user: "
                                << *block->getTerminator());
        implicitEndOfLifetimeLivenessUses.insert(block->getTerminator());
      }
      return;
    }

    if (address->getCheckKind() ==
        MarkMustCheckInst::CheckKind::AssignableButNotConsumable) {
      if (auto *bai = dyn_cast<BeginAccessInst>(address->getOperand())) {
        for (auto *eai : bai->getEndAccesses()) {
          LLVM_DEBUG(llvm::dbgs() << "    Adding end_access as implicit end of "
                                     "lifetime liveness user: "
                                  << *eai);
          implicitEndOfLifetimeLivenessUses.insert(eai);
        }
      }
    }
  }

  bool isConsume(SILInstruction *inst, TypeTreeLeafTypeRange span) const {
    {
      auto iter = takeInsts.find(inst);
      if (iter != takeInsts.end()) {
        if (span.setIntersection(iter->second))
          return true;
      }
    }
    {
      auto iter = copyInsts.find(inst);
      if (iter != copyInsts.end()) {
        if (span.setIntersection(iter->second))
          return true;
      }
    }
    return false;
  }

  bool isCopy(SILInstruction *inst, const SmallBitVector &bv) const {
    auto iter = copyInsts.find(inst);
    if (iter != copyInsts.end()) {
      for (unsigned index : iter->second.getRange()) {
        if (bv[index])
          return true;
      }
    }
    return false;
  }

  bool isLivenessUse(SILInstruction *inst, TypeTreeLeafTypeRange span) const {
    {
      auto iter = livenessUses.find(inst);
      if (iter != livenessUses.end()) {
        if (span.intersects(iter->second))
          return true;
      }
    }
    {
      auto iter = borrows.find(inst);
      if (iter != borrows.end()) {
        if (span.setIntersection(iter->second))
          return true;
      }
    }

    if (!isReinitToInitConvertibleInst(inst)) {
      auto iter = reinitInsts.find(inst);
      if (iter != reinitInsts.end()) {
        if (span.intersects(iter->second))
          return true;
      }
    }

    // An "inout terminator use" is an implicit liveness use of the entire
    // value. This is because we need to ensure that our inout value is
    // reinitialized along exit paths.
    if (implicitEndOfLifetimeLivenessUses.count(inst))
      return true;

    return false;
  }

  bool isInitUse(SILInstruction *inst, TypeTreeLeafTypeRange span) const {
    {
      auto iter = initInsts.find(inst);
      if (iter != initInsts.end()) {
        if (span.intersects(iter->second))
          return true;
      }
    }
    if (isReinitToInitConvertibleInst(inst)) {
      auto iter = reinitInsts.find(inst);
      if (iter != reinitInsts.end()) {
        if (span.intersects(iter->second))
          return true;
      }
    }
    return false;
  }

  bool isInitUse(SILInstruction *inst, const SmallBitVector &requiredBits,
                 SmallBitVector &foundInitBits) const {
    {
      auto iter = initInsts.find(inst);
      if (iter != initInsts.end()) {
        foundInitBits = iter->second & requiredBits;
        if (foundInitBits.any())
          return true;
      }
    }

    if (isReinitToInitConvertibleInst(inst)) {
      auto iter = reinitInsts.find(inst);
      if (iter != reinitInsts.end()) {
        foundInitBits = iter->second & requiredBits;
        if (foundInitBits.any())
          return true;
      }
    }
    return false;
  }
};

} // namespace

//===----------------------------------------------------------------------===//
//                       MARK: Partial Apply Utilities
//===----------------------------------------------------------------------===//

static bool findNonEscapingPartialApplyUses(PartialApplyInst *pai,
                                            TypeTreeLeafTypeRange leafRange,
                                            UseState &useState) {
  StackList<Operand *> worklist(pai->getFunction());
  for (auto *use : pai->getUses())
    worklist.push_back(use);

  LLVM_DEBUG(llvm::dbgs() << "Searching for partial apply uses!\n");
  while (!worklist.empty()) {
    auto *use = worklist.pop_back_val();

    if (use->isTypeDependent())
      continue;

    auto *user = use->getUser();

    // These instructions do not cause us to escape.
    if (isIncidentalUse(user) || isa<DestroyValueInst>(user))
      continue;

    // Look through these instructions.
    if (isa<BeginBorrowInst>(user) || isa<CopyValueInst>(user) ||
        isa<MoveValueInst>(user) ||
        // If we capture this partial_apply in another partial_apply, then we
        // know that said partial_apply must not have escaped the value since
        // otherwise we could not have an inout_aliasable argument or be
        // on_stack. Process it recursively so that we treat uses of that
        // partial_apply and applies of that partial_apply as uses of our
        // partial_apply.
        //
        // We have this separately from the other look through sections so that
        // we can make it clearer what we are doing here.
        isa<PartialApplyInst>(user) ||
        // Likewise with convert_function. Any valid function conversion that
        // doesn't prevent stack promotion of the closure must retain the
        // invariants on its transitive uses.
        isa<ConvertFunctionInst>(user)) {
      for (auto *use : cast<SingleValueInstruction>(user)->getUses())
        worklist.push_back(use);
      continue;
    }

    // If we have a mark_dependence and are the value, look through the
    // mark_dependence.
    if (auto *mdi = dyn_cast<MarkDependenceInst>(user)) {
      if (mdi->getValue() == use->get()) {
        for (auto *use : mdi->getUses())
          worklist.push_back(use);
        continue;
      }
    }

    if (auto apply = FullApplySite::isa(user)) {
      // If we apply the function or pass the function off to an apply, then we
      // need to treat the function application as a liveness use of the
      // variable since if the partial_apply is invoked within the function
      // application, we may access the captured variable.
      useState.recordLivenessUse(user, leafRange);
      if (apply.beginsCoroutineEvaluation()) {
        // If we have a coroutine, we need to treat the abort_apply and
        // end_apply as liveness uses since once we execute one of those
        // instructions, we have returned control to the coroutine which means
        // that we could then access the captured variable again.
        auto *bai = cast<BeginApplyInst>(user);
        SmallVector<EndApplyInst *, 4> endApplies;
        SmallVector<AbortApplyInst *, 4> abortApplies;
        bai->getCoroutineEndPoints(endApplies, abortApplies);
        for (auto *eai : endApplies)
          useState.recordLivenessUse(eai, leafRange);
        for (auto *aai : abortApplies)
          useState.recordLivenessUse(aai, leafRange);
      }
      continue;
    }

    LLVM_DEBUG(
        llvm::dbgs()
        << "Found instruction we did not understand... returning false!\n");
    LLVM_DEBUG(llvm::dbgs() << "Instruction: " << *user);
    return false;
  }

  return true;
}

void UseState::initializeLiveness(
    FieldSensitiveMultiDefPrunedLiveRange &liveness) {
  assert(liveness.getNumSubElements() == getNumSubelements());
  // We begin by initializing all of our init uses.
  for (auto initInstAndValue : initInsts) {
    LLVM_DEBUG(llvm::dbgs() << "Found def: " << *initInstAndValue.first);

    liveness.initializeDef(initInstAndValue.first, initInstAndValue.second);
  }

  // If we have a reinitInstAndValue that we are going to be able to convert
  // into a simple init, add it as an init. We are going to consider the rest of
  // our reinit uses to be liveness uses.
  for (auto reinitInstAndValue : reinitInsts) {
    if (isReinitToInitConvertibleInst(reinitInstAndValue.first)) {
      LLVM_DEBUG(llvm::dbgs() << "Found def: " << *reinitInstAndValue.first);
      liveness.initializeDef(reinitInstAndValue.first,
                             reinitInstAndValue.second);
    }
  }

  // Then check if our markedValue is from an argument that is in,
  // in_guaranteed, inout, or inout_aliasable, consider the marked address to be
  // the initialization point.
  {
    SILValue operand = address->getOperand();
    if (auto *c = dyn_cast<CopyableToMoveOnlyWrapperAddrInst>(operand))
      operand = c->getOperand();
    if (auto *fArg = dyn_cast<SILFunctionArgument>(operand)) {
      switch (fArg->getArgumentConvention()) {
      case swift::SILArgumentConvention::Indirect_In:
      case swift::SILArgumentConvention::Indirect_In_Guaranteed:
      case swift::SILArgumentConvention::Indirect_Inout:
      case swift::SILArgumentConvention::Indirect_InoutAliasable:
        // We need to add our address to the initInst array to make sure that
        // later invariants that we assert upon remain true.
        LLVM_DEBUG(
            llvm::dbgs()
            << "Found in/in_guaranteed/inout/inout_aliasable argument as "
               "an init... adding mark_must_check as init!\n");
        // We cheat here slightly and use our address's operand.
        recordInitUse(address, address, liveness.getTopLevelSpan());
        liveness.initializeDef(address, liveness.getTopLevelSpan());
        break;
      case swift::SILArgumentConvention::Indirect_Out:
        llvm_unreachable("Should never have out addresses here");
      case swift::SILArgumentConvention::Direct_Owned:
      case swift::SILArgumentConvention::Direct_Unowned:
      case swift::SILArgumentConvention::Direct_Guaranteed:
      case swift::SILArgumentConvention::Pack_Inout:
      case swift::SILArgumentConvention::Pack_Guaranteed:
      case swift::SILArgumentConvention::Pack_Owned:
      case swift::SILArgumentConvention::Pack_Out:
        llvm_unreachable("Working with addresses");
      }
    }
  }

  // See if our address is from a closure guaranteed box that we did not promote
  // to an address. In such a case, just treat our mark_must_check as the init
  // of our value.
  if (auto *projectBox = dyn_cast<ProjectBoxInst>(stripAccessMarkers(address->getOperand()))) {
    if (auto *fArg = dyn_cast<SILFunctionArgument>(projectBox->getOperand())) {
      if (fArg->isClosureCapture()) {
        assert(fArg->getArgumentConvention() ==
                   SILArgumentConvention::Direct_Guaranteed &&
               "Just a paranoid assert check to make sure this code is thought "
               "about if we change the convention in some way");
        // We need to add our address to the initInst array to make sure that
        // later invariants that we assert upon remain true.
        LLVM_DEBUG(llvm::dbgs() << "Found move only arg closure box use... "
                                   "adding mark_must_check as init!\n");
        recordInitUse(address, address, liveness.getTopLevelSpan());
        liveness.initializeDef(address, liveness.getTopLevelSpan());
      }
    } else if (auto *box = dyn_cast<AllocBoxInst>(
                   lookThroughOwnershipInsts(projectBox->getOperand()))) {
      LLVM_DEBUG(llvm::dbgs() << "Found move only var allocbox use... "
                 "adding mark_must_check as init!\n");
      recordInitUse(address, address, liveness.getTopLevelSpan());
      liveness.initializeDef(address, liveness.getTopLevelSpan());
    }
  }

  // Check if our address is from a ref_element_addr. In such a case, we treat
  // the mark_must_check as the initialization.
  if (auto *refEltAddr = dyn_cast<RefElementAddrInst>(
          stripAccessMarkers(address->getOperand()))) {
    LLVM_DEBUG(llvm::dbgs() << "Found ref_element_addr use... "
                               "adding mark_must_check as init!\n");
    recordInitUse(address, address, liveness.getTopLevelSpan());
    liveness.initializeDef(address, liveness.getTopLevelSpan());
  }

  // Check if our address is from a global_addr. In such a case, we treat the
  // mark_must_check as the initialization.
  if (auto *globalAddr =
          dyn_cast<GlobalAddrInst>(stripAccessMarkers(address->getOperand()))) {
    LLVM_DEBUG(llvm::dbgs() << "Found global_addr use... "
                               "adding mark_must_check as init!\n");
    recordInitUse(address, address, liveness.getTopLevelSpan());
    liveness.initializeDef(address, liveness.getTopLevelSpan());
  }

  if (auto *ptai = dyn_cast<PointerToAddressInst>(
          stripAccessMarkers(address->getOperand()))) {
    assert(ptai->isStrict());
    LLVM_DEBUG(llvm::dbgs() << "Found pointer to address use... "
                               "adding mark_must_check as init!\n");
    recordInitUse(address, address, liveness.getTopLevelSpan());
    liveness.initializeDef(address, liveness.getTopLevelSpan());
  }

  // Now that we have finished initialization of defs, change our multi-maps
  // from their array form to their map form.
  liveness.finishedInitializationOfDefs();

  LLVM_DEBUG(llvm::dbgs() << "Liveness with just inits:\n";
             liveness.print(llvm::dbgs()));

  for (auto initInstAndValue : initInsts) {
    // If our init inst is a store_borrow, treat the end_borrow as liveness
    // uses.
    //
    // NOTE: We do not need to check for access scopes here since store_borrow
    // can only apply to alloc_stack today.
    if (auto *sbi = dyn_cast<StoreBorrowInst>(initInstAndValue.first)) {
      // We can only store_borrow if our mark_must_check is a
      // no_consume_or_assign.
      assert(address->getCheckKind() ==
                 MarkMustCheckInst::CheckKind::NoConsumeOrAssign &&
             "store_borrow implies no_consume_or_assign since we cannot "
             "consume a borrowed inited value");
      for (auto *ebi : sbi->getEndBorrows()) {
        liveness.updateForUse(ebi, initInstAndValue.second,
                              false /*lifetime ending*/);
      }
    }
  }

  // Now at this point, we have defined all of our defs so we can start adding
  // uses to the liveness.
  for (auto reinitInstAndValue : reinitInsts) {
    recordConsumingBlock(reinitInstAndValue.first->getParent(),
                         reinitInstAndValue.second);
    if (!isReinitToInitConvertibleInst(reinitInstAndValue.first)) {
      liveness.updateForUse(reinitInstAndValue.first, reinitInstAndValue.second,
                            false /*lifetime ending*/);
      LLVM_DEBUG(llvm::dbgs() << "Added liveness for reinit: "
                              << *reinitInstAndValue.first;
                 liveness.print(llvm::dbgs()));
    }
  }

  // Then add all of the takes that we saw propagated up to the top of our
  // block. Since we have done this for all of our defs
  for (auto takeInstAndValue : takeInsts) {
    liveness.updateForUse(takeInstAndValue.first, takeInstAndValue.second,
                          true /*lifetime ending*/);
    recordConsumingBlock(takeInstAndValue.first->getParent(),
                         takeInstAndValue.second);
    LLVM_DEBUG(llvm::dbgs()
                   << "Added liveness for take: " << *takeInstAndValue.first;
               liveness.print(llvm::dbgs()));
  }
  for (auto copyInstAndValue : copyInsts) {
    liveness.updateForUse(copyInstAndValue.first, copyInstAndValue.second,
                          true /*lifetime ending*/);
    recordConsumingBlock(copyInstAndValue.first->getParent(),
                         copyInstAndValue.second);
    LLVM_DEBUG(llvm::dbgs()
                   << "Added liveness for copy: " << *copyInstAndValue.first;
               liveness.print(llvm::dbgs()));
  }

  for (auto destroyInstAndValue : destroys) {
    recordConsumingBlock(destroyInstAndValue.first->getParent(),
                         destroyInstAndValue.second);
  }

  // Do the same for our borrow and liveness insts.
  for (auto livenessInstAndValue : borrows) {
    liveness.updateForUse(livenessInstAndValue.first,
                          livenessInstAndValue.second,
                          false /*lifetime ending*/);
    auto *li = cast<LoadInst>(livenessInstAndValue.first);
    auto accessPathWithBase =
        AccessPathWithBase::computeInScope(li->getOperand());
    if (auto *beginAccess =
            dyn_cast<BeginAccessInst>(accessPathWithBase.base)) {
      for (auto *endAccess : beginAccess->getEndAccesses()) {
        liveness.updateForUse(endAccess, livenessInstAndValue.second,
                              false /*lifetime ending*/);
      }
    }
    // NOTE: We used to add the destroy_value of our loads here to liveness. We
    // instead add them to the livenessUses array so that we can successfully
    // find them later when performing a forward traversal to find them for
    // error purposes.
    LLVM_DEBUG(llvm::dbgs() << "Added liveness for borrow: "
                            << *livenessInstAndValue.first;
               liveness.print(llvm::dbgs()));
  }

  for (auto livenessInstAndValue : livenessUses) {
    if (auto *lbi = dyn_cast<LoadBorrowInst>(livenessInstAndValue.first)) {
      auto accessPathWithBase =
          AccessPathWithBase::computeInScope(lbi->getOperand());
      if (auto *beginAccess =
              dyn_cast<BeginAccessInst>(accessPathWithBase.base)) {
        for (auto *endAccess : beginAccess->getEndAccesses()) {
          liveness.updateForUse(endAccess, livenessInstAndValue.second,
                                false /*lifetime ending*/);
        }
      } else {
        for (auto *ebi : lbi->getEndBorrows()) {
          liveness.updateForUse(ebi, livenessInstAndValue.second,
                                false /*lifetime ending*/);
        }
      }
    } else {
      liveness.updateForUse(livenessInstAndValue.first,
                            livenessInstAndValue.second,
                            false /*lifetime ending*/);
    }
    LLVM_DEBUG(llvm::dbgs() << "Added liveness for livenessInst: "
                            << *livenessInstAndValue.first;
               liveness.print(llvm::dbgs()));
  }

  // Finally, if we have an inout argument or an access scope associated with a
  // ref_element_addr or global_addr, add a liveness use of the entire value on
  // the implicit end lifetime instruction. For inout this is terminators for
  // ref_element_addr, global_addr it is the end_access instruction.
  for (auto *inst : implicitEndOfLifetimeLivenessUses) {
    liveness.updateForUse(inst, TypeTreeLeafTypeRange(address),
                          false /*lifetime ending*/);
    LLVM_DEBUG(llvm::dbgs() << "Added liveness for inoutTermUser: " << *inst;
               liveness.print(llvm::dbgs()));
  }

  LLVM_DEBUG(llvm::dbgs() << "Final Liveness:\n"; liveness.print(llvm::dbgs()));
}

//===----------------------------------------------------------------------===//
//                          MARK: Global Block State
//===----------------------------------------------------------------------===//

namespace {

struct BlockState {
  using Map = llvm::DenseMap<SILBasicBlock *, BlockState>;

  /// This is either the liveness up or take up inst that projects
  /// up. We set this state according to the following rules:
  ///
  /// 1. If we are tracking a takeUp, we always take it even if we have a
  /// livenessUp.
  ///
  /// 2. If we have a livenessUp and do not have a take up, we track that
  /// instead.
  ///
  /// The reason why we do this is that we want to catch use after frees when
  /// non-consuming uses are later than a consuming use.
  SILInstruction *userUp;

  /// If we are init down, then we know that we can not transfer our take
  /// through this block and should stop traversing.
  bool isInitDown;

  BlockState() : userUp(nullptr) {}

  BlockState(SILInstruction *userUp, bool isInitDown)
      : userUp(userUp), isInitDown(isInitDown) {}
};

} // namespace

//===----------------------------------------------------------------------===//
//                 MARK: Forward Declaration of Main Checker
//===----------------------------------------------------------------------===//

namespace {

struct ConsumeInfo {
  /// Map blocks on the lifetime boundary to the last consuming instruction.
  llvm::MapVector<SILBasicBlock *,
                  SmallVector<std::pair<SILInstruction *, SmallBitVector>, 1>>
      finalBlockConsumes;

  bool isFrozen = false;

public:
  void print(llvm::raw_ostream &os) const {
    for (auto &blockInstRangePairVector : finalBlockConsumes) {
      os << "Dumping state for block bb"
         << blockInstRangePairVector.first->getDebugID() << '\n';
      for (auto &instRangePairVector : blockInstRangePairVector.second) {
        auto *inst = instRangePairVector.first;
        if (!inst)
          continue;
        os << "Inst: " << *inst;
        os << "Range: " << instRangePairVector.second;
        os << '\n';
      }
    }
  }

  void clear() {
    finalBlockConsumes.clear();
    isFrozen = false;
  }

  /// This is expensive! Only use it in debug mode!
  bool hasUnclaimedConsumes() const {
    assert(isFrozen);
    bool foundAny = false;
    for (auto range : finalBlockConsumes) {
      for (auto elt : range.second) {
        foundAny |= bool(elt.first);
      }
    }
    return foundAny;
  }

  void recordFinalConsume(SILInstruction *inst, SmallBitVector const &bits) {
    assert(!isFrozen);
    auto *block = inst->getParent();
    auto iter = finalBlockConsumes.find(block);
    if (iter == finalBlockConsumes.end()) {
      iter = finalBlockConsumes.insert({block, {}}).first;
    }
    LLVM_DEBUG(llvm::dbgs() << "Recorded Final Consume: " << *inst);
    iter->second.emplace_back(inst, bits);
  }

  void finishRecordingFinalConsumes() {
    assert(!isFrozen);
    for (auto &pair : finalBlockConsumes) {
      llvm::stable_sort(
          pair.second,
          [](const std::pair<SILInstruction *, SmallBitVector> &lhs,
             const std::pair<SILInstruction *, SmallBitVector> &rhs) {
            return lhs.first < rhs.first;
          });
    }
    isFrozen = true;

    LLVM_DEBUG(llvm::dbgs() << "Final recorded consumes!\n";
               print(llvm::dbgs()));
  }

  // Return true if this instruction is marked as a final consume point of the
  // current def's live range. A consuming instruction can only be claimed once
  // because instructions like `tuple` can consume the same value via multiple
  // operands.
  //
  // Can only be used once frozen.
  bool claimConsume(SILInstruction *inst, SmallBitVector const &bits) {
    assert(isFrozen);

    bool claimedConsume = false;

    auto &iter = finalBlockConsumes[inst->getParent()];
    for (unsigned i : indices(iter)) {
      auto &instRangePair = iter[i];
      if (instRangePair.first == inst && instRangePair.second == bits) {
        instRangePair.first = nullptr;
        claimedConsume = true;
        LLVM_DEBUG(llvm::dbgs() << "Claimed consume: " << *inst);
      }
    }

    return claimedConsume;
  }

  ConsumeInfo() {}
  ConsumeInfo(CanonicalOSSAConsumeInfo const &) = delete;
  ConsumeInfo &operator=(ConsumeInfo const &) = delete;
};

struct MoveOnlyAddressCheckerPImpl {
  bool changed = false;

  SILFunction *fn;

  DominanceInfo *domTree;

  /// A set of mark_must_check that we are actually going to process.
  SmallSetVector<MarkMustCheckInst *, 32> moveIntroducersToProcess;

  /// The instruction deleter used by \p canonicalizer.
  InstructionDeleter deleter;

  /// State to run CanonicalizeOSSALifetime.
  OSSACanonicalizer canonicalizer;

  /// Per mark must check address use state.
  UseState addressUseState;

  /// Diagnostic emission routines wrapped around a consuming use cache. This
  /// ensures that we only emit a single error per use per marked value.
  DiagnosticEmitter &diagnosticEmitter;

  /// Information about destroys that we use when inserting destroys.
  ConsumeInfo consumes;

  /// PostOrderAnalysis used by the BorrowToDestructureTransform.
  PostOrderAnalysis *poa;

  /// Allocator used by the BorrowToDestructureTransform.
  borrowtodestructure::IntervalMapAllocator &allocator;

  MoveOnlyAddressCheckerPImpl(
      SILFunction *fn, DiagnosticEmitter &diagnosticEmitter,
      DominanceInfo *domTree, PostOrderAnalysis *poa,
      borrowtodestructure::IntervalMapAllocator &allocator)
      : fn(fn), domTree(domTree), deleter(),
        canonicalizer(fn, domTree, deleter),
        diagnosticEmitter(diagnosticEmitter), poa(poa), allocator(allocator) {
    deleter.setCallbacks(std::move(
        InstModCallbacks().onDelete([&](SILInstruction *instToDelete) {
          if (auto *mvi = dyn_cast<MarkMustCheckInst>(instToDelete))
            moveIntroducersToProcess.remove(mvi);
          instToDelete->eraseFromParent();
        })));
    diagnosticEmitter.initCanonicalizer(&canonicalizer);
  }

  /// Search through the current function for candidate mark_must_check
  /// [noimplicitcopy]. If we find one that does not fit a pattern that we
  /// understand, emit an error diagnostic telling the programmer that the move
  /// checker did not know how to recognize this code pattern.
  ///
  /// Returns true if we emitted a diagnostic. Returns false otherwise.
  bool searchForCandidateMarkMustChecks();

  /// Emits an error diagnostic for \p markedValue.
  void performObjectCheck(MarkMustCheckInst *markedValue);

  bool performSingleCheck(MarkMustCheckInst *markedValue);

  void insertDestroysOnBoundary(MarkMustCheckInst *markedValue,
                                FieldSensitiveMultiDefPrunedLiveRange &liveness,
                                FieldSensitivePrunedLivenessBoundary &boundary);

  void rewriteUses(MarkMustCheckInst *markedValue,
                   FieldSensitiveMultiDefPrunedLiveRange &liveness,
                   const FieldSensitivePrunedLivenessBoundary &boundary);

  /// Identifies and diagnoses reinitializations that are reachable from a
  /// discard statement.
  void checkForReinitAfterDiscard();

  void handleSingleBlockDestroy(SILInstruction *destroy, bool isReinit);
};

class ExtendUnconsumedLiveness {
  UseState addressUseState;
  FieldSensitiveMultiDefPrunedLiveRange &liveness;
  FieldSensitivePrunedLivenessBoundary &boundary;

  enum class DestroyKind {
    Destroy,
    Take,
    Reinit,
  };
  using DestroysCollection =
      llvm::SmallMapVector<SILInstruction *, DestroyKind, 8>;
  using ConsumingBlocksCollection = SmallPtrSetVector<SILBasicBlock *, 8>;

public:
  ExtendUnconsumedLiveness(UseState addressUseState,
                           FieldSensitiveMultiDefPrunedLiveRange &liveness,
                           FieldSensitivePrunedLivenessBoundary &boundary)
      : addressUseState(addressUseState), liveness(liveness),
        boundary(boundary) {}

  void run();

  void runOnField(unsigned element, DestroysCollection &destroys,
                  ConsumingBlocksCollection &consumingBlocks);

private:
  bool hasDefAfter(SILInstruction *inst, unsigned element);
  bool isLiveAtBegin(SILBasicBlock *block, unsigned element, bool isLiveAtEnd,
                     DestroysCollection const &destroys);

  bool
  shouldAddDestroyToLiveness(SILInstruction *destroy, unsigned element,
                             BasicBlockSet const &consumedAtExitBlocks,
                             BasicBlockSetVector const &consumedAtEntryBlocks);

  void addPreviousInstructionToLiveness(SILInstruction *inst, unsigned element,
                                        bool lifetimeEnding);
};

} // namespace

//===----------------------------------------------------------------------===//
//                     MARK: CopiedLoadBorrowElimination
//===----------------------------------------------------------------------===//

namespace {

struct CopiedLoadBorrowEliminationState {
  SILFunction *fn;
  StackList<LoadBorrowInst *> targets;

  CopiedLoadBorrowEliminationState(SILFunction *fn) : fn(fn), targets(fn) {}

  void process() {
    if (targets.empty())
      return;

    while (!targets.empty()) {
      auto *lbi = targets.pop_back_val();
      SILBuilderWithScope builder(lbi);
      SILValue li = builder.emitLoadValueOperation(
          lbi->getLoc(), lbi->getOperand(), LoadOwnershipQualifier::Copy);
      SILValue borrow = builder.createBeginBorrow(lbi->getLoc(), li);

      for (auto *ebi : lbi->getEndBorrows()) {
        auto *next = ebi->getNextInstruction();
        SILBuilderWithScope builder(next);
        auto loc = RegularLocation::getAutoGeneratedLocation();
        builder.emitDestroyValueOperation(loc, li);
      }

      lbi->replaceAllUsesWith(borrow);
      lbi->eraseFromParent();
    }

    LLVM_DEBUG(llvm::dbgs() << "After Load Borrow Elim. Func Dump Start! ";
               fn->print(llvm::dbgs()));
    LLVM_DEBUG(llvm::dbgs() << "After Load Borrow Elim. Func Dump End!\n");
  }
};

/// An early transform that we run to convert any load_borrow that are copied
/// directly or that have any subelement that is copied to a load [copy]. This
/// lets the rest of the optimization handle these as appropriate.
struct CopiedLoadBorrowEliminationVisitor
    : public TransitiveAddressWalker<CopiedLoadBorrowEliminationVisitor> {
  CopiedLoadBorrowEliminationState &state;

  CopiedLoadBorrowEliminationVisitor(CopiedLoadBorrowEliminationState &state)
      : state(state) {}

  bool visitUse(Operand *op) {
    LLVM_DEBUG(llvm::dbgs() << "CopiedLBElim visiting ";
               llvm::dbgs() << " User: " << *op->getUser());
    auto *lbi = dyn_cast<LoadBorrowInst>(op->getUser());
    if (!lbi)
      return true;

    LLVM_DEBUG(llvm::dbgs() << "Found load_borrow: " << *lbi);

    StackList<Operand *> useWorklist(lbi->getFunction());
    for (auto *use : lbi->getUses())
      useWorklist.push_back(use);

    bool shouldConvertToLoadCopy = false;
    while (!useWorklist.empty()) {
      auto *nextUse = useWorklist.pop_back_val();
      switch (nextUse->getOperandOwnership()) {
      case OperandOwnership::NonUse:
      case OperandOwnership::ForwardingUnowned:
      case OperandOwnership::PointerEscape:
        continue;

        // These might be uses that we need to perform a destructure or insert
        // struct_extracts for.
      case OperandOwnership::TrivialUse:
      case OperandOwnership::InstantaneousUse:
      case OperandOwnership::UnownedInstantaneousUse:
      case OperandOwnership::InteriorPointer:
      case OperandOwnership::BitwiseEscape: {
        // Look through copy_value of a move only value. We treat copy_value of
        // copyable values as normal uses.
        if (auto *cvi = dyn_cast<CopyValueInst>(nextUse->getUser())) {
          if (!isCopyableValue(cvi->getOperand())) {
            shouldConvertToLoadCopy = true;
            break;
          }
        }
        continue;
      }

      case OperandOwnership::ForwardingConsume:
      case OperandOwnership::DestroyingConsume:
        // We can only hit this if our load_borrow was copied.
        llvm_unreachable("We should never hit this");

      case OperandOwnership::GuaranteedForwarding: {
        SmallVector<SILValue, 8> forwardedValues;
        auto *fn = nextUse->getUser()->getFunction();
        ForwardingOperand(nextUse).visitForwardedValues([&](SILValue value) {
          if (value->getType().isTrivial(fn))
            return true;
          forwardedValues.push_back(value);
          return true;
        });

        // If we do not have any forwarded values, just continue.
        if (forwardedValues.empty())
          continue;

        while (!forwardedValues.empty()) {
          for (auto *use : forwardedValues.pop_back_val()->getUses())
            useWorklist.push_back(use);
        }

        // If we have a switch_enum, we always need to convert it to a load
        // [copy] since we need to destructure through it.
        shouldConvertToLoadCopy |= isa<SwitchEnumInst>(nextUse->getUser());
        continue;
      }
      case OperandOwnership::Borrow:
        LLVM_DEBUG(llvm::dbgs() << "        Found recursive borrow!\n");
        // Look through borrows.
        for (auto value : nextUse->getUser()->getResults()) {
          for (auto *use : value->getUses()) {
            useWorklist.push_back(use);
          }
        }
        continue;
      case OperandOwnership::EndBorrow:
        LLVM_DEBUG(llvm::dbgs() << "        Found end borrow!\n");
        continue;
      case OperandOwnership::Reborrow:
        llvm_unreachable("Unsupported for now?!");
      }

      if (shouldConvertToLoadCopy)
        break;
    }

    LLVM_DEBUG(llvm::dbgs()
               << "Load Borrow was copied: "
               << (shouldConvertToLoadCopy ? "true" : "false") << '\n');
    if (!shouldConvertToLoadCopy)
      return true;

    state.targets.push_back(lbi);
    return true;
  }
};

} // namespace

//===----------------------------------------------------------------------===//
//                   MARK: Partial Consume/Reinit Checking
//===----------------------------------------------------------------------===//

namespace {

/// When partial consumption is enabled, we only allow for destructure through
/// deinits. When partial consumption is disabled, we error on /all/ partial
/// consumption.
enum class IsPartialConsumeOrReinit_t {
  IsPartialConsume,
  IsPartialReinit,
};

} // namespace

static std::pair<SILType, NominalTypeDecl *>
shouldEmitPartialError(UseState &useState, SILInstruction *user,
                       SILType useType, TypeTreeLeafTypeRange usedBits) {
  SILFunction *fn = useState.getFunction();

  // We walk down from our ancestor to our projection, emitting an error if
  // any of our types have a deinit.
  auto iterType = useState.address->getType();
  if (iterType.isMoveOnlyWrapped())
    return {SILType(), nullptr};

  TypeOffsetSizePair pair(usedBits);
  auto targetType = useType;
  TypeOffsetSizePair iterPair(iterType, fn);

  LLVM_DEBUG(llvm::dbgs() << "    Iter Type: " << iterType << '\n'
                          << "    Target Type: " << targetType << '\n');

  if (!fn->getModule().getASTContext().LangOpts.hasFeature(
          Feature::MoveOnlyPartialConsumption)) {
    LLVM_DEBUG(llvm::dbgs() << "    MoveOnlyPartialConsumption disabled!\n");
    // If the types equal, just bail early.
    if (iterType == targetType) {
      LLVM_DEBUG(llvm::dbgs() << "    IterType is TargetType! Exiting early "
                                 "without emitting error!\n");
      return {SILType(), nullptr};
    }

    // Emit the error.
    return {iterType, nullptr};
  }

  // Otherwise, walk the type looking for the deinit.
  while (iterType != targetType) {
    // If we have a nominal type as our parent type, see if it has a
    // deinit. We know that it must be non-copyable since copyable types
    // cannot contain non-copyable types and that our parent root type must be
    // an enum, tuple, or struct.
    if (auto *nom = iterType.getNominalOrBoundGenericNominal()) {
      if (nom->getValueTypeDestructor()) {
        // If we find one, emit an error since we are going to have to extract
        // through the deinit. Emit a nice error saying what it is. Since we
        // are emitting an error, we do a bit more work and construct the
        // actual projection string.
        return {iterType, nom};
      }
    }

    // Otherwise, walk one level towards our child type. We unconditionally
    // unwrap since we should never fail here due to earlier checking.
    std::tie(iterPair, iterType) =
        *pair.walkOneLevelTowardsChild(iterPair, iterType, fn);
  }

  return {SILType(), nullptr};
}

static void
checkForPartialConsume(UseState &useState, DiagnosticEmitter &diagnosticEmitter,
                       SILInstruction *user, SILType useType,
                       TypeTreeLeafTypeRange usedBits,
                       IsPartialConsumeOrReinit_t isPartialConsumeOrReinit) {
  SILFunction *fn = useState.getFunction();

  // We walk down from our ancestor to our projection, emitting an error if
  // any of our types have a deinit.
  TypeOffsetSizePair pair(usedBits);
  SILType errorIterType;
  NominalTypeDecl *nom;
  std::tie(errorIterType, nom) =
      shouldEmitPartialError(useState, user, useType, usedBits);
  if (!errorIterType)
    return;

  if (!fn->getModule().getASTContext().LangOpts.hasFeature(
          Feature::MoveOnlyPartialConsumption)) {
    // Otherwise, build up the path string and emit the error.
    SmallString<128> pathString;
    auto rootType = useState.address->getType();
    if (errorIterType != rootType) {
      llvm::raw_svector_ostream os(pathString);
      pair.constructPathString(errorIterType, {rootType, fn}, rootType, fn, os);
    }

    diagnosticEmitter.emitCannotPartiallyConsumeError(
        useState.address, pathString, nullptr /*nominal*/, user,
        false /*deinit only*/);
    return;
  }

  LLVM_DEBUG(llvm::dbgs() << "    MoveOnlyPartialConsumption enabled!\n");

  SmallString<128> pathString;
  auto rootType = useState.address->getType();
  if (errorIterType != rootType) {
    llvm::raw_svector_ostream os(pathString);
    pair.constructPathString(errorIterType, {rootType, fn}, rootType, fn, os);
  }

  diagnosticEmitter.emitCannotPartiallyConsumeError(
      useState.address, pathString, nom, user, true /*deinit only*/);
}

static void
checkForPartialConsume(UseState &useState, DiagnosticEmitter &diagnosticEmitter,
                       Operand *op, TypeTreeLeafTypeRange usedBits,
                       IsPartialConsumeOrReinit_t isPartialConsumeOrReinit) {
  return checkForPartialConsume(useState, diagnosticEmitter, op->getUser(),
                                op->get()->getType(), usedBits,
                                isPartialConsumeOrReinit);
}

static void diagnosePartialReinitError(UseState &useState,
                                       DiagnosticEmitter &diagnosticEmitter,
                                       SILInstruction *user, SILType errorType,
                                       NominalTypeDecl *nom,
                                       SILInstruction *earlierConsumingUse,
                                       TypeTreeLeafTypeRange usedBits) {
  SILFunction *fn = useState.getFunction();

  // We walk down from our ancestor to our projection, emitting an error if
  // any of our types have a deinit.
  TypeOffsetSizePair pair(usedBits);
  if (!fn->getModule().getASTContext().LangOpts.hasFeature(
          Feature::MoveOnlyPartialConsumption)) {
    // Otherwise, build up the path string and emit the error.
    SmallString<128> pathString;
    auto rootType = useState.address->getType();
    if (errorType != rootType) {
      llvm::raw_svector_ostream os(pathString);
      pair.constructPathString(errorType, {rootType, fn}, rootType, fn, os);
    }

    diagnosticEmitter.emitCannotPartiallyReinitError(
        useState.address, pathString, nullptr /*nominal*/, user,
        earlierConsumingUse, false /*deinit only*/);
    return;
  }

  LLVM_DEBUG(llvm::dbgs() << "    MoveOnlyPartialConsumption enabled!\n");

  SmallString<128> pathString;
  auto rootType = useState.address->getType();
  if (errorType != rootType) {
    llvm::raw_svector_ostream os(pathString);
    pair.constructPathString(errorType, {rootType, fn}, rootType, fn, os);
  }

  diagnosticEmitter.emitCannotPartiallyReinitError(
      useState.address, pathString, nom, user, earlierConsumingUse,
      true /*deinit only*/);
}

namespace {

struct PartialReinitChecker {
  UseState &useState;
  DiagnosticEmitter &diagnosticEmitter;

  PartialReinitChecker(UseState &useState, DiagnosticEmitter &diagnosticEmitter)
      : useState(useState), diagnosticEmitter(diagnosticEmitter) {}

  void
  performPartialReinitChecking(FieldSensitiveMultiDefPrunedLiveRange &liveness);

private:
  void checkForPartialConsumeOrInitError(
      SILInstruction *user, SILType useType, TypeTreeLeafTypeRange usedBits,
      IsPartialConsumeOrReinit_t isPartialConsumeOrReinit) {
    ::checkForPartialConsume(useState, diagnosticEmitter, user, useType,
                             usedBits, isPartialConsumeOrReinit);
  }
};

} // namespace

void PartialReinitChecker::performPartialReinitChecking(
    FieldSensitiveMultiDefPrunedLiveRange &liveness) {
  // Perform checks that rely on liveness information.
  for (auto initToValues : useState.initToValueMultiMap.getRange()) {
    LLVM_DEBUG(llvm::dbgs() << "Checking init: " << *initToValues.first);
    bool emittedError = false;
    for (SILValue value : initToValues.second) {
      LLVM_DEBUG(llvm::dbgs() << "    Checking operand value: " << value);
      // By computing the bits here directly, we do not need to worry about
      // having to split contiguous ranges into separate representable SILTypes.
      SmallBitVector neededElements(useState.getNumSubelements());
      auto range = *TypeTreeLeafTypeRange::get(value, useState.address);
      for (unsigned index : range.getRange()) {
        emittedError = !liveness.findEarlierConsumingUse(
            initToValues.first, index,
            [&](SILInstruction *consumingInst) -> bool {
              SILType errorType;
              NominalTypeDecl *nom;
              std::tie(errorType, nom) = shouldEmitPartialError(
                  useState, initToValues.first, value->getType(),
                  TypeTreeLeafTypeRange(index, index + 1));
              if (!errorType)
                return true;

              diagnosePartialReinitError(
                  useState, diagnosticEmitter, initToValues.first, errorType,
                  nom, consumingInst, TypeTreeLeafTypeRange(index, index + 1));
              return false;
            });

        // If we emitted an error for this index break. We only want to emit one
        // error per value.
        if (emittedError)
          break;
      }

      // If we emitted an error for this value break. We only want to emit one
      // error per instruction.
      if (emittedError)
        break;
    }
  }

  for (auto reinitToValues : useState.reinitToValueMultiMap.getRange()) {
    if (!isReinitToInitConvertibleInst(reinitToValues.first))
      continue;

    LLVM_DEBUG(llvm::dbgs() << "Checking reinit: " << *reinitToValues.first);
    bool emittedError = false;
    for (SILValue value : reinitToValues.second) {
      LLVM_DEBUG(llvm::dbgs() << "    Checking operand value: " << value);
      // By computing the bits here directly, we do not need to worry about
      // having to split contiguous ranges into separate representable SILTypes.
      SmallBitVector neededElements(useState.getNumSubelements());
      auto range = *TypeTreeLeafTypeRange::get(value, useState.address);
      for (unsigned index : range.getRange()) {
        emittedError = !liveness.findEarlierConsumingUse(
            reinitToValues.first, index,
            [&](SILInstruction *consumingInst) -> bool {
              SILType errorType;
              NominalTypeDecl *nom;
              std::tie(errorType, nom) = shouldEmitPartialError(
                  useState, reinitToValues.first, value->getType(),
                  TypeTreeLeafTypeRange(index, index + 1));
              if (!errorType)
                return true;

              diagnosePartialReinitError(
                  useState, diagnosticEmitter, reinitToValues.first, errorType,
                  nom, consumingInst, TypeTreeLeafTypeRange(index, index + 1));
              return false;
            });
        if (emittedError)
          break;
      }
      if (emittedError)
        break;
    }
  }
}
//===----------------------------------------------------------------------===//
//                   MARK: GatherLexicalLifetimeUseVisitor
//===----------------------------------------------------------------------===//

namespace {

/// Visit all of the uses of value in preparation for running our algorithm.
struct GatherUsesVisitor : public TransitiveAddressWalker<GatherUsesVisitor> {
  MoveOnlyAddressCheckerPImpl &moveChecker;
  UseState &useState;
  MarkMustCheckInst *markedValue;
  DiagnosticEmitter &diagnosticEmitter;

  // Pruned liveness used to validate that load [take]/load [copy] can be
  // converted to load_borrow without violating exclusivity.
  BitfieldRef<SSAPrunedLiveness> liveness;

  GatherUsesVisitor(MoveOnlyAddressCheckerPImpl &moveChecker,
                    UseState &useState, MarkMustCheckInst *markedValue,
                    DiagnosticEmitter &diagnosticEmitter)
      : moveChecker(moveChecker), useState(useState), markedValue(markedValue),
        diagnosticEmitter(diagnosticEmitter) {}

  bool visitUse(Operand *op);
  void reset(MarkMustCheckInst *address) { useState.address = address; }
  void clear() { useState.clear(); }

  /// For now always markedValue. If we start using this for move address
  /// checking, we need to check against the operand of the markedValue. This is
  /// because for move checking, our marker is placed along the variables
  /// initialization so we are always going to have all later uses from the
  /// marked value. For the move operator though we will want this to be the
  /// base address that we are checking which should be the operand of the mark
  /// must check value.
  SILValue getRootAddress() const { return markedValue; }

  /// Returns true if we emitted an error.
  bool checkForExclusivityHazards(LoadInst *li) {
    BitfieldRef<SSAPrunedLiveness>::StackState state(liveness,
                                                     li->getFunction());

    LLVM_DEBUG(llvm::dbgs() << "Checking for exclusivity hazards for: " << *li);

    // Grab our access path with in scope. We want to find the inner most access
    // scope.
    auto accessPathWithBase =
        AccessPathWithBase::computeInScope(li->getOperand());
    auto accessPath = accessPathWithBase.accessPath;
    // TODO: Make this a we don't understand error.
    assert(accessPath.isValid() && "Invalid access path?!");

    auto *bai = dyn_cast<BeginAccessInst>(accessPathWithBase.base);

    if (!bai) {
      LLVM_DEBUG(llvm::dbgs()
                 << "    No begin access... so no exclusivity violation!\n");
      return false;
    }

    bool emittedError = false;
    liveness->initializeDef(bai);
    liveness->computeSimple();
    for (auto *consumingUse : li->getConsumingUses()) {
      if (!liveness->isWithinBoundary(consumingUse->getUser())) {
        diagnosticEmitter.emitAddressExclusivityHazardDiagnostic(
            markedValue, consumingUse->getUser());
        emittedError = true;
      }
    }
    return emittedError;
  }
};

} // end anonymous namespace

// Filter out recognized uses that do not write to memory.
//
// TODO: Ensure that all of the conditional-write logic below is encapsulated in
// mayWriteToMemory and just call that instead. Possibly add additional
// verification that visitAccessPathUses recognizes all instructions that may
// propagate pointers (even though they don't write).
bool GatherUsesVisitor::visitUse(Operand *op) {
  // If this operand is for a dependent type, then it does not actually access
  // the operand's address value. It only uses the metatype defined by the
  // operation (e.g. open_existential).
  if (op->isTypeDependent()) {
    return true;
  }

  // For convenience, grab the user of op.
  auto *user = op->getUser();

  LLVM_DEBUG(llvm::dbgs() << "Visiting user: " << *user;);

  // First check if we have init/reinit. These are quick/simple.
  if (noncopyable::memInstMustInitialize(op)) {
    LLVM_DEBUG(llvm::dbgs() << "Found init: " << *user);

    // TODO: What about copy_addr of itself. We really should just pre-process
    // those maybe.
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
    if (!leafRange)
      return false;

    useState.recordInitUse(user, op->get(), *leafRange);
    return true;
  }

  if (noncopyable::memInstMustReinitialize(op)) {
    LLVM_DEBUG(llvm::dbgs() << "Found reinit: " << *user);
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
    if (!leafRange)
      return false;
    useState.recordReinitUse(user, op->get(), *leafRange);
    return true;
  }

  // Then handle destroy_addr specially. We want to as part of our dataflow to
  // ignore destroy_addr, so we need to track it separately from other uses.
  if (auto *dvi = dyn_cast<DestroyAddrInst>(user)) {
    // If we see a destroy_addr not on our base address, bail! Just error and
    // say that we do not understand the code.
    if (dvi->getOperand() != useState.address) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!!! Error! Found destroy_addr no on base address: "
                 << *useState.address << "destroy: " << *dvi);
      return false;
    }

    LLVM_DEBUG(llvm::dbgs() << "Found destroy_addr: " << *dvi);
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
    if (!leafRange)
      return false;

    useState.destroys.insert({dvi, *leafRange});
    return true;
  }

  // Ignore dealloc_stack.
  if (isa<DeallocStackInst>(user))
    return true;

  // Ignore end_access.
  if (isa<EndAccessInst>(user))
    return true;

  // This visitor looks through store_borrow instructions but does visit the
  // end_borrow of the store_borrow. If we see such an end_borrow, register the
  // store_borrow instead. Since we use sets, if we visit multiple end_borrows,
  // we will only record the store_borrow once.
  if (auto *ebi = dyn_cast<EndBorrowInst>(user)) {
    if (auto *sbi = dyn_cast<StoreBorrowInst>(ebi->getOperand())) {
      LLVM_DEBUG(llvm::dbgs() << "Found store_borrow: " << *sbi);
      auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
      if (!leafRange)
        return false;

      useState.recordInitUse(user, op->get(), *leafRange);
      return true;
    }
  }

  if (auto *di = dyn_cast<DebugValueInst>(user)) {
    // Save the debug_value if it is attached directly to this mark_must_check.
    // If the underlying storage we're checking is immutable, then the access
    // being checked is not confined to an explicit access, but every other
    // use of the storage must also be immutable, so it is fine if we see
    // debug_values or other uses that aren't directly related to the current
    // marked use; they will have to behave compatibly anyway.
    if (di->getOperand() == getRootAddress()) {
      useState.debugValue = di;
    }
    return true;
  }

  // At this point, we have handled all of the non-loadTakeOrCopy/consuming
  // uses.
  if (auto *copyAddr = dyn_cast<CopyAddrInst>(user)) {
    assert(op->getOperandNumber() == CopyAddrInst::Src &&
           "Should have dest above in memInstMust{Rei,I}nitialize");

    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
    if (!leafRange)
      return false;

    // If we have a non-move only type, just treat this as a liveness use.
    if (isCopyableValue(copyAddr->getSrc())) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Found copy of copyable type. Treating as liveness use! "
                 << *user);
      useState.recordLivenessUse(user, *leafRange);
      return true;
    }

    if (markedValue->getCheckKind() ==
        MarkMustCheckInst::CheckKind::NoConsumeOrAssign) {
      if (isa<ProjectBoxInst>(stripAccessMarkers(markedValue->getOperand()))) {
        LLVM_DEBUG(llvm::dbgs()
                   << "Found mark must check [nocopy] use of escaping box: " << *user);
        diagnosticEmitter.emitAddressEscapingClosureCaptureLoadedAndConsumed(
            markedValue);
        return true;
      }

      LLVM_DEBUG(llvm::dbgs()
                 << "Found mark must check [nocopy] error: " << *user);
      diagnosticEmitter.emitAddressDiagnosticNoCopy(markedValue, copyAddr);
      return true;
    }

    // TODO: Add borrow checking here like below.

    // If we have a copy_addr, we are either going to have a take or a
    // copy... in either case, this copy_addr /is/ going to be a consuming
    // operation. Make sure to check if we semantically destructure.
    checkForPartialConsume(useState, diagnosticEmitter, op, *leafRange,
                           IsPartialConsumeOrReinit_t::IsPartialConsume);

    if (copyAddr->isTakeOfSrc()) {
      LLVM_DEBUG(llvm::dbgs() << "Found take: " << *user);
      useState.takeInsts.insert({user, *leafRange});
    } else {
      LLVM_DEBUG(llvm::dbgs() << "Found copy: " << *user);
      useState.copyInsts.insert({user, *leafRange});
    }
    return true;
  }

  // Then find load [copy], load [take] that are really takes since we need
  // copies for the loaded value. If we find that we need copies at that level
  // (due to e.x.: multiple consuming uses), we emit an error and bail. This
  // ensures that later on, we can assume that all of our load [take], load
  // [copy] actually follow move semantics at the object level and thus are
  // viewed as a consume requiring a copy. This is important since SILGen often
  // emits code of this form and we need to recognize it as a copy of the
  // underlying var.
  if (auto *li = dyn_cast<LoadInst>(user)) {
    // Before we do anything, see if this load is of a copyable field or is a
    // trivial load. If it is, then we just treat this as a liveness requiring
    // use.
    if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Trivial ||
        isCopyableValue(li)) {
      auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
      if (!leafRange)
        return false;
      useState.recordLivenessUse(user, *leafRange);
      return true;
    }

    // We must have a load [take] or load [copy] here since we are in OSSA.
    OSSACanonicalizer::LivenessState livenessState(moveChecker.canonicalizer,
                                                   li);

    // Before we do anything, run the borrow to destructure transform in case
    // we have a switch_enum user.
    unsigned numDiagnostics =
        moveChecker.diagnosticEmitter.getDiagnosticCount();
    BorrowToDestructureTransform borrowToDestructure(
        moveChecker.allocator, markedValue, li, moveChecker.diagnosticEmitter,
        moveChecker.poa);
    if (!borrowToDestructure.transform()) {
      assert(moveChecker.diagnosticEmitter
                 .didEmitCheckerDoesntUnderstandDiagnostic());
      LLVM_DEBUG(llvm::dbgs()
                 << "Failed to perform borrow to destructure transform!\n");
      return false;
    }

    // If we emitted an error diagnostic, do not transform further and instead
    // mark that we emitted an early diagnostic and return true.
    if (numDiagnostics != moveChecker.diagnosticEmitter.getDiagnosticCount()) {
      LLVM_DEBUG(llvm::dbgs() << "Emitting borrow to destructure error!\n");
      return true;
    }

    // Now, validate that what we will transform into a take isn't a take that
    // would invalidate a field that has a deinit.
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
    if (!leafRange) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Failed to compute leaf range for: " << *op->get());
      return false;
    }

    // Canonicalize the lifetime of the load [take], load [copy].
    LLVM_DEBUG(llvm::dbgs() << "Running copy propagation!\n");
    moveChecker.changed |= moveChecker.canonicalizer.canonicalize();

    // Export the drop_deinit's discovered by the ObjectChecker into the
    // AddressChecker to preserve it for later use. We need to do this since
    // the ObjectChecker's state gets cleared after running on this LoadInst.
    for (auto *dropDeinit : moveChecker.canonicalizer.getDropDeinitUses())
      moveChecker.addressUseState.dropDeinitInsts.insert(dropDeinit);

    // If we are asked to perform no_consume_or_assign checking or
    // assignable_but_not_consumable checking, if we found any consumes of our
    // load, then we need to emit an error.
    auto checkKind = markedValue->getCheckKind();
    if (checkKind != MarkMustCheckInst::CheckKind::ConsumableAndAssignable) {
      if (moveChecker.canonicalizer.foundAnyConsumingUses()) {
        LLVM_DEBUG(llvm::dbgs()
                   << "Found mark must check [nocopy] error: " << *user);
        auto *fArg = dyn_cast<SILFunctionArgument>(
            stripAccessMarkers(markedValue->getOperand()));
        // If we have a closure captured that we specialized, we should have a
        // no consume or assign and should emit a normal guaranteed diagnostic.
        if (fArg && fArg->isClosureCapture() &&
            fArg->getArgumentConvention().isInoutConvention()) {
          assert(checkKind == MarkMustCheckInst::CheckKind::NoConsumeOrAssign);
          moveChecker.diagnosticEmitter.emitObjectGuaranteedDiagnostic(
              markedValue);
          return true;
        }

        // If we have a function argument that is no_consume_or_assign and we do
        // not have any partial apply uses, then we know that we have a use of
        // an address only borrowed parameter that we need to
        if (fArg &&
            checkKind == MarkMustCheckInst::CheckKind::NoConsumeOrAssign &&
            !moveChecker.canonicalizer.hasPartialApplyConsumingUse()) {
          moveChecker.diagnosticEmitter.emitObjectGuaranteedDiagnostic(
              markedValue);
          return true;
        }

        // Finally try to emit either a global or class field error...
        if (!moveChecker.diagnosticEmitter
                 .emitGlobalOrClassFieldLoadedAndConsumed(markedValue)) {
          // And otherwise if we failed emit an escaping closure error.
          moveChecker.diagnosticEmitter
              .emitAddressEscapingClosureCaptureLoadedAndConsumed(markedValue);
        }

        return true;
      }

      // If set, this will tell the checker that we can change this load into
      // a load_borrow.
      auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
      if (!leafRange)
        return false;

      LLVM_DEBUG(llvm::dbgs() << "Found potential borrow: " << *user);

      if (checkForExclusivityHazards(li)) {
        LLVM_DEBUG(llvm::dbgs() << "Found exclusivity violation?!\n");
        return true;
      }

      useState.borrows.insert({user, *leafRange});

      // If we had a load [copy], borrow then we know that all of its destroys
      // must have been destroy_value. So we can just gather up those
      // destroy_value and use then to create liveness to ensure that our
      // value is alive over the entire borrow scope we are going to create.
      LLVM_DEBUG(llvm::dbgs() << "Adding destroys from load as liveness uses "
                                 "since they will become end_borrows.\n");
      for (auto *consumeUse : li->getConsumingUses()) {
        auto *dvi = cast<DestroyValueInst>(consumeUse->getUser());
        useState.recordLivenessUse(dvi, *leafRange);
      }

      return true;
    }

    // First check if we had any consuming uses that actually needed a
    // copy. This will always be an error and we allow the user to recompile
    // and eliminate the error. This just allows us to rely on invariants
    // later.
    if (moveChecker.canonicalizer.foundConsumingUseRequiringCopy()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Found that load at object level requires copies!\n");
      // If we failed to understand how to perform the check or did not find
      // any targets... continue. In the former case we want to fail with a
      // checker did not understand diagnostic later and in the former, we
      // succeeded.
      // Otherwise, emit the diagnostic.
      moveChecker.diagnosticEmitter.emitObjectOwnedDiagnostic(markedValue);
      LLVM_DEBUG(llvm::dbgs() << "Emitted early object level diagnostic.\n");
      return true;
    }

    if (!moveChecker.canonicalizer.foundFinalConsumingUses()) {
      LLVM_DEBUG(llvm::dbgs() << "Found potential borrow inst: " << *user);
      if (checkForExclusivityHazards(li)) {
        LLVM_DEBUG(llvm::dbgs() << "Found exclusivity violation?!\n");
        return true;
      }

      useState.borrows.insert({user, *leafRange});
      // If we had a load [copy], borrow then we know that all of its destroys
      // must have been destroy_value. So we can just gather up those
      // destroy_value and use then to create liveness to ensure that our
      // value is alive over the entire borrow scope we are going to create.
      LLVM_DEBUG(llvm::dbgs() << "Adding destroys from load as liveness uses "
                                 "since they will become end_borrows.\n");
      for (auto *consumeUse : li->getConsumingUses()) {
        auto *dvi = cast<DestroyValueInst>(consumeUse->getUser());
        useState.recordLivenessUse(dvi, *leafRange);
      }
    } else {
      // Now that we know that we are going to perform a take, perform a
      // checkForDestructure.
      checkForPartialConsume(useState, diagnosticEmitter, op, *leafRange,
                             IsPartialConsumeOrReinit_t::IsPartialConsume);

      // If we emitted an error diagnostic, do not transform further and instead
      // mark that we emitted an early diagnostic and return true.
      if (numDiagnostics !=
          moveChecker.diagnosticEmitter.getDiagnosticCount()) {
        LLVM_DEBUG(llvm::dbgs()
                   << "Emitting destructure through deinit error!\n");
        return true;
      }

      // If we had a load [copy], store this into the copy list. These are the
      // things that we must merge into destroy_addr or reinits after we are
      // done checking. The load [take] are already complete and good to go.
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        LLVM_DEBUG(llvm::dbgs() << "Found take inst: " << *user);
        useState.takeInsts.insert({user, *leafRange});
      } else {
        LLVM_DEBUG(llvm::dbgs() << "Found copy inst: " << *user);
        useState.copyInsts.insert({user, *leafRange});
      }
    }
    return true;
  }

  // Now that we have handled or loadTakeOrCopy, we need to now track our
  // additional pure takes.
  if (noncopyable::memInstMustConsume(op)) {
    // If we don't have a consumeable and assignable check kind, then we can't
    // consume. Emit an error.
    //
    // NOTE: Since SILGen eagerly loads loadable types from memory, this
    // generally will only handle address only types.
    if (markedValue->getCheckKind() !=
        MarkMustCheckInst::CheckKind::ConsumableAndAssignable) {
      auto *fArg = dyn_cast<SILFunctionArgument>(
          stripAccessMarkers(markedValue->getOperand()));
      if (fArg && fArg->isClosureCapture() && fArg->getType().isAddress()) {
        moveChecker.diagnosticEmitter.emitPromotedBoxArgumentError(markedValue,
                                                                   fArg);
      } else {
        moveChecker.diagnosticEmitter
            .emitAddressEscapingClosureCaptureLoadedAndConsumed(markedValue);
      }
      return true;
    }

    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
    if (!leafRange)
      return false;

    // Now check if we have a destructure through deinit. If we do, emit an
    // error.
    unsigned numDiagnostics =
        moveChecker.diagnosticEmitter.getDiagnosticCount();
    checkForPartialConsume(useState, diagnosticEmitter, op, *leafRange,
                           IsPartialConsumeOrReinit_t::IsPartialConsume);
    if (numDiagnostics != moveChecker.diagnosticEmitter.getDiagnosticCount()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Emitting destructure through deinit error!\n");
      return true;
    }

    LLVM_DEBUG(llvm::dbgs() << "Pure consuming use: " << *user);
    useState.takeInsts.insert({user, *leafRange});
    return true;
  }

  if (auto fas = FullApplySite::isa(user)) {
    switch (fas.getArgumentConvention(*op)) {
    case SILArgumentConvention::Indirect_In_Guaranteed: {
      auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
      if (!leafRange)
        return false;

      useState.recordLivenessUse(user, *leafRange);
      return true;
    }

    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_InoutAliasable:
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_Out:
    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Owned:
    case SILArgumentConvention::Direct_Guaranteed:
    case SILArgumentConvention::Pack_Inout:
    case SILArgumentConvention::Pack_Owned:
    case SILArgumentConvention::Pack_Guaranteed:
    case SILArgumentConvention::Pack_Out:
      break;
    }
  }

  if (auto *yi = dyn_cast<YieldInst>(user)) {
    if (yi->getYieldInfoForOperand(*op).isGuaranteed()) {
      auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
      if (!leafRange)
        return false;

      useState.recordLivenessUse(user, *leafRange);
      return true;
    }
  }

  if (auto *pas = dyn_cast<PartialApplyInst>(user)) {
    if (auto *fArg = dyn_cast<SILFunctionArgument>(
            stripAccessMarkers(markedValue->getOperand()))) {
      // If we are processing an inout convention and we emitted an error on the
      // partial_apply, we shouldn't process this mark_must_check, but squelch
      // the compiler doesn't understand error.
      if (fArg->getArgumentConvention().isInoutConvention() &&
          pas->getCalleeFunction()->hasSemanticsAttr(
              semantics::NO_MOVEONLY_DIAGNOSTICS)) {
        diagnosticEmitter.emitEarlierPassEmittedDiagnostic(markedValue);
        return false;
      }
    }

    // If our partial apply takes this parameter as an inout parameter and it
    // has the no move only diagnostics marker on it, do not emit an error
    // either.
    if (auto *f = pas->getCalleeFunction()) {
      if (f->hasSemanticsAttr(semantics::NO_MOVEONLY_DIAGNOSTICS)) {
        if (ApplySite(pas).getCaptureConvention(*op).isInoutConvention()) {
          diagnosticEmitter.emitEarlierPassEmittedDiagnostic(markedValue);
          return false;
        }
      }
    }

    if (pas->isOnStack() ||
        ApplySite(pas).getArgumentConvention(*op).isInoutConvention()) {
      LLVM_DEBUG(llvm::dbgs() << "Found on stack partial apply or inout usage!\n");
      // On-stack partial applications and their final consumes are always a
      // liveness use of their captures.
      auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
      if (!leafRange) {
        LLVM_DEBUG(llvm::dbgs() << "Failed to compute leaf range!\n");
        return false;
      }

      // Attempt to find calls of the non-escaping partial apply and places
      // where the partial apply is passed to a function. We treat those as
      // liveness uses. If we find a use we don't understand, we return false
      // here.
      if (!findNonEscapingPartialApplyUses(pas, *leafRange, useState)) {
        LLVM_DEBUG(
            llvm::dbgs()
            << "Failed to understand use of a non-escaping partial apply?!\n");
        return false;
      }

      return true;
    }
  }

  if (auto *explicitCopy = dyn_cast<ExplicitCopyAddrInst>(op->getUser())) {
    assert(op->getOperandNumber() == ExplicitCopyAddrInst::Src &&
           "Dest should have been handled earlier");
    assert(!explicitCopy->isTakeOfSrc() &&
           "If we had a take of src, this should have already been identified "
           "as a must consume");
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
    if (!leafRange) {
      LLVM_DEBUG(llvm::dbgs() << "Failed to compute leaf range!\n");
      return false;
    }

    useState.recordLivenessUse(user, *leafRange);
    return true;
  }

  // If we don't fit into any of those categories, just track as a liveness
  // use. We assume all such uses must only be reads to the memory. So we assert
  // to be careful.
  auto leafRange = TypeTreeLeafTypeRange::get(op->get(), getRootAddress());
  if (!leafRange)
    return false;

  LLVM_DEBUG(llvm::dbgs() << "Found liveness use: " << *user);
#ifndef NDEBUG
  if (user->mayWriteToMemory()) {
    llvm::errs() << "Found a write classified as a liveness use?!\n";
    llvm::errs() << "Use: " << *user;
    llvm_unreachable("standard failure");
  }
#endif
  useState.recordLivenessUse(user, *leafRange);

  return true;
}

//===----------------------------------------------------------------------===//
//                           MARK: Global Dataflow
//===----------------------------------------------------------------------===//

namespace {

using InstLeafTypePair = std::pair<SILInstruction *, TypeTreeLeafTypeRange>;
using InstOptionalLeafTypePair =
    std::pair<SILInstruction *, llvm::Optional<TypeTreeLeafTypeRange>>;

/// Post process the found liveness and emit errors if needed. TODO: Better
/// name.
struct GlobalLivenessChecker {
  UseState &addressUseState;
  DiagnosticEmitter &diagnosticEmitter;
  FieldSensitiveMultiDefPrunedLiveRange &liveness;
  SmallBitVector livenessVector;
  bool hadAnyErrorUsers = false;

  GlobalLivenessChecker(UseState &addressUseState,
                        DiagnosticEmitter &diagnosticEmitter,
                        FieldSensitiveMultiDefPrunedLiveRange &liveness)
      : addressUseState(addressUseState), diagnosticEmitter(diagnosticEmitter),
        liveness(liveness) {}

  /// Returns true if we emitted any errors.
  bool compute();

  bool testInstVectorLiveness(
      llvm::SmallMapVector<SILInstruction *, TypeTreeLeafTypeRange, 4>
          &instsToTest);

  void clear() {
    livenessVector.clear();
    hadAnyErrorUsers = false;
  }
};

} // namespace

bool GlobalLivenessChecker::testInstVectorLiveness(
    llvm::SmallMapVector<SILInstruction *, TypeTreeLeafTypeRange, 4>
        &instsToTest) {
  bool emittedDiagnostic = false;

  for (auto takeInstAndValue : instsToTest) {
    LLVM_DEBUG(llvm::dbgs() << "    Checking: " << *takeInstAndValue.first);

    // Check if we are in the boundary...

    // If the bit vector does not contain any set bits, then we know that we did
    // not have any boundary violations for any leaf node of our root value.
    if (!liveness.isWithinBoundary(takeInstAndValue.first,
                                   takeInstAndValue.second)) {
      // TODO: Today, we don't tell the user the actual field itself where the
      // violation occurred and just instead just shows the two instructions. We
      // could be more specific though...
      LLVM_DEBUG(llvm::dbgs() << "        Not within the boundary.\n");
      continue;
    }
    LLVM_DEBUG(llvm::dbgs()
               << "        Within the boundary! Emitting an error\n");

    // Ok, we have an error and via the bit vector know which specific leaf
    // elements of our root type were within the per field boundary. We need to
    // go find the next reachable use that overlap with its sub-element. We only
    // emit a single error per use even if we get multiple sub elements that
    // match it. That helps reduce the amount of errors.
    //
    // DISCUSSION: It is important to note that this follows from the separation
    // of concerns behind this pass: we have simplified how we handle liveness
    // by losing this information. That being said, since we are erroring it is
    // ok that we are taking a little more time since we are not going to
    // codegen this code.
    //
    // That being said, set the flag that we saw at least one error, so we can
    // exit early after this loop.
    hadAnyErrorUsers = true;

    // B/c of the separation of concerns with our liveness, we now need to walk
    // blocks to go find the specific later takes that are reachable from this
    // take. It is ok that we are doing a bit more work here since we are going
    // to exit and not codegen.
    auto *errorUser = takeInstAndValue.first;
    auto errorSpan = takeInstAndValue.second;

    // First walk from errorUser to the end of the block, looking for a take or
    // a liveness use. If we find a single block error, emit the error and
    // continue.
    if (errorUser != errorUser->getParent()->getTerminator()) {
      bool foundSingleBlockError = false;
      for (auto ii = std::next(errorUser->getIterator()),
                ie = errorUser->getParent()->end();
           ii != ie; ++ii) {
        if (addressUseState.isConsume(&*ii, errorSpan)) {
          diagnosticEmitter.emitAddressDiagnostic(
              addressUseState.address, &*ii, errorUser, true /*is consuming*/);
          foundSingleBlockError = true;
          emittedDiagnostic = true;
          break;
        }

        if (addressUseState.isLivenessUse(&*ii, errorSpan)) {
          diagnosticEmitter.emitAddressDiagnostic(
              addressUseState.address, &*ii, errorUser, false /*is consuming*/,
              addressUseState.isImplicitEndOfLifetimeLivenessUses(&*ii));
          foundSingleBlockError = true;
          emittedDiagnostic = true;
          break;
        }

        // Check if we have a non-consuming liveness use.
        //
        // DISCUSSION: In certain cases, we only represent uses like end_borrow
        // in liveness and not in address use state. This ensures that we
        // properly emit a diagnostic in these cases.
        //
        // TODO: We should include liveness uses of the load_borrow itself in an
        // array and emit an error on those instead since it would be a better
        // error than using end_borrow here.
        {
          if (liveness.isInterestingUserOfKind(
                  &*ii, FieldSensitivePrunedLiveness::NonLifetimeEndingUse,
                  errorSpan)) {
            diagnosticEmitter.emitAddressDiagnostic(
                addressUseState.address, &*ii, errorUser,
                false /*is consuming*/,
                addressUseState.isImplicitEndOfLifetimeLivenessUses(&*ii));
            foundSingleBlockError = true;
            emittedDiagnostic = true;
            break;
          }
        }

        if (addressUseState.isInitUse(&*ii, errorSpan)) {
          llvm::errs() << "Should not have errored if we see an init?! Init: "
                       << *ii;
          llvm_unreachable("Standard compiler error");
        }
      }
      if (foundSingleBlockError)
        continue;
    }

    // If we didn't find a single block error, then we need to go search for our
    // liveness error in successor blocks. We know that this means that our
    // current block must be live out. Do a quick check just to be careful.
    using IsLive = FieldSensitivePrunedLiveBlocks::IsLive;
    SmallVector<IsLive, 8> isLiveArray;
#ifndef NDEBUG
    liveness.getBlockLiveness(errorUser->getParent(), errorSpan, isLiveArray);
    assert(llvm::all_of(
               isLiveArray,
               [](IsLive liveness) { return liveness = IsLive::LiveOut; }) &&
           "Should be live out?!");
    isLiveArray.clear();
#endif

    BasicBlockWorklist worklist(errorUser->getFunction());
    for (auto *succBlock : errorUser->getParent()->getSuccessorBlocks())
      worklist.pushIfNotVisited(succBlock);

    LLVM_DEBUG(llvm::dbgs() << "Performing forward traversal from errorUse "
                               "looking for the cause of liveness!\n");

    SmallSetVector<SILInstruction *, 1> violatingInst;
    bool foundSingleBlockError = false;
    while (auto *block = worklist.pop()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Visiting block: bb" << block->getDebugID() << "\n");

      SWIFT_DEFER { isLiveArray.clear(); };
      liveness.getBlockLiveness(block, takeInstAndValue.second, isLiveArray);

      // If we hit an init or dead along all bits in the block, we do not need
      // to process further successors.
      bool shouldVisitSuccessors = false;

      // Now search forward for uses.
      for (auto isLive : isLiveArray) {
        switch (isLive) {
        case IsLive::Dead:
          LLVM_DEBUG(llvm::dbgs() << "    Dead block!\n");
          // Ignore a dead block. Our error use could not be in such a block.
          //
          // This can happen for instance along an exit block of a loop where
          // the error use is within the loop.
          continue;
        case IsLive::LiveOut:
          LLVM_DEBUG(llvm::dbgs() << "    Live out block!\n");
          // If we see a live out block that is also a def block, we need to fa
          assert(!liveness.isDefBlock(block, errorSpan) &&
                 "If in def block... we are in liveness block");
          [[clang::fallthrough]];
        case IsLive::LiveWithin:
          if (isLive == IsLive::LiveWithin)
            LLVM_DEBUG(llvm::dbgs() << "    Live within block!\n");

          bool foundInit = false;
          for (auto &blockInst : *block) {
            LLVM_DEBUG(llvm::dbgs() << "        Inst: " << blockInst);

            if (addressUseState.isConsume(&blockInst, errorSpan)) {
              LLVM_DEBUG(llvm::dbgs() << "            Is consume!\n");
              diagnosticEmitter.emitAddressDiagnostic(addressUseState.address,
                                                      &blockInst, errorUser,
                                                      true /*is consuming*/);
              foundSingleBlockError = true;
              emittedDiagnostic = true;
              break;
            }

            if (addressUseState.isLivenessUse(&blockInst, errorSpan)) {
              LLVM_DEBUG(llvm::dbgs() << "            Is liveness use!\n");
              diagnosticEmitter.emitAddressDiagnostic(
                  addressUseState.address, &blockInst, errorUser,
                  false /*is consuming*/,
                  addressUseState.isImplicitEndOfLifetimeLivenessUses(
                      &blockInst));
              foundSingleBlockError = true;
              emittedDiagnostic = true;
              break;
            }

            // If we find an init use for this bit... just break.
            if (addressUseState.isInitUse(&blockInst, errorSpan)) {
              foundInit = true;
              break;
            }
          }

          // If we did not find an init and visited the entire block... we need
          // to visit successors for at least one bit.
          if (!foundInit)
            shouldVisitSuccessors = true;

          assert((isLive == IsLive::LiveOut || foundSingleBlockError ||
                  foundInit) &&
                 "Should either have a pure live out, found an init, or we "
                 "should have found "
                 "an error.");
        }

        // If we found an error, break out of the loop. We don't have further
        // work to do.
        if (foundSingleBlockError) {
          break;
        }
      }

      // If we found an error, just bail without processing additional blocks.
      if (foundSingleBlockError)
        break;

      // If we saw only dead blocks or found inits for all bits... then we do
      // not need to process further
      if (!shouldVisitSuccessors)
        continue;

      // If we didn't find a single block error, add successors to the worklist
      // and visit them.
      for (auto *succBlock : block->getSuccessorBlocks())
        worklist.pushIfNotVisited(succBlock);
    }
  }

  return emittedDiagnostic;
}

bool GlobalLivenessChecker::compute() {
  // Then revisit our takes, this time checking if we are within the boundary
  // and if we are, emit an error.
  LLVM_DEBUG(llvm::dbgs() << "Checking takes for errors!\n");
  bool emittedDiagnostic = false;

  emittedDiagnostic |= testInstVectorLiveness(addressUseState.takeInsts);
  emittedDiagnostic |= testInstVectorLiveness(addressUseState.copyInsts);

  // If we emitted an error user, we should always emit at least one
  // diagnostic. If we didn't there is a bug in the implementation.
  assert(hadAnyErrorUsers == emittedDiagnostic);
  return hadAnyErrorUsers;
}

//===----------------------------------------------------------------------===//
//                       MARK: Main Pass Implementation
//===----------------------------------------------------------------------===//

/// Create a new destroy_value instruction before the specified instruction and
/// record it as a final consume.
static void insertDestroyBeforeInstruction(UseState &addressUseState,
                                           SILInstruction *nextInstruction,
                                           SILValue baseAddress,
                                           SmallBitVector &bv,
                                           ConsumeInfo &consumes) {
  // If we need all bits...
  if (bv.all()) {
    // And our next instruction is a destroy_addr on the base address, just
    // claim that destroy instead of inserting another destroy_addr.
    if (auto *dai = dyn_cast<DestroyAddrInst>(nextInstruction)) {
      if (dai->getOperand() == baseAddress) {
        consumes.recordFinalConsume(dai, bv);
        return;
      }
    }

    // Otherwise, create a new destroy addr on the entire address.
    SILBuilderWithScope builder(nextInstruction);
    auto loc =
        RegularLocation::getAutoGeneratedLocation(nextInstruction->getLoc());
    auto *dai = builder.createDestroyAddr(loc, baseAddress);
    consumes.recordFinalConsume(dai, bv);
    addressUseState.destroys.insert({dai, TypeTreeLeafTypeRange(0, bv.size())});
    return;
  }

  // Otherwise, we have a partially destroyed type. Create new destroy addr for
  // each contiguous range of elts. This should only happen for structs/tuples.
  SILBuilderWithScope builder(nextInstruction);
  auto loc =
      RegularLocation::getAutoGeneratedLocation(nextInstruction->getLoc());
  SmallVector<std::pair<SILValue, TypeTreeLeafTypeRange>> valuesToDestroy;
  TypeTreeLeafTypeRange::constructProjectionsForNeededElements(
      baseAddress, nextInstruction, bv, valuesToDestroy);
  while (!valuesToDestroy.empty()) {
    auto pair = valuesToDestroy.pop_back_val();
    if (pair.first->getType().isTrivial(*nextInstruction->getFunction()))
      continue;
    auto *dai = builder.createDestroyAddr(loc, pair.first);
    SmallBitVector consumedBits(bv.size());
    pair.second.setBits(consumedBits);
    consumes.recordFinalConsume(dai, consumedBits);
    addressUseState.destroys.insert({dai, pair.second});
  }
}

void MoveOnlyAddressCheckerPImpl::insertDestroysOnBoundary(
    MarkMustCheckInst *markedValue,
    FieldSensitiveMultiDefPrunedLiveRange &liveness,
    FieldSensitivePrunedLivenessBoundary &boundary) {
  using IsInterestingUser = FieldSensitivePrunedLiveness::IsInterestingUser;
  LLVM_DEBUG(llvm::dbgs() << "Inserting destroys on boundary!\n");

  // If we're in no_consume_or_assign mode, we don't insert destroys, as we've
  // already checked that there are no consumes. There can only be borrow uses,
  // which means no destruction is needed at all.
  //
  // NOTE: This also implies that we do not need to insert invalidating
  // debug_value undef since our value will not be invalidated.
  if (markedValue->getCheckKind() ==
      MarkMustCheckInst::CheckKind::NoConsumeOrAssign) {
    LLVM_DEBUG(llvm::dbgs()
               << "    Skipping destroy insertion b/c no_consume_or_assign\n");
    consumes.finishRecordingFinalConsumes();
    return;
  }

  LLVM_DEBUG(llvm::dbgs() << "    Visiting users!\n");

  auto debugVar = DebugVarCarryingInst::getFromValue(
      stripAccessMarkers(markedValue->getOperand()));

  // Local helper that insert a debug_value undef to invalidate a noncopyable
  // value that has been moved. Importantly, for LLVM to recognize that we are
  // referring to the same debug variable as the original definition, we have to
  // use the same debug scope and location as the original debug var.
  auto insertUndefDebugValue = [&debugVar](SILInstruction *insertPt) {
    insertDebugValueBefore(insertPt, debugVar, [&]{
      return SILUndef::get(debugVar.getOperandForDebugValueClone()->getType(),
                           insertPt->getModule());
    });
  };

  // Control flow merge blocks used as insertion points.
  llvm::DenseMap<SILBasicBlock *, SmallBitVector> mergeBlocks;

  for (auto &pair : boundary.getLastUsers()) {
    auto *inst = pair.first;
    auto &bv = pair.second;

    LLVM_DEBUG(llvm::dbgs() << "        User: " << *inst);

    auto interestingUser = liveness.getInterestingUser(inst);
    SmallVector<std::pair<TypeTreeLeafTypeRange, IsInterestingUser>, 4> ranges;
    if (interestingUser) {
      interestingUser->getContiguousRanges(ranges, bv);
    }

    for (auto rangePair : ranges) {
      SmallBitVector bits(bv.size());
      rangePair.first.setBits(bits);
      switch (rangePair.second) {
      case IsInterestingUser::LifetimeEndingUse: {
        LLVM_DEBUG(
            llvm::dbgs()
            << "        Lifetime ending use! Recording final consume!\n");
        // If we have a consuming use, when we stop at the consuming use we want
        // the value to still be around. We only want the value to be
        // invalidated once the consume operation has occured. Thus we always
        // place the debug_value undef strictly after the consuming operation.
        if (auto *ti = dyn_cast<TermInst>(inst)) {
          for (auto *succBlock : ti->getSuccessorBlocks()) {
            insertUndefDebugValue(&succBlock->front());
          }
        } else {
          insertUndefDebugValue(inst->getNextInstruction());
        }
        consumes.recordFinalConsume(inst, bits);
        continue;
      }
      case IsInterestingUser::NonUser:
        break;
      case IsInterestingUser::NonLifetimeEndingUse:
        LLVM_DEBUG(llvm::dbgs() << "        NonLifetimeEndingUse! "
                                   "inserting destroy before instruction!\n");
        // If we are dealing with an inout parameter, we will have modeled our
        // last use by treating a return inst as a last use. Since it doesn't
        // have any successors, this results in us not inserting any
        // destroy_addr.
        if (isa<TermInst>(inst)) {
          auto *block = inst->getParent();
          for (auto *succBlock : block->getSuccessorBlocks()) {
            auto iter = mergeBlocks.find(succBlock);
            if (iter == mergeBlocks.end()) {
              iter = mergeBlocks.insert({succBlock, bits}).first;
            } else {
              // NOTE: We use |= here so that different regions of the same
              // terminator get updated appropriately.
              SmallBitVector &alreadySetBits = iter->second;
              bool hadCommon = alreadySetBits.anyCommon(bits);
              alreadySetBits |= bits;
              if (hadCommon)
                continue;
            }

            auto *insertPt = &*succBlock->begin();
            insertDestroyBeforeInstruction(addressUseState, insertPt,
                                           liveness.getRootValue(), bits,
                                           consumes);
            // We insert the debug_value undef /after/ the last use since we
            // want the value to be around when we stop at the last use
            // instruction.
            insertUndefDebugValue(insertPt);
          }
          continue;
        }

        // If we have an implicit end of lifetime use, we do not insert a
        // destroy_addr. Instead, we insert an undef debug value after the
        // use. This occurs if we have an end_access associated with a
        // global_addr or a ref_element_addr field access.
        if (addressUseState.isImplicitEndOfLifetimeLivenessUses(inst)) {
          LLVM_DEBUG(
              llvm::dbgs()
              << "    Use was an implicit end of lifetime liveness use!\n");
          insertUndefDebugValue(inst->getNextInstruction());
          continue;
        }

        auto *insertPt = inst->getNextInstruction();
        insertDestroyBeforeInstruction(addressUseState, insertPt,
                                       liveness.getRootValue(), bits, consumes);
        // We insert the debug_value undef /after/ the last use since we want
        // the value to be around when we stop at the last use instruction.
        insertUndefDebugValue(insertPt);
        continue;
      }
    }
  }

  for (auto pair : boundary.getBoundaryEdges()) {
    auto *insertPt = &*pair.first->begin();
    insertDestroyBeforeInstruction(addressUseState, insertPt,
                                   liveness.getRootValue(), pair.second,
                                   consumes);
    insertUndefDebugValue(insertPt);
    LLVM_DEBUG(llvm::dbgs() << "    Inserting destroy on edge bb"
                            << pair.first->getDebugID() << "\n");
  }

  for (auto defPair : boundary.getDeadDefs()) {
    LLVM_DEBUG(llvm::dbgs()
               << "    Inserting destroy on dead def" << *defPair.first);

    if (auto *arg = dyn_cast<SILArgument>(defPair.first)) {
      auto *insertPt = &*arg->getParent()->begin();
      insertDestroyBeforeInstruction(addressUseState, insertPt,
                                     liveness.getRootValue(), defPair.second,
                                     consumes);
      insertUndefDebugValue(insertPt);
    } else {
      auto *inst = cast<SILInstruction>(defPair.first);

      // If we have a dead def that is our mark must check and that mark must
      // check was an init but not consumable, then do not destroy that
      // def. This is b/c we are in some sort of class initialization and we are
      // looking at the initial part of the live range before the initialization
      // has occured. This is our way of makinmg this fit the model that the
      // checker expects (which is that values are always initialized at the def
      // point).
      if (markedValue &&
          markedValue->getCheckKind() ==
              MarkMustCheckInst::CheckKind::InitableButNotConsumable)
        continue;

      auto *insertPt = inst->getNextInstruction();
      assert(insertPt && "def instruction was a terminator");
      insertDestroyBeforeInstruction(addressUseState, insertPt,
                                     liveness.getRootValue(), defPair.second,
                                     consumes);
      insertUndefDebugValue(insertPt);
    }
  }

  consumes.finishRecordingFinalConsumes();
}

void MoveOnlyAddressCheckerPImpl::rewriteUses(
    MarkMustCheckInst *markedValue,
    FieldSensitiveMultiDefPrunedLiveRange &liveness,
    const FieldSensitivePrunedLivenessBoundary &boundary) {
  LLVM_DEBUG(llvm::dbgs() << "MoveOnlyAddressChecker Rewrite Uses!\n");

  /// Whether the marked value appeared in a discard statement.
  const bool isDiscardingContext = !addressUseState.dropDeinitInsts.empty();

  // Process destroys
  for (auto destroyPair : addressUseState.destroys) {
    /// Is this destroy instruction a final consuming use?
    SmallBitVector bits(liveness.getNumSubElements());
    destroyPair.second.setBits(bits);
    bool isFinalConsume = consumes.claimConsume(destroyPair.first, bits);

    // Remove destroys that are not the final consuming use.
    if (!isFinalConsume) {
      destroyPair.first->eraseFromParent();
      continue;
    }

    // Otherwise, if we're in a discarding context, flag this final destroy_addr
    // as a point where we're missing an explicit `consume self`. The reasoning
    // here is that if a destroy of self is the final consuming use,
    // then these are the points where we implicitly destroy self to clean-up
    // that self var before exiting the scope. An explicit 'consume self'
    // that is thrown away is a consume of this mark_must_check'd var and not a
    // destroy of it, according to the use classifier.
    if (isDiscardingContext) {

      // Since the boundary computations treat a newly-added destroy prior to
      // a reinit within that same block as a "final consuming use", exclude
      // such destroys-before-reinit. We are only interested in the final
      // destroy of a var, not intermediate destroys of the var.
      if (addressUseState.precedesReinitInSameBlock(destroyPair.first))
        continue;

      auto *dropDeinit = addressUseState.dropDeinitInsts.front();
      diagnosticEmitter.emitMissingConsumeInDiscardingContext(destroyPair.first,
                                                              dropDeinit);
    }
  }

  auto debugVar = DebugVarCarryingInst::getFromValue(
    stripAccessMarkers(markedValue->getOperand()));

  // Then convert all claimed reinits to inits.
  for (auto reinitPair : addressUseState.reinitInsts) {
    if (!isReinitToInitConvertibleInst(reinitPair.first))
      continue;
    if (!consumes.claimConsume(reinitPair.first, reinitPair.second))
      convertMemoryReinitToInitForm(reinitPair.first, debugVar);
  }

  // Check all takes.
  for (auto takeInst : addressUseState.takeInsts) {
    SmallBitVector bits(liveness.getNumSubElements());
    takeInst.second.setBits(bits);
    bool claimedConsume = consumes.claimConsume(takeInst.first, bits);
    (void)claimedConsume;
    if (!claimedConsume) {
      llvm::errs()
          << "Found consume that was not recorded as a 'claimed consume'!\n";
      llvm::errs() << "Unrecorded consume: " << *takeInst.first;
      llvm_unreachable("Standard compiler abort?!");
    }
  }

  // Then rewrite all copy insts to be takes and claim them.
  for (auto copyInst : addressUseState.copyInsts) {
    SmallBitVector bits(liveness.getNumSubElements());
    copyInst.second.setBits(bits);
    bool claimedConsume = consumes.claimConsume(copyInst.first, bits);
    if (!claimedConsume) {
      llvm::errs()
          << "Found consume that was not recorded as a 'claimed consume'!\n";
      llvm::errs() << "Unrecorded consume: " << *copyInst.first;
      llvm_unreachable("Standard compiler abort?!");
    }
    if (auto *li = dyn_cast<LoadInst>(copyInst.first)) {
      // Convert this to its take form.
      auto accessPath = AccessPathWithBase::computeInScope(li->getOperand());
      if (auto *access = dyn_cast<BeginAccessInst>(accessPath.base))
        access->setAccessKind(SILAccessKind::Modify);
      li->setOwnershipQualifier(LoadOwnershipQualifier::Take);
      changed = true;
      continue;
    }

    if (auto *copy = dyn_cast<CopyAddrInst>(copyInst.first)) {
      // Convert this to its take form.
      auto accessPath = AccessPathWithBase::computeInScope(copy->getSrc());
      if (auto *access = dyn_cast<BeginAccessInst>(accessPath.base))
        access->setAccessKind(SILAccessKind::Modify);
      copy->setIsTakeOfSrc(IsTake);
      continue;
    }

    llvm::dbgs() << "Unhandled copy user: " << *copyInst.first;
    llvm_unreachable("Unhandled case?!");
  }

  // Finally now that we have placed all of our destroys in the appropriate
  // places, convert any copies that we know are borrows into begin_borrow. We
  // do not need to worry about expanding scopes since if we needed to expand a
  // scope, we would have emitted the scope expansion error during diagnostics.
  for (auto pair : addressUseState.borrows) {
    if (auto *li = dyn_cast<LoadInst>(pair.first)) {
      // If we had a load -> load_borrow then we know that all of its destroys
      // must have been destroy_value. So we can just gather up those
      // destroy_value and use then to create a new load_borrow scope.
      SILBuilderWithScope builder(li);
      auto *lbi = builder.createLoadBorrow(li->getLoc(), li->getOperand());
      // We use this auxillary list to avoid iterator invalidation of
      // li->getConsumingUse();
      StackList<DestroyValueInst *> toDelete(lbi->getFunction());
      for (auto *consumeUse : li->getConsumingUses()) {
        auto *dvi = cast<DestroyValueInst>(consumeUse->getUser());
        SILBuilderWithScope destroyBuilder(dvi);
        destroyBuilder.createEndBorrow(dvi->getLoc(), lbi);
        toDelete.push_back(dvi);
        changed = true;
      }
      while (!toDelete.empty())
        toDelete.pop_back_val()->eraseFromParent();

      li->replaceAllUsesWith(lbi);
      li->eraseFromParent();
      continue;
    }

    llvm::dbgs() << "Borrow: " << *pair.first;
    llvm_unreachable("Unhandled case?!");
  }

#ifndef NDEBUG
  if (consumes.hasUnclaimedConsumes()) {
    llvm::errs() << "Found unclaimed consumes?!\n";
    consumes.print(llvm::errs());
    llvm_unreachable("Standard error?!");
  }
#endif
}

void MoveOnlyAddressCheckerPImpl::checkForReinitAfterDiscard() {
  auto const &dropDeinits = addressUseState.dropDeinitInsts;
  auto const &reinits = addressUseState.reinitInsts;

  if (dropDeinits.empty() || reinits.empty())
    return;

  using BasicBlockMap = llvm::DenseMap<SILBasicBlock *,
                                       llvm::SmallPtrSet<SILInstruction *, 2>>;
  BasicBlockMap blocksWithReinit;
  for (auto const &info : reinits) {
    auto *reinit = info.first;
    blocksWithReinit[reinit->getParent()].insert(reinit);
  }

  // Starting from each drop_deinit instruction, can we reach a reinit of self?
  for (auto *dropInst : dropDeinits) {
    auto *dropBB = dropInst->getParent();

    // First, if the block containing this drop_deinit also contains a reinit,
    // check if that reinit happens after this drop_deinit.
    auto result = blocksWithReinit.find(dropBB);
    if (result != blocksWithReinit.end()) {
      auto &blockReinits = result->second;
      for (auto ii = std::next(dropInst->getIterator()); ii != dropBB->end();
           ++ii) {
        SILInstruction *current = &*ii;
        if (blockReinits.contains(current)) {
          // Then the drop_deinit can reach a reinit immediately after it in the
          // same block.
          diagnosticEmitter.emitReinitAfterDiscardError(current, dropInst);
          return;
        }
      }
    }

    BasicBlockWorklist worklist(fn);

    // Seed the search with the successors of the drop_init block, so that if we
    // visit the drop_deinit block again, we'll know the reinits _before_ the
    // drop_deinit are reachable via some back-edge / cycle.
    for (auto *succ : dropBB->getSuccessorBlocks())
      worklist.pushIfNotVisited(succ);

    // Determine reachability across blocks.
    while (auto *bb = worklist.pop()) {
      // Set-up next iteration.
      for (auto *succ : bb->getSuccessorBlocks())
        worklist.pushIfNotVisited(succ);

      auto result = blocksWithReinit.find(bb);
      if (result == blocksWithReinit.end())
        continue;

      // We found a reachable reinit! Identify the earliest reinit in this block
      // for diagnosis.
      auto &blockReinits = result->second;
      SILInstruction *firstBadReinit = nullptr;
      for (auto &inst : *bb) {
        if (blockReinits.contains(&inst)) {
          firstBadReinit = &inst;
          break;
        }
      }

      if (!firstBadReinit)
        llvm_unreachable("bug");

      diagnosticEmitter.emitReinitAfterDiscardError(firstBadReinit, dropInst);
      return;
    }
  }
}

void ExtendUnconsumedLiveness::run() {
  ConsumingBlocksCollection consumingBlocks;
  DestroysCollection destroys;
  for (unsigned element = 0, count = liveness.getNumSubElements();
       element < count; ++element) {

    for (auto pair : addressUseState.consumingBlocks) {
      if (pair.second.test(element)) {
        consumingBlocks.insert(pair.first);
      }
    }

    for (auto pair : addressUseState.destroys) {
      if (pair.second.contains(element)) {
        destroys[pair.first] = DestroyKind::Destroy;
      }
    }
    for (auto pair : addressUseState.takeInsts) {
      if (pair.second.contains(element)) {
        destroys[pair.first] = DestroyKind::Take;
      }
    }
    for (auto pair : addressUseState.reinitInsts) {
      if (pair.second.test(element)) {
        destroys[pair.first] = DestroyKind::Reinit;
      }
    }

    runOnField(element, destroys, consumingBlocks);

    consumingBlocks.clear();
    destroys.clear();
  }
}

/// Extend liveness of each field as far as possible within the original live
/// range as far as possible without incurring any copies.
///
/// The strategy has two parts.
///
/// (1) The global analysis:
/// - Collect the blocks in which the field was live before canonicalization.
///   These are the "original" live blocks (originalLiveBlocks).
///   [Color these blocks green.]
/// - From within that collection, collect the blocks which contain a _final_
///   consuming, non-destroy use, and their iterative successors.
///   These are the "consumed" blocks (consumedAtExitBlocks).
///   [Color these blocks red.]
/// - Extend liveness down to the boundary between originalLiveBlocks and
///   consumedAtExitBlocks blocks.
///   [Extend liveness down to the boundary between green blocks and red.]
/// - In particular, in regions of originalLiveBlocks which have no boundary
///   with consumedAtExitBlocks, liveness should be extended to its original
///   extent.
///   [Extend liveness down to the boundary between green blocks and uncolored.]
///
/// (2) The local analysis:
/// - For in-block lifetimes, extend liveness forward from non-consuming uses
///   and dead defs to the original destroy.
void ExtendUnconsumedLiveness::runOnField(
    unsigned element, DestroysCollection &destroys,
    ConsumingBlocksCollection &consumingBlocks) {
  SILValue currentDef = addressUseState.address;

  // First, collect the blocks that were _originally_ live.  We can't use
  // liveness here because it doesn't include blocks that occur before a
  // destroy_addr.
  BasicBlockSet originalLiveBlocks(currentDef->getFunction());
  {
    // Some of the work here was already done by initializeLiveness.
    // Specifically, it already discovered all blocks containing (transitive)
    // uses and blocks that appear between them and the def.
    //
    // Seed the set with what it already discovered.
    for (auto *discoveredBlock : liveness.getDiscoveredBlocks())
      originalLiveBlocks.insert(discoveredBlock);

    // Start the walk from the consuming blocks (which includes destroys as well
    // as the other consuming uses).
    BasicBlockWorklist worklist(currentDef->getFunction());
    for (auto *consumingBlock : consumingBlocks) {
      if (!originalLiveBlocks.insert(consumingBlock)
          // Don't walk into the predecessors of blocks which kill liveness.
          && !isLiveAtBegin(consumingBlock, element, /*isLiveAtEnd=*/true, destroys)) {
        continue;
      }
      for (auto *predecessor : consumingBlock->getPredecessorBlocks()) {
        worklist.pushIfNotVisited(predecessor);
      }
    }

    // Walk backwards from consuming blocks.
    while (auto *block = worklist.pop()) {
      if (!originalLiveBlocks.insert(block)) {
        continue;
      }
      for (auto *predecessor : block->getPredecessorBlocks()) {
        worklist.pushIfNotVisited(predecessor);
      }
    }
  }

  // Second, collect the blocks which occur after a consuming use.
  BasicBlockSet consumedAtExitBlocks(currentDef->getFunction());
  BasicBlockSetVector consumedAtEntryBlocks(currentDef->getFunction());
  {
    // Start the forward walk from blocks which contain non-destroy consumes not
    // followed by defs.
    //
    // Because they contain a consume not followed by a def, these are
    // consumed-at-exit.
    BasicBlockWorklist worklist(currentDef->getFunction());
    for (auto iterator : boundary.getLastUsers()) {
      if (!iterator.second.test(element))
        continue;
      auto *instruction = iterator.first;
      // Skip over destroys on the boundary.
      auto iter = destroys.find(instruction);
      if (iter != destroys.end() && iter->second != DestroyKind::Take) {
        continue;
      }
      // Skip over non-consuming users.
      auto interestingUser = liveness.isInterestingUser(instruction, element);
      assert(interestingUser !=
             FieldSensitivePrunedLiveness::IsInterestingUser::NonUser);
      if (interestingUser !=
          FieldSensitivePrunedLiveness::IsInterestingUser::LifetimeEndingUse) {
        continue;
      }
      // A consume with a subsequent def doesn't cause the block to be
      // consumed-at-exit.
      if (hasDefAfter(instruction, element))
        continue;
      worklist.push(instruction->getParent());
    }
    while (auto *block = worklist.pop()) {
      consumedAtExitBlocks.insert(block);
      for (auto *successor : block->getSuccessorBlocks()) {
        if (!originalLiveBlocks.contains(successor))
          continue;
        worklist.pushIfNotVisited(successor);
        consumedAtEntryBlocks.insert(successor);
      }
    }
  }

  // Third, find the blocks on the boundary between the originally-live blocks
  // and the originally-live-but-consumed blocks.  Extend liveness "to the end"
  // of these blocks.
  for (auto *block : consumedAtEntryBlocks) {
    for (auto *predecessor : block->getPredecessorBlocks()) {
      if (consumedAtExitBlocks.contains(predecessor))
        continue;
      // Add "the instruction(s) before the terminator" of the predecessor to
      // liveness.
      addPreviousInstructionToLiveness(predecessor->getTerminator(), element,
                                       /*lifetimeEnding*/ false);
    }
  }

  // Finally, preserve the destroys which weren't in the consumed region in
  // place: hoisting such destroys would not avoid copies.
  for (auto pair : destroys) {
    auto *destroy = pair.first;
    if (!shouldAddDestroyToLiveness(destroy, element, consumedAtExitBlocks,
                                    consumedAtEntryBlocks))
      continue;
    addPreviousInstructionToLiveness(destroy, element,
                                     /*lifetimeEnding*/ false);
  }
}

bool ExtendUnconsumedLiveness::shouldAddDestroyToLiveness(
    SILInstruction *destroy, unsigned element,
    BasicBlockSet const &consumedAtExitBlocks,
    BasicBlockSetVector const &consumedAtEntryBlocks) {
  auto *block = destroy->getParent();
  bool followedByDef = hasDefAfter(destroy, element);
  if (!followedByDef) {
    // This destroy is the last write to the field in the block.
    //
    // If the block is consumed-at-exit, then there is some other consuming use
    // before this destroy.  Liveness can't be extended.
    return !consumedAtExitBlocks.contains(block);
  }
  for (auto *inst = destroy->getPreviousInstruction(); inst;
       inst = inst->getPreviousInstruction()) {
    if (liveness.isDef(inst, element)) {
      // Found the corresponding def with no intervening users.  Liveness
      // can be extended to the destroy.
      return true;
    }
    auto interestingUser = liveness.isInterestingUser(inst, element);
    switch (interestingUser) {
    case FieldSensitivePrunedLiveness::IsInterestingUser::NonUser:
      break;
    case FieldSensitivePrunedLiveness::IsInterestingUser::NonLifetimeEndingUse:
      // The first use seen is non-consuming.  Liveness can be extended to the
      // destroy.
      return true;
      break;
    case FieldSensitivePrunedLiveness::IsInterestingUser::LifetimeEndingUse:
      // Found a consuming use.  Liveness can't be extended to the destroy
      // (without creating a copy and triggering a diagnostic).
      return false;
      break;
    }
  }
  // Found no uses or defs between the destroy and the top of the block.  If the
  // block was not consumed at entry, liveness can be extended to the destroy.
  return !consumedAtEntryBlocks.contains(block);
}

/// Compute the block's effect on liveness and apply it to \p isLiveAtEnd.
bool ExtendUnconsumedLiveness::isLiveAtBegin(SILBasicBlock *block,
                                             unsigned element, bool isLiveAtEnd,
                                             DestroysCollection const &destroys) {
  enum class Effect {
    None, // 0
    Kill, // 1
    Gen,  // 2
  };
  auto effect = Effect::None;
  for (auto &instruction : llvm::reverse(*block)) {
    // An instruction can be both a destroy and a def.  If it is, its
    // behavior is first to destroy and then to init.  So when walking
    // backwards, its last action is to destroy, so its effect is that of any
    // destroy.
    if (destroys.find(&instruction) != destroys.end()) {
      effect = Effect::Gen;
    } else if (liveness.isDef(&instruction, element)) {
      effect = Effect::Kill;
    }
  }
  switch (effect) {
  case Effect::None:
    return isLiveAtEnd;
  case Effect::Kill:
    return false;
  case Effect::Gen:
    return true;
  }
}

bool ExtendUnconsumedLiveness::hasDefAfter(SILInstruction *start,
                                           unsigned element) {
  // NOTE: Start iteration at \p start, not its sequel, because
  //       it might be both a consuming use and a def.
  for (auto *inst = start; inst; inst = inst->getNextInstruction()) {
    if (liveness.isDef(inst, element))
      return true;
  }
  return false;
}

void ExtendUnconsumedLiveness::addPreviousInstructionToLiveness(
    SILInstruction *inst, unsigned element, bool lifetimeEnding) {
  auto range = TypeTreeLeafTypeRange(element, element + 1);
  if (auto *previous = inst->getPreviousInstruction()) {
    liveness.updateForUse(previous, range, lifetimeEnding);
  } else {
    for (auto *predecessor : inst->getParent()->getPredecessorBlocks()) {
      liveness.updateForUse(predecessor->getTerminator(), range,
                            lifetimeEnding);
    }
  }
}

bool MoveOnlyAddressCheckerPImpl::performSingleCheck(
    MarkMustCheckInst *markedAddress) {
  SWIFT_DEFER { diagnosticEmitter.clearUsesWithDiagnostic(); };
  unsigned diagCount = diagnosticEmitter.getDiagnosticCount();

  // Before we do anything, canonicalize load_borrow + copy_value into load
  // [copy] + begin_borrow for further processing. This just eliminates a case
  // that the checker doesn't need to know about.
  {
    RAIILLVMDebug l("CopiedLoadBorrowEliminationVisitor");

    CopiedLoadBorrowEliminationState state(markedAddress->getFunction());
    CopiedLoadBorrowEliminationVisitor copiedLoadBorrowEliminator(state);
    if (AddressUseKind::Unknown ==
        std::move(copiedLoadBorrowEliminator).walk(markedAddress)) {
      LLVM_DEBUG(llvm::dbgs() << "Failed copied load borrow eliminator visit: "
                              << *markedAddress);
      return false;
    }
    state.process();
  }

  // Then if we have a let allocation, see if we have any copy_addr on our
  // markedAddress that form temporary allocation chains. This occurs when we
  // emit SIL for code like:
  //
  // let x: AddressOnlyType = ...
  // let _ = x.y.z
  //
  // SILGen will treat y as a separate rvalue from x and will create a temporary
  // allocation. In contrast if we have a var, we treat x like an lvalue and
  // just create GEPs appropriately.
  {
    RAIILLVMDebug l("temporary allocations from rvalue accesses");

    if (eliminateTemporaryAllocationsFromLet(markedAddress)) {
      LLVM_DEBUG(
          llvm::dbgs()
              << "Succeeded in eliminating temporary allocations! Fn after:\n";
          markedAddress->getFunction()->dump());
      changed = true;
    }
  }

  // Then gather all uses of our address by walking from def->uses. We use this
  // to categorize the uses of this address into their ownership behavior (e.g.:
  // init, reinit, take, destroy, etc.).
  GatherUsesVisitor visitor(*this, addressUseState, markedAddress,
                            diagnosticEmitter);
  SWIFT_DEFER { visitor.clear(); };

  {
    RAIILLVMDebug l("main use gathering visitor");

    visitor.reset(markedAddress);
    if (AddressUseKind::Unknown == std::move(visitor).walk(markedAddress)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Failed access path visit: " << *markedAddress);
      return false;
    }
  }

  // If we found a load [copy] or copy_addr that requires multiple copies or an
  // exclusivity error, then we emitted an early error. Bail now and allow the
  // user to fix those errors and recompile to get further errors.
  //
  // DISCUSSION: The reason why we do this is in the dataflow below we want to
  // be able to assume that the load [copy] or copy_addr/copy_addr [init] are
  // actual last uses, but the frontend that emitted the code for simplicity
  // emitted a copy from the base address + a destroy_addr of the use. By
  // bailing here, we can make that assumption since we would have errored
  // earlier otherwise.
  if (diagCount != diagnosticEmitter.getDiagnosticCount())
    return true;

  // Now that we know that we have run our visitor and did not emit any errors
  // and successfully visited everything, see if have any
  // assignable_but_not_consumable of address only types that are consumed.
  //
  // DISCUSSION: For non address only types, this is not an issue since we
  // eagerly load

  addressUseState.initializeImplicitEndOfLifetimeLivenessUses();

  //===---
  // Liveness Checking
  //

  SmallVector<SILBasicBlock *, 32> discoveredBlocks;
  FieldSensitiveMultiDefPrunedLiveRange liveness(fn, markedAddress,
                                                 &discoveredBlocks);

  {
    RAIILLVMDebug logger("liveness initialization");

    addressUseState.initializeLiveness(liveness);
  }

  // Now freeze our multimaps.
  addressUseState.freezeMultiMaps();

  {
    RAIILLVMDebug l("checking for partial reinits");
    PartialReinitChecker checker(addressUseState, diagnosticEmitter);
    unsigned count = diagnosticEmitter.getDiagnosticCount();
    checker.performPartialReinitChecking(liveness);
    if (count != diagnosticEmitter.getDiagnosticCount()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Found a partial reinit error! Ending early!\n");
      return true;
    }
  }

  {
    RAIILLVMDebug l("performing global liveness checks");
    // Then compute the takes that are within the cumulative boundary of
    // liveness that we have computed. If we find any, they are the errors
    // ones.
    GlobalLivenessChecker emitter(addressUseState, diagnosticEmitter, liveness);

    // If we had any errors, we do not want to modify the SIL... just bail.
    if (emitter.compute()) {
      return true;
    }
  }

  // First add any debug_values that we saw as liveness uses. This is important
  // since the debugger wants to see live values when we define a debug_value,
  // but we do not want to use them earlier when emitting diagnostic errors.
  if (auto *di = addressUseState.debugValue) {
    // Move the debug_value to right after the markedAddress to ensure that we
    // do not actually change our liveness computation.
    //
    // NOTE: The author is not sure if this can ever happen with SILGen output,
    // but this is being put just to be safe.
    di->moveAfter(markedAddress);
    liveness.updateForUse(di, TypeTreeLeafTypeRange(markedAddress),
                          false /*lifetime ending*/);
  }

  // Compute our initial boundary.
  FieldSensitivePrunedLivenessBoundary boundary(liveness.getNumSubElements());
  liveness.computeBoundary(boundary);
  LLVM_DEBUG(llvm::dbgs() << "Initial use based boundary:\n"; boundary.dump());

  if (!DisableMoveOnlyAddressCheckerLifetimeExtension) {
    ExtendUnconsumedLiveness extension(addressUseState, liveness, boundary);
    extension.run();
  }
  boundary.clear();
  liveness.computeBoundary(boundary);

  LLVM_DEBUG(llvm::dbgs() << "Final maximized boundary:\n"; boundary.dump());

  //===
  // Final Transformation
  //

  // Ok, we now know that we fit our model since we did not emit errors and thus
  // can begin the transformation.
  SWIFT_DEFER { consumes.clear(); };

  insertDestroysOnBoundary(markedAddress, liveness, boundary);
  checkForReinitAfterDiscard();
  rewriteUses(markedAddress, liveness, boundary);

  return true;
}

//===----------------------------------------------------------------------===//
//                         MARK: Top Level Entrypoint
//===----------------------------------------------------------------------===//

#ifndef NDEBUG
static llvm::cl::opt<uint64_t> NumTopLevelToProcess(
    "sil-move-only-address-checker-num-top-level-to-process",
    llvm::cl::desc("Allows for bisecting on move introducer that causes an "
                   "error. Only meant for debugging!"),
    llvm::cl::init(UINT64_MAX));
#endif

static llvm::cl::opt<bool> DumpSILBeforeRemovingMarkMustCheck(
    "sil-move-only-address-checker-dump-before-removing-mark-must-check",
    llvm::cl::desc("When bisecting it is useful to dump the SIL before the "
                   "rest of the checker removes mark_must_check. This lets one "
                   "grab the SIL of a bad variable after all of the rest have "
                   "been processed to work with further in sil-opt."),
    llvm::cl::init(false));

bool MoveOnlyAddressChecker::check(
    SmallSetVector<MarkMustCheckInst *, 32> &moveIntroducersToProcess) {
  assert(moveIntroducersToProcess.size() &&
         "Must have checks to process to call this function");
  MoveOnlyAddressCheckerPImpl pimpl(fn, diagnosticEmitter, domTree, poa,
                                    allocator);

#ifndef NDEBUG
  static uint64_t numProcessed = 0;
#endif
  for (auto *markedValue : moveIntroducersToProcess) {
#ifndef NDEBUG
    ++numProcessed;
    if (NumTopLevelToProcess <= numProcessed)
      break;
#endif
    LLVM_DEBUG(llvm::dbgs()
               << "======>>> Visiting top level: " << *markedValue);

    // Perform our address check.
    unsigned diagnosticEmittedByEarlierPassCount =
      diagnosticEmitter.getDiagnosticEmittedByEarlierPassCount();
    if (!pimpl.performSingleCheck(markedValue)) {
      if (diagnosticEmittedByEarlierPassCount !=
          diagnosticEmitter.getDiagnosticEmittedByEarlierPassCount()) {
        LLVM_DEBUG(
            llvm::dbgs()
            << "Failed to perform single check but found earlier emitted "
               "error. Not emitting checker doesn't understand diagnostic!\n");
        continue;
      }
      LLVM_DEBUG(llvm::dbgs() << "Failed to perform single check! Emitting "
                                 "compiler doesn't understand diagnostic!\n");
      // If we fail the address check in some way, set the diagnose!
      diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(markedValue);
    }
  }

  if (DumpSILBeforeRemovingMarkMustCheck) {
    LLVM_DEBUG(llvm::dbgs()
                   << "Dumping SIL before removing mark must checks!\n";
               fn->dump());
  }
  return pimpl.changed;
}
