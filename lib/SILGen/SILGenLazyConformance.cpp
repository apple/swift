//===--- SILGenLazyConformance.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file forces emission of lazily-generated ClangImporter-synthesized
//  conformances.
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "swift/AST/Decl.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/Basic/Assertions.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILVisitor.h"

using namespace swift;
using namespace swift::Lowering;

void SILGenModule::useConformance(ProtocolConformanceRef conformanceRef) {
  // If the conformance is invalid, crash deterministically even in noassert
  // builds.
  if (conformanceRef.isInvalid()) {
    // When lazy type checking is enabled, a conformance may only be diagnosed
    // as invalid during SILGen. Ignore it instead of asserting.
    auto &ctx = getASTContext();
    if (ctx.TypeCheckerOpts.EnableLazyTypecheck && ctx.hadError())
      return;

    llvm::report_fatal_error("Invalid conformance in type-checked AST");
  }

  // We don't need to emit dependent conformances.
  if (conformanceRef.isAbstract())
    return;

  // Recursively visit pack conformances.
  if (conformanceRef.isPack()) {
    auto *packConformance = conformanceRef.getPack();

    for (auto patternConformanceRef : packConformance->getPatternConformances())
      useConformance(patternConformanceRef);

    return;
  }

  auto conformance = conformanceRef.getConcrete();

  // Always look through inherited conformances.
  if (auto *inherited = dyn_cast<InheritedProtocolConformance>(conformance))
    conformance = inherited->getInheritedConformance();

  // Emit any conformances implied by conditional requirements.
  if (auto *specialized = dyn_cast<SpecializedProtocolConformance>(conformance)) {
    useConformancesFromSubstitutions(specialized->getSubstitutionMap());
    conformance = specialized->getGenericConformance();
  }

  // Get the normal conformance. If we don't have one, this is a self
  // conformance, which we can ignore.
  auto normal = dyn_cast<NormalProtocolConformance>(conformance);
  if (normal == nullptr)
    return;

  // If this conformance was not synthesized, we're not going to be emitting
  // it lazily either, so we can avoid doing anything below.
  if (!normal->isSynthesized())
    return;

  // If we already emitted this witness table, we don't need to track the fact
  // we need it.
  if (emittedWitnessTables.count(normal))
    return;

  // Check if we already forced this witness table but haven't emitted it yet.
  if (!forcedConformances.insert(normal).second)
    return;

  pendingConformances.push_back(normal);
}

void SILGenModule::useConformancesFromSubstitutions(
                                                const SubstitutionMap subs) {
  for (auto conf : subs.getConformances())
    useConformance(conf);
}

void SILGenModule::useConformancesFromType(CanType type) {
  if (!usedConformancesFromTypes.insert(type.getPointer()).second)
    return;

  type.visit([&](Type t) {
    auto *decl = t->getAnyNominal();
    if (!decl)
      return;

    if (isa<ProtocolDecl>(decl))
      return;

    auto genericSig = decl->getGenericSignature();
    if (!genericSig)
      return;

    auto subMap = t->getContextSubstitutionMap(SwiftModule, decl);
    useConformancesFromSubstitutions(subMap);
    return;
  });
}

void SILGenModule::useConformancesFromObjectiveCType(CanType type) {
  if (!usedConformancesFromObjectiveCTypes.insert(type.getPointer()).second)
    return;

  auto &ctx = getASTContext();
  auto objectiveCBridgeable = ctx.getProtocol(
      KnownProtocolKind::ObjectiveCBridgeable);
  auto bridgedStoredNSError = ctx.getProtocol(
      KnownProtocolKind::BridgedStoredNSError);
  if (!objectiveCBridgeable && !bridgedStoredNSError)
    return;

  type.visit([&](Type t) {
    auto *decl = t->getAnyNominal();
    if (!decl)
      return;

    if (!isa<ClangModuleUnit>(decl->getModuleScopeContext()))
      return;

    if (objectiveCBridgeable) {
      if (auto subConformance =
              SwiftModule->lookupConformance(t, objectiveCBridgeable))
        useConformance(subConformance);
    }

    if (bridgedStoredNSError) {
      if (auto subConformance =
              SwiftModule->lookupConformance(t, bridgedStoredNSError))
        useConformance(subConformance);
    }
  });
}

/// A visitor class that tries to guess which SIL instructions can cause
/// IRGen to emit references to witness tables. This is used to emit
/// ClangImporter-synthesized conformances lazily.
///
/// In the long run, we'll instead have IRGen directly ask SILGen to
/// generate a witness table when needed, so that we don't have to do
/// any "guessing" here.
class LazyConformanceEmitter : public SILInstructionVisitor<LazyConformanceEmitter> {
  SILGenModule &SGM;

public:
  LazyConformanceEmitter(SILGenModule &SGM) : SGM(SGM) {}

