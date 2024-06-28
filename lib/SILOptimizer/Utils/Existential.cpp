//===--- Existential.cpp - Functions analyzing existentials.  -------------===//
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

#include "swift/SILOptimizer/Utils/Existential.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace swift;

/// Determine InitExistential from global_addr.
/// %3 = global_addr @$P : $*SomeP
/// %4 = init_existential_addr %3 : $*SomeP, $SomeC
/// %5 = alloc_ref $SomeC
/// store %5 to %4 : $*SomeC
/// %8 = alloc_stack $SomeP
/// copy_addr %3 to [init] %8 : $*SomeP
/// %10 = apply %9(%3) : $@convention(thin) (@in_guaranteed SomeP)
/// Assumptions: Insn is a direct user of GAI (e.g., copy_addr or 
/// apply pattern shown above) and that a valid init_existential_addr 
/// value is returned only if it can prove that the value it 
/// initializes is the same value at the use point.
static InitExistentialAddrInst *
findInitExistentialFromGlobalAddr(GlobalAddrInst *GAI, SILInstruction *Insn) {
  /// Check for a single InitExistential usage for GAI and
  /// a simple dominance check: both InitExistential and Insn are in
  /// the same basic block and only one InitExistential
  /// occurs between GAI and Insn.
  llvm::SmallPtrSet<SILInstruction *, 8> IEUses;
  for (auto *Use : GAI->getUses()) {
    if (auto *InitExistential =
            dyn_cast<InitExistentialAddrInst>(Use->getUser())) {
      IEUses.insert(InitExistential);
    }
  }

  /// No InitExistential found in the basic block.
  if (IEUses.empty())
    return nullptr;

  /// Walk backwards from Insn instruction till the beginning of the basic block
  /// looking for an InitExistential.
  InitExistentialAddrInst *SingleIE = nullptr;
  for (auto II = Insn->getIterator().getReverse(),
            IE = Insn->getParent()->rend();
       II != IE; ++II) {
    if (!IEUses.count(&*II))
      continue;
    if (SingleIE)
      return nullptr;

    SingleIE = cast<InitExistentialAddrInst>(&*II);
  }
  return SingleIE;
}

