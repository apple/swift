//===--- SILCombinerMiscVisitors.cpp --------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-combine"

#include "SILCombiner.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace swift::PatternMatch;

/// This flag is used to disable alloc stack optimizations to ease testing of
/// other SILCombine optimizations.
static llvm::cl::opt<bool>
    DisableAllocStackOpts("sil-combine-disable-alloc-stack-opts",
                          llvm::cl::init(false));

SILInstruction*
SILCombiner::visitAllocExistentialBoxInst(AllocExistentialBoxInst *AEBI) {
  // Optimize away the pattern below that happens when exceptions are created
  // and in some cases, due to inlining, are not needed.
  //
  //   %6 = alloc_existential_box $Error, $ColorError
  //   %7 = enum $VendingMachineError, #ColorError.Red
  //   store %7 to %6#1 : $*ColorError
  //   debug_value %6#0 : $Error
  //   strong_release %6#0 : $Error
  SILValue boxedValue =
    getConcreteValueOfExistentialBox(AEBI, /*ignoreUser*/ nullptr);
  if (!boxedValue)
    return nullptr;

  // Make sure that the stored value will be post-dominated by the release of
  // the box by making sure the stored value is in the same blcok as the aebi.
  if (auto *inst = boxedValue->getDefiningInsertionPoint()) {
    if (inst->getParent() != AEBI->getParent())
      return nullptr;
  }

  // Check if the box is released at a single place . That's the end of its
  // lifetime.
  //
  // NOTE: When ownership is enabled this includes any consumes of any copies
  // that we create of the box.
  SILInstruction *singleRelease = nullptr;
  bool foundSingleConsumingUse = false;
  SmallVector<Operand *, 32> worklist(AEBI->getUses());
  while (!worklist.empty()) {
    auto *use = worklist.pop_back_val();
    auto *user = use->getUser();
    if (isa<StrongReleaseInst>(user) || isa<DestroyValueInst>(user)) {
      // If this is not the only release of the box then bail out.
      if (singleRelease || foundSingleConsumingUse)
        return nullptr;
      singleRelease = user;
      foundSingleConsumingUse = true;
      continue;
    }

    if (!Builder.hasOwnership())
      continue;

    if (auto *cvi = dyn_cast<CopyValueInst>(user)) {
      for (auto *use : cvi->getUses())
        worklist.push_back(use);
      continue;
    }

    if (use->isConsumingUse()) {
      if (foundSingleConsumingUse)
        return nullptr;
      foundSingleConsumingUse = true;
    }
  }
  if (!singleRelease)
    return nullptr;

  // Release the value that was stored into the existential box. The box
  // is going away so we need to release the stored value.
  // NOTE: It's important that the release is inserted at the single
  // release of the box and not at the store, because a balancing retain could
  // be _after_ the store, e.g:
  //      %box = alloc_existential_box
  //      %addr = project_existential_box %box
  //      store %value to %addr
  //      retain_value %value    // must insert the release after this retain
  //      strong_release %box
  Builder.setInsertionPoint(singleRelease);
  Builder.emitDestroyValueOperation(AEBI->getLoc(), boxedValue);

  eraseInstIncludingUsers(AEBI);
  return nullptr;
}

/// Return the enum case injected by an inject_enum_addr if it is the only
/// instruction which writes to \p Addr.
static EnumElementDecl *getInjectEnumCaseTo(SILValue Addr) {
  while (true) {
    // For everything else than an alloc_stack we cannot easily prove that we
    // see all writes.
    if (!isa<AllocStackInst>(Addr))
      return nullptr;

    SILInstruction *WritingInst = nullptr;
    int NumWrites = 0;
    for (auto *Use : getNonDebugUses(Addr)) {
      SILInstruction *User = Use->getUser();
      switch (User->getKind()) {
        // Handle a very narrow set of known not harmful instructions.
        case swift::SILInstructionKind::DestroyAddrInst:
        case swift::SILInstructionKind::DeallocStackInst:
        case swift::SILInstructionKind::SwitchEnumAddrInst:
          break;
        case swift::SILInstructionKind::ApplyInst:
        case swift::SILInstructionKind::TryApplyInst: {
          // Check if the addr is only passed to in_guaranteed arguments.
          FullApplySite AI(User);
          for (Operand &Op : AI.getArgumentOperands()) {
            if (Op.get() == Addr &&
                AI.getArgumentConvention(Op) !=
                  SILArgumentConvention::Indirect_In_Guaranteed)
              return nullptr;
          }
          break;
        }
        case swift::SILInstructionKind::InjectEnumAddrInst:
          WritingInst = User;
          ++NumWrites;
          break;
        case swift::SILInstructionKind::CopyAddrInst:
          if (Addr == cast<CopyAddrInst>(User)->getDest()) {
            WritingInst = User;
            ++NumWrites;
          }
          break;
        default:
          return nullptr;
      }
    }
    if (NumWrites != 1)
      return nullptr;
    if (auto *IEA = dyn_cast<InjectEnumAddrInst>(WritingInst))
      return IEA->getElement();

    // In case of a copy_addr continue with the source of the copy.
    Addr = dyn_cast<CopyAddrInst>(WritingInst)->getSrc();
  }
}

SILInstruction *SILCombiner::visitSwitchEnumAddrInst(SwitchEnumAddrInst *SEAI) {
  // Convert switch_enum_addr -> br
  // if the only thing which writes to the address is an inject_enum_addr.
  SILValue Addr = SEAI->getOperand();
  if (EnumElementDecl *EnumCase = getInjectEnumCaseTo(Addr)) {
    SILBasicBlock *Dest = SEAI->getCaseDestination(EnumCase);
    // If the only instruction which writes to Addr is an inject_enum_addr we
    // know that there cannot be an enum payload.
    assert(Dest->getNumArguments() == 0 &&
           "didn't expect a payload argument");
    Builder.createBranch(SEAI->getLoc(), Dest);
    return eraseInstFromFunction(*SEAI);
  }

  SILType Ty = Addr->getType();
  if (!Ty.isLoadable(*SEAI->getFunction()))
    return nullptr;

  // Promote switch_enum_addr to switch_enum if the enum is loadable.
  //   switch_enum_addr %ptr : $*Optional<SomeClass>, case ...
  //     ->
  //   %value = load %ptr
  //   switch_enum %value
  //
  // If we are using ownership, we perform a load_borrow right before the new
  // switch_enum and end the borrow scope right afterwards.
  Builder.setCurrentDebugScope(SEAI->getDebugScope());
  SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 8> Cases;
  for (int i : range(SEAI->getNumCases())) {
    Cases.push_back(SEAI->getCase(i));
  }

  SILBasicBlock *Default = SEAI->hasDefault() ? SEAI->getDefaultBB() : nullptr;
  SILValue EnumVal = Builder.emitLoadBorrowOperation(SEAI->getLoc(), Addr);
  auto *sei = Builder.createSwitchEnum(SEAI->getLoc(), EnumVal, Default, Cases);

  if (Builder.hasOwnership()) {
    for (int i : range(sei->getNumCases())) {
      auto c = sei->getCase(i);
      if (c.first->hasAssociatedValues()) {
        auto eltType = Addr->getType().getEnumElementType(
            c.first, Builder.getModule(), Builder.getTypeExpansionContext());
        eltType = eltType.getObjectType();
        if (eltType.isTrivial(Builder.getFunction())) {
          c.second->createPhiArgument(eltType, ValueOwnershipKind::None);
        } else {
          c.second->createPhiArgument(eltType, ValueOwnershipKind::Guaranteed);
        }
      }
      Builder.setInsertionPoint(c.second->front().getIterator());
      Builder.emitEndBorrowOperation(SEAI->getLoc(), EnumVal);
    }

    if (auto defaultBlock = sei->getDefaultBBOrNull()) {
      defaultBlock.get()->createPhiArgument(EnumVal->getType(),
                                            ValueOwnershipKind::Guaranteed);
      Builder.setInsertionPoint(defaultBlock.get()->front().getIterator());
      Builder.emitEndBorrowOperation(SEAI->getLoc(), EnumVal);
    }
  }

  return eraseInstFromFunction(*SEAI);
}

SILInstruction *SILCombiner::visitSelectEnumAddrInst(SelectEnumAddrInst *seai) {
  // Canonicalize a select_enum_addr: if the default refers to exactly one case,
  // then replace the default with that case.
  Builder.setCurrentDebugScope(seai->getDebugScope());
  if (seai->hasDefault()) {
    NullablePtr<EnumElementDecl> elementDecl = seai->getUniqueCaseForDefault();
    if (elementDecl.isNonNull()) {
      // Construct a new instruction by copying all the case entries.
      SmallVector<std::pair<EnumElementDecl *, SILValue>, 4> caseValues;
      for (int idx = 0, numIdcs = seai->getNumCases(); idx < numIdcs; ++idx) {
        caseValues.push_back(seai->getCase(idx));
      }
      // Add the default-entry of the original instruction as case-entry.
      caseValues.push_back(
          std::make_pair(elementDecl.get(), seai->getDefaultResult()));

      return Builder.createSelectEnumAddr(
          seai->getLoc(), seai->getEnumOperand(), seai->getType(), SILValue(),
          caseValues);
    }
  }

  // Promote select_enum_addr to select_enum if the enum is loadable.
  //   = select_enum_addr %ptr : $*Optional<SomeClass>, case ...
  //     ->
  //   %value = load %ptr
  //   = select_enum %value
  SILType ty = seai->getEnumOperand()->getType();
  if (!ty.isLoadable(*seai->getFunction()))
    return nullptr;

  SmallVector<std::pair<EnumElementDecl *, SILValue>, 8> cases;
  for (int i = 0, e = seai->getNumCases(); i < e; ++i)
    cases.push_back(seai->getCase(i));

  SILValue defaultCase =
      seai->hasDefault() ? seai->getDefaultResult() : SILValue();
  auto enumVal =
      Builder.emitLoadBorrowOperation(seai->getLoc(), seai->getEnumOperand());
  auto *result = Builder.createSelectEnum(seai->getLoc(), enumVal,
                                          seai->getType(), defaultCase, cases);
  Builder.emitEndBorrowOperation(seai->getLoc(), enumVal);
  replaceInstUsesWith(*seai, result);
  return eraseInstFromFunction(*seai);
}