  void visitAllocExistentialBoxInst(AllocExistentialBoxInst *AEBI) {
    SGM.useConformancesFromType(AEBI->getFormalConcreteType());
    SGM.useConformancesFromObjectiveCType(AEBI->getFormalConcreteType());
    for (auto conformance : AEBI->getConformances())
      SGM.useConformance(conformance);
  }

  void visitAllocGlobalInst(AllocGlobalInst *AGI) {
    SGM.useConformancesFromType(
        AGI->getReferencedGlobal()->getLoweredType().getASTType());
  }

  void visitAllocRefInst(AllocRefInst *ARI) {
    SGM.useConformancesFromType(ARI->getType().getASTType());
  }

  void visitAllocStackInst(AllocStackInst *ASI) {
    SGM.useConformancesFromType(ASI->getType().getASTType());
  }

  void visitApplyInst(ApplyInst *AI) {
    SGM.useConformancesFromObjectiveCType(AI->getSubstCalleeType());
    SGM.useConformancesFromSubstitutions(AI->getSubstitutionMap());
  }

  void visitBeginApplyInst(BeginApplyInst *BAI) {
    SGM.useConformancesFromObjectiveCType(BAI->getSubstCalleeType());
    SGM.useConformancesFromSubstitutions(BAI->getSubstitutionMap());
  }

  void visitBuiltinInst(BuiltinInst *BI) {
    SGM.useConformancesFromSubstitutions(BI->getSubstitutions());
  }

  void visitCheckedCastBranchInst(CheckedCastBranchInst *CCBI) {
    SGM.useConformancesFromType(CCBI->getSourceFormalType());
    SGM.useConformancesFromType(CCBI->getTargetFormalType());
    SGM.useConformancesFromObjectiveCType(CCBI->getSourceFormalType());
    SGM.useConformancesFromObjectiveCType(CCBI->getTargetFormalType());
  }