/// Returns the instruction that initializes the given stack address. This is
/// currently either a init_existential_addr, unconditional_checked_cast_addr,
/// store, or copy_addr (if the instruction initializing the source of the copy
/// cannot be determined). Returns nullptr if the initializer does not dominate
/// the alloc_stack user \p ASIUser.  If the value is copied from another stack
/// location, \p isCopied is set to true.
///
/// allocStackAddr may either itself be an AllocStackInst or an
/// InitEnumDataAddrInst that projects the value of an AllocStackInst.
static SILInstruction *getStackInitInst(SILValue allocStackAddr,
                                        SILInstruction *ASIUser,
                                        bool &isCopied) {
  SILInstruction *SingleWrite = nullptr;
  // Check that this alloc_stack is initialized only once.
  for (auto Use : allocStackAddr->getUses()) {
    auto *User = Use->getUser();

    // Ignore instructions which don't write to the stack location.
    // Also ignore ASIUser (only kicks in if ASIUser is the original apply).
    if (isa<DeallocStackInst>(User) ||
        DebugValueInst::hasAddrVal(User) ||
        isa<DestroyAddrInst>(User) || isa<WitnessMethodInst>(User) ||
        isa<DeinitExistentialAddrInst>(User) ||
        OpenExistentialAddrInst::isRead(User) || User == ASIUser) {
      continue;
    }
    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      if (CAI->getDest() == allocStackAddr) {
        if (SingleWrite)
          return nullptr;
        SingleWrite = CAI;
        isCopied = true;
      }
      continue;
    }
    // An unconditional_checked_cast_addr also copies a value into this addr.
    if (auto *UCCAI = dyn_cast<UnconditionalCheckedCastAddrInst>(User)) {
      if (UCCAI->getDest() == allocStackAddr) {
        if (SingleWrite)
          return nullptr;
        SingleWrite = UCCAI;
        isCopied = true;
      }
      continue;
    }
    if (auto *store = dyn_cast<StoreInst>(User)) {
      if (store->getDest() == allocStackAddr) {
        if (SingleWrite)
          return nullptr;
        SingleWrite = store;
        // When we support OSSA here, we need to insert a new copy of the value
        // before `store` (and make sure that the copy is destroyed when
        // replacing the apply operand).
        assert(store->getOwnershipQualifier() ==
               StoreOwnershipQualifier::Unqualified);
      }
      continue;
    }
    if (isa<InitExistentialAddrInst>(User)) {
      if (SingleWrite)
        return nullptr;
      SingleWrite = User;
      continue;
    }
    if (isa<ApplyInst>(User) || isa<TryApplyInst>(User)) {
      // Ignore function calls which do not write to the stack location.
      auto Conv = FullApplySite(User).getArgumentConvention(*Use);
      if (Conv != SILArgumentConvention::Indirect_In &&
          Conv != SILArgumentConvention::Indirect_In_Guaranteed)
        return nullptr;
      continue;
    }
    // Bail if there is any unknown (and potentially writing) instruction.
    return nullptr;
  }
  if (!SingleWrite)
    return nullptr;

  // A very simple dominance check. As ASI is an operand of ASIUser,
  // SingleWrite dominates ASIUser if it is in the same block as ASI or
  // ASIUser. (SingleWrite can't occur after ASIUser because the address would
  // be uninitialized on use).
  //
  // If allocStack holds an Optional, then ASI is an InitEnumDataAddrInst
  // projection and not strictly an operand of ASIUser. We rely on the guarantee
  // that this InitEnumDataAddrInst must occur before the InjectEnumAddrInst
  // that was the source of the existential address.
  SILBasicBlock *BB = SingleWrite->getParent();
  if (BB != allocStackAddr->getParentBlock() && BB != ASIUser->getParent())
    return nullptr;

  if (auto *store = dyn_cast<StoreInst>(SingleWrite))
    return store;

  if (auto *IE = dyn_cast<InitExistentialAddrInst>(SingleWrite))
    return IE;

  if (auto *UCCA = dyn_cast<UnconditionalCheckedCastAddrInst>(SingleWrite)) {
    assert(isCopied && "isCopied not set for a unconditional_checked_cast_addr");
    return UCCA;
  }
  auto *CAI = cast<CopyAddrInst>(SingleWrite);
  assert(isCopied && "isCopied not set for a copy_addr");
  // Attempt to recurse to find a concrete type.
  if (auto *ASI = dyn_cast<AllocStackInst>(CAI->getSrc()))
    return getStackInitInst(ASI, CAI, isCopied);

  // Peek through a stack location holding an Enum.
  //   %stack_adr = alloc_stack
  //   %data_adr  = init_enum_data_addr %stk_adr
  //   %enum_adr  = inject_enum_addr %stack_adr
  //   %copy_src  = unchecked_take_enum_data_addr %enum_adr
  // Replace %copy_src with %data_adr and recurse.
  //
  // TODO: a general Optional elimination sil-combine could
  // supersede this check.
  if (auto *UTEDAI = dyn_cast<UncheckedTakeEnumDataAddrInst>(CAI->getSrc())) {
    if (InitEnumDataAddrInst *IEDAI = findInitAddressForTrivialEnum(UTEDAI))
      return getStackInitInst(IEDAI, CAI, isCopied);
  }

  // Check if the CAISrc is a global_addr.
  if (auto *GAI = dyn_cast<GlobalAddrInst>(CAI->getSrc()))
    return findInitExistentialFromGlobalAddr(GAI, CAI);

  // If the source of the copy cannot be determined, return the copy itself
  // because the caller may have special handling for the source address.
  return CAI;
}

/// Check if the given operand originates from a recognized OpenArchetype
/// instruction. If so, return the Opened, otherwise return nullptr.
OpenedArchetypeInfo::OpenedArchetypeInfo(Operand &use) {
  SILValue openedVal = use.get();
  SILInstruction *user = use.getUser();
  if (auto *instance = dyn_cast<AllocStackInst>(openedVal)) {
    // Handle:
    //   %opened = open_existential_addr
    //   %instance = alloc $opened
    //   <copy|store> %opened to %stack
    //   <opened_use> %instance
    if (auto *initI = getStackInitInst(instance, user, isOpenedValueCopied)) {
      // init_existential_addr isn't handled here because it isn't considered an
      // "opened" archtype. init_existential_addr should be handled by
      // ConcreteExistentialInfo.

      if (auto *CAI = dyn_cast<CopyAddrInst>(initI))
        openedVal = CAI->getSrc();
      if (auto *store = dyn_cast<StoreInst>(initI))
        openedVal = store->getSrc();
    }
  }
  if (auto *Open = dyn_cast<OpenExistentialAddrInst>(openedVal)) {
    OpenedArchetype = Open->getType().castTo<OpenedArchetypeType>();
    OpenedArchetypeValue = Open;
    ExistentialValue = Open->getOperand();
    return;
  }
  if (auto *Open = dyn_cast<OpenExistentialRefInst>(openedVal)) {
    OpenedArchetype = Open->getType().castTo<OpenedArchetypeType>();
    OpenedArchetypeValue = Open;
    ExistentialValue = Open->getOperand();
    return;
  }
  if (auto *Open = dyn_cast<OpenExistentialMetatypeInst>(openedVal)) {
    auto Ty = Open->getType().getASTType();
    while (auto Metatype = dyn_cast<MetatypeType>(Ty))
      Ty = Metatype.getInstanceType();
    OpenedArchetype = cast<OpenedArchetypeType>(Ty);
    OpenedArchetypeValue = Open;
    ExistentialValue = Open->getOperand();
  }
}