SILInstruction *SILCombiner::visitSwitchValueInst(SwitchValueInst *svi) {
  SILValue cond = svi->getOperand();
  BuiltinIntegerType *condTy = cond->getType().getAs<BuiltinIntegerType>();
  if (!condTy || !condTy->isFixedWidth(1))
    return nullptr;

  SILBasicBlock *falseBB = nullptr;
  SILBasicBlock *trueBB = nullptr;
  for (unsigned idx : range(svi->getNumCases())) {
    auto switchCase = svi->getCase(idx);
    auto *caseVal = dyn_cast<IntegerLiteralInst>(switchCase.first);
    if (!caseVal)
      return nullptr;
    SILBasicBlock *destBB = switchCase.second;
    assert(destBB->args_empty() &&
           "switch_value case destination cannot take arguments");
    if (caseVal->getValue() == 0) {
      assert(!falseBB && "double case value 0 in switch_value");
      falseBB = destBB;
    } else {
      assert(!trueBB && "double case value 1 in switch_value");
      trueBB = destBB;
    }
  }

  if (svi->hasDefault()) {
    assert(svi->getDefaultBB()->args_empty() &&
           "switch_value default destination cannot take arguments");
    if (!falseBB) {
      falseBB = svi->getDefaultBB();
    } else if (!trueBB) {
      trueBB = svi->getDefaultBB();
    }
  }

  if (!falseBB || !trueBB)
    return nullptr;

  Builder.setCurrentDebugScope(svi->getDebugScope());
  return Builder.createCondBranch(svi->getLoc(), cond, trueBB, falseBB);
}

namespace {

/// A SILInstruction visitor that analyzes alloc stack values for dead live
/// range and promotion opportunities.
///
/// init_existential_addr instructions behave like memory allocation within the
/// allocated object. We can promote the init_existential_addr allocation into a
/// dedicated allocation.
///
/// We detect this pattern
/// %0 = alloc_stack $LogicValue
/// %1 = init_existential_addr %0 : $*LogicValue, $*Bool
/// ...
/// use of %1
/// ...
/// destroy_addr %0 : $*LogicValue
/// dealloc_stack %0 : $*LogicValue
///
/// At the same we time also look for dead alloc_stack live ranges that are only
/// copied into.
///
/// %0 = alloc_stack
/// copy_addr %src, %0
/// destroy_addr %0 : $*LogicValue
/// dealloc_stack %0 : $*LogicValue
struct AllocStackAnalyzer : SILInstructionVisitor<AllocStackAnalyzer> {
  /// The alloc_stack that we are analyzing.
  AllocStackInst *ASI;

  /// Do all of the users of the alloc stack allow us to perform optimizations.
  bool LegalUsers = true;

  /// If we saw an init_existential_addr in the use list of the alloc_stack,
  /// this is the init_existential_addr. We are conservative in the face of
  /// having multiple init_existential_addr. In such a case, we say that the use
  /// list of the alloc_stack does not allow for optimizations to occur.
  InitExistentialAddrInst *IEI = nullptr;

  /// If we saw an open_existential_addr in the use list of the alloc_stack,
  /// this is the open_existential_addr. We are conservative in the case of
  /// multiple open_existential_addr. In such a case, we say that the use list
  /// of the alloc_stack does not allow for optimizations to occur.
  OpenExistentialAddrInst *OEI = nullptr;

  /// Did we see any copies into the alloc stack.
  bool HaveSeenCopyInto = false;

public:
  AllocStackAnalyzer(AllocStackInst *ASI) : ASI(ASI) {}

  /// Analyze the alloc_stack instruction's uses.
  void analyze() {
    // Scan all of the uses of the AllocStack and check if it is not used for
    // anything other than the init_existential_addr/open_existential_addr
    // container.

    for (auto *Op : getNonDebugUses(ASI)) {
      visit(Op->getUser());

      // If we found a non-legal user, bail early.
      if (!LegalUsers)
        break;
    }
  }

  /// Given an unhandled case, we have an illegal use for our optimization
  /// purposes. Set LegalUsers to false and return.
  void visitSILInstruction(SILInstruction *I) { LegalUsers = false; }

  // Destroy and dealloc are both fine.
  void visitDestroyAddrInst(DestroyAddrInst *I) {}
  void visitDeinitExistentialAddrInst(DeinitExistentialAddrInst *I) {}
  void visitDeallocStackInst(DeallocStackInst *I) {}

  void visitInitExistentialAddrInst(InitExistentialAddrInst *I) {
    // If we have already seen an init_existential_addr, we cannot
    // optimize. This is because we only handle the single init_existential_addr
    // case.
    if (IEI || HaveSeenCopyInto) {
      LegalUsers = false;
      return;
    }
    IEI = I;
  }

  void visitOpenExistentialAddrInst(OpenExistentialAddrInst *I) {
    // If we have already seen an open_existential_addr, we cannot
    // optimize. This is because we only handle the single open_existential_addr
    // case.
    if (OEI) {
      LegalUsers = false;
      return;
    }

    // Make sure that the open_existential does not have any uses except
    // destroy_addr.
    for (auto *Use : getNonDebugUses(I)) {
      if (!isa<DestroyAddrInst>(Use->getUser())) {
        LegalUsers = false;
        return;
      }
    }

    OEI = I;
  }

  void visitCopyAddrInst(CopyAddrInst *I) {
    if (IEI) {
      LegalUsers = false;
      return;
    }

    // Copies into the alloc_stack live range are safe.
    if (I->getDest() == ASI) {
      HaveSeenCopyInto = true;
      return;
    }

    LegalUsers = false;
  }
};

} // end anonymous namespace

/// Returns true if there is a retain instruction between \p from and the
/// destroy or deallocation of \p alloc.
static bool somethingIsRetained(SILInstruction *from, AllocStackInst *alloc) {
  llvm::SmallVector<SILInstruction *, 8> workList;
  llvm::SmallPtrSet<SILBasicBlock *, 8> handled;
  workList.push_back(from);
  while (!workList.empty()) {
    SILInstruction *start = workList.pop_back_val();
    for (auto iter = start->getIterator(), end = start->getParent()->end();
         iter != end;
         ++iter) {
      SILInstruction *inst = &*iter;
      if (isa<RetainValueInst>(inst) || isa<StrongRetainInst>(inst) ||
          isa<CopyValueInst>(inst)) {
        return true;
      }
      if ((isa<DeallocStackInst>(inst) || isa<DestroyAddrInst>(inst)) &&
          inst->getOperand(0) == alloc) {
        break;
      }
      if (isa<TermInst>(inst)) {
        for (SILBasicBlock *succ : start->getParent()->getSuccessors()) {
          if (handled.insert(succ).second)
            workList.push_back(&*succ->begin());
        }
      }
    }
  }
  return false;
}

/// Replaces an alloc_stack of an enum by an alloc_stack of the payload if only
/// one enum case (with payload) is stored to that location.
///
/// For example:
///
///   %loc = alloc_stack $Optional<T>
///   %payload = init_enum_data_addr %loc
///   store %value to %payload
///   ...
///   %take_addr = unchecked_take_enum_data_addr %loc
///   %l = load %take_addr
///
/// is transformed to
///
///   %loc = alloc_stack $T
///   store %value to %loc
///   ...
///   %l = load %loc
bool SILCombiner::optimizeStackAllocatedEnum(AllocStackInst *asi) {
  EnumDecl *enumDecl = asi->getType().getEnumOrBoundGenericEnum();
  if (!enumDecl)
    return false;
  
  EnumElementDecl *element = nullptr;
  unsigned numInits =0;
  unsigned numTakes = 0;
  SILBasicBlock *initBlock = nullptr;
  SILBasicBlock *takeBlock = nullptr;
  SILType payloadType;
  
  // First step: check if the stack location is only used to hold one specific
  // enum case with payload.
  for (auto *use : asi->getUses()) {
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
    case SILInstructionKind::DebugValueAddrInst:
    case SILInstructionKind::DestroyAddrInst:
    case SILInstructionKind::DeallocStackInst:
    case SILInstructionKind::InjectEnumAddrInst:
      // We'll check init_enum_addr below.
      break;
    case SILInstructionKind::InitEnumDataAddrInst: {
      auto *ieda = cast<InitEnumDataAddrInst>(user);
      auto *el = ieda->getElement();
      if (element && el != element)
        return false;
      element = el;
      assert(!payloadType || payloadType == ieda->getType());
      payloadType = ieda->getType();
      numInits++;
      initBlock = user->getParent();
      break;
    }
    case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
      auto *el = cast<UncheckedTakeEnumDataAddrInst>(user)->getElement();
      if (element && el != element)
        return false;
      element = el;
      numTakes++;
      takeBlock = user->getParent();
      break;
    }
    default:
      return false;
    }
  }
  if (!element || !payloadType)
    return false;

  // If the enum has a single init-take pair in a single block, we know that
  // the enum cannot contain any valid payload outside that init-take pair.
  //
  // This also means that we can ignore any inject_enum_addr of another enum
  // case, because this can only inject a case without a payload.
  bool singleInitTakePair =
    (numInits == 1 && numTakes == 1 && initBlock == takeBlock);
  if (!singleInitTakePair) {
    // No single init-take pair: We cannot ignore inject_enum_addrs with a
    // mismatching case.
    for (auto *use : asi->getUses()) {
      if (auto *inject = dyn_cast<InjectEnumAddrInst>(use->getUser())) {
        if (inject->getElement() != element)
          return false;
      }
    }
  }

  // Second step: replace the enum alloc_stack with a payload alloc_stack.
  auto *newAlloc = Builder.createAllocStack(
      asi->getLoc(), payloadType, asi->getVarInfo(), asi->hasDynamicLifetime());

  while (!asi->use_empty()) {
    Operand *use = *asi->use_begin();
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
    case SILInstructionKind::InjectEnumAddrInst:
    case SILInstructionKind::DebugValueAddrInst:
      eraseInstFromFunction(*user);
      break;
    case SILInstructionKind::DestroyAddrInst:
      if (singleInitTakePair) {
        // It's not possible that the enum has a payload at the destroy_addr,
        // because it must have already been taken by the take of the
        // single init-take pair.
        // We _have_ to remove the destroy_addr, because we also remove all
        // inject_enum_addrs which might inject a payload-less case before
        // the destroy_addr.
        eraseInstFromFunction(*user);
      } else {
        // The enum payload can still be valid at the destroy_addr, so we have
        // to keep the destroy_addr. Just replace the enum with the payload
        // (and because it's not a singleInitTakePair, we can be sure that the
        // enum cannot have any other case than the payload case).
        use->set(newAlloc);
      }
      break;
    case SILInstructionKind::DeallocStackInst:
      use->set(newAlloc);
      break;
    case SILInstructionKind::InitEnumDataAddrInst:
    case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
      auto *svi = cast<SingleValueInstruction>(user);
      svi->replaceAllUsesWith(newAlloc);
      eraseInstFromFunction(*svi);
      break;
    }
    default:
      llvm_unreachable("unexpected alloc_stack user");
    }
  }
  return true;
}

SILInstruction *SILCombiner::visitAllocStackInst(AllocStackInst *AS) {
  if (optimizeStackAllocatedEnum(AS))
    return nullptr;

  // If we are testing SILCombine and we are asked not to eliminate
  // alloc_stacks, just return.
  if (DisableAllocStackOpts)
    return nullptr;

  AllocStackAnalyzer Analyzer(AS);
  Analyzer.analyze();

  // If when analyzing, we found a user that makes our optimization, illegal,
  // bail early.
  if (!Analyzer.LegalUsers)
    return nullptr;

  InitExistentialAddrInst *IEI = Analyzer.IEI;
  OpenExistentialAddrInst *OEI = Analyzer.OEI;

  // If the only users of the alloc_stack are alloc, destroy and
  // init_existential_addr then we can promote the allocation of the init
  // existential.
  // Be careful with open archetypes, because they cannot be moved before
  // their definitions.
  if (IEI && !OEI &&
      !IEI->getLoweredConcreteType().hasOpenedExistential()) {
    assert(!IEI->getLoweredConcreteType().isOpenedExistential());
    auto *ConcAlloc = Builder.createAllocStack(
        AS->getLoc(), IEI->getLoweredConcreteType(), AS->getVarInfo());
    IEI->replaceAllUsesWith(ConcAlloc);
    eraseInstFromFunction(*IEI);

    for (auto UI = AS->use_begin(), UE = AS->use_end(); UI != UE;) {
      auto *Op = *UI;
      ++UI;
      if (auto *DA = dyn_cast<DestroyAddrInst>(Op->getUser())) {
        Builder.setInsertionPoint(DA);
        Builder.createDestroyAddr(DA->getLoc(), ConcAlloc);
        eraseInstFromFunction(*DA);
        continue;
      }

      if (!isa<DeallocStackInst>(Op->getUser()))
        continue;

      auto *DS = cast<DeallocStackInst>(Op->getUser());
      Builder.setInsertionPoint(DS);
      Builder.createDeallocStack(DS->getLoc(), ConcAlloc);
      eraseInstFromFunction(*DS);
    }

    return eraseInstFromFunction(*AS);
  }

  // If we have a live 'live range' or a live range that we have not sen a copy
  // into, bail.
  if (!Analyzer.HaveSeenCopyInto || IEI)
    return nullptr;

  // Otherwise remove the dead live range that is only copied into.
  //
  // TODO: Do we not remove purely dead live ranges here? Seems like we should.
  SmallPtrSet<SILInstruction *, 16> ToDelete;
  SmallVector<CopyAddrInst *, 4> takingCopies;

  for (auto *Op : AS->getUses()) {
    // Replace a copy_addr [take] %src ... by a destroy_addr %src if %src is
    // no the alloc_stack.
    // Otherwise, just delete the copy_addr.
    if (auto *CopyAddr = dyn_cast<CopyAddrInst>(Op->getUser())) {
      if (CopyAddr->isTakeOfSrc() && CopyAddr->getSrc() != AS) {
        takingCopies.push_back(CopyAddr);
      }
    }

    if (auto *OEAI = dyn_cast<OpenExistentialAddrInst>(Op->getUser())) {
      for (auto *Op : OEAI->getUses()) {
        assert(isa<DestroyAddrInst>(Op->getUser()) ||
               Op->getUser()->isDebugInstruction() && "Unexpected instruction");
        ToDelete.insert(Op->getUser());
      }
    }

    assert(isa<CopyAddrInst>(Op->getUser()) ||
           isa<OpenExistentialAddrInst>(Op->getUser()) ||
           isa<DestroyAddrInst>(Op->getUser()) ||
           isa<DeallocStackInst>(Op->getUser()) ||
           isa<DeinitExistentialAddrInst>(Op->getUser()) ||
           Op->getUser()->isDebugInstruction() && "Unexpected instruction");
    ToDelete.insert(Op->getUser());
  }

  // Check if a retain is moved after the copy_addr. If the retained object
  // happens to be the source of the copy_addr it might be only kept alive by
  // the stack location. This cannot happen with OSSA.
  // TODO: remove this check once we have OSSA.
  for (CopyAddrInst *copy : takingCopies) {
    if (somethingIsRetained(copy, AS))
      return nullptr;
  }

  for (CopyAddrInst *copy : takingCopies) {
    SILBuilderWithScope destroyBuilder(copy, Builder.getBuilderContext());
    destroyBuilder.createDestroyAddr(copy->getLoc(), copy->getSrc());
  }

  // Erase the 'live-range'
  for (auto *Inst : ToDelete) {
    Inst->replaceAllUsesOfAllResultsWithUndef();
    eraseInstFromFunction(*Inst);
  }
  return eraseInstFromFunction(*AS);
}

SILInstruction *SILCombiner::visitAllocRefInst(AllocRefInst *AR) {
  // Check if the only uses are deallocating stack or deallocating.
  SmallPtrSet<SILInstruction *, 16> ToDelete;
  bool HasNonRemovableUses = false;
  for (auto UI = AR->use_begin(), UE = AR->use_end(); UI != UE;) {
    auto *Op = *UI;
    ++UI;
    auto *User = Op->getUser();
    if (!isa<DeallocRefInst>(User) && !isa<SetDeallocatingInst>(User) &&
        !isa<FixLifetimeInst>(User)) {
      HasNonRemovableUses = true;
      break;
    }
    ToDelete.insert(User);
  }

  if (HasNonRemovableUses)
    return nullptr;

  // Remove the instruction and all its uses.
  for (auto *I : ToDelete)
    eraseInstFromFunction(*I);
  eraseInstFromFunction(*AR);
  return nullptr;
}

/// Returns the base address if \p val is an index_addr with constant index.
static SILValue isConstIndexAddr(SILValue val, unsigned &index) {
  auto *IA = dyn_cast<IndexAddrInst>(val);
  if (!IA)
    return nullptr;
  auto *Index = dyn_cast<IntegerLiteralInst>(IA->getIndex());

  // Limiting to 32 bits is more than enough. The reason why not limiting to 64
  // bits is to leave room for overflow when we add two indices.
  if (!Index || Index->getValue().getActiveBits() > 32)
    return nullptr;

  index = Index->getValue().getZExtValue();
  return IA->getBase();
}

/// Optimize loading bytes from a string literal.
/// Example in SIL pseudo code:
///     %0 = string_literal "abc"
///     %1 = integer_literal 2
///     %2 = index_addr %0, %1
///     %3 = load %2
/// ->
///     %3 = integer_literal 'c'
SILInstruction *
SILCombiner::optimizeLoadFromStringLiteral(SingleValueInstruction *LI) {
  assert(isa<LoadInst>(LI) || isa<LoadBorrowInst>(LI));
  auto *SEA = dyn_cast<StructElementAddrInst>(LI->getOperand(0));
  if (!SEA)
    return nullptr;

  SILValue addr = SEA->getOperand();
  unsigned index = 0;
  if (SILValue iaBase = isConstIndexAddr(addr, index))
    addr = iaBase;

  auto *PTA = dyn_cast<PointerToAddressInst>(addr);
  if (!PTA)
    return nullptr;
  auto *Literal = dyn_cast<StringLiteralInst>(PTA->getOperand());
  if (!Literal || Literal->getEncoding() != StringLiteralInst::Encoding::UTF8)
    return nullptr;

  BuiltinIntegerType *BIType = LI->getType().getAs<BuiltinIntegerType>();
  if (!BIType || !BIType->isFixedWidth(8))
    return nullptr;

  StringRef str = Literal->getValue();
  if (index >= str.size())
    return nullptr;

  return Builder.createIntegerLiteral(LI->getLoc(), LI->getType(), str[index]);
}

/// Returns true if \p LI loads a zero integer from the empty Array, Dictionary
/// or Set singleton.
static bool isZeroLoadFromEmptyCollection(SingleValueInstruction *LI) {
  assert(isa<LoadInst>(LI) || isa<LoadBorrowInst>(LI));
  auto intTy = LI->getType().getAs<BuiltinIntegerType>();
  if (!intTy)
    return false;

  SILValue addr = LI->getOperand(0);

  // Find the root object of the load-address.
  for (;;) {
    switch (addr->getKind()) {
      case ValueKind::GlobalAddrInst: {
        StringRef gName =
          cast<GlobalAddrInst>(addr)->getReferencedGlobal()->getName();
        return gName == "_swiftEmptyArrayStorage" ||
               gName == "_swiftEmptyDictionarySingleton" ||
               gName == "_swiftEmptySetSingleton";
      }
      case ValueKind::StructElementAddrInst: {
        auto *SEA = cast<StructElementAddrInst>(addr);
        // For Array, we only support "count". The value of "capacityAndFlags"
        // is not defined in the ABI and could change in another version of the
        // runtime (the capacity must be 0, but the flags may be not 0).
        if (SEA->getStructDecl()->getName().is("_SwiftArrayBodyStorage") &&
            !SEA->getField()->getName().is("count")) {
          return false;
        }
        addr = SEA->getOperand();
        break;
      }
      case ValueKind::RefElementAddrInst: {
        auto *REA = cast<RefElementAddrInst>(addr);
        Identifier className = REA->getClassDecl()->getName();
        // For Dictionary and Set we support "count" and "capacity".
        if (className.is("__RawDictionaryStorage") ||
            className.is("__RawSetStorage")) {
          Identifier fieldName = REA->getField()->getName();
          if (!fieldName.is("_count") && !fieldName.is("_capacity"))
            return false;
        }
        addr = REA->getOperand();
        break;
      }
      case ValueKind::UncheckedRefCastInst:
      case ValueKind::UpcastInst:
      case ValueKind::RawPointerToRefInst:
      case ValueKind::AddressToPointerInst:
      case ValueKind::BeginBorrowInst:
      case ValueKind::CopyValueInst:
      case ValueKind::EndCOWMutationInst:
        addr = cast<SingleValueInstruction>(addr)->getOperand(0);
        break;
      default:
        return false;
    }
  }
}

static SingleValueInstruction *getValueFromStaticLet(SILValue v) {
  if (auto *globalAddr = dyn_cast<GlobalAddrInst>(v)) {
    SILGlobalVariable *global = globalAddr->getReferencedGlobal();
    if (!global->isLet())
      return nullptr;
    return dyn_cast_or_null<SingleValueInstruction>(
             global->getStaticInitializerValue());
  }
  if (auto *seai = dyn_cast<StructElementAddrInst>(v)) {
    auto *structVal = getValueFromStaticLet(seai->getOperand());
    if (!structVal)
      return nullptr;
    return cast<SingleValueInstruction>(
      cast<StructInst>(structVal)->getOperandForField(seai->getField())->get());
  }
  if (auto *teai = dyn_cast<TupleElementAddrInst>(v)) {
    auto *tupleVal = getValueFromStaticLet(teai->getOperand());
    if (!tupleVal)
      return nullptr;
    return cast<SingleValueInstruction>(
      cast<TupleInst>(tupleVal)->getElement(teai->getFieldIndex()));
  }
  return nullptr;
}