  void visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CCABI) {
    SGM.useConformancesFromType(CCABI->getSourceFormalType());
    SGM.useConformancesFromType(CCABI->getTargetFormalType());
    SGM.useConformancesFromObjectiveCType(CCABI->getSourceFormalType());
    SGM.useConformancesFromObjectiveCType(CCABI->getTargetFormalType());
  }

  void visitCopyAddrInst(CopyAddrInst *CAI) {
    SGM.useConformancesFromType(CAI->getSrc()->getType().getASTType());
    SGM.useConformancesFromType(CAI->getDest()->getType().getASTType());
  }

  void visitMarkUnresolvedMoveAddrInst(MarkUnresolvedMoveAddrInst *MAI) {
    SGM.useConformancesFromType(MAI->getSrc()->getType().getASTType());
    SGM.useConformancesFromType(MAI->getDest()->getType().getASTType());
  }

  void visitCopyValueInst(CopyValueInst *CVI) {
    SGM.useConformancesFromType(CVI->getOperand()->getType().getASTType());
  }

  void visitDestroyAddrInst(DestroyAddrInst *DAI) {
    SGM.useConformancesFromType(DAI->getOperand()->getType().getASTType());
  }

  void visitDestroyValueInst(DestroyValueInst *DVI) {
    SGM.useConformancesFromType(DVI->getOperand()->getType().getASTType());
  }

  void visitGlobalAddrInst(GlobalAddrInst *GAI) {
    SGM.useConformancesFromType(
        GAI->getReferencedGlobal()->getLoweredType().getASTType());
  }

  void visitGlobalValueInst(GlobalValueInst *GVI) {
    SGM.useConformancesFromType(
        GVI->getReferencedGlobal()->getLoweredType().getASTType());
  }

  void visitKeyPathInst(KeyPathInst *KPI) {
    SGM.useConformancesFromSubstitutions(KPI->getSubstitutions());
  }

  void visitInitEnumDataAddrInst(InitEnumDataAddrInst *IEDAI) {
    SGM.useConformancesFromType(
        IEDAI->getOperand()->getType().getASTType());
  }

  void visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI) {
    SGM.useConformancesFromType(IEAI->getOperand()->getType().getASTType());
  }

  void visitInitExistentialAddrInst(InitExistentialAddrInst *IEAI) {
    SGM.useConformancesFromType(IEAI->getFormalConcreteType());
    SGM.useConformancesFromObjectiveCType(IEAI->getFormalConcreteType());
    for (auto conformance : IEAI->getConformances())
      SGM.useConformance(conformance);
  }

  void visitInitExistentialMetatypeInst(InitExistentialMetatypeInst *IEMI) {
    SGM.useConformancesFromType(IEMI->getOperand()->getType().getASTType());
    for (auto conformance : IEMI->getConformances())
      SGM.useConformance(conformance);
  }

  void visitInitExistentialRefInst(InitExistentialRefInst *IERI) {
    SGM.useConformancesFromType(IERI->getFormalConcreteType());
    SGM.useConformancesFromObjectiveCType(IERI->getFormalConcreteType());
    for (auto conformance : IERI->getConformances())
      SGM.useConformance(conformance);
  }

  void visitInitExistentialValueInst(InitExistentialValueInst *IEVI) {
    SGM.useConformancesFromType(IEVI->getFormalConcreteType());
    SGM.useConformancesFromObjectiveCType(IEVI->getFormalConcreteType());
    for (auto conformance : IEVI->getConformances())
      SGM.useConformance(conformance);
  }

  void visitMetatypeInst(MetatypeInst *MI) {
    SGM.useConformancesFromType(MI->getType().getASTType());
  }

  void visitPartialApplyInst(PartialApplyInst *PAI) {
    SGM.useConformancesFromObjectiveCType(PAI->getSubstCalleeType());
    SGM.useConformancesFromSubstitutions(PAI->getSubstitutionMap());
  }

  void visitSelectEnumAddrInst(SelectEnumAddrInst *SEAI) {
    SGM.useConformancesFromType(
        SEAI->getEnumOperand()->getType().getASTType());
  }

  void visitStructElementAddrInst(StructElementAddrInst *SEAI) {
    SGM.useConformancesFromType(SEAI->getOperand()->getType().getASTType());
  }

  void visitTryApplyInst(TryApplyInst *TAI) {
    SGM.useConformancesFromObjectiveCType(TAI->getSubstCalleeType());
    SGM.useConformancesFromSubstitutions(TAI->getSubstitutionMap());
  }

  void visitTupleElementAddrInst(TupleElementAddrInst *TEAI) {
    SGM.useConformancesFromType(TEAI->getOperand()->getType().getASTType());
  }

  void visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
    SGM.useConformancesFromType(UCCI->getSourceFormalType());
    SGM.useConformancesFromType(UCCI->getTargetFormalType());
    SGM.useConformancesFromObjectiveCType(UCCI->getSourceFormalType());
    SGM.useConformancesFromObjectiveCType(UCCI->getTargetFormalType());
  }

  void visitUnconditionalCheckedCastAddrInst(UnconditionalCheckedCastAddrInst *UCCAI) {
    SGM.useConformancesFromType(UCCAI->getSourceFormalType());
    SGM.useConformancesFromType(UCCAI->getTargetFormalType());
    SGM.useConformancesFromObjectiveCType(UCCAI->getSourceFormalType());
    SGM.useConformancesFromObjectiveCType(UCCAI->getTargetFormalType());
  }

  void visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *UTEDAI) {
    SGM.useConformancesFromType(UTEDAI->getOperand()->getType().getASTType());
  }

  void visitWitnessMethodInst(WitnessMethodInst *WMI) {
    SGM.useConformance(WMI->getConformance());
  }

  void visitSILInstruction(SILInstruction *I) {}
};

void SILGenModule::emitLazyConformancesForFunction(SILFunction *F) {
  LazyConformanceEmitter emitter(*this);

  for (auto &BB : *F)
    for (auto &I : BB)
      emitter.visit(&I);
}

void SILGenModule::emitLazyConformancesForType(NominalTypeDecl *NTD) {
  auto genericSig = NTD->getGenericSignature();

  for (auto reqt : genericSig.getRequirements()) {
    if (reqt.getKind() != RequirementKind::Layout)
      useConformancesFromType(reqt.getSecondType()->getCanonicalType());
  }

  if (auto *ED = dyn_cast<EnumDecl>(NTD)) {
    for (auto *EED : ED->getAllElements()) {
      if (EED->hasAssociatedValues()) {
        useConformancesFromType(EED->getArgumentInterfaceType()
                                   ->getReducedType(genericSig));
      }
    }
  }

  if (isa<StructDecl>(NTD) || isa<ClassDecl>(NTD)) {
    for (auto *VD : NTD->getStoredProperties()) {
      useConformancesFromType(VD->getValueInterfaceType()
                                ->getReducedType(genericSig));
    }
  }

  if (auto *CD = dyn_cast<ClassDecl>(NTD))
    if (auto superclass = CD->getSuperclass())
      useConformancesFromType(superclass->getReducedType(genericSig));

  if (auto *PD = dyn_cast<ProtocolDecl>(NTD)) {
    for (auto reqt : PD->getRequirementSignature().getRequirements()) {
      if (reqt.getKind() != RequirementKind::Layout)
        useConformancesFromType(reqt.getSecondType()->getCanonicalType());
    }
  }
}