/// Initialize ExistentialSubs from the given conformance list, using the
/// already initialized ExistentialType and ConcreteType.
void ConcreteExistentialInfo::initializeSubstitutionMap(
    ArrayRef<ProtocolConformanceRef> ExistentialConformances, SILModule *M) {

  // Construct a single-generic-parameter substitution map directly to the
  // ConcreteType with this existential's full list of conformances.
  //
  // NOTE: getOpenedExistentialSignature() generates the signature for passing an
  // opened existential as a generic parameter. No opened archetypes are
  // actually involved here--the API is only used as a convenient way to create
  // a substitution map. Since opened archetypes have different conformances
  // than their corresponding existential, ExistentialConformances needs to be
  // filtered when using it with this (phony) generic signature.
  CanGenericSignature ExistentialSig =
      M->getASTContext().getOpenedExistentialSignature(ExistentialType,
                                                       GenericSignature());
  ExistentialSubs = SubstitutionMap::get(
      ExistentialSig, [&](SubstitutableType *type) { return ConcreteType; },
      [&](CanType /*depType*/, Type /*replaceType*/,
          ProtocolDecl *proto) -> ProtocolConformanceRef {
        // Directly providing ExistentialConformances to the SubstitutionMap will
        // fail because of the mismatch between opened archetype conformance and
        // existential value conformance. Instead, provide a conformance lookup
        // function that pulls only the necessary conformances out of
        // ExistentialConformances. This assumes that existential conformances
        // are a superset of opened archetype conformances.
        auto iter =
            llvm::find_if(ExistentialConformances,
                          [&](const ProtocolConformanceRef &conformance) {
                            return conformance.getRequirement() == proto;
                          });
        assert(iter != ExistentialConformances.end() && "missing conformance");
        return *iter;
      });
  assert(isValid());
}

/// If the ConcreteType is an opened existential, also initialize
/// ConcreteTypeDef to the definition of that type.
void ConcreteExistentialInfo::initializeConcreteTypeDef(
    SILInstruction *typeConversionInst) {
  if (!ConcreteType->isOpenedExistential())
    return;

  assert(isValid());

  // If the concrete type is another existential, we're "forwarding" an
  // opened existential type, so we must keep track of the original
  // defining instruction.
  if (!typeConversionInst->getTypeDependentOperands().empty()) {
    ConcreteTypeDef = cast<SingleValueInstruction>(
        typeConversionInst->getTypeDependentOperands()[0].get());
    return;
  }

  auto typeOperand =
      cast<InitExistentialMetatypeInst>(typeConversionInst)->getOperand();
  assert(typeOperand->getType().hasOpenedExistential()
         && "init_existential is supposed to have a typedef operand");
  ConcreteTypeDef = cast<SingleValueInstruction>(typeOperand);
}

/// Construct this ConcreteExistentialInfo based on the given existential use.
///
/// Finds the init_existential, or an address with concrete type used to
/// initialize the given \p openedUse. If the value is copied
/// from another stack location, \p isCopied is set to true.
///
/// If successful, ConcreteExistentialInfo will be valid upon return, with the
/// following fields assigned:
/// - ExistentialType
/// - isCopied
/// - ConcreteType
/// - ConcreteValue
/// - ConcreteTypeDef
/// - ExistentialSubs
ConcreteExistentialInfo::ConcreteExistentialInfo(SILValue existential,
                                                 SILInstruction *user) {
  if (existential->getType().isAddress()) {
    auto *ASI = dyn_cast<AllocStackInst>(existential);
    if (!ASI)
      return;

    SILInstruction *stackInit =
        getStackInitInst(ASI, user, isConcreteValueCopied);
    if (!stackInit)
      return;

    if (auto *IE = dyn_cast<InitExistentialAddrInst>(stackInit)) {
      ExistentialType = IE->getOperand()->getType().getASTType();
      ConcreteType = IE->getFormalConcreteType();
      ConcreteValue = IE;
      initializeSubstitutionMap(IE->getConformances(), &IE->getModule());
      initializeConcreteTypeDef(IE);
      return;
    }
    // TODO: Once we have a way to introduce more constrained archetypes, handle
    // any unconditional_checked_cast that wasn't already statically eliminated.
    //
    // Unexpected stack write.
    return;
  }

  if (auto *IER = dyn_cast<InitExistentialRefInst>(existential)) {
    ExistentialType = IER->getType().getASTType();
    ConcreteType = IER->getFormalConcreteType();
    ConcreteValue = IER->getOperand();
    initializeSubstitutionMap(IER->getConformances(), &IER->getModule());
    initializeConcreteTypeDef(IER);
    return;
  }

  if (auto *IEM = dyn_cast<InitExistentialMetatypeInst>(existential)) {
    ExistentialType = IEM->getType().getASTType();
    ConcreteValue = IEM->getOperand();
    ConcreteType = ConcreteValue->getType().getASTType();
    while (auto InstanceType =
               dyn_cast<ExistentialMetatypeType>(ExistentialType)) {
      ExistentialType = InstanceType.getInstanceType();
      ConcreteType = cast<MetatypeType>(ConcreteType).getInstanceType();
    }
    initializeSubstitutionMap(IEM->getConformances(), &IEM->getModule());
    initializeConcreteTypeDef(IEM);
    return;
  }
  // Unrecognized opened existential producer.
  return;
}

/// Initialize a ConcreteExistentialInfo based on a concrete type and protocol
/// declaration that has already been computed via whole module type
/// inference. A cast instruction will be introduced to produce the concrete
/// value from the opened value.
///
/// The simpler constructor taking only the existential value is preferred
/// because it generates simpler SIL and does not require an extra
/// cast. However, if that constructor fails to produce a valid
/// ConcreteExistentialInfo, this constructor may succeed because it doesn't
/// needs to rediscover the whole-module inferred ConcreteTypeCandidate.
ConcreteExistentialInfo::ConcreteExistentialInfo(SILValue existential,
                                                 SILInstruction *user,
                                                 CanType ConcreteTypeCandidate,
                                                 ProtocolDecl *Protocol) {
  SILModule *M = existential->getModule();

  // We have the open_existential; we still need the conformance.
  auto ConformanceRef =
      M->getSwiftModule()->checkConformance(ConcreteTypeCandidate, Protocol);
  if (ConformanceRef.isInvalid())
    return;

  // Assert that the conformance is complete.
  auto *ConcreteConformance = ConformanceRef.getConcrete();
  assert(ConcreteConformance->isComplete());

  ConcreteType = ConcreteTypeCandidate;
  // There is no ConcreteValue in this case.

  /// Determine the ExistentialConformances and SubstitutionMap.
  ExistentialType = Protocol->getDeclaredInterfaceType()->getCanonicalType();
  initializeSubstitutionMap(ProtocolConformanceRef(ConcreteConformance), M);

  assert(isValid());
}

ConcreteOpenedExistentialInfo::ConcreteOpenedExistentialInfo(Operand &use)
    : OAI(use) {
  if (!OAI.isValid())
    return;

  CEI.emplace(OAI.ExistentialValue, OAI.OpenedArchetypeValue);
  if (!CEI->isValid()) {
    CEI.reset();
    return;
  }
  CEI->isConcreteValueCopied |= OAI.isOpenedValueCopied;
}

ConcreteOpenedExistentialInfo::ConcreteOpenedExistentialInfo(
    Operand &use, CanType concreteType, ProtocolDecl *protocol)
    : OAI(use) {
  if (!OAI.isValid())
    return;

  CEI.emplace(OAI.ExistentialValue, OAI.OpenedArchetypeValue, concreteType,
              protocol);
  if (!CEI->isValid()) {
    CEI.reset();
    return;
  }
  CEI->isConcreteValueCopied |= OAI.isOpenedValueCopied;
}

void LLVM_ATTRIBUTE_USED OpenedArchetypeInfo::dump() const {
  if (!isValid()) {
    llvm::dbgs() << "invalid OpenedArchetypeInfo\n";
    return;
  }
  llvm::dbgs() << "OpendArchetype: ";
  OpenedArchetype->dump(llvm::dbgs());
  llvm::dbgs() << "OpendArchetypeValue: ";
  OpenedArchetypeValue->dump();
  llvm::dbgs() << (isOpenedValueCopied ? "copied " : "") << "ExistentialValue: ";
  ExistentialValue->dump();
}

void LLVM_ATTRIBUTE_USED ConcreteExistentialInfo::dump() const {
  llvm::dbgs() << "ExistentialType: ";
  ExistentialType->dump(llvm::dbgs());
  llvm::dbgs() << "ConcreteType: ";
  ConcreteType->dump(llvm::dbgs());
  llvm::dbgs() << (isConcreteValueCopied ? "copied " : "") << "ConcreteValue: ";
  ConcreteValue->dump();
  if (ConcreteTypeDef) {
    llvm::dbgs() << "ConcreteTypeDef: ";
    ConcreteTypeDef->dump();
  }
  ExistentialSubs.dump(llvm::dbgs());
  llvm::dbgs() << '\n';
}

void LLVM_ATTRIBUTE_USED ConcreteOpenedExistentialInfo::dump() const {
  OAI.dump();
  if (CEI) {
    CEI->dump();
  } else {
    llvm::dbgs() << "no ConcreteExistentialInfo\n";
  }
}