SILInstruction *SILCombiner::visitLoadBorrowInst(LoadBorrowInst *lbi) {
  // (load (upcast-ptr %x)) -> (upcast-ref (load %x))
  Builder.setCurrentDebugScope(lbi->getDebugScope());
  if (auto *UI = dyn_cast<UpcastInst>(lbi->getOperand())) {
    auto newLBI = Builder.createLoadBorrow(lbi->getLoc(), UI->getOperand());
    return Builder.createUpcast(lbi->getLoc(), newLBI, lbi->getType());
  }

  if (SILInstruction *I = optimizeLoadFromStringLiteral(lbi))
    return I;

  // Constant-propagate the 0 value when loading "count" or "capacity" from the
  // empty Array, Set or Dictionary storage.
  // On high-level SIL this optimization is also done by the
  // ArrayCountPropagation pass, but only for Array. And even for Array it's
  // sometimes needed to propagate the empty-array count when high-level
  // semantics function are already inlined.
  // Note that for non-empty arrays/sets/dictionaries, the count can be
  // propagated by redundant load elimination.
  if (isZeroLoadFromEmptyCollection(lbi))
    return Builder.createIntegerLiteral(lbi->getLoc(), lbi->getType(), 0);

  // Propagate a value from a static "let" global variable.
  // This optimization is also done by GlobalOpt, but not with de-serialized
  // globals, which can occur with cross-module optimization.
  if (SingleValueInstruction *initVal =
          getValueFromStaticLet(lbi->getOperand())) {
    StaticInitCloner cloner(lbi);
    cloner.add(initVal);
    return cloner.clone(initVal);
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitLoadInst(LoadInst *LI) {
  // (load (upcast-ptr %x)) -> (upcast-ref (load %x))
  Builder.setCurrentDebugScope(LI->getDebugScope());
  if (auto *UI = dyn_cast<UpcastInst>(LI->getOperand())) {
    auto NewLI = Builder.emitLoadValueOperation(LI->getLoc(), UI->getOperand(),
                                                LI->getOwnershipQualifier());
    return Builder.createUpcast(LI->getLoc(), NewLI, LI->getType());
  }

  if (SILInstruction *I = optimizeLoadFromStringLiteral(LI))
    return I;

  // Constant-propagate the 0 value when loading "count" or "capacity" from the
  // empty Array, Set or Dictionary storage.
  // On high-level SIL this optimization is also done by the
  // ArrayCountPropagation pass, but only for Array. And even for Array it's
  // sometimes needed to propagate the empty-array count when high-level
  // semantics function are already inlined.
  // Note that for non-empty arrays/sets/dictionaries, the count can be
  // propagated by redundant load elimination.
  if (isZeroLoadFromEmptyCollection(LI))
    return Builder.createIntegerLiteral(LI->getLoc(), LI->getType(), 0);

  // Propagate a value from a static "let" global variable.
  // This optimization is also done by GlobalOpt, but not with de-serialized
  // globals, which can occur with cross-module optimization.
  if (SingleValueInstruction *initVal = getValueFromStaticLet(LI->getOperand())) {
    StaticInitCloner cloner(LI);
    cloner.add(initVal);
    return cloner.clone(initVal);
  }

  // If we have a load [copy] whose only non-debug users are destroy_value, just
  // eliminate it.
  if (LI->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
    if (llvm::all_of(getNonDebugUses(LI), [](Operand *use) {
          return isa<DestroyValueInst>(use->getUser());
        })) {
      eraseInstIncludingUsers(LI);
      return nullptr;
    }
  }

  return nullptr;
}

/// Optimize nested index_addr instructions:
/// Example in SIL pseudo code:
///    %1 = index_addr %ptr, x
///    %2 = index_addr %1, y
/// ->
///    %2 = index_addr %ptr, x+y
SILInstruction *SILCombiner::visitIndexAddrInst(IndexAddrInst *IA) {
  unsigned index = 0;
  SILValue base = isConstIndexAddr(IA, index);
  if (!base)
    return nullptr;

  unsigned index2 = 0;
  SILValue base2 = isConstIndexAddr(base, index2);
  if (!base2)
    return nullptr;

  auto *newIndex = Builder.createIntegerLiteral(IA->getLoc(),
                                    IA->getIndex()->getType(), index + index2);
  return Builder.createIndexAddr(IA->getLoc(), base2, newIndex);
}

SILInstruction *SILCombiner::visitReleaseValueInst(ReleaseValueInst *RVI) {
  assert(!RVI->getFunction()->hasOwnership());

  SILValue Operand = RVI->getOperand();
  SILType OperandTy = Operand->getType();

  // Destroy value of an enum with a trivial payload or no-payload is a no-op.
  if (auto *EI = dyn_cast<EnumInst>(Operand)) {
    if (!EI->hasOperand() ||
        EI->getOperand()->getType().isTrivial(*EI->getFunction()))
      return eraseInstFromFunction(*RVI);

    // retain_value of an enum_inst where we know that it has a payload can be
    // reduced to a retain_value on the payload.
    if (EI->hasOperand()) {
      return Builder.createReleaseValue(RVI->getLoc(), EI->getOperand(),
                                        RVI->getAtomicity());
    }
  }

  // ReleaseValueInst of a loadable reference storage type needs the
  // corresponding release instruction.
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  if (OperandTy.is<Name##StorageType>())                                       \
    return Builder.create##Name##Release(RVI->getLoc(), Operand,               \
                                        RVI->getAtomicity());
#include "swift/AST/ReferenceStorage.def"

  // ReleaseValueInst of a reference type is a strong_release.
  if (OperandTy.isReferenceCounted(RVI->getModule()))
    return Builder.createStrongRelease(RVI->getLoc(), Operand,
                                       RVI->getAtomicity());

  // ReleaseValueInst of a trivial type is a no-op.
  if (OperandTy.isTrivial(*RVI->getFunction()))
    return eraseInstFromFunction(*RVI);

  // Do nothing for non-trivial non-reference types.
  return nullptr;
}

SILInstruction *SILCombiner::visitRetainValueInst(RetainValueInst *RVI) {
  assert(!RVI->getFunction()->hasOwnership());

  SILValue Operand = RVI->getOperand();
  SILType OperandTy = Operand->getType();

  // retain_value of an enum with a trivial payload or no-payload is a no-op +
  // RAUW.
  if (auto *EI = dyn_cast<EnumInst>(Operand)) {
    if (!EI->hasOperand() ||
        EI->getOperand()->getType().isTrivial(*RVI->getFunction())) {
      return eraseInstFromFunction(*RVI);
    }

    // retain_value of an enum_inst where we know that it has a payload can be
    // reduced to a retain_value on the payload.
    if (EI->hasOperand()) {
      return Builder.createRetainValue(RVI->getLoc(), EI->getOperand(),
                                       RVI->getAtomicity());
    }
  }

  // RetainValueInst of a loadable reference storage type needs the
  // corresponding retain instruction.
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  if (OperandTy.is<Name##StorageType>())                                       \
    return Builder.create##Name##Retain(RVI->getLoc(), Operand,                \
                                       RVI->getAtomicity());
#include "swift/AST/ReferenceStorage.def"

  // RetainValueInst of a reference type is a strong_release.
  if (OperandTy.isReferenceCounted(RVI->getModule())) {
    return Builder.createStrongRetain(RVI->getLoc(), Operand,
                                      RVI->getAtomicity());
  }

  // RetainValueInst of a trivial type is a no-op + use propagation.
  if (OperandTy.isTrivial(*RVI->getFunction())) {
    return eraseInstFromFunction(*RVI);
  }

  // Sometimes in the stdlib due to hand offs, we will see code like:
  //
  // release_value %0
  // retain_value %0
  //
  // with the matching retain_value to the release_value in a predecessor basic
  // block and the matching release_value for the retain_value_retain in a
  // successor basic block.
  //
  // Due to the matching pairs being in different basic blocks, the ARC
  // Optimizer (which is currently local to one basic block does not handle
  // it). But that does not mean that we cannot eliminate this pair with a
  // peephole.

  // If we are not the first instruction in this basic block...
  if (RVI != &*RVI->getParent()->begin()) {
    SILBasicBlock::iterator Pred = std::prev(RVI->getIterator());

    // ...and the predecessor instruction is a release_value on the same value
    // as our retain_value...
    if (auto *Release = dyn_cast<ReleaseValueInst>(&*Pred))
      // Remove them...
      if (Release->getOperand() == RVI->getOperand()) {
        eraseInstFromFunction(*Release);
        return eraseInstFromFunction(*RVI);
      }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitCondFailInst(CondFailInst *CFI) {
  // Remove runtime asserts such as overflow checks and bounds checks.
  if (RemoveCondFails)
    return eraseInstFromFunction(*CFI);

  auto *I = dyn_cast<IntegerLiteralInst>(CFI->getOperand());
  if (!I)
    return nullptr;

  // Erase. (cond_fail 0)
  if (!I->getValue().getBoolValue())
    return eraseInstFromFunction(*CFI);

  // Remove any code that follows a (cond_fail 1) and set the block's
  // terminator to unreachable.

  // Nothing more to do here
  if (isa<UnreachableInst>(std::next(SILBasicBlock::iterator(CFI))))
    return nullptr;

  // Collect together all the instructions after this point
  llvm::SmallVector<SILInstruction *, 32> ToRemove;
  for (auto Inst = CFI->getParent()->rbegin(); &*Inst != CFI; ++Inst)
    ToRemove.push_back(&*Inst);

  for (auto *Inst : ToRemove) {
    // Replace any still-remaining uses with undef and erase.
    Inst->replaceAllUsesOfAllResultsWithUndef();
    eraseInstFromFunction(*Inst);
  }

  // Add an `unreachable` to be the new terminator for this block
  Builder.setInsertionPoint(CFI->getParent());
  Builder.createUnreachable(ArtificialUnreachableLocation());

  return nullptr;
}

SILInstruction *SILCombiner::visitStrongRetainInst(StrongRetainInst *SRI) {
  assert(!SRI->getFunction()->hasOwnership());

  // Retain of ThinToThickFunction is a no-op.
  SILValue funcOper = SRI->getOperand();
  if (auto *CFI = dyn_cast<ConvertFunctionInst>(funcOper))
    funcOper = CFI->getOperand();

  if (isa<ThinToThickFunctionInst>(funcOper))
    return eraseInstFromFunction(*SRI);

  if (isa<ObjCExistentialMetatypeToObjectInst>(SRI->getOperand()) ||
      isa<ObjCMetatypeToObjectInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  // Retain and Release of tagged strings is a no-op.
  // The builtin code pattern to find tagged strings is:
  // builtin "stringObjectOr_Int64" (or to tag the string)
  // value_to_bridge_object (cast the UInt to bridge object)
  if (isa<ValueToBridgeObjectInst>(SRI->getOperand())) {
    return eraseInstFromFunction(*SRI);
  }

  // Sometimes in the stdlib due to hand offs, we will see code like:
  //
  // strong_release %0
  // strong_retain %0
  //
  // with the matching strong_retain to the strong_release in a predecessor
  // basic block and the matching strong_release for the strong_retain in a
  // successor basic block.
  //
  // Due to the matching pairs being in different basic blocks, the ARC
  // Optimizer (which is currently local to one basic block does not handle
  // it). But that does not mean that we cannot eliminate this pair with a
  // peephole.

  // If we are not the first instruction in this basic block...
  if (SRI != &*SRI->getParent()->begin()) {
    auto Pred = std::prev(SRI->getIterator());

    // ...and the predecessor instruction is a strong_release on the same value
    // as our strong_retain...
    if (auto *Release = dyn_cast<StrongReleaseInst>(&*Pred))
      // Remove them...
      if (Release->getOperand() == SRI->getOperand()) {
        eraseInstFromFunction(*Release);
        return eraseInstFromFunction(*SRI);
      }
  }

  return nullptr;
}

/// Create a value from stores to an address.
///
/// If there are only stores to \p addr, return the stored value. Also, if there
/// are address projections, create aggregate instructions for it.
/// If builder is null, it's just a dry-run to check if it's possible.
static SILValue createValueFromAddr(SILValue addr, SILBuilder *builder,
                                    SILLocation loc) {
  SmallVector<SILValue, 4> elems;
  enum Kind {
    none, store, tuple
  } kind = none;

  for (Operand *use : addr->getUses()) {
    SILInstruction *user = use->getUser();
    if (user->isDebugInstruction())
      continue;

    auto *st = dyn_cast<StoreInst>(user);
    if (st) {
      // We do not support Assign today.
      if (st->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
        return SILValue();

      if (kind == none && st->getDest() == addr) {
        elems.push_back(st->getSrc());
        kind = store;
        // We cannot just return st->getSrc() here because we also have to check
        // if the store destination is the only use of addr.
        continue;
      }
    }

    if (auto *telem = dyn_cast<TupleElementAddrInst>(user)) {
      if (kind == none) {
        elems.resize(addr->getType().castTo<TupleType>()->getNumElements());
        kind = tuple;
      }
      if (kind == tuple) {
        if (elems[telem->getFieldIndex()])
          return SILValue();
        elems[telem->getFieldIndex()] = createValueFromAddr(telem, builder, loc);
        continue;
      }
    }
    // TODO: handle StructElementAddrInst to create structs.

    return SILValue();
  }
  switch (kind) {
  case none:
    return SILValue();
  case store:
    assert(elems.size() == 1);
    return elems[0];
  case tuple:
    if (std::any_of(elems.begin(), elems.end(),
                    [](SILValue v){ return !(bool)v; }))
      return SILValue();
    if (builder) {
      return builder->createTuple(loc, addr->getType().getObjectType(), elems);
    }
    // Just return anything not null for the dry-run.
    return elems[0];
  }
  llvm_unreachable("invalid kind");
}

/// Simplify the following two frontend patterns:
///
///   %payload_addr = init_enum_data_addr %payload_allocation
///   store %payload to %payload_addr
///   inject_enum_addr %payload_allocation, $EnumType.case
///
///   inject_enum_addr %nopayload_allocation, $EnumType.case
///
/// for a concrete enum type $EnumType.case to:
///
///   %1 = enum $EnumType, $EnumType.case, %payload
///   store %1 to %payload_addr
///
///   %1 = enum $EnumType, $EnumType.case
///   store %1 to %nopayload_addr
///
/// We leave the cleaning up to mem2reg.
SILInstruction *SILCombiner::visitInjectEnumAddrInst(InjectEnumAddrInst *ieai) {
  // Given an inject_enum_addr of a concrete type without payload, promote it to
  // a store of an enum. Mem2reg/load forwarding will clean things up for us. We
  // can't handle the payload case here due to the flow problems caused by the
  // dependency in between the enum and its data.

  assert(ieai->getOperand()->getType().isAddress() && "Must be an address");
  Builder.setCurrentDebugScope(ieai->getDebugScope());

  if (ieai->getOperand()->getType().isAddressOnly(*ieai->getFunction())) {
    // Check for the following pattern inside the current basic block:
    // inject_enum_addr %payload_allocation, $EnumType.case1
    // ... no insns storing anything into %payload_allocation
    // select_enum_addr  %payload_allocation,
    //                   case $EnumType.case1: %Result1,
    //                   case case $EnumType.case2: %bResult2
    //                   ...
    //
    // Replace the select_enum_addr by %Result1
    auto *term = ieai->getParent()->getTerminator();
    if (isa<CondBranchInst>(term) || isa<SwitchValueInst>(term)) {
      auto beforeTerm = std::prev(std::prev(ieai->getParent()->end()));
      auto *seai = dyn_cast<SelectEnumAddrInst>(beforeTerm);
      if (!seai)
        return nullptr;

      if (seai->getOperand() != ieai->getOperand())
        return nullptr;

      SILBasicBlock::iterator ii = ieai->getIterator();
      StoreInst *si = nullptr;
      for (;;) {
        SILInstruction *ci = &*ii;
        if (ci == seai)
          break;
        ++ii;
        si = dyn_cast<StoreInst>(ci);
        if (si) {
          if (si->getDest() == ieai->getOperand())
            return nullptr;
          // For now do not support assign.
          if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
            return nullptr;
        }
        // Allow all instructions in between, which don't have any dependency to
        // the store.
        if (AA->mayWriteToMemory(&*ii, ieai->getOperand()))
          return nullptr;
      }

      auto *injectedEnumElement = ieai->getElement();
      auto result = seai->getCaseResult(injectedEnumElement);

      // Replace select_enum_addr by the result
      replaceInstUsesWith(*seai, result);
      return nullptr;
    }

    // Check for the following pattern inside the current basic block:
    // inject_enum_addr %payload_allocation, $EnumType.case1
    // ... no insns storing anything into %payload_allocation
    // switch_enum_addr  %payload_allocation,
    //                   case $EnumType.case1: %bbX,
    //                   case case $EnumType.case2: %bbY
    //                   ...
    //
    // Replace the switch_enum_addr by select_enum_addr, switch_value.
    if (auto *sei = dyn_cast<SwitchEnumAddrInst>(term)) {
      if (sei->getOperand() != ieai->getOperand())
        return nullptr;

      SILBasicBlock::iterator ii = ieai->getIterator();
      StoreInst *si = nullptr;
      for (;;) {
        SILInstruction *ci = &*ii;
        if (ci == sei)
          break;
        ++ii;
        si = dyn_cast<StoreInst>(ci);
        if (si) {
          if (si->getDest() == ieai->getOperand())
            return nullptr;
          if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
            return nullptr;
        }
        // Allow all instructions in between, which don't have any dependency to
        // the store.
        if (AA->mayWriteToMemory(&*ii, ieai->getOperand()))
          return nullptr;
      }

      // Replace switch_enum_addr by a branch instruction.
      SILBuilderWithScope innerBuilder(sei);
      SmallVector<std::pair<EnumElementDecl *, SILValue>, 8> caseValues;
      SmallVector<std::pair<SILValue, SILBasicBlock *>, 8> caseBBs;

      auto intTy =
          SILType::getBuiltinIntegerType(32, innerBuilder.getASTContext());

      for (unsigned i : range(sei->getNumCases())) {
        auto pair = sei->getCase(i);
        auto *il = innerBuilder.createIntegerLiteral(sei->getLoc(), intTy,
                                                     APInt(32, i, false));
        SILValue ilValue = SILValue(il);
        caseValues.push_back(std::make_pair(pair.first, ilValue));
        caseBBs.push_back(std::make_pair(ilValue, pair.second));
      }

      SILValue defaultValue;
      SILBasicBlock *defaultBB = nullptr;

      if (sei->hasDefault()) {
        auto *il = innerBuilder.createIntegerLiteral(
            sei->getLoc(), intTy,
            APInt(32, static_cast<uint64_t>(sei->getNumCases()), false));
        defaultValue = SILValue(il);
        defaultBB = sei->getDefaultBB();
      }

      auto *seai = innerBuilder.createSelectEnumAddr(
          sei->getLoc(), sei->getOperand(), intTy, defaultValue, caseValues);

      innerBuilder.createSwitchValue(sei->getLoc(), SILValue(seai), defaultBB,
                                     caseBBs);

      return eraseInstFromFunction(*sei);
    }

    return nullptr;
  }

  // If the enum does not have a payload create the enum/store since we don't
  // need to worry about payloads.
  if (!ieai->getElement()->hasAssociatedValues()) {
    EnumInst *e =
        Builder.createEnum(ieai->getLoc(), SILValue(), ieai->getElement(),
                           ieai->getOperand()->getType().getObjectType());
    Builder.emitStoreValueOperation(ieai->getLoc(), e, ieai->getOperand(),
                                    StoreOwnershipQualifier::Init);
    return eraseInstFromFunction(*ieai);
  }

  // Ok, we have a payload enum, make sure that we have a store previous to
  // us...
  SILValue aso = ieai->getOperand();
  if (!isa<AllocStackInst>(aso)) {
    return nullptr;
  }
  InitEnumDataAddrInst *dataAddrInst = nullptr;
  InjectEnumAddrInst *enumAddrIns = nullptr;
  SmallPtrSet<SILInstruction *, 32> writeSet;
  for (auto usersIt : aso->getUses()) {
    SILInstruction *currUser = usersIt->getUser();
    if (currUser->isDeallocatingStack()) {
      // we don't care about the dealloc stack instructions
      continue;
    }

    if (currUser->isDebugInstruction() || isa<LoadBorrowInst>(currUser) ||
        isa<LoadInst>(currUser)) {
      // These Instructions are a non-risky use we can ignore.
      //
      // DISCUSSION: The reason why we do not need to care about load [take]
      // here is assuming that the IR is well formed. load [take] deinitializes
      // the memory at the location and moves the value bitwise out of the
      // memory... but it does not actually write to the
      // memory. Deinitialization is not equivalent to a write since the actual
      // underlying value is not changed. So we are not getting a new value
      // stored to the memory location.
      continue;
    }

    if (auto *currInst = dyn_cast<InitEnumDataAddrInst>(currUser)) {
      if (dataAddrInst) {
        return nullptr;
      }
      dataAddrInst = currInst;
      continue;
    }

    if (auto *currInst = dyn_cast<InjectEnumAddrInst>(currUser)) {
      if (enumAddrIns) {
        return nullptr;
      }
      enumAddrIns = currInst;
      continue;
    }

    if (auto *si = dyn_cast<StoreInst>(currUser)) {
      if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
        return nullptr;
      // The only MayWrite Instruction we can safely handle
      writeSet.insert(currUser);
      continue;
    }

    // It is too risky to continue if it is any other instruction.
    return nullptr;
  }

  if (!dataAddrInst || !enumAddrIns) {
    return nullptr;
  }

  assert((enumAddrIns == ieai) &&
         "Found InitEnumDataAddrInst differs from IEAI");
  // Make sure the enum pattern instructions are the only ones which write to
  // this location
  if (!writeSet.empty()) {
    // Analyze the instructions (implicit dominator analysis)
    // If we find any of MayWriteSet, return nullptr
    SILBasicBlock *initEnumBB = dataAddrInst->getParent();
    assert(initEnumBB && "DataAddrInst is not in a valid Basic Block");
    SmallVector<SILInstruction *, 64> worklist;
    worklist.push_back(ieai);
    SmallPtrSet<SILBasicBlock *, 16> preds;
    preds.insert(ieai->getParent());
    while (!worklist.empty()) {
      SILInstruction *currIns = worklist.pop_back_val();
      SILBasicBlock *currBB = currIns->getParent();

      if (currBB->isEntry() && currBB != initEnumBB) {
        // reached prologue without encountering the init bb
        return nullptr;
      }

      for (auto insIt = ++currIns->getIterator().getReverse();
           insIt != currBB->rend(); ++insIt) {
        SILInstruction *ins = &*insIt;
        if (ins == dataAddrInst) {
          // don't care about what comes before init enum in the basic block
          break;
        }
        if (writeSet.count(ins) != 0) {
          return nullptr;
        }
      }

      if (currBB == initEnumBB) {
        continue;
      }

      // Go to predecessors and do all that again
      for (SILBasicBlock *pred : currBB->getPredecessorBlocks()) {
        // If it's already in the set, then we've already queued and/or
        // processed the predecessors.
        if (preds.insert(pred).second) {
          worklist.push_back(&*pred->rbegin());
        }
      }
    }
  }

  // Check if we can replace all stores to the enum data with an enum of the
  // stored value. We can also handle tuples as payloads, e.g.
  //
  //   %payload_addr = init_enum_data_addr %enum_addr
  //   %elem0_addr = tuple_element_addr %payload_addr, 0
  //   %elem1_addr = tuple_element_addr %payload_addr, 1
  //   store %payload0 to %elem0_addr
  //   store %payload1 to %elem1_addr
  //   inject_enum_addr %enum_addr, $EnumType.case
  //
  if (createValueFromAddr(dataAddrInst, nullptr, dataAddrInst->getLoc())) {
    SILValue en =
        createValueFromAddr(dataAddrInst, &Builder, dataAddrInst->getLoc());
    assert(en);

    // In that case, create the payload enum/store.
    EnumInst *e = Builder.createEnum(
        dataAddrInst->getLoc(), en, dataAddrInst->getElement(),
        dataAddrInst->getOperand()->getType().getObjectType());
    Builder.emitStoreValueOperation(dataAddrInst->getLoc(), e,
                                    dataAddrInst->getOperand(),
                                    StoreOwnershipQualifier::Init);
    // Cleanup.
    eraseUsesOfInstruction(dataAddrInst);
    recursivelyDeleteTriviallyDeadInstructions(dataAddrInst, true);
    return eraseInstFromFunction(*ieai);
  }

  // Check whether we have an apply initializing the enum.
  //  %iedai = init_enum_data_addr %enum_addr
  //         = apply(%iedai,...)
  //  inject_enum_addr %enum_addr
  //
  // We can localize the store to an alloc_stack.
  // Allowing us to perform the same optimization as for the store.
  //
  //  %alloca = alloc_stack
  //            apply(%alloca,...)
  //  %load = load %alloca
  //  %1 = enum $EnumType, $EnumType.case, %load
  //  store %1 to %nopayload_addr
  //
  auto *ai = dyn_cast_or_null<ApplyInst>(getSingleNonDebugUser(dataAddrInst));
  if (!ai)
    return nullptr;
  unsigned argIdx = 0;
  Operand *enumInitOperand = nullptr;
  for (auto &opd : ai->getArgumentOperands()) {
    // Found an apply that initializes the enum. We can optimize this by
    // localizing the initialization to an alloc_stack and loading from it.
    dataAddrInst = dyn_cast<InitEnumDataAddrInst>(opd.get());
    if (dataAddrInst && dataAddrInst->getOperand() == ieai->getOperand() &&
        argIdx < ai->getSubstCalleeConv().getNumIndirectSILResults()) {
      enumInitOperand = &opd;
      break;
    }
    ++argIdx;
  }

  if (!enumInitOperand) {
    return nullptr;
  }

  // Localize the address access.
  Builder.setInsertionPoint(ai);
  auto *allocStack = Builder.createAllocStack(
      dataAddrInst->getLoc(), enumInitOperand->get()->getType());
  enumInitOperand->set(allocStack);
  Builder.setInsertionPoint(std::next(SILBasicBlock::iterator(ai)));
  auto load = Builder.emitLoadValueOperation(dataAddrInst->getLoc(), allocStack,
                                             LoadOwnershipQualifier::Take);
  EnumInst *e = Builder.createEnum(
      dataAddrInst->getLoc(), load, dataAddrInst->getElement(),
      dataAddrInst->getOperand()->getType().getObjectType());
  Builder.emitStoreValueOperation(dataAddrInst->getLoc(), e,
                                  dataAddrInst->getOperand(),
                                  StoreOwnershipQualifier::Init);
  Builder.createDeallocStack(dataAddrInst->getLoc(), allocStack);
  eraseInstFromFunction(*dataAddrInst);
  return eraseInstFromFunction(*ieai);
}

SILInstruction *
SILCombiner::
visitUnreachableInst(UnreachableInst *UI) {
  // Make sure that this unreachable instruction
  // is the last instruction in the basic block.
  if (UI->getParent()->getTerminator() == UI)
    return nullptr;

  // Collect together all the instructions after this point
  llvm::SmallVector<SILInstruction *, 32> ToRemove;
  for (auto Inst = UI->getParent()->rbegin(); &*Inst != UI; ++Inst)
    ToRemove.push_back(&*Inst);

  for (auto *Inst : ToRemove) {
    // Replace any still-remaining uses with undef values and erase.
    Inst->replaceAllUsesOfAllResultsWithUndef();
    eraseInstFromFunction(*Inst);
  }

  return nullptr;
}

/// We really want to eliminate unchecked_take_enum_data_addr. Thus if we find
/// one go through all of its uses and see if they are all loads and address
/// projections (in many common situations this is true). If so, perform:
///
/// (load (unchecked_take_enum_data_addr x)) -> (unchecked_enum_data (load x))
///
/// FIXME: Implement this for address projections.
///
/// Also remove dead unchecked_take_enum_data_addr:
///   (destroy_addr (unchecked_take_enum_data_addr x)) -> (destroy_addr x)
SILInstruction *SILCombiner::visitUncheckedTakeEnumDataAddrInst(
    UncheckedTakeEnumDataAddrInst *tedai) {
  // If our TEDAI has no users, there is nothing to do.
  if (tedai->use_empty())
    return nullptr;

  bool onlyLoads = true;
  bool onlyDestroys = true;
  for (auto U : getNonDebugUses(tedai)) {
    // Check if it is load. If it is not a load, bail...
    if (!isa<LoadInst>(U->getUser()))
      onlyLoads = false;
    if (!isa<DestroyAddrInst>(U->getUser()))
      onlyDestroys = false;
  }

  if (onlyDestroys) {
    // The unchecked_take_enum_data_addr is dead: remove it and replace all
    // destroys with a destroy of its operand.
    while (!tedai->use_empty()) {
      Operand *use = *tedai->use_begin();
      SILInstruction *user = use->getUser();
      if (auto *dai = dyn_cast<DestroyAddrInst>(user)) {
        dai->setOperand(tedai->getOperand());
      } else {
        assert(user->isDebugInstruction());
        eraseInstFromFunction(*user);
      }
    }
    return eraseInstFromFunction(*tedai);
  }

  if (!onlyLoads)
    return nullptr;

  // If our enum type is address only, we cannot do anything here. The key
  // thing to remember is that an enum is address only if any of its cases are
  // address only. So we *could* have a loadable payload resulting from the
  // TEDAI without the TEDAI being loadable itself.
  if (tedai->getOperand()->getType().isAddressOnly(*tedai->getFunction()))
    return nullptr;

  // Grab the EnumAddr.
  SILLocation loc = tedai->getLoc();
  Builder.setCurrentDebugScope(tedai->getDebugScope());
  SILValue enumAddr = tedai->getOperand();
  EnumElementDecl *enumElt = tedai->getElement();
  SILType payloadType = tedai->getType().getObjectType();

  // Go back through a second time now that we know all of our users are
  // loads. Perform the transformation on each load.
  SmallVector<LoadInst *, 4> toRemove;
  for (auto U : getNonDebugUses(tedai)) {
    // Grab the load.
    LoadInst *l = cast<LoadInst>(U->getUser());

    // If we have ownership we need to specially handle the case where our
    // payload is trivial and our enum is not. In this case, we need to insert a
    // load_borrow, extract the trivial value from that, and then end the
    // borrow.
    if (Builder.hasOwnership() &&
        tedai->getType().isTrivial(Builder.getFunction()) &&
        !enumAddr->getType().isTrivial(Builder.getFunction())) {
      auto *ld = Builder.createLoadBorrow(loc, enumAddr);
      auto *d = Builder.createUncheckedEnumData(loc, ld, enumElt, payloadType);
      Builder.createEndBorrow(loc, ld);
      replaceInstUsesWith(*l, d);
    } else {
      auto *ld = Builder.createLoad(loc, enumAddr, l->getOwnershipQualifier());
      auto *d = Builder.createUncheckedEnumData(loc, ld, enumElt, payloadType);
      replaceInstUsesWith(*l, d);
    }
    toRemove.push_back(l);
  }

  for (auto *ld : toRemove) {
    eraseInstFromFunction(*ld);
  }

  return eraseInstFromFunction(*tedai);
}

SILInstruction *SILCombiner::visitStrongReleaseInst(StrongReleaseInst *SRI) {
  assert(!SRI->getFunction()->hasOwnership());

  // Release of ThinToThickFunction is a no-op.
  if (isa<ThinToThickFunctionInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  if (isa<ObjCExistentialMetatypeToObjectInst>(SRI->getOperand()) ||
      isa<ObjCMetatypeToObjectInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  // Retain and Release of tagged strings is a no-op.
  // The builtin code pattern to find tagged strings is:
  // builtin "stringObjectOr_Int64" (or to tag the string)
  // value_to_bridge_object (cast the UInt to bridge object)
  if (isa<ValueToBridgeObjectInst>(SRI->getOperand())) {
    return eraseInstFromFunction(*SRI);
  }

  // Release of a classbound existential converted from a class is just a
  // release of the class, squish the conversion.
  if (auto ier = dyn_cast<InitExistentialRefInst>(SRI->getOperand()))
    if (ier->hasOneUse()) {
      SRI->setOperand(ier->getOperand());
      eraseInstFromFunction(*ier);
      return SRI;
    }
  
  return nullptr;
}

SILInstruction *SILCombiner::visitCopyValueInst(CopyValueInst *cvi) {
  if (cvi->getOperand().getOwnershipKind() == ValueOwnershipKind::None) {
    replaceInstUsesWith(*cvi, cvi->getOperand());
    return eraseInstFromFunction(*cvi);
  }

  // If we have a copy_value that only has destroy values as non debug uses,
  // just eliminate it.
  //
  // We do this sort of transform in SemanticARCOpts. This just prevents small
  // phase ordering issues.
  if (llvm::all_of(getNonDebugUses(cvi), [](Operand *use) {
        return isa<DestroyValueInst>(use->getUser());
      })) {
    eraseInstIncludingUsers(cvi);
    return nullptr;
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitBeginBorrowInst(BeginBorrowInst *bbi) {
  // Eliminate trivial borrow scopes.
  auto kind = bbi->getOperand().getOwnershipKind();
  SmallVector<EndBorrowInst *, 16> endBorrows;
  for (auto *op : bbi->getUses()) {
    if (op->canAcceptKindWithoutConsuming(kind))
      continue;

    // Otherwise, this borrow is being consumed. See if our consuming inst is an
    // end_borrow. If it isn't, then return false, this scope is
    // needed. Otherwise, add the end_borrow to our list of end borrows.
    auto *ebi = dyn_cast<EndBorrowInst>(op->getUser());
    if (!ebi) {
      return nullptr;
    }
    endBorrows.push_back(ebi);
  }

  // At this point, we know that the begin_borrow's operand can be
  // used as an argument to all non-end borrow uses. Eliminate the
  // begin borrow and end borrows.
  while (!endBorrows.empty()) {
    auto *ebi = endBorrows.pop_back_val();
    eraseInstFromFunction(*ebi);
  }

  replaceInstUsesWith(*bbi, bbi->getOperand());
  return eraseInstFromFunction(*bbi);
}

SILInstruction *SILCombiner::visitDestroyValueInst(DestroyValueInst *dvi) {
  if (dvi->getOperand().getOwnershipKind() == ValueOwnershipKind::None)
    return eraseInstFromFunction(*dvi);

  // If our operand is a single value forwarding instruction and we are its only
  // use, we can just eliminate the forwarding inst.
  if (auto *svi = dyn_cast<SingleValueInstruction>(dvi->getOperand())) {
    // TODO: We should be able to get the forwarding operand if we have multiple
    // results.
    if (svi->hasOneUse() && svi->getNumOperands() == 1 &&
        isOwnedForwardingInstruction(svi)) {
      dvi->setOperand(svi->getOperand(0));
      eraseInstFromFunction(*svi);
      return dvi;
    }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitCondBranchInst(CondBranchInst *CBI) {
  // cond_br(xor(x, 1)), t_label, f_label -> cond_br x, f_label, t_label
  // cond_br(x == 0), t_label, f_label -> cond_br x, f_label, t_label
  // cond_br(x != 1), t_label, f_label -> cond_br x, f_label, t_label
  SILValue X;
  if (match(CBI->getCondition(),
            m_CombineOr(
                // xor(x, 1)
                m_ApplyInst(BuiltinValueKind::Xor, m_SILValue(X), m_One()),
                // xor(1,x)
                m_ApplyInst(BuiltinValueKind::Xor, m_One(), m_SILValue(X)),
                // x == 0
                m_ApplyInst(BuiltinValueKind::ICMP_EQ, m_SILValue(X), m_Zero()),
                // x != 1
                m_ApplyInst(BuiltinValueKind::ICMP_NE, m_SILValue(X),
                            m_One()))) &&
      X->getType() ==
          SILType::getBuiltinIntegerType(1, CBI->getModule().getASTContext())) {
    SmallVector<SILValue, 4> OrigTrueArgs, OrigFalseArgs;
    for (const auto Op : CBI->getTrueArgs())
      OrigTrueArgs.push_back(Op);
    for (const auto Op : CBI->getFalseArgs())
      OrigFalseArgs.push_back(Op);
    return Builder.createCondBranch(CBI->getLoc(), X,
                                    CBI->getFalseBB(), OrigFalseArgs,
                                    CBI->getTrueBB(), OrigTrueArgs);
  }

  // cond_br (select_enum) -> switch_enum
  //
  // This pattern often occurs as a result of using optionals.
  if (auto *SEI = dyn_cast<SelectEnumInst>(CBI->getCondition())) {
    // No bb args should be passed
    if (!CBI->getTrueArgs().empty() || !CBI->getFalseArgs().empty())
      return nullptr;
    auto EnumOperandTy = SEI->getEnumOperand()->getType();
    // Type should be loadable
    if (!EnumOperandTy.isLoadable(*SEI->getFunction()))
      return nullptr;

    // Result of the select_enum should be a boolean.
    if (SEI->getType() != CBI->getCondition()->getType())
      return nullptr;

    // If any of cond_br edges are critical edges, do not perform
    // the transformation, as SIL in canonical form may
    // only have critical edges that are originating from cond_br
    // instructions.
    if (!CBI->getTrueBB()->getSinglePredecessorBlock())
      return nullptr;

    if (!CBI->getFalseBB()->getSinglePredecessorBlock())
      return nullptr;

    SILBasicBlock *DefaultBB = nullptr;
    match_integer<0> Zero;

    if (SEI->hasDefault()) {
      // Default result should be an integer constant.
      if (!isa<IntegerLiteralInst>(SEI->getDefaultResult()))
        return nullptr;
      bool isFalse = match(SEI->getDefaultResult(), Zero);
      // Pick the default BB.
      DefaultBB = isFalse ? CBI->getFalseBB() : CBI->getTrueBB();
    }

    if (!DefaultBB) {
      // Find the targets for the majority of cases and pick it
      // as a default BB.
      unsigned TrueBBCases = 0;
      unsigned FalseBBCases = 0;
      for (int i = 0, e = SEI->getNumCases(); i < e; ++i) {
        auto Pair = SEI->getCase(i);
        if (isa<IntegerLiteralInst>(Pair.second)) {
          bool isFalse = match(Pair.second, Zero);
          if (!isFalse) {
            ++TrueBBCases;
          } else {
            ++FalseBBCases;
          }
          continue;
        }
        return nullptr;
      }

      if (FalseBBCases > TrueBBCases)
        DefaultBB = CBI->getFalseBB();
      else
        DefaultBB = CBI->getTrueBB();
    }

    assert(DefaultBB && "Default should be defined at this point");

    unsigned NumTrueBBCases = 0;
    unsigned NumFalseBBCases = 0;

    if (DefaultBB == CBI->getFalseBB())
      ++NumFalseBBCases;
    else
      ++NumTrueBBCases;

    // We can now convert cond_br(select_enum) into switch_enum.
    SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 8> Cases;
    for (int i = 0, e = SEI->getNumCases(); i < e; ++i) {
      auto Pair = SEI->getCase(i);

      // Bail if one of the results is not an integer constant.
      if (!isa<IntegerLiteralInst>(Pair.second))
        return nullptr;

      // Add a switch case.
      bool isFalse = match(Pair.second, Zero);
      if (!isFalse && DefaultBB != CBI->getTrueBB()) {
        Cases.push_back(std::make_pair(Pair.first, CBI->getTrueBB()));
        ++NumTrueBBCases;
      }
      if (isFalse && DefaultBB != CBI->getFalseBB()) {
        Cases.push_back(std::make_pair(Pair.first, CBI->getFalseBB()));
        ++NumFalseBBCases;
      }
    }

    // Bail if a switch_enum would introduce a critical edge.
    if (NumTrueBBCases > 1 || NumFalseBBCases > 1)
      return nullptr;

    // The select_enum_inst is a point use with ownership meaning that we do not
    // consume the parameter if it is owned. switch_enum in contrast will
    // consume this argument. So we need to borrow our operand for the switch
    // operation.
    SILValue operand =
        Builder.emitBeginBorrowOperation(SEI->getLoc(), SEI->getEnumOperand());
    bool insertedBorrow = operand != SEI->getEnumOperand();
    auto *sei =
        Builder.createSwitchEnum(SEI->getLoc(), operand, DefaultBB, Cases);
    // Now that we are setup, add our guaranteed phi arguments/end borrows.
    for (const auto &c : Cases) {
      if (c.first->hasAssociatedValues()) {
        SILType eltType = operand->getType().getEnumElementType(
            c.first, Builder.getModule(), Builder.getTypeExpansionContext());
        eltType = eltType.getObjectType();
        if (eltType.isTrivial(Builder.getFunction())) {
          c.second->createPhiArgument(eltType, ValueOwnershipKind::None);
        } else {
          c.second->createPhiArgument(eltType, ValueOwnershipKind::Guaranteed);
        }
      }

      if (insertedBorrow) {
        Builder.setInsertionPoint(c.second->front().getIterator());
        Builder.emitEndBorrowOperation(SEI->getLoc(), operand);
      }
    }

    if (Builder.hasOwnership() && DefaultBB) {
      if (operand->getType().isTrivial(Builder.getFunction())) {
        DefaultBB->createPhiArgument(operand->getType(),
                                     ValueOwnershipKind::None);
      } else {
        DefaultBB->createPhiArgument(operand->getType(),
                                     ValueOwnershipKind::Guaranteed);
      }
      if (insertedBorrow) {
        Builder.setInsertionPoint(DefaultBB->front().getIterator());
        Builder.emitEndBorrowOperation(SEI->getLoc(), operand);
      }
    }
    return sei;
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitSelectEnumInst(SelectEnumInst *SEI) {
  // Canonicalize a select_enum: if the default refers to exactly one case, then
  // replace the default with that case.
  if (SEI->hasDefault()) {
    NullablePtr<EnumElementDecl> elementDecl = SEI->getUniqueCaseForDefault();
    if (elementDecl.isNonNull()) {
      // Construct a new instruction by copying all the case entries.
      SmallVector<std::pair<EnumElementDecl *, SILValue>, 4> CaseValues;
      for (int idx = 0, numIdcs = SEI->getNumCases(); idx < numIdcs; ++idx) {
        CaseValues.push_back(SEI->getCase(idx));
      }
      // Add the default-entry of the original instruction as case-entry.
      CaseValues.push_back(
          std::make_pair(elementDecl.get(), SEI->getDefaultResult()));

      return Builder.createSelectEnum(SEI->getLoc(), SEI->getEnumOperand(),
                                      SEI->getType(), SILValue(), CaseValues);
    }
  }

  // TODO: We should be able to flat-out replace the select_enum instruction
  // with the selected value in another pass. For parity with the enum_is_tag
  // combiner pass, handle integer literals for now.
  auto *EI = dyn_cast<EnumInst>(SEI->getEnumOperand());
  if (!EI)
    return nullptr;

  SILValue selected;
  for (unsigned i = 0, e = SEI->getNumCases(); i < e; ++i) {
    auto casePair = SEI->getCase(i);
    if (casePair.first == EI->getElement()) {
      selected = casePair.second;
      break;
    }
  }
  if (!selected)
    selected = SEI->getDefaultResult();

  if (auto *ILI = dyn_cast<IntegerLiteralInst>(selected)) {
    return Builder.createIntegerLiteral(ILI->getLoc(), ILI->getType(),
                                        ILI->getValue());
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitTupleExtractInst(TupleExtractInst *TEI) {
  // tuple_extract(apply([add|sub|...]overflow(x, 0)), 1) -> 0
  // if it can be proven that no overflow can happen.
  if (TEI->getFieldIndex() != 1)
    return nullptr;

  Builder.setCurrentDebugScope(TEI->getDebugScope());
  if (auto *BI = dyn_cast<BuiltinInst>(TEI->getOperand()))
    if (!canOverflow(BI))
      return Builder.createIntegerLiteral(TEI->getLoc(), TEI->getType(),
                                          APInt(1, 0));
  return nullptr;
}

SILInstruction *SILCombiner::visitFixLifetimeInst(FixLifetimeInst *FLI) {
  // fix_lifetime(alloc_stack) -> fix_lifetime(load(alloc_stack))
  Builder.setCurrentDebugScope(FLI->getDebugScope());
  if (auto *AI = dyn_cast<AllocStackInst>(FLI->getOperand())) {
    if (FLI->getOperand()->getType().isLoadable(*FLI->getFunction())) {
      auto Load = Builder.emitLoadBorrowOperation(FLI->getLoc(), AI);
      auto *newFLI = Builder.createFixLifetime(FLI->getLoc(), Load);
      replaceInstUsesPairwiseWith(FLI, newFLI);
      Builder.emitEndBorrowOperation(newFLI->getLoc(), Load);
      return eraseInstFromFunction(*FLI);
    }
  }
  return nullptr;
}

SILInstruction *
SILCombiner::
visitAllocRefDynamicInst(AllocRefDynamicInst *ARDI) {
  SmallVector<SILValue, 4> Counts;
  auto getCounts = [&] (AllocRefDynamicInst *AI) -> ArrayRef<SILValue> {
    for (Operand &Op : AI->getTailAllocatedCounts()) {
      Counts.push_back(Op.get());
    }
    return Counts;
  };

  // %1 = metatype $X.Type
  // %2 = alloc_ref_dynamic %1 : $X.Type, Y
  // ->
  // alloc_ref X
  Builder.setCurrentDebugScope(ARDI->getDebugScope());

  SILValue MDVal = ARDI->getMetatypeOperand();
  if (auto *UC = dyn_cast<UpcastInst>(MDVal))
    MDVal = UC->getOperand();

  SingleValueInstruction *NewInst = nullptr;
  if (auto *MI = dyn_cast<MetatypeInst>(MDVal)) {
    auto MetaTy = MI->getType().castTo<MetatypeType>();
    auto InstanceTy = MetaTy.getInstanceType();
    if (auto SelfTy = dyn_cast<DynamicSelfType>(InstanceTy))
      InstanceTy = SelfTy.getSelfType();
    auto SILInstanceTy = SILType::getPrimitiveObjectType(InstanceTy);
    if (!SILInstanceTy.getClassOrBoundGenericClass())
      return nullptr;

    NewInst = Builder.createAllocRef(ARDI->getLoc(), SILInstanceTy,
                                     ARDI->isObjC(), false,
                                     ARDI->getTailAllocatedTypes(),
                                     getCounts(ARDI));

  } else if (isa<SILArgument>(MDVal)) {

    // checked_cast_br [exact] $Y.Type to $X.Type, bbSuccess, bbFailure
    // ...
    // bbSuccess(%T: $X.Type)
    // alloc_ref_dynamic %T : $X.Type, $X
    // ->
    // alloc_ref $X
    auto *PredBB = ARDI->getParent()->getSinglePredecessorBlock();
    if (!PredBB)
      return nullptr;
    auto *CCBI = dyn_cast<CheckedCastBranchInst>(PredBB->getTerminator());
    if (CCBI && CCBI->isExact() && ARDI->getParent() == CCBI->getSuccessBB()) {
      auto MetaTy = cast<MetatypeType>(CCBI->getTargetFormalType());
      auto InstanceTy = MetaTy.getInstanceType();
      if (auto SelfTy = dyn_cast<DynamicSelfType>(InstanceTy))
        InstanceTy = SelfTy.getSelfType();
      auto SILInstanceTy = SILType::getPrimitiveObjectType(InstanceTy);
      if (!SILInstanceTy.getClassOrBoundGenericClass())
        return nullptr;
      NewInst = Builder.createAllocRef(ARDI->getLoc(), SILInstanceTy,
                                       ARDI->isObjC(), false,
                                       ARDI->getTailAllocatedTypes(),
                                       getCounts(ARDI));
    }
  }
  if (NewInst && NewInst->getType() != ARDI->getType()) {
    // In case the argument was an upcast of the metatype, we have to upcast the
    // resulting reference.
    NewInst = Builder.createUpcast(ARDI->getLoc(), NewInst, ARDI->getType());
  }
  return NewInst;
}

SILInstruction *SILCombiner::visitMarkDependenceInst(MarkDependenceInst *mdi) {
  // Simplify the base operand of a MarkDependenceInst to eliminate unnecessary
  // instructions that aren't adding value.
  //
  // Conversions to Optional.Some(x) often happen here, this isn't important
  // for us, we can just depend on 'x' directly.
  if (auto *eiBase = dyn_cast<EnumInst>(mdi->getBase())) {
    if (eiBase->hasOperand()) {
      // Without ownership this always works. With ownership this only works
      // with guaranteed values.
      if (eiBase->hasOneUse()) {
        mdi->setBase(eiBase->getOperand());
        eraseInstFromFunction(*eiBase);
        return mdi;
      }

      // Otherwise, we may have an owned value in such a case, we need to make
      // sure our enum only has 2 uses, the mdi and a destroy_value. In such a
      // case, we can perform the same optimization but we need to change the
      // destroy_value to be on the other base value.
      if (eiBase->hasTwoUses()) {
        if (auto *dvi =
                eiBase->getSingleConsumingUserOfType<DestroyValueInst>()) {
          SILBuilderWithScope builder(dvi, Builder);
          builder.emitDestroyValueOperation(dvi->getLoc(),
                                            eiBase->getOperand());

          mdi->setBase(eiBase->getOperand());
          eraseInstFromFunction(*dvi);
          eraseInstFromFunction(*eiBase);
          return mdi;
        }
      }
    }
  }
  
  // Conversions from a class to AnyObject also happen a lot, we can just depend
  // on the class reference.
  if (auto *ier = dyn_cast<InitExistentialRefInst>(mdi->getBase())) {
    // While this is true without ownership, this is not true in general. With
    // ownership we need to handle the owned case specially.
    if (ier->getOwnershipKind() != ValueOwnershipKind::Owned) {
      mdi->setBase(ier->getOperand());
      if (ier->use_empty())
        eraseInstFromFunction(*ier);
      return mdi;
    }

    // Otherwise if we have an owned thing, we need to be a little more
    // careful. Specifically, we need to make sure that our ier has only two
    // uses, our mdi and a destroy_value. Then we can eliminte it.
    if (ier->hasTwoUses()) {
      if (auto *dvi = ier->getSingleConsumingUserOfType<DestroyValueInst>()) {
        SILBuilderWithScope builder(dvi, Builder);
        builder.emitDestroyValueOperation(dvi->getLoc(), ier->getOperand());

        mdi->setBase(ier->getOperand());
        eraseInstFromFunction(*dvi);
        eraseInstFromFunction(*ier);
        return mdi;
      }
    }

    return mdi;
  }

  // Conversions from a class to AnyObject also happen a lot, we can just depend
  // on the class reference.
  if (auto *oeri = dyn_cast<OpenExistentialRefInst>(mdi->getBase())) {
    if (oeri->getOwnershipKind() != ValueOwnershipKind::Owned) {
      mdi->setBase(oeri->getOperand());
      if (oeri->use_empty())
        eraseInstFromFunction(*oeri);
      return mdi;
    }
    // Otherwise if we have an owned thing, we need to be a little more
    // careful. Specifically, we need to make sure that our ier has only two
    // uses, our mdi and a destroy_value. Then we can eliminte it.
    if (oeri->hasTwoUses()) {
      if (auto *dvi = oeri->getSingleConsumingUserOfType<DestroyValueInst>()) {
        SILBuilderWithScope builder(dvi, Builder);
        builder.emitDestroyValueOperation(dvi->getLoc(), oeri->getOperand());

        mdi->setBase(oeri->getOperand());
        eraseInstFromFunction(*dvi);
        eraseInstFromFunction(*oeri);
        return mdi;
      }
    }
    return mdi;
  }

  // Sometimes due to specialization/builtins, we can get a mark_dependence
  // whose base is a trivial typed object. In such a case, the mark_dependence
  // does not have a meaning, so just eliminate it.
  {
    SILType baseType = mdi->getBase()->getType();
    if (baseType.isObject() && baseType.isTrivial(*mdi->getFunction())) {
      SILValue value = mdi->getValue();
      mdi->replaceAllUsesWith(value);
      return eraseInstFromFunction(*mdi);
    }
  }

  return nullptr;
}


SILInstruction *SILCombiner::
visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *CBOI) {
  auto *URC = dyn_cast<UncheckedRefCastInst>(CBOI->getOperand());
  if (!URC)
    return nullptr;

  auto type = URC->getOperand()->getType().getASTType();
  if (ClassDecl *cd = type->getClassOrBoundGenericClass()) {
    if (!cd->isObjC()) {
      auto int1Ty = SILType::getBuiltinIntegerType(1, Builder.getASTContext());
      SILValue zero = Builder.createIntegerLiteral(CBOI->getLoc(),
                                                   int1Ty, 0);
      return Builder.createTuple(CBOI->getLoc(), { zero, zero });
    }
  }

  return nullptr;
}

