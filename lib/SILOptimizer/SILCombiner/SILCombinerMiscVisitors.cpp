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
#include "swift/AST/SemanticAttrs.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
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
  //   %6a = project_existential_box %6
  //   %7 = enum $VendingMachineError, #ColorError.Red
  //   store %7 to %6a : $*ColorError
  //   debug_value %6 : $Error
  //   strong_release %6 : $Error
  //
  //   %6 = alloc_existential_box $Error, $ColorError
  //   %6a = project_existential_box %6
  //   %7 = enum $VendingMachineError, #ColorError.Red
  //   store %7 to [init] %6a : $*ColorError
  //   debug_value %6 : $Error
  //   destroy_value %6 : $Error
  SILValue boxedValue =
    getConcreteValueOfExistentialBox(AEBI, /*ignoreUser*/ nullptr);
  if (!boxedValue)
    return nullptr;

  // Check if the box is destroyed at a single place. That's the end of its
  // lifetime.
  SILInstruction *singleDestroy = nullptr;
  if (hasOwnership()) {
    if (auto *use = AEBI->getSingleConsumingUse()) {
      singleDestroy = dyn_cast<DestroyValueInst>(use->getUser());
    }
  } else {
    for (Operand *use : AEBI->getUses()) {
      auto *user = use->getUser();
      if (isa<StrongReleaseInst>(user) || isa<ReleaseValueInst>(user)) {
        if (singleDestroy)
          return nullptr;
        singleDestroy = user;
      }
    }
  }

  if (!singleDestroy)
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
  Builder.setInsertionPoint(singleDestroy);
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
  SILValue Addr = SEAI->getOperand();

  // Convert switch_enum_addr -> br
  //
  // If the only thing which writes to the address is an inject_enum_addr. We
  // only perform these optimizations when we are not in OSSA since this
  // eliminates an edge from the CFG and we want SILCombine in OSSA to never do
  // that, so in the future we can invalidate less.
  if (!SEAI->getFunction()->hasOwnership()) {
    if (EnumElementDecl *EnumCase = getInjectEnumCaseTo(Addr)) {
      SILBasicBlock *Dest = SEAI->getCaseDestination(EnumCase);
      // If the only instruction which writes to Addr is an inject_enum_addr we
      // know that there cannot be an enum payload.
      assert(Dest->getNumArguments() == 0 &&
             "didn't expect a payload argument");
      Builder.createBranch(SEAI->getLoc(), Dest);
      return eraseInstFromFunction(*SEAI);
    }
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
        sei->createResult(c.second, eltType);
      }
      Builder.setInsertionPoint(c.second->front().getIterator());
      Builder.emitEndBorrowOperation(sei->getLoc(), EnumVal);
    }
    sei->createDefaultResult();
    if (auto defaultBlock = sei->getDefaultBBOrNull()) {
      Builder.setInsertionPoint(defaultBlock.get()->front().getIterator());
      Builder.emitEndBorrowOperation(sei->getLoc(), EnumVal);
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
  BasicBlockSet handled(from->getFunction());
  workList.push_back(from);
  while (!workList.empty()) {
    SILInstruction *start = workList.pop_back_val();
    for (auto iter = start->getIterator(), end = start->getParent()->end();
         iter != end;
         ++iter) {
      SILInstruction *inst = &*iter;
      if (isa<RetainValueInst>(inst) || isa<StrongRetainInst>(inst)) {
        return true;
      }
      if ((isa<DeallocStackInst>(inst) || isa<DestroyAddrInst>(inst)) &&
          inst->getOperand(0) == alloc) {
        break;
      }
      if (isa<TermInst>(inst)) {
        for (SILBasicBlock *succ : start->getParent()->getSuccessors()) {
          if (handled.insert(succ))
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
bool SILCombiner::optimizeStackAllocatedEnum(AllocStackInst *AS) {
  EnumDecl *enumDecl = AS->getType().getEnumOrBoundGenericEnum();
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
  for (auto *use : AS->getUses()) {
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
      case SILInstructionKind::DestroyAddrInst:
      case SILInstructionKind::DeallocStackInst:
      case SILInstructionKind::InjectEnumAddrInst:
        // We'll check init_enum_addr below.
        break;
      case SILInstructionKind::DebugValueInst:
        if (DebugValueInst::hasAddrVal(user))
          break;
        return false;
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
    for (auto *use : AS->getUses()) {
      if (auto *inject = dyn_cast<InjectEnumAddrInst>(use->getUser())) {
        if (inject->getElement() != element)
          return false;
      }
    }
  }

  // Second step: replace the enum alloc_stack with a payload alloc_stack.
  auto *newAlloc = Builder.createAllocStack(
      AS->getLoc(), payloadType, AS->getVarInfo(), AS->hasDynamicLifetime());

  while (!AS->use_empty()) {
    Operand *use = *AS->use_begin();
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
      case SILInstructionKind::InjectEnumAddrInst:
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
      case SILInstructionKind::DebugValueInst:
        if (DebugValueInst::hasAddrVal(user)) {
          eraseInstFromFunction(*user);
          break;
        }
        LLVM_FALLTHROUGH;
      default:
        llvm_unreachable("unexpected alloc_stack user");
    }
  }
  return true;
}

SILInstruction *SILCombiner::visitAllocStackInst(AllocStackInst *AS) {
  if (AS->getFunction()->hasOwnership())
    return nullptr;

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

      if (isa<DeinitExistentialAddrInst>(Op->getUser())) {
        eraseInstFromFunction(*Op->getUser());
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
        !isa<FixLifetimeInst>(User) && !isa<DeallocStackRefInst>(User)) {
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
SILInstruction *SILCombiner::optimizeLoadFromStringLiteral(LoadInst *LI) {
  auto *SEA = dyn_cast<StructElementAddrInst>(LI->getOperand());
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

static bool isShiftRightByAtLeastOne(SILInstruction *inst) {
  auto *bi = dyn_cast<BuiltinInst>(inst);
  if (!bi)
    return false;
  if (bi->getBuiltinInfo().ID != BuiltinValueKind::LShr)
    return false;
  auto *shiftVal = dyn_cast<IntegerLiteralInst>(bi->getArguments()[1]);
  if (!shiftVal)
    return false;
  return shiftVal->getValue().isStrictlyPositive();
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
        addr = SEA->getOperand();
        if (!SEA->getStructDecl()->getName().is("_SwiftArrayBodyStorage"))
          break;
        if (SEA->getField()->getName().is("count"))
          break;
        // For Array, the value of `capacityAndFlags` has only a zero capacity
        // but not necessarily a zero flag (in fact, the flag is 1).
        // Therefore only replace `capacityAndFlags` with zero if the flag is
        // masked out by a right-shift of 1.
        if (SEA->getField()->getName().is("_capacityAndFlags")) {
          for (Operand *loadUse : LI->getUses()) {
            if (!isShiftRightByAtLeastOne(loadUse->getUser()))
              return false;
          }
          break;
        }
        return false;
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
      case ValueKind::MultipleValueInstructionResult:
        if (auto *bci = dyn_cast<BeginCOWMutationInst>(
                                              addr->getDefiningInstruction())) {
          addr = bci->getOperand();
          break;
        }
        return false;
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
  if (auto *ui = dyn_cast<UpcastInst>(lbi->getOperand())) {
    // We want to RAUW the current load_borrow with the upcast. To do that
    // safely, we need to insert new end_borrow on the new load_borrow, erase
    // the end_borrow and then RAUW.
    SmallVector<EndBorrowInst *, 32> endBorrowInst;
    for (auto *ebi : lbi->getEndBorrows())
      endBorrowInst.push_back(ebi);
    auto newLBI = Builder.createLoadBorrow(lbi->getLoc(), ui->getOperand());
    for (auto *ebi : endBorrowInst) {
      SILBuilderWithScope builder(ebi, Builder);
      builder.emitEndBorrowOperation(ebi->getLoc(), newLBI);
      eraseInstFromFunction(*ebi);
    }
    auto *uci = Builder.createUpcast(lbi->getLoc(), newLBI, lbi->getType());
    replaceInstUsesWith(*lbi, uci);
    return eraseInstFromFunction(*lbi);
  }

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

  // If we have a load_borrow that only has non_debug end_borrow uses, delete
  // it.
  if (llvm::all_of(getNonDebugUses(lbi), [](Operand *use) {
        return isa<EndBorrowInst>(use->getUser());
      })) {
    eraseInstIncludingUsers(lbi);
    return nullptr;
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
    if (cloner.add(initVal)) {
      return cloner.clone(initVal).getDefiningInstruction();
    }
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
  return Builder.createIndexAddr(IA->getLoc(), base2, newIndex,
    IA->needsStackProtection() || cast<IndexAddrInst>(base)->needsStackProtection());
}

/// Walks over all fields of an aggregate and checks if a reference count
/// operation for \p value is required. This differs from a simple `isTrivial`
/// check, because it treats a value_to_bridge_object instruction as "trivial".
/// It can also handle non-trivial enums with trivial cases.
static bool isTrivial(SILValue value, SILFunction *function) {
  SmallVector<ValueBase *, 32> workList;
  SmallPtrSet<ValueBase *, 16> visited;
  workList.push_back(value);
  while (!workList.empty()) {
    SILValue v = workList.pop_back_val();
    if (v->getType().isTrivial(*function))
      continue;
    if (isa<ValueToBridgeObjectInst>(v))
      continue;
    if (isa<StructInst>(v) || isa<TupleInst>(v)) {
      for (SILValue op : cast<SingleValueInstruction>(v)->getOperandValues()) {
        if (visited.insert(op).second)
          workList.push_back(op);
      }
      continue;
    }
    if (auto *en = dyn_cast<EnumInst>(v)) {
      if (en->hasOperand() && visited.insert(en->getOperand()).second)
        workList.push_back(en->getOperand());
      continue;
    }
    return false;
  }
  return true;
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
  if (isTrivial(Operand, RVI->getFunction()))
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

SILInstruction *SILCombiner::visitCopyValueInst(CopyValueInst *cvi) {
  assert(cvi->getFunction()->hasOwnership());

  // Sometimes when RAUWing code we get copy_value on .none values (consider
  // transformations around function types that result in given a copy_value a
  // thin_to_thick_function argument). In such a case, just RAUW with the
  // copy_value's operand since it is a no-op.
  if (cvi->getOperand()->getOwnershipKind() == OwnershipKind::None) {
    replaceInstUsesWith(*cvi, cvi->getOperand());
    return eraseInstFromFunction(*cvi);
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitDestroyValueInst(DestroyValueInst *dvi) {
  assert(dvi->getFunction()->hasOwnership());

  // Sometimes when RAUWing code we get destroy_value on .none values. In such a
  // case, just delete the destroy_value.
  //
  // As an example, consider transformations around function types that result
  // in a thin_to_thick_function being passed to a destroy_value.
  if (dvi->getOperand()->getOwnershipKind() == OwnershipKind::None) {
    eraseInstFromFunction(*dvi);
    return nullptr;
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
    if (st && kind == none && st->getDest() == addr) {
      elems.push_back(st->getSrc());
      kind = store;
      // We cannot just return st->getSrc() here because we also have to check
      // if the store destination is the only use of addr.
      continue;
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
SILInstruction *
SILCombiner::visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI) {
  if (IEAI->getFunction()->hasOwnership())
    return nullptr;

  // Given an inject_enum_addr of a concrete type without payload, promote it to
  // a store of an enum. Mem2reg/load forwarding will clean things up for us. We
  // can't handle the payload case here due to the flow problems caused by the
  // dependency in between the enum and its data.

  // Disable this for empty typle type because empty tuple stack locations maybe
  // uninitialized. And converting to value form loses tag information.
  if (IEAI->getElement()->hasAssociatedValues()) {
    SILType elemType = IEAI->getOperand()->getType().getEnumElementType(
        IEAI->getElement(), IEAI->getFunction());
    if (elemType.isEmpty(*IEAI->getFunction())) {
      return nullptr;
    }
  }

  assert(IEAI->getOperand()->getType().isAddress() && "Must be an address");
  Builder.setCurrentDebugScope(IEAI->getDebugScope());

  if (IEAI->getOperand()->getType().isAddressOnly(*IEAI->getFunction())) {
    // Check for the following pattern inside the current basic block:
    // inject_enum_addr %payload_allocation, $EnumType.case1
    // ... no insns storing anything into %payload_allocation
    // select_enum_addr  %payload_allocation,
    //                   case $EnumType.case1: %Result1,
    //                   case case $EnumType.case2: %bResult2
    //                   ...
    //
    // Replace the select_enum_addr by %Result1

    auto *Term = IEAI->getParent()->getTerminator();
    if (isa<CondBranchInst>(Term) || isa<SwitchValueInst>(Term)) {
      auto BeforeTerm = std::prev(std::prev(IEAI->getParent()->end()));
      auto *SEAI = dyn_cast<SelectEnumAddrInst>(BeforeTerm);
      if (!SEAI)
        return nullptr;

      if (SEAI->getOperand() != IEAI->getOperand())
        return nullptr;

      SILBasicBlock::iterator II = IEAI->getIterator();
      StoreInst *SI = nullptr;
      for (;;) {
        SILInstruction *CI = &*II;
        if (CI == SEAI)
          break;
        ++II;
        SI = dyn_cast<StoreInst>(CI);
        if (SI) {
          if (SI->getDest() == IEAI->getOperand())
            return nullptr;
        }
        // Allow all instructions in between, which don't have any dependency to
        // the store.
        if (AA->mayWriteToMemory(&*II, IEAI->getOperand()))
          return nullptr;
      }

      auto *InjectedEnumElement = IEAI->getElement();
      auto Result = SEAI->getCaseResult(InjectedEnumElement);

      // Replace select_enum_addr by the result
      replaceInstUsesWith(*SEAI, Result);
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
    if (auto *SEI = dyn_cast<SwitchEnumAddrInst>(Term)) {
      if (SEI->getOperand() != IEAI->getOperand())
        return nullptr;

      SILBasicBlock::iterator II = IEAI->getIterator();
      StoreInst *SI = nullptr;
      for (;;) {
        SILInstruction *CI = &*II;
        if (CI == SEI)
          break;
        ++II;
        SI = dyn_cast<StoreInst>(CI);
        if (SI) {
          if (SI->getDest() == IEAI->getOperand())
            return nullptr;
        }
        // Allow all instructions in between, which don't have any dependency to
        // the store.
        if (AA->mayWriteToMemory(&*II, IEAI->getOperand()))
          return nullptr;
      }

      // Replace switch_enum_addr by a branch instruction.
      SILBuilderWithScope B(SEI);
      SmallVector<std::pair<EnumElementDecl *, SILValue>, 8> CaseValues;
      SmallVector<std::pair<SILValue, SILBasicBlock *>, 8> CaseBBs;

      auto IntTy = SILType::getBuiltinIntegerType(32, B.getASTContext());

      for (int i = 0, e = SEI->getNumCases(); i < e; ++i) {
        auto Pair = SEI->getCase(i);
        auto *IL = B.createIntegerLiteral(SEI->getLoc(), IntTy, APInt(32, i, false));
        SILValue ILValue = SILValue(IL);
        CaseValues.push_back(std::make_pair(Pair.first, ILValue));
        CaseBBs.push_back(std::make_pair(ILValue, Pair.second));
      }

      SILValue DefaultValue;
      SILBasicBlock *DefaultBB = nullptr;

      if (SEI->hasDefault()) {
        auto *IL = B.createIntegerLiteral(
          SEI->getLoc(), IntTy,
          APInt(32, static_cast<uint64_t>(SEI->getNumCases()), false));
        DefaultValue = SILValue(IL);
        DefaultBB = SEI->getDefaultBB();
      }

      auto *SEAI = B.createSelectEnumAddr(SEI->getLoc(), SEI->getOperand(), IntTy, DefaultValue, CaseValues);

      B.createSwitchValue(SEI->getLoc(), SILValue(SEAI), DefaultBB, CaseBBs);

      return eraseInstFromFunction(*SEI);
    }

    return nullptr;
  }

  // If the enum does not have a payload create the enum/store since we don't
  // need to worry about payloads.
  if (!IEAI->getElement()->hasAssociatedValues()) {
    EnumInst *E =
      Builder.createEnum(IEAI->getLoc(), SILValue(), IEAI->getElement(),
                          IEAI->getOperand()->getType().getObjectType());
    Builder.createStore(IEAI->getLoc(), E, IEAI->getOperand(),
                        StoreOwnershipQualifier::Unqualified);
    return eraseInstFromFunction(*IEAI);
  }

  // Ok, we have a payload enum, make sure that we have a store previous to
  // us...
  SILValue ASO = IEAI->getOperand();
  if (!isa<AllocStackInst>(ASO)) {
    return nullptr;
  }
  InitEnumDataAddrInst *DataAddrInst = nullptr;
  InjectEnumAddrInst *EnumAddrIns = nullptr;
  InstructionSetWithSize WriteSet(IEAI->getFunction());
  for (auto UsersIt : ASO->getUses()) {
    SILInstruction *CurrUser = UsersIt->getUser();
    if (CurrUser->isDeallocatingStack()) {
      // we don't care about the dealloc stack instructions
      continue;
    }
    if (CurrUser->isDebugInstruction() || isa<LoadInst>(CurrUser)) {
      // These Instructions are a non-risky use we can ignore
      continue;
    }
    if (auto *CurrInst = dyn_cast<InitEnumDataAddrInst>(CurrUser)) {
      if (DataAddrInst) {
        return nullptr;
      }
      DataAddrInst = CurrInst;
      continue;
    }
    if (auto *CurrInst = dyn_cast<InjectEnumAddrInst>(CurrUser)) {
      if (EnumAddrIns) {
        return nullptr;
      }
      EnumAddrIns = CurrInst;
      continue;
    }
    if (isa<StoreInst>(CurrUser)) {
      // The only MayWrite Instruction we can safely handle
      WriteSet.insert(CurrUser);
      continue;
    }
    // It is too risky to continue if it is any other instruction.
    return nullptr;
  }

  if (!DataAddrInst || !EnumAddrIns) {
    return nullptr;
  }
  assert((EnumAddrIns == IEAI) &&
         "Found InitEnumDataAddrInst differs from IEAI");
  // Make sure the enum pattern instructions are the only ones which write to
  // this location
  if (!WriteSet.empty()) {
    // Analyze the instructions (implicit dominator analysis)
    // If we find any of MayWriteSet, return nullptr
    SILBasicBlock *InitEnumBB = DataAddrInst->getParent();
    assert(InitEnumBB && "DataAddrInst is not in a valid Basic Block");
    llvm::SmallVector<SILInstruction *, 64> Worklist;
    Worklist.push_back(IEAI);
    BasicBlockSet Preds(InitEnumBB->getParent());
    Preds.insert(IEAI->getParent());
    while (!Worklist.empty()) {
      SILInstruction *CurrIns = Worklist.pop_back_val();
      SILBasicBlock *CurrBB = CurrIns->getParent();

      if (CurrBB->isEntry() && CurrBB != InitEnumBB) {
        // reached prologue without encountering the init bb
        return nullptr;
      }

      for (auto InsIt = ++CurrIns->getIterator().getReverse();
           InsIt != CurrBB->rend(); ++InsIt) {
        SILInstruction *Ins = &*InsIt;
        if (Ins == DataAddrInst) {
          // don't care about what comes before init enum in the basic block
          break;
        }
        if (WriteSet.contains(Ins) != 0) {
          return nullptr;
        }
      }

      if (CurrBB == InitEnumBB) {
        continue;
      }

      // Go to predecessors and do all that again
      for (SILBasicBlock *Pred : CurrBB->getPredecessorBlocks()) {
        // If it's already in the set, then we've already queued and/or
        // processed the predecessors.
        if (Preds.insert(Pred)) {
          Worklist.push_back(&*Pred->rbegin());
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
  if (createValueFromAddr(DataAddrInst, nullptr, DataAddrInst->getLoc())) {
    SILValue en =
      createValueFromAddr(DataAddrInst, &Builder, DataAddrInst->getLoc());
    assert(en);

    // In that case, create the payload enum/store.
    EnumInst *E = Builder.createEnum(
        DataAddrInst->getLoc(), en, DataAddrInst->getElement(),
        DataAddrInst->getOperand()->getType().getObjectType());
    Builder.createStore(DataAddrInst->getLoc(), E, DataAddrInst->getOperand(),
                        StoreOwnershipQualifier::Unqualified);
    // Cleanup.
    getInstModCallbacks().notifyWillBeDeleted(DataAddrInst);
    deleter.forceDeleteWithUsers(DataAddrInst);
    deleter.forceDelete(IEAI);
    deleter.cleanupDeadInstructions();
    return nullptr;
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
  auto *AI = dyn_cast_or_null<ApplyInst>(getSingleNonDebugUser(DataAddrInst));
  if (!AI)
    return nullptr;
  unsigned ArgIdx = 0;
  Operand *EnumInitOperand = nullptr;
  for (auto &Opd : AI->getArgumentOperands()) {
    // Found an apply that initializes the enum. We can optimize this by
    // localizing the initialization to an alloc_stack and loading from it.
    DataAddrInst = dyn_cast<InitEnumDataAddrInst>(Opd.get());
    if (DataAddrInst && DataAddrInst->getOperand() == IEAI->getOperand()
        && ArgIdx < AI->getSubstCalleeConv().getNumIndirectSILResults()) {
      EnumInitOperand = &Opd;
      break;
    }
    ++ArgIdx;
  }

  if (!EnumInitOperand) {
    return nullptr;
  }

  // Localize the address access.
  Builder.setInsertionPoint(AI);
  auto *AllocStack = Builder.createAllocStack(DataAddrInst->getLoc(),
                                              EnumInitOperand->get()->getType());
  EnumInitOperand->set(AllocStack);
  Builder.setInsertionPoint(std::next(SILBasicBlock::iterator(AI)));
  SILValue Load(Builder.createLoad(DataAddrInst->getLoc(), AllocStack,
                                   LoadOwnershipQualifier::Unqualified));
  EnumInst *E = Builder.createEnum(
      DataAddrInst->getLoc(), Load, DataAddrInst->getElement(),
      DataAddrInst->getOperand()->getType().getObjectType());
  Builder.createStore(DataAddrInst->getLoc(), E, DataAddrInst->getOperand(),
                      StoreOwnershipQualifier::Unqualified);
  Builder.createDeallocStack(DataAddrInst->getLoc(), AllocStack);
  eraseInstFromFunction(*DataAddrInst);
  return eraseInstFromFunction(*IEAI);
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
    if (!isa<LoadInst>(U->getUser()) && !isa<LoadBorrowInst>(U->getUser()))
      onlyLoads = false;

    // If we have a load_borrow, perform an additional check that we do not have
    // any reborrow uses. We do not handle reborrows in this optimization.
    if (auto *lbi = dyn_cast<LoadBorrowInst>(U->getUser())) {
      // false if any consuming use is not an end_borrow.
      for (auto *use : lbi->getConsumingUses()) {
        if (!isa<EndBorrowInst>(use->getUser())) {
          onlyLoads = false;
          break;
        }
      }
    }

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
  // loads. Perform the transformation on each load at the load's use site. The
  // reason that we have to do this is that otherwise we would be hoisting the
  // loads causing us to need to consider additional ARC issues.
  while (!tedai->use_empty()) {
    auto *use = *tedai->use_begin();
    auto *user = use->getUser();

    // Delete debug insts.
    if (user->isDebugInstruction()) {
      eraseInstFromFunction(*user);
      continue;
    }

    // Insert a new Load of the enum and extract the data from that.
    //
    // NOTE: This is potentially hoisting the load, so we need to insert
    // compensating destroys.
    auto *svi = cast<SingleValueInstruction>(user);
    SILValue newValue;
    if (auto *oldLoad = dyn_cast<LoadInst>(svi)) {
      SILBuilderWithScope localBuilder(oldLoad, Builder);
      // If the old load is trivial and our enum addr is non-trivial, we need to
      // use a load_borrow here. We know that the unchecked_enum_data will
      // produce a trivial value meaning that we can just do a
      // load_borrow/immediately end the lifetime here.
      if (oldLoad->getOwnershipQualifier() == LoadOwnershipQualifier::Trivial &&
          !enumAddr->getType().isTrivial(Builder.getFunction())) {
        localBuilder.emitScopedBorrowOperation(
            loc, enumAddr, [&](SILValue newLoad) {
              newValue = localBuilder.createUncheckedEnumData(
                  loc, newLoad, enumElt, payloadType);
            });
      } else {
        auto newLoad = localBuilder.emitLoadValueOperation(
            loc, enumAddr, oldLoad->getOwnershipQualifier());
        newValue = localBuilder.createUncheckedEnumData(loc, newLoad, enumElt,
                                                        payloadType);
      }
    } else if (auto *lbi = cast<LoadBorrowInst>(svi)) {
      SILBuilderWithScope localBuilder(lbi, Builder);
      auto newLoad = localBuilder.emitLoadBorrowOperation(loc, enumAddr);
      for (auto ui = lbi->consuming_use_begin(), ue = lbi->consuming_use_end();
           ui != ue; ui = lbi->consuming_use_begin()) {
        // We already checked that all of our uses here are end_borrow above.
        assert(isa<EndBorrowInst>(ui->getUser()) &&
               "Expected only end_borrow consuming uses");
        ui->set(newLoad);
      }
      // Any lifetime ending uses of our original load_borrow have been
      // rewritten by the previous loop to be on the new load_borrow. The reason
      // that we must do this is end_borrows only are placed on borrow
      // introducing guaranteed values and our unchecked_enum_data (unlike the
      // old load_borrow of the same type) is not one.
      newValue = localBuilder.createUncheckedEnumData(loc, newLoad, enumElt,
                                                      payloadType);
    }
    assert(newValue);

    // Replace all uses of the old load with the newValue and erase the old
    // load.
    replaceInstUsesWith(*svi, newValue);
    eraseInstFromFunction(*svi);
  }

  return eraseInstFromFunction(*tedai);
}

SILInstruction *SILCombiner::visitCondBranchInst(CondBranchInst *CBI) {
  // NOTE: All of the following optimizations do invalidates branches by
  // replacing the branches, but do not modify the underlying CFG properties
  // such as dominance and reachability.

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

    if (!hasOwnership()) {
      return Builder.createSwitchEnum(SEI->getLoc(), SEI->getEnumOperand(),
                                      DefaultBB, Cases);
    }

    // If we do have ownership, we need to do significantly more
    // work. Specifically:
    //
    // 1. Our select_enum may not be right next to our cond_br, so we need to
    //    lifetime extend our enum parameter to our switch_enum.
    //
    // 2. A switch_enum needs to propagate its operands into destination block
    //    arguments. We need to create those.
    //
    // 3. In each destination block, we need to create an argument and end the
    //    lifetime of that argument.
    SILValue selectEnumOperand = SEI->getEnumOperand();
    SILValue switchEnumOperand = selectEnumOperand;
    if (selectEnumOperand->getOwnershipKind() != OwnershipKind::None) {
      switchEnumOperand =
          makeCopiedValueAvailable(selectEnumOperand, Builder.getInsertionBB());
    }
    auto *switchEnum = Builder.createSwitchEnum(
        SEI->getLoc(), switchEnumOperand, DefaultBB, Cases);
    auto enumOperandType = SEI->getEnumOperand()->getType();
    for (auto pair : Cases) {
      // We only need to create the phi argument if our case doesn't have an
      // associated value.
      auto *enumEltDecl = pair.first;
      if (!enumEltDecl->hasAssociatedValues())
        continue;

      auto *block = pair.second;

      auto enumEltType =
          enumOperandType.getEnumElementType(enumEltDecl, block->getParent());
      auto *arg = switchEnum->createResult(block, enumEltType);
      SILBuilderWithScope innerBuilder(arg->getNextInstruction(), Builder);
      // The switch enum may change ownership resulting in Guaranteed or None.
      if (arg->getOwnershipKind() == OwnershipKind::Owned) {
        auto loc = RegularLocation::getAutoGeneratedLocation();
        innerBuilder.emitDestroyValueOperation(loc, arg);
      }
    }
    if (auto defaultArg = switchEnum->createDefaultResult()) {
      SILBuilderWithScope innerBuilder(defaultArg->getNextInstruction(), SEI);
      // The switch enum may change ownership resulting in Guaranteed or None.
      if (defaultArg->getOwnershipKind() == OwnershipKind::Owned) {
        auto loc = RegularLocation::getAutoGeneratedLocation();
        innerBuilder.emitDestroyValueOperation(loc, defaultArg);
      }
    }
    return switchEnum;
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
  if (TEI->getFieldIndex() == 1) {
    Builder.setCurrentDebugScope(TEI->getDebugScope());
    if (auto *BI = dyn_cast<BuiltinInst>(TEI->getOperand()))
      if (!canOverflow(BI))
        return Builder.createIntegerLiteral(TEI->getLoc(), TEI->getType(),
                                            APInt(1, 0));
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitFixLifetimeInst(FixLifetimeInst *fli) {
  // fix_lifetime(alloc_stack) -> fix_lifetime(load(alloc_stack))
  Builder.setCurrentDebugScope(fli->getDebugScope());
  if (auto *ai = dyn_cast<AllocStackInst>(fli->getOperand())) {
    if (fli->getOperand()->getType().isLoadable(*fli->getFunction())) {
      // load when ossa is disabled
      auto load = Builder.emitLoadBorrowOperation(fli->getLoc(), ai);
      Builder.createFixLifetime(fli->getLoc(), load);
      // no-op when ossa is disabled
      Builder.emitEndBorrowOperation(fli->getLoc(), load);
      return eraseInstFromFunction(*fli);
    }
  }
  return nullptr;
}

static Optional<SILType>
shouldReplaceCallByContiguousArrayStorageAnyObject(SILFunction &F,
                                                   CanType storageMetaTy) {
  auto metaTy = dyn_cast<MetatypeType>(storageMetaTy);
  if (!metaTy || metaTy->getRepresentation() != MetatypeRepresentation::Thick)
    return None;

  auto storageTy = metaTy.getInstanceType()->getCanonicalType();
  if (!storageTy->is_ContiguousArrayStorage())
    return None;

  auto boundGenericTy = dyn_cast<BoundGenericType>(storageTy);
  if (!boundGenericTy)
    return None;

  // On SwiftStdlib 5.7 we can replace the call.
  auto &ctxt = storageMetaTy->getASTContext();
  auto deployment = AvailabilityContext::forDeploymentTarget(ctxt);
  if (!deployment.isContainedIn(ctxt.getSwift57Availability()))
    return None;

  auto genericArgs = boundGenericTy->getGenericArgs();
  if (genericArgs.size() != 1)
    return None;

  auto ty = genericArgs[0]->getCanonicalType();
  if (!ty->getClassOrBoundGenericClass() && !ty->isObjCExistentialType())
    return None;

  auto anyObjectTy = ctxt.getAnyObjectType();
  auto arrayStorageTy =
      BoundGenericClassType::get(ctxt.get_ContiguousArrayStorageDecl(), nullptr,
                                 {anyObjectTy})
          ->getCanonicalType();
  return F.getTypeLowering(arrayStorageTy).getLoweredType();
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
  while (auto *UCI = dyn_cast<UpcastInst>(MDVal))
    MDVal = UCI->getOperand();

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
  } else if (auto *AI = dyn_cast<ApplyInst>(MDVal)) {
    SILFunction *SF = AI->getReferencedFunctionOrNull();
    if (!SF)
      return nullptr;

    if (!SF->hasSemanticsAttr(semantics::ARRAY_GET_CONTIGUOUSARRAYSTORAGETYPE))
      return nullptr;

    auto use = AI->getSingleUse();
    if (!use || use->getUser() != ARDI)
      return nullptr;

    auto storageTy = AI->getType().getASTType();
    // getContiguousArrayStorageType<SomeClass> =>
    //   ContiguousArrayStorage<AnyObject>
    auto instanceTy = shouldReplaceCallByContiguousArrayStorageAnyObject(
        *AI->getFunction(), storageTy);
    if (!instanceTy)
      return nullptr;
    NewInst = Builder.createAllocRef(
        ARDI->getLoc(), *instanceTy, ARDI->isObjC(), false,
        ARDI->getTailAllocatedTypes(), getCounts(ARDI));
    NewInst = Builder.createUncheckedRefCast(ARDI->getLoc(), NewInst,
                                             ARDI->getType());
    return NewInst;
  }
  if (NewInst && NewInst->getType() != ARDI->getType()) {
    // In case the argument was an upcast of the metatype, we have to upcast the
    // resulting reference.
    NewInst = Builder.createUpcast(ARDI->getLoc(), NewInst, ARDI->getType());
  }
  return NewInst;
}

/// Returns true if \p val is a literal instruction or a struct of a literal
/// instruction.
/// What we want to catch here is a UnsafePointer<Int8> of a string literal.
static bool isLiteral(SILValue val) {
  while (auto *str = dyn_cast<StructInst>(val)) {
    if (str->getNumOperands() != 1)
      return false;
    val = str->getOperand(0);
  }
  return isa<LiteralInst>(val);
}

SILInstruction *SILCombiner::visitMarkDependenceInst(MarkDependenceInst *mdi) {
  auto base = lookThroughOwnershipInsts(mdi->getBase());

  // Simplify the base operand of a MarkDependenceInst to eliminate unnecessary
  // instructions that aren't adding value.
  //
  // Conversions to Optional.Some(x) often happen here, this isn't important
  // for us, we can just depend on 'x' directly.
  if (auto *eiBase = dyn_cast<EnumInst>(base)) {
    if (eiBase->hasOperand()) {
      auto *use = &mdi->getOperandRef(MarkDependenceInst::Base);
      OwnershipReplaceSingleUseHelper helper(ownershipFixupContext,
                                             use, eiBase->getOperand());
      if (helper) {
        helper.perform();
        tryEliminateOnlyOwnershipUsedForwardingInst(eiBase,
                                                    getInstModCallbacks());
        return mdi;
      }
    }
  }

  // Conversions from a class to AnyObject also happen a lot, we can just depend
  // on the class reference.
  if (auto *ier = dyn_cast<InitExistentialRefInst>(base)) {
    auto *use = &mdi->getOperandRef(MarkDependenceInst::Base);
    OwnershipReplaceSingleUseHelper helper(ownershipFixupContext,
                                           use, ier->getOperand());
    if (helper) {
      helper.perform();
      tryEliminateOnlyOwnershipUsedForwardingInst(ier, getInstModCallbacks());
      return mdi;
    }
  }

  // Conversions from a class to AnyObject also happen a lot, we can just depend
  // on the class reference.
  if (auto *oeri = dyn_cast<OpenExistentialRefInst>(base)) {
    auto *use = &mdi->getOperandRef(MarkDependenceInst::Base);
    OwnershipReplaceSingleUseHelper helper(ownershipFixupContext,
                                           use, oeri->getOperand());
    if (helper) {
      helper.perform();
      tryEliminateOnlyOwnershipUsedForwardingInst(oeri, getInstModCallbacks());
      return mdi;
    }
  }

  // Sometimes due to specialization/builtins, we can get a mark_dependence
  // whose base is a trivial typed object. In such a case, the mark_dependence
  // does not have a meaning, so just eliminate it.
  {
    SILType baseType = base->getType();
    if (baseType.isObject()) {
      if ((hasOwnership() && base->getOwnershipKind() == OwnershipKind::None) ||
          baseType.isTrivial(*mdi->getFunction())) {
        SILValue value = mdi->getValue();
        replaceInstUsesWith(*mdi, value);
        return eraseInstFromFunction(*mdi);
      }
    }
  }
  
  if (isLiteral(mdi->getValue())) {
    // A literal lives forever, so no mark_dependence is needed.
    // This pattern can occur after StringOptimization when a utf8CString of
    // a literal is replace by the string_literal itself.
    replaceInstUsesWith(*mdi, mdi->getValue());
    return eraseInstFromFunction(*mdi);
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *cboi) {
  auto *urc = dyn_cast<UncheckedRefCastInst>(cboi->getOperand());
  if (!urc)
    return nullptr;

  auto type = urc->getOperand()->getType().getASTType();
  if (ClassDecl *cd = type->getClassOrBoundGenericClass()) {
    if (!cd->isObjC()) {
      auto int1Ty = SILType::getBuiltinIntegerType(1, Builder.getASTContext());
      SILValue zero = Builder.createIntegerLiteral(cboi->getLoc(), int1Ty, 0);
      return Builder.createTuple(cboi->getLoc(), {zero, zero});
    }
  }

  return nullptr;
}

/// Returns true if reference counting and debug_value users of a global_value
/// can be deleted.
static bool checkGlobalValueUsers(SILValue val,
                                  SmallVectorImpl<SILInstruction *> &toDelete) {
  for (Operand *use : val->getUses()) {
    SILInstruction *user = use->getUser();
    if (isa<RefCountingInst>(user) || isa<DebugValueInst>(user)) {
      toDelete.push_back(user);
      continue;
    }
    if (auto *upCast = dyn_cast<UpcastInst>(user)) {
      if (!checkGlobalValueUsers(upCast, toDelete))
        return false;
      continue;
    }
    // Projection instructions don't access the object header, so they don't
    // prevent deleting reference counting instructions.
    if (isa<RefElementAddrInst>(user) || isa<RefTailAddrInst>(user))
      continue;
    return false;
  }
  return true;
}

SILInstruction *
SILCombiner::legacyVisitGlobalValueInst(GlobalValueInst *globalValue) {
  // Delete all reference count instructions on a global_value if the only other
  // users are projections (ref_element_addr and ref_tail_addr).
  SmallVector<SILInstruction *, 8> toDelete;
  if (!checkGlobalValueUsers(globalValue, toDelete))
    return nullptr;
  for (SILInstruction *inst : toDelete) {
    eraseInstFromFunction(*inst);
  }
  return nullptr;

}

// Simplify `differentiable_function_extract` of `differentiable_function`.
//
// Before:
// %diff_func = differentiable_function(%orig, %jvp, %vjp)
// %orig' = differentiable_function_extract [original] %diff_func
// %jvp'  = differentiable_function_extract [jvp]      %diff_func
// %vjp'  = differentiable_function_extract [vjp]      %diff_func
//
// After:
// %orig' = %orig
// %jvp' = %jvp
// %vjp' = %vjp
SILInstruction *
SILCombiner::visitDifferentiableFunctionExtractInst(DifferentiableFunctionExtractInst *DFEI) {
  auto *DFI = dyn_cast<DifferentiableFunctionInst>(DFEI->getOperand());
  if (!DFI)
    return nullptr;

  if (!DFI->hasExtractee(DFEI->getExtractee()))
    return nullptr;

  SILValue newValue = DFI->getExtractee(DFEI->getExtractee());

  // If the type of the `differentiable_function` operand does not precisely
  // match the type of the original `differentiable_function_extract`,
  // create a `convert_function`.
  if (newValue->getType() != DFEI->getType()) {
    CanSILFunctionType opTI = newValue->getType().castTo<SILFunctionType>();
    CanSILFunctionType resTI = DFEI->getType().castTo<SILFunctionType>();
    if (!opTI->isABICompatibleWith(resTI, *DFEI->getFunction()).isCompatible())
      return nullptr;

    std::tie(newValue, std::ignore) =
      castValueToABICompatibleType(&Builder, DFEI->getLoc(),
                                   newValue,
                                   newValue->getType(), DFEI->getType(), {});
  }

  replaceInstUsesWith(*DFEI, newValue);
  return eraseInstFromFunction(*DFEI);
}

// Simplify `pack_length` with constant-length pack.
//
// Before:
// %len = pack_length $Pack{Int, String, Float}
//
// After:
// %len = integer_literal Builtin.Word, 3
SILInstruction *SILCombiner::visitPackLengthInst(PackLengthInst *PLI) {
  auto PackTy = PLI->getPackType();
  if (!PackTy->containsPackExpansionType()) {
    return Builder.createIntegerLiteral(PLI->getLoc(), PLI->getType(),
                                        PackTy->getNumElements());
  }

  return nullptr;
}

// Simplify `pack_element_get` where the index is a `dynamic_pack_index` with
// a constant operand.
//
// Before:
// %idx = integer_literal Builtin.Word, N
// %pack_idx = dynamic_pack_index %Pack{Int, String, Float}, %idx
// %pack_elt = pack_element_get %pack_value, %pack_idx, @element("...")
//
// After:
// %pack_idx = scalar_pack_index %Pack{Int, String, Float}, N
// %concrete_elt = pack_element_get %pack_value, %pack_idx, <<concrete type>>
// %pack_elt = unchecked_addr_cast %concrete_elt, @element("...")
SILInstruction *SILCombiner::visitPackElementGetInst(PackElementGetInst *PEGI) {
  auto *DPII = dyn_cast<DynamicPackIndexInst>(PEGI->getIndex());
  if (DPII == nullptr)
    return nullptr;

  auto PackTy = PEGI->getPackType();
  if (PackTy->containsPackExpansionType())
    return nullptr;

  auto *Op = dyn_cast<IntegerLiteralInst>(DPII->getOperand());
  if (Op == nullptr)
    return nullptr;

  if (Op->getValue().uge(PackTy->getNumElements()))
    return nullptr;

  unsigned Index = Op->getValue().getZExtValue();
  auto *SPII = Builder.createScalarPackIndex(
      DPII->getLoc(), Index, DPII->getIndexedPackType());

  auto ElementTy = SILType::getPrimitiveAddressType(
      PEGI->getPackType().getElementType(Index));
  auto *NewPEGI = Builder.createPackElementGet(
      PEGI->getLoc(), SPII, PEGI->getPack(),
      ElementTy);

  return Builder.createUncheckedAddrCast(
      PEGI->getLoc(), NewPEGI, PEGI->getElementType());
}

// Simplify `tuple_pack_element_addr` where the index is a `dynamic_pack_index`
//with a constant operand.
//
// Before:
// %idx = integer_literal Builtin.Word, N
// %pack_idx = dynamic_pack_index %Pack{Int, String, Float}, %idx
// %tuple_elt = tuple_pack_element_addr %tuple_value, %pack_idx, @element("...")
//
// After:
// %concrete_elt = tuple_element_addr %tuple_value, N
// %tuple_elt = unchecked_addr_cast %concrete_elt, @element("...")
SILInstruction *
SILCombiner::visitTuplePackElementAddrInst(TuplePackElementAddrInst *TPEAI) {
  auto *DPII = dyn_cast<DynamicPackIndexInst>(TPEAI->getIndex());
  if (DPII == nullptr)
    return nullptr;

  auto PackTy = DPII->getIndexedPackType();
  if (PackTy->containsPackExpansionType())
    return nullptr;

  auto *Op = dyn_cast<IntegerLiteralInst>(DPII->getOperand());
  if (Op == nullptr)
    return nullptr;

  if (Op->getValue().uge(PackTy->getNumElements()))
    return nullptr;

  unsigned Index = Op->getValue().getZExtValue();

  auto *TEAI = Builder.createTupleElementAddr(
      TPEAI->getLoc(), TPEAI->getTuple(), Index);
  return Builder.createUncheckedAddrCast(
      TPEAI->getLoc(), TEAI, TPEAI->getElementType());
}

// This is a hack. When optimizing a simple pack expansion expression which
// forms a tuple from a pack, like `(repeat each t)`, after the above
// peepholes we end up with:
//
// %src = unchecked_addr_cast %real_src, @element("...")
// %dst = unchecked_addr_cast %real_dst, @element("...")
// copy_addr %src, %dst
//
// Simplify this to
//
// copy_addr %real_src, %real_dst
//
// Assuming that %real_src and %real_dst have the same type.
//
// In this simple case, this eliminates the opened element archetype entirely.
// However, a more principled peephole would be to transform an
// open_pack_element with a scalar index by replacing all usages of the
// element archetype with a concrete type.
SILInstruction *
SILCombiner::visitCopyAddrInst(CopyAddrInst *CAI) {
  auto *Src = dyn_cast<UncheckedAddrCastInst>(CAI->getSrc());
  auto *Dst = dyn_cast<UncheckedAddrCastInst>(CAI->getDest());

  if (Src == nullptr || Dst == nullptr)
    return nullptr;

  if (Src->getType() != Dst->getType() ||
      !Src->getType().is<ElementArchetypeType>())
    return nullptr;

  if (Src->getOperand()->getType() != Dst->getOperand()->getType())
    return nullptr;

  return Builder.createCopyAddr(
      CAI->getLoc(), Src->getOperand(), Dst->getOperand(),
      CAI->isTakeOfSrc(), CAI->isInitializationOfDest());
}