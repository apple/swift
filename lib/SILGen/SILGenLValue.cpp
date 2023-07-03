//===--- SILGenLValue.cpp - Constructs logical lvalues for SILGen ---------===//
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
//
// Emission of l-value expressions and basic operations on them.
//
//===----------------------------------------------------------------------===//

#include "ASTVisitor.h"
#include "ArgumentScope.h"
#include "ArgumentSource.h"
#include "Conversion.h"
#include "ExecutorBreadcrumb.h"
#include "Initialization.h"
#include "LValue.h"
#include "ManagedValue.h"
#include "RValue.h"
#include "SILGen.h"
#include "SILGenFunction.h"
#include "Scope.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//

namespace {

struct LValueWritebackCleanup : Cleanup {
  FormalEvaluationContext::stable_iterator Depth;

  LValueWritebackCleanup() : Depth() {}

  void emit(SILGenFunction &SGF, CleanupLocation loc,
            ForUnwind_t forUnwind) override {
    FullExpr scope(SGF.Cleanups, loc);

    // TODO: honor forUnwind!
    auto &evaluation = getEvaluation(SGF);
    evaluation.performWriteback(SGF, /*isFinal*/ false);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "LValueWritebackCleanup\n"
                 << "State: " << getState() << " Depth: " << Depth.getDepth()
                 << "\n";
#endif
  }

private:
  ExclusiveBorrowFormalAccess &getEvaluation(SILGenFunction &SGF) {
    auto &evaluation = *SGF.FormalEvalContext.find(Depth);
    assert(evaluation.getKind() == FormalAccess::Exclusive);
    return static_cast<ExclusiveBorrowFormalAccess &>(evaluation);
  }
};

} // end anonymous namespace

/// Push a writeback onto the current LValueWriteback stack.
static void pushWriteback(SILGenFunction &SGF,
                          SILLocation loc,
                          std::unique_ptr<LogicalPathComponent> &&comp,
                          ManagedValue base,
                          MaterializedLValue materialized) {
  assert(SGF.isInFormalEvaluationScope());

  // Push a cleanup to execute the writeback consistently.
  auto &context = SGF.FormalEvalContext;
  LValueWritebackCleanup &cleanup =
      SGF.Cleanups.pushCleanup<LValueWritebackCleanup>();
  CleanupHandle handle = SGF.Cleanups.getTopCleanup();

  context.push<ExclusiveBorrowFormalAccess>(loc, std::move(comp), base,
                                            materialized, handle);
  cleanup.Depth = context.stable_begin();
}

static bool areCertainlyEqualArgumentLists(const ArgumentList *l1,
                                           const ArgumentList *l2);

void ExclusiveBorrowFormalAccess::diagnoseConflict(
                                    const ExclusiveBorrowFormalAccess &rhs,
                                    SILGenFunction &SGF) const {
  // If the two writebacks we're comparing are of different kinds (e.g.
  // ownership conversion vs a computed property) then they aren't the
  // same and thus cannot conflict.
  if (component->getKind() != rhs.component->getKind())
    return;

  // If the lvalues don't have the same base value (possibly null), then
  // they aren't the same.  Note that this is the primary source of false
  // negative for this diagnostic.
  SILValue lhsBaseValue = base.getValue(), rhsBaseValue = rhs.base.getValue();
  if (lhsBaseValue != rhsBaseValue &&
      (!lhsBaseValue || !rhsBaseValue ||
       !RValue::areObviouslySameValue(lhsBaseValue, rhsBaseValue))) {
    return;
  }

  auto lhsStorage = component->getAccessStorage();
  if (!lhsStorage) return;

  auto rhsStorage = rhs.component->getAccessStorage();
  if (!rhsStorage) return;

  // If the decls match, then this could conflict.
  if (lhsStorage->Storage != rhsStorage->Storage ||
      !lhsStorage->Storage ||
      lhsStorage->IsSuper != rhsStorage->IsSuper)
    return;

  assert((lhsStorage->Indices != nullptr) == (rhsStorage->Indices != nullptr));

  auto storage = lhsStorage->Storage;

  // If the decl is monomorphically a stored property, allow aliases.
  // It could be overridden by a computed property in a subclass, but
  // that's not likely enough to be worth the strictness here.
  auto impl = storage->getImplInfo();
  // TODO: Stored properties with didSet accessors that don't look at the
  // oldValue could also be addressed.
  if ((impl.getReadImpl() == ReadImplKind::Stored ||
       impl.getReadImpl() == ReadImplKind::Address) &&
      (impl.getWriteImpl() == WriteImplKind::Immutable ||
       impl.getWriteImpl() == WriteImplKind::Stored ||
       impl.getWriteImpl() == WriteImplKind::MutableAddress)) {
    return;
  }

  // If the property is a generic requirement, allow aliases, because
  // it may be conformed to using a stored property.
  if (isa<ProtocolDecl>(storage->getDeclContext()))
    return;

  // If this is a simple property access, then we must have a conflict.
  if (!lhsStorage->Indices) {
    assert(isa<VarDecl>(storage));
    SGF.SGM.diagnose(loc, diag::writeback_overlap_property,
                     storage->getBaseIdentifier())
       .highlight(loc.getSourceRange());
    SGF.SGM.diagnose(rhs.loc, diag::writebackoverlap_note)
       .highlight(rhs.loc.getSourceRange());
    return;
  }

  // Otherwise, it is a subscript, check the index values.

  // If the indices are literally identical SILValue's, then there is
  // clearly a conflict.
  if (!lhsStorage->Indices->isObviouslyEqual(*rhsStorage->Indices)) {
    // If the index value doesn't lower to literally the same SILValue's,
    // do some fuzzy matching to catch the common case.
    if (!lhsStorage->ArgListForDiagnostics ||
        !rhsStorage->ArgListForDiagnostics ||
        !areCertainlyEqualArgumentLists(lhsStorage->ArgListForDiagnostics,
                                        rhsStorage->ArgListForDiagnostics))
      return;
  }

  // The locations for the subscripts are almost certainly SubscriptExprs.
  // If so, dig into them to produce better location info in the
  // diagnostics and be able to do more precise analysis.
  auto expr1 = loc.getAsASTNode<SubscriptExpr>();
  auto expr2 = rhs.loc.getAsASTNode<SubscriptExpr>();

  if (expr1 && expr2) {
    SGF.SGM.diagnose(loc, diag::writeback_overlap_subscript)
       .highlight(expr1->getBase()->getSourceRange());

    SGF.SGM.diagnose(rhs.loc, diag::writebackoverlap_note)
       .highlight(expr2->getBase()->getSourceRange());

  } else {
    SGF.SGM.diagnose(loc, diag::writeback_overlap_subscript)
       .highlight(loc.getSourceRange());
    SGF.SGM.diagnose(rhs.loc, diag::writebackoverlap_note)
       .highlight(rhs.loc.getSourceRange());
  }
}

//===----------------------------------------------------------------------===//

static CanType getSubstFormalRValueType(Expr *expr) {
  return expr->getType()->getRValueType()->getCanonicalType();
}

static LValueTypeData getAbstractedTypeData(TypeExpansionContext context,
                                            SILGenModule &SGM,
                                            SGFAccessKind accessKind,
                                            AbstractionPattern origFormalType,
                                            CanType substFormalType) {
  return {
      accessKind, origFormalType, substFormalType,
      SGM.Types.getLoweredRValueType(context, origFormalType, substFormalType)};
}

static LValueTypeData getLogicalStorageTypeData(TypeExpansionContext context,
                                                SILGenModule &SGM,
                                                SGFAccessKind accessKind,
                                                CanType substFormalType) {
  assert(!isa<ReferenceStorageType>(substFormalType));
  AbstractionPattern origFormalType(
      substFormalType.getReferenceStorageReferent());
  return getAbstractedTypeData(context, SGM, accessKind, origFormalType,
                               substFormalType);
}

static LValueTypeData getPhysicalStorageTypeData(TypeExpansionContext context,
                                                 SILGenModule &SGM,
                                                 SGFAccessKind accessKind,
                                                 AbstractStorageDecl *storage,
                                                 SubstitutionMap subs,
                                                 CanType substFormalType) {
  assert(!isa<ReferenceStorageType>(substFormalType));
  auto origFormalType = SGM.Types.getAbstractionPattern(storage)
                                 .getReferenceStorageReferentType()
                                 .withSubstitutions(subs);
  return getAbstractedTypeData(context, SGM, accessKind, origFormalType,
                               substFormalType);
}

static bool shouldUseUnsafeEnforcement(VarDecl *var) {
  if (var->isDebuggerVar())
    return true;

  return false;
}

static bool hasExclusivityAttr(VarDecl *var, ExclusivityAttr::Mode mode) {
  if (!var)
    return false;
  auto *exclAttr = var->getAttrs().getAttribute<ExclusivityAttr>();
  return exclAttr && exclAttr->getMode() == mode;
}

llvm::Optional<SILAccessEnforcement>
SILGenFunction::getStaticEnforcement(VarDecl *var) {
  if (var && shouldUseUnsafeEnforcement(var))
    return SILAccessEnforcement::Unsafe;

  return SILAccessEnforcement::Static;
}

llvm::Optional<SILAccessEnforcement>
SILGenFunction::getDynamicEnforcement(VarDecl *var) {
  if (getOptions().EnforceExclusivityDynamic) {
    if (var && shouldUseUnsafeEnforcement(var))
      return SILAccessEnforcement::Unsafe;
    if (hasExclusivityAttr(var, ExclusivityAttr::Unchecked))
      return SILAccessEnforcement::Unsafe;
    return SILAccessEnforcement::Dynamic;
  } else if (hasExclusivityAttr(var, ExclusivityAttr::Checked)) {
    return SILAccessEnforcement::Dynamic;
  }
  return llvm::None;
}

llvm::Optional<SILAccessEnforcement>
SILGenFunction::getUnknownEnforcement(VarDecl *var) {
  if (var && shouldUseUnsafeEnforcement(var))
    return SILAccessEnforcement::Unsafe;

  return SILAccessEnforcement::Unknown;
}

/// SILGenLValue - An ASTVisitor for building logical lvalues.
class LLVM_LIBRARY_VISIBILITY SILGenLValue
  : public Lowering::ExprVisitor<SILGenLValue, LValue,
                                 SGFAccessKind, LValueOptions>
{

public:
  SILGenFunction &SGF;
  SILGenLValue(SILGenFunction &SGF) : SGF(SGF) {}
  
  LValue visitRec(Expr *e, SGFAccessKind accessKind, LValueOptions options,
                  AbstractionPattern orig = AbstractionPattern::getInvalid());
  
  /// Dummy handler to log unimplemented nodes.
  LValue visitExpr(Expr *e, SGFAccessKind accessKind, LValueOptions options);

  // Nodes that form the root of lvalue paths
  LValue visitDiscardAssignmentExpr(DiscardAssignmentExpr *e,
                                    SGFAccessKind accessKind,
                                    LValueOptions options);
  LValue visitDeclRefExpr(DeclRefExpr *e, SGFAccessKind accessKind,
                          LValueOptions options);
  LValue visitOpaqueValueExpr(OpaqueValueExpr *e, SGFAccessKind accessKind,
                              LValueOptions options);
  LValue visitPackElementExpr(PackElementExpr *e, SGFAccessKind accessKind,
                              LValueOptions options);

  // Nodes that make up components of lvalue paths
  
  LValue visitMemberRefExpr(MemberRefExpr *e, SGFAccessKind accessKind,
                            LValueOptions options);
  LValue visitSubscriptExpr(SubscriptExpr *e, SGFAccessKind accessKind,
                            LValueOptions options);
  LValue visitTupleElementExpr(TupleElementExpr *e, SGFAccessKind accessKind,
                               LValueOptions options);
  LValue visitForceValueExpr(ForceValueExpr *e, SGFAccessKind accessKind,
                             LValueOptions options);
  LValue visitBindOptionalExpr(BindOptionalExpr *e, SGFAccessKind accessKind,
                               LValueOptions options);
  LValue visitOpenExistentialExpr(OpenExistentialExpr *e,
                                  SGFAccessKind accessKind,
                                  LValueOptions options);
  LValue visitKeyPathApplicationExpr(KeyPathApplicationExpr *e,
                                     SGFAccessKind accessKind,
                                     LValueOptions options);
  LValue visitConsumeExpr(ConsumeExpr *e, SGFAccessKind accessKind,
                          LValueOptions options);
  LValue visitCopyExpr(CopyExpr *e, SGFAccessKind accessKind,
                       LValueOptions options);
  LValue visitABISafeConversionExpr(ABISafeConversionExpr *e,
                                    SGFAccessKind accessKind,
                                    LValueOptions options);

  // Expressions that wrap lvalues
  
  LValue visitInOutExpr(InOutExpr *e, SGFAccessKind accessKind,
                        LValueOptions options);
  LValue visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *e,
                                       SGFAccessKind accessKind,
                                       LValueOptions options);
};

/// Read this component.
ManagedValue
LogicalPathComponent::projectForRead(SILGenFunction &SGF, SILLocation loc,
                                     ManagedValue base,
                                     SGFAccessKind accessKind) && {
  assert(isReadAccess(accessKind));

  const TypeLowering &RValueTL = SGF.getTypeLowering(getTypeOfRValue());

  // If the access doesn't require us to make an owned address, don't
  // force a materialization.
  if (!isReadAccessResultAddress(accessKind)) {
    auto rvalue = std::move(*this).get(SGF, loc, base, SGFContext());
    return std::move(rvalue).getAsSingleValue(SGF, loc);
  }

  TemporaryInitializationPtr tempInit;
  ManagedValue result;
  RValue rvalue;

  // If the RValue type has an openedExistential, then the RValue must be
  // materialized before allocating a temporary for the RValue type. In that
  // case, the RValue cannot be emitted directly into the temporary.
  if (getTypeOfRValue().hasOpenedExistential()) {
    // Emit a 'get'.
    rvalue = std::move(*this).get(SGF, loc, base, SGFContext());

    // Create a temporary, whose type may depend on the 'get'.
    tempInit = SGF.emitFormalAccessTemporary(loc, RValueTL);
    result = tempInit->getManagedAddress();
  } else {
    // Create a temporary for a static (non-dependent) RValue type.
    tempInit = SGF.emitFormalAccessTemporary(loc, RValueTL);
    result = tempInit->getManagedAddress();

    // Emit a 'get' directly into the temporary.
    rvalue = std::move(*this).get(SGF, loc, base, SGFContext(tempInit.get()));
  }
  // `this` is now dead.

  // Force `value` into a temporary if is wasn't emitted there.
  if (!rvalue.isInContext())
    std::move(rvalue).forwardInto(SGF, loc, tempInit.get());

  assert(result);
  return result;
}

ManagedValue LogicalPathComponent::project(SILGenFunction &SGF,
                                           SILLocation loc,
                                           ManagedValue base) && {
  auto accessKind = getAccessKind();
  if (isReadAccess(accessKind))
    return std::move(*this).projectForRead(SGF, loc, base, accessKind);

  // AccessKind is Write or ReadWrite. We need to emit a get and set.
  assert(SGF.isInFormalEvaluationScope() &&
         "materializing l-value for modification without writeback scope");

  // Clone anything else about the component that we might need in the
  // writeback.
  auto clonedComponent = clone(SGF, loc);

  ManagedValue temp =
    std::move(*this).projectForRead(SGF, loc, base,
                                    SGFAccessKind::OwnedAddressRead);

  if (SGF.getOptions().VerifyExclusivity) {
    // Begin an access of the temporary. It is unenforced because enforcement
    // isn't required for RValues.
    SILValue accessAddress = UnenforcedFormalAccess::enter(
        SGF, loc, temp.getValue(), SILAccessKind::Modify);
    temp = std::move(temp).transform(accessAddress);
  }
  // Push a writeback for the temporary.
  pushWriteback(SGF, loc, std::move(clonedComponent), base,
                MaterializedLValue(temp));
  return ManagedValue::forLValue(temp.getValue());
}

void LogicalPathComponent::writeback(SILGenFunction &SGF, SILLocation loc,
                                     ManagedValue base,
                                     MaterializedLValue materialized,
                                     bool isFinal) {
  assert(!materialized.callback &&
         "unexpected materialized lvalue with callback!");

  // Load the value from the temporary unless the type is address-only
  // and this is the final use, in which case we can just consume the
  // value as-is.
  auto temporary = materialized.temporary;

  assert(temporary.getType().isAddress());
  auto &tempTL = SGF.getTypeLowering(temporary.getType());
  if (!tempTL.isAddressOnly() || !isFinal ||
      !SGF.silConv.useLoweredAddresses()) {
    if (isFinal) temporary.forward(SGF);
    temporary = SGF.emitLoad(loc, temporary.getValue(), tempTL,
                             SGFContext(), IsTake_t(isFinal));
  }
  RValue rvalue(SGF, loc, getSubstFormalType(), temporary);

  // Don't consume cleanups on the base if this isn't final.
  if (base && !isFinal) { base = ManagedValue::forUnmanaged(base.getValue()); }

  // Clone the component if this isn't final.
  std::unique_ptr<LogicalPathComponent> clonedComponent =
    (isFinal ? nullptr : clone(SGF, loc));
  LogicalPathComponent *component = (isFinal ? this : &*clonedComponent);
  std::move(*component).set(SGF, loc, ArgumentSource(loc, std::move(rvalue)),
                            base);
}

InOutConversionScope::InOutConversionScope(SILGenFunction &SGF)
  : SGF(SGF)
{
  assert(SGF.isInFormalEvaluationScope()
         && "inout conversions should happen in writeback scopes");
  assert(!SGF.InInOutConversionScope
         && "inout conversions should not be nested");
  SGF.InInOutConversionScope = true;
}

InOutConversionScope::~InOutConversionScope() {
  assert(SGF.InInOutConversionScope && "already exited conversion scope?!");
  SGF.InInOutConversionScope = false;
}

void PathComponent::_anchor() {}
void PhysicalPathComponent::_anchor() {}

void PhysicalPathComponent::set(SILGenFunction &SGF, SILLocation loc,
                                ArgumentSource &&value, ManagedValue base) && {
  auto finalDestAddr = std::move(*this).project(SGF, loc, base);
  assert(finalDestAddr.getType().isAddress());

  auto srcRValue = std::move(value).getAsRValue(SGF).ensurePlusOne(SGF, loc);
  std::move(srcRValue).assignInto(SGF, loc, finalDestAddr.getValue());
}

void PathComponent::dump() const {
  dump(llvm::errs());
}

/// Return the LValueTypeData for a SIL value with the given AST formal type.
static LValueTypeData getValueTypeData(SGFAccessKind accessKind,
                                       CanType formalType, SILValue value) {
  return {
    accessKind,
    AbstractionPattern(formalType),
    formalType,
    value->getType().getASTType(),
  };
}
static LValueTypeData getValueTypeData(SILGenFunction &SGF,
                                       SGFAccessKind accessKind, Expr *e) {
  CanType formalType = getSubstFormalRValueType(e);
  CanType loweredType = SGF.getLoweredType(formalType).getASTType();

  return {
    accessKind,
    AbstractionPattern(formalType),
    formalType,
    loweredType
  };
}

/// Given the address of an optional value, unsafely project out the
/// address of the value.
static ManagedValue getAddressOfOptionalValue(SILGenFunction &SGF,
                                              SILLocation loc,
                                              ManagedValue optAddr,
                                        const LValueTypeData &valueTypeData) {
  // Project out the 'Some' payload.
  EnumElementDecl *someDecl = SGF.getASTContext().getOptionalSomeDecl();

  // If the base is +1, we want to forward the cleanup.
  bool hadCleanup = optAddr.hasCleanup();

  // UncheckedTakeEnumDataAddr is safe to apply to Optional, because it is
  // a single-payload enum. There will (currently) never be spare bits
  // embedded in the payload.
  SILValue valueAddr =
    SGF.B.createUncheckedTakeEnumDataAddr(loc, optAddr.forward(SGF), someDecl,
                                          SILType::getPrimitiveAddressType(
                                            valueTypeData.TypeOfRValue));

  // Return the value as +1 if the optional was +1.
  if (hadCleanup) {
    return SGF.emitManagedBufferWithCleanup(valueAddr);
  } else {
    return ManagedValue::forLValue(valueAddr);
  }
}

namespace {
  /// A helper class for creating writebacks associated with l-value
  /// components that don't really need them.
  class WritebackPseudoComponent : public LogicalPathComponent {
  protected:
    WritebackPseudoComponent(const LValueTypeData &typeData)
      : LogicalPathComponent(typeData, WritebackPseudoKind) {}

  public:
    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation l) const override {
      llvm_unreachable("called clone on pseudo-component");
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      llvm_unreachable("called get on a pseudo-component");
    }
    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      llvm_unreachable("called set on a pseudo-component");
    }
    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      llvm_unreachable("called project on a pseudo-component");
    }

    llvm::Optional<AccessStorage> getAccessStorage() const override {
      return llvm::None;
    }
  };

  class EndAccessPseudoComponent : public WritebackPseudoComponent {
  private:
    ExecutorBreadcrumb ExecutorHop;
  public:
    EndAccessPseudoComponent(const LValueTypeData &typeData,
                             ExecutorBreadcrumb &&executorHop)
      : WritebackPseudoComponent(typeData),
        ExecutorHop(std::move(executorHop)) {}

  private:
    void writeback(SILGenFunction &SGF, SILLocation loc,
                   ManagedValue base,
                   MaterializedLValue materialized,
                   bool isFinal) override {
      loc.markAutoGenerated();

      assert(base.isLValue());
      loc.markAutoGenerated();
      SGF.B.createEndAccess(loc, base.getValue(), /*abort*/ false);
      ExecutorHop.emit(SGF, loc);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "EndAccessPseudoComponent\n";
    }
  };
} // end anonymous namespace

static SILValue enterAccessScope(SILGenFunction &SGF, SILLocation loc,
                                 ManagedValue base, SILValue addr,
                                 LValueTypeData typeData,
                                 SGFAccessKind accessKind,
                                 SILAccessEnforcement enforcement,
                                 llvm::Optional<ActorIsolation> actorIso) {
  auto silAccessKind = SILAccessKind::Modify;
  if (isReadAccess(accessKind))
    silAccessKind = SILAccessKind::Read;
  else if (isConsumeAccess(accessKind))
    silAccessKind = SILAccessKind::Deinit;

  assert(SGF.isInFormalEvaluationScope() &&
         "tried to enter access scope without a writeback scope!");

  ExecutorBreadcrumb prevExecutor =
      SGF.emitHopToTargetActor(loc, actorIso, base);

  // Enter the access.
  addr = SGF.B.createBeginAccess(loc, addr, silAccessKind, enforcement,
                                 /*hasNoNestedConflict=*/false,
                                 /*fromBuiltin=*/false);

  // Push a writeback to end it.
  auto accessedMV = ManagedValue::forLValue(addr);
  std::unique_ptr<LogicalPathComponent> component(
      new EndAccessPseudoComponent(typeData, std::move(prevExecutor)));
  pushWriteback(SGF, loc, std::move(component), accessedMV,
                MaterializedLValue());

  return addr;
}

static ManagedValue enterAccessScope(SILGenFunction &SGF, SILLocation loc,
                                     ManagedValue base, ManagedValue addr,
                                     LValueTypeData typeData,
                                     SGFAccessKind accessKind,
                                     SILAccessEnforcement enforcement,
                                     llvm::Optional<ActorIsolation> actorIso) {
  return ManagedValue::forLValue(
      enterAccessScope(SGF, loc, base, addr.getLValueAddress(), typeData,
                       accessKind, enforcement, actorIso));
}

// Find the base of the formal access at `address`. If the base requires an
// access marker, then create a begin_access on `address`. Return the
// address to be used for the access.
//
// FIXME: In order to generate more consistent and verifiable SIL patterns, or
// subobject projections, create the access on the base address and recreate the
// projection.
SILValue UnenforcedAccess::beginAccess(SILGenFunction &SGF, SILLocation loc,
                                       SILValue address, SILAccessKind kind) {
  if (!SGF.getOptions().VerifyExclusivity)
    return address;

  auto storage = AccessStorage::compute(address);
  // Unsafe access may have invalid storage (e.g. a RawPointer).
  if (storage && !isPossibleFormalAccessStorage(storage, &SGF.F))
    return address;

  auto BAI =
    SGF.B.createBeginAccess(loc, address, kind, SILAccessEnforcement::Unsafe,
                            /*hasNoNestedConflict=*/false,
                            /*fromBuiltin=*/false);
  beginAccessPtr = BeginAccessPtr(BAI, DeleterCheck());

  return BAI;
}

void UnenforcedAccess::endAccess(SILGenFunction &SGF) {
  emitEndAccess(SGF);
  beginAccessPtr.release();
}

void UnenforcedAccess::emitEndAccess(SILGenFunction &SGF) {
  if (!beginAccessPtr)
    return;

  SGF.B.createEndAccess(beginAccessPtr->getLoc(), beginAccessPtr.get(),
                        /*abort*/ false);
}

// Emit an end_access marker when executing a cleanup (on a side branch).
void UnenforcedFormalAccess::emitEndAccess(SILGenFunction &SGF) {
  access.emitEndAccess(SGF);
}

// End the access when existing the FormalEvaluationScope.
void UnenforcedFormalAccess::finishImpl(SILGenFunction &SGF) {
  access.endAccess(SGF);
}

namespace {
struct UnenforcedAccessCleanup : Cleanup {
  FormalEvaluationContext::stable_iterator Depth;

  UnenforcedAccessCleanup() : Depth() {}

  void emit(SILGenFunction &SGF, CleanupLocation loc,
            ForUnwind_t forUnwind) override {
    auto &evaluation = *SGF.FormalEvalContext.find(Depth);
    assert(evaluation.getKind() == FormalAccess::Unenforced);
    auto &formalAccess = static_cast<UnenforcedFormalAccess &>(evaluation);
    formalAccess.emitEndAccess(SGF);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "UnenforcedAccessCleanup\n"
                 << "State: " << getState() << " Depth: " << Depth.getDepth()
                 << "\n";
#endif
  }
};
} // end anonymous namespace

SILValue UnenforcedFormalAccess::enter(SILGenFunction &SGF, SILLocation loc,
                                       SILValue address, SILAccessKind kind) {
  assert(SGF.isInFormalEvaluationScope());

  UnenforcedAccess access;
  SILValue accessAddress = access.beginAccess(SGF, loc, address, kind);
  if (!access.beginAccessPtr)
    return address;

  auto &cleanup = SGF.Cleanups.pushCleanup<UnenforcedAccessCleanup>();
  CleanupHandle handle = SGF.Cleanups.getTopCleanup();
  auto &context = SGF.FormalEvalContext;
  context.push<UnenforcedFormalAccess>(loc, std::move(access), handle);
  cleanup.Depth = context.stable_begin();

  return accessAddress;
}

static void copyBorrowedYieldsIntoTemporary(SILGenFunction &SGF,
                                            SILLocation loc,
                                            ArrayRef<ManagedValue> &yields,
                                            AbstractionPattern origFormalType,
                                            CanType substFormalType,
                                            Initialization *init) {
  if (!origFormalType.isTuple()) {
    auto value = yields.front();
    yields = yields.drop_front();
    init->copyOrInitValueInto(SGF, loc, value, /*isInit*/ false);
    init->finishInitialization(SGF);
    return;
  }

  assert(init->canSplitIntoTupleElements());
  SmallVector<InitializationPtr, 4> scratch;
  auto eltInits =
    init->splitIntoTupleElements(SGF, loc, substFormalType, scratch);
  for (size_t i : indices(eltInits)) {
    auto origEltType = origFormalType.getTupleElementType(i);
    auto substEltType = cast<TupleType>(substFormalType).getElementType(i);
    copyBorrowedYieldsIntoTemporary(SGF, loc, yields, origEltType,
                                    substEltType, eltInits[i].get());
  }
  init->finishInitialization(SGF);
}

namespace {
  class RefElementComponent : public PhysicalPathComponent {
    VarDecl *Field;
    SILType SubstFieldType;
    bool IsNonAccessing;
  public:
    RefElementComponent(VarDecl *field, LValueOptions options,
                        SILType substFieldType, LValueTypeData typeData,
                        llvm::Optional<ActorIsolation> actorIso)
        : PhysicalPathComponent(typeData, RefElementKind, actorIso),
          Field(field), SubstFieldType(substFieldType),
          IsNonAccessing(options.IsNonAccessing) {}

    virtual bool isLoadingPure() const override { return true; }

    VarDecl *getField() const { return Field; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base.getType().hasReferenceSemantics() &&
             "base for ref element component must be a reference type");

      // Borrow the ref element addr using formal access. If we need the ref
      // element addr, we will load it in this expression.
      if (base.getType().isAddress()) {
        base = SGF.B.createFormalAccessLoadBorrow(loc, base);
      } else {
        base = base.formalAccessBorrow(SGF, loc);
      }
      SILValue result =
        SGF.B.createRefElementAddr(loc, base.getUnmanagedValue(),
                                   Field, SubstFieldType);

      // Avoid emitting access markers completely for non-accesses or immutable
      // declarations. Access marker verification is aware of these cases.
      if (!IsNonAccessing && !Field->isLet()) {
        if (auto enforcement = SGF.getDynamicEnforcement(Field)) {
          result = enterAccessScope(SGF, loc, base, result, getTypeData(),
                                    getAccessKind(), *enforcement,
                                    takeActorIsolation());
        }
      }

      // If we have a move only type, add a marker.
      //
      // NOTE: We purposely do this on the access itself to ensure that when we
      // hoist destroy_addr, they stay within the access scope.
      if (result->getType().isMoveOnly()) {
        auto checkKind =
            MarkMustCheckInst::CheckKind::AssignableButNotConsumable;
        if (isReadAccess(getAccessKind())) {
          // Add a mark_must_check [no_consume_or_assign].
          checkKind = MarkMustCheckInst::CheckKind::NoConsumeOrAssign;
        }
        result = SGF.B.createMarkMustCheckInst(loc, result, checkKind);
      }

      return ManagedValue::forLValue(result);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "RefElementComponent(" << Field->getName() << ")\n";
    }
  };

  class TupleElementComponent : public PhysicalPathComponent {
    unsigned ElementIndex;
  public:
    TupleElementComponent(unsigned elementIndex, LValueTypeData typeData)
        : PhysicalPathComponent(typeData, TupleElementKind,
                                /*actorIsolation=*/llvm::None),
          ElementIndex(elementIndex) {}

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base && "invalid value for element base");
      if (base.getType().isObject()) {
        return SGF.B.createTupleExtract(loc, base, ElementIndex);
      }

      // TODO: if the base is +1, break apart its cleanup.
      auto Res = SGF.B.createTupleElementAddr(loc, base.getValue(),
                                              ElementIndex,
                                              getTypeOfRValue().getAddressType());
      return ManagedValue::forLValue(Res);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "TupleElementComponent(" << ElementIndex << ")\n";
    }
  };

  class StructElementComponent : public PhysicalPathComponent {
    VarDecl *Field;
    SILType SubstFieldType;
  public:
    StructElementComponent(VarDecl *field, SILType substFieldType,
                           LValueTypeData typeData,
                           llvm::Optional<ActorIsolation> actorIso)
        : PhysicalPathComponent(typeData, StructElementKind, actorIso),
          Field(field), SubstFieldType(substFieldType) {}

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base && "invalid value for element base");
      if (base.getType().isObject()) {
        return SGF.B.createStructExtract(loc, base, Field);
      }

      // If we have a moveonlywrapped type, unwrap it. The reason why is that
      // any fields that we access we want to be treated as copyable.
      if (base.getType().isMoveOnlyWrapped())
        base = ManagedValue::forLValue(
            SGF.B.createMoveOnlyWrapperToCopyableAddr(loc, base.getValue()));

      // TODO: if the base is +1, break apart its cleanup.
      auto Res = SGF.B.createStructElementAddr(loc, base.getValue(),
                                               Field, SubstFieldType);

      if (!Field->getPointerAuthQualifier().isPresent() ||
          !SGF.getOptions().EnableImportPtrauthFieldFunctionPointers) {
        return ManagedValue::forLValue(Res);
      }
      auto beginAccess =
          enterAccessScope(SGF, loc, base, Res, getTypeData(), getAccessKind(),
                           SILAccessEnforcement::Signed, takeActorIsolation());
      return ManagedValue::forLValue(beginAccess);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "StructElementComponent("
                        << Field->getName() << ")\n";
    }
  };

  /// A physical path component which force-projects the address of
  /// the value of an optional l-value.
  class ForceOptionalObjectComponent : public PhysicalPathComponent {
    bool isImplicitUnwrap;
  public:
    ForceOptionalObjectComponent(LValueTypeData typeData, bool isImplicitUnwrap)
        : PhysicalPathComponent(typeData, OptionalObjectKind,
                                /*actorIsolation=*/llvm::None),
          isImplicitUnwrap(isImplicitUnwrap) {}

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      // Assert that the optional value is present and return the projected out
      // payload.
      return SGF.emitPreconditionOptionalHasValue(loc, base, isImplicitUnwrap);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "ForceOptionalObjectComponent(" << isImplicitUnwrap << ")\n";
    }
  };

  /// A physical path component which projects out an opened archetype
  /// from an existential.
  class OpenOpaqueExistentialComponent : public PhysicalPathComponent {
  public:
    OpenOpaqueExistentialComponent(CanArchetypeType openedArchetype,
                                   LValueTypeData typeData)
        : PhysicalPathComponent(typeData, OpenOpaqueExistentialKind,
                                /*actorIsolation=*/llvm::None) {
      assert(getSubstFormalType() == openedArchetype);
    }

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base.getType().isExistentialType() &&
             "base for open existential component must be an existential");
      assert((base.getType().isAddress() ||
              base.getType().getPreferredExistentialRepresentation() ==
                  ExistentialRepresentation::Boxed) &&
             "base value of open-existential component was not an address or a "
             "boxed existential?");
      SILValue addr;

      auto rep = base.getType().getPreferredExistentialRepresentation();
      switch (rep) {
      case ExistentialRepresentation::Opaque:
        addr = SGF.B.createOpenExistentialAddr(
          loc, base.getValue(), getTypeOfRValue().getAddressType(),
          getOpenedExistentialAccessFor(getFormalAccessKind(getAccessKind())));
        break;
      case ExistentialRepresentation::Boxed: {
        ManagedValue error;
        if (base.getType().isObject()) {
          error = base;
        } else {
          auto &TL = SGF.getTypeLowering(base.getType());
          error = SGF.emitLoad(loc, base.getValue(), TL,
                               SGFContext(), IsNotTake);
          // Error comes back to us with a +1 cleanup that is not a formal
          // access cleanup. We need it to be that so that the load nests
          // properly with other lvalue scoped things like the borrow below.
          error = SGF.emitFormalAccessManagedRValueWithCleanup(
              loc, error.forward(SGF));
        }
        SILType addrType = getTypeOfRValue().getAddressType();
        addr = SGF.B.createOpenExistentialBox(loc, error, addrType).getValue();
        break;
      }
      default:
        llvm_unreachable("Bad existential representation for address-only type");
      }

      return ManagedValue::forLValue(addr);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OpenOpaqueExistentialComponent("
                        << getSubstFormalType() << ")\n";
    }
  };

  /// A local path component for the payload of a class or metatype existential.
  ///
  /// TODO: Could be physical if we had a way to project out the
  /// payload.
  class OpenNonOpaqueExistentialComponent : public LogicalPathComponent {
    CanArchetypeType OpenedArchetype;
  public:
    OpenNonOpaqueExistentialComponent(CanArchetypeType openedArchetype,
                                      LValueTypeData typeData)
      : LogicalPathComponent(typeData, OpenNonOpaqueExistentialKind),
        OpenedArchetype(openedArchetype) {}

    virtual bool isLoadingPure() const override { return true; }

    llvm::Optional<AccessStorage> getAccessStorage() const override {
      return llvm::None;
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      auto refType = base.getType().getObjectType();
      auto &TL = SGF.getTypeLowering(refType);

      // Load the original value if necessary.
      auto result = base.getType().isAddress()
                      ? SGF.emitLoad(loc, base.getValue(), TL,
                                     SGFContext(), IsNotTake)
                      : base;

      assert(refType.isAnyExistentialType() &&
             "base for open existential component must be an existential");
      ManagedValue ref;
      if (refType.is<ExistentialMetatypeType>()) {
        assert(refType.getPreferredExistentialRepresentation()
                 == ExistentialRepresentation::Metatype);
        ref = ManagedValue::forUnmanaged(
                SGF.B.createOpenExistentialMetatype(loc,
                                                    result.getUnmanagedValue(),
                                                    getTypeOfRValue()));
      } else {
        assert(refType.getPreferredExistentialRepresentation()
                 == ExistentialRepresentation::Class);
        ref = SGF.B.createOpenExistentialRef(loc, result, getTypeOfRValue());
      }

      return RValue(SGF, loc, getSubstFormalType(), ref);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      auto payload = std::move(value).getAsSingleValue(SGF).forward(SGF);

      SmallVector<ProtocolConformanceRef, 2> conformances;
      for (auto proto : OpenedArchetype->getConformsTo())
        conformances.push_back(ProtocolConformanceRef(proto));

      SILValue ref;
      if (base.getType().is<ExistentialMetatypeType>()) {
        ref = SGF.B.createInitExistentialMetatype(
                loc,
                payload,
                base.getType().getObjectType(),
                SGF.getASTContext().AllocateCopy(conformances));
      } else {
        assert(getSubstFormalType()->isBridgeableObjectType());
        ref = SGF.B.createInitExistentialRef(
                loc,
                base.getType().getObjectType(),
                getSubstFormalType(),
                payload,
                SGF.getASTContext().AllocateCopy(conformances));
      }

      auto &TL = SGF.getTypeLowering(base.getType());
      SGF.emitSemanticStore(loc, ref,
                            base.getValue(), TL, IsNotInitialization);
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone =
        new OpenNonOpaqueExistentialComponent(OpenedArchetype, getTypeData());
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OpenNonOpaqueExistentialComponent("
                        << OpenedArchetype << ", ...)\n";
    }
  };

  /// A physical path component which returns a literal address.
  class ValueComponent : public PhysicalPathComponent {
    ManagedValue Value;
    llvm::Optional<SILAccessEnforcement> Enforcement;
    bool IsRValue;
  public:
    ValueComponent(ManagedValue value,
                   llvm::Optional<SILAccessEnforcement> enforcement,
                   LValueTypeData typeData, bool isRValue = false,
                   llvm::Optional<ActorIsolation> actorIso = llvm::None)
        : PhysicalPathComponent(typeData, ValueKind, actorIso), Value(value),
          Enforcement(enforcement), IsRValue(isRValue) {
      assert(IsRValue || value.getType().isAddress() ||
             value.getType().isBoxedNonCopyableType(value.getFunction()));
    }

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(!base && "value component must be root of lvalue path");

      // See if we have a noncopyable address from a project_box or global.
      if (Value.getType().isAddress() && Value.getType().isMoveOnly()) {
        SILValue addr = Value.getValue();
        auto box = dyn_cast<ProjectBoxInst>(addr);
        if (box || isa<GlobalAddrInst>(addr)) {
          if (Enforcement)
            addr = enterAccessScope(SGF, loc, base, addr, getTypeData(),
                                    getAccessKind(), *Enforcement,
                                    takeActorIsolation());
          // Mark all move only as having mark_must_check.
          //
          // DISCUSSION: LValue access to let boxes must have a mark_must_check
          // to allow for DI to properly handle delayed initialization of the
          // boxes and convert those to initable_but_not_consumable.
          addr = SGF.B.createMarkMustCheckInst(
              loc, addr,
              isReadAccess(getAccessKind())
                  ? MarkMustCheckInst::CheckKind::NoConsumeOrAssign
                  : MarkMustCheckInst::CheckKind::AssignableButNotConsumable);
          return ManagedValue::forLValue(addr);
        }
      }

      if (!Enforcement)
        return Value;

      assert(Value.getType().isAddress() && "Must have an address here?!");

      SILValue addr = Value.getLValueAddress();
      addr =
          enterAccessScope(SGF, loc, base, addr, getTypeData(), getAccessKind(),
                           *Enforcement, takeActorIsolation());

      return ManagedValue::forLValue(addr);
    }

    bool isRValue() const override {
      return IsRValue;
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS << "ValueComponent(";
      if (IsRValue) OS << "rvalue, ";
      if (Enforcement) {
        OS << getSILAccessEnforcementName(*Enforcement);
      } else {
        OS << "unenforced";
      }
      if (hasActorIsolation()) OS << " requires actor-hop";
      OS << "):\n";
      Value.dump(OS, indent + 2);
    }
  };
  
  class BorrowValueComponent : public PhysicalPathComponent {
  public:
    BorrowValueComponent(LValueTypeData typeData)
        : PhysicalPathComponent(typeData, BorrowValueKind, llvm::None) {}

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base
             && base.getType().isAddress()
             && "should have an address base to borrow from");
      // If the base value is address-only then we can borrow from the
      // address in-place.
      if (!base.getType().isLoadable(SGF.F)) {
        return base;
      }
      auto result = SGF.B.createLoadBorrow(loc, base.getValue());
      return SGF.emitFormalEvaluationManagedBorrowedRValueWithCleanup(loc,
         base.getValue(), result);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "BorrowValueComponent\n";
    }
  };
} // end anonymous namespace

static bool isReadNoneFunction(const Expr *e) {
  // If this is a curried call to an integer literal conversion operations, then
  // we can "safely" assume it is readnone (btw, yes this is totally gross).
  // This is better to be attribute driven, a la rdar://15587352.
  if (auto *dre = dyn_cast<DeclRefExpr>(e)) {
    const DeclName name = dre->getDecl()->getName();
    return (name.getArgumentNames().size() == 1 &&
            name.getBaseName() == DeclBaseName::createConstructor() &&
            !name.getArgumentNames()[0].empty() &&
            (name.getArgumentNames()[0].str() == "integerLiteral" ||
             name.getArgumentNames()[0].str() == "_builtinIntegerLiteral"));
  }
  
  // Look through DotSyntaxCallExpr, since the literal functions are curried.
  if (auto *CRCE = dyn_cast<ConstructorRefCallExpr>(e))
    return isReadNoneFunction(CRCE->getFn());
  
  return false;
}

/// Given two expressions used as arguments to the same SubscriptDecl (and thus
/// are guaranteed to have the same AST type) check to see if they are going to
/// produce the same value.
static bool areCertainlyEqualArgs(const Expr *e1, const Expr *e2) {
  if (e1->getKind() != e2->getKind()) return false;

  // Look through ParenExpr's.
  if (auto *pe1 = dyn_cast<ParenExpr>(e1)) {
    auto *pe2 = cast<ParenExpr>(e2);
    return areCertainlyEqualArgs(pe1->getSubExpr(), pe2->getSubExpr());
  }

  // Calls are identical if the callee and operands are identical and we know
  // that the call is something that is "readnone".
  if (auto *ae1 = dyn_cast<ApplyExpr>(e1)) {
    auto *ae2 = cast<ApplyExpr>(e2);
    return areCertainlyEqualArgs(ae1->getFn(), ae2->getFn()) &&
           areCertainlyEqualArgumentLists(ae1->getArgs(), ae2->getArgs()) &&
           isReadNoneFunction(ae1->getFn());
  }

  // TypeExpr's that produce the same metatype type are identical.
  if (isa<TypeExpr>(e1))
    return true;

  if (auto *dre1 = dyn_cast<DeclRefExpr>(e1)) {
    auto *dre2 = cast<DeclRefExpr>(e2);
    return dre1->getDecl() == dre2->getDecl() &&
           dre1->getType()->isEqual(dre2->getType());
  }

  // Compare a variety of literals.
  if (auto *il1 = dyn_cast<IntegerLiteralExpr>(e1)) {
    const auto &val1 = il1->getValue();
    const auto &val2 = cast<IntegerLiteralExpr>(e2)->getValue();
    // If the integers are arbitrary-precision, their bit-widths may differ,
    // but only if they represent different values.
    return val1.getBitWidth() == val2.getBitWidth() && val1 == val2;
  }
  if (auto *il1 = dyn_cast<FloatLiteralExpr>(e1))
    return il1->getValue().bitwiseIsEqual(
                                        cast<FloatLiteralExpr>(e2)->getValue());
  if (auto *bl1 = dyn_cast<BooleanLiteralExpr>(e1))
    return bl1->getValue() == cast<BooleanLiteralExpr>(e2)->getValue();
  if (auto *sl1 = dyn_cast<StringLiteralExpr>(e1))
    return sl1->getValue() == cast<StringLiteralExpr>(e2)->getValue();

  // Compare tuple expressions.
  if (auto *te1 = dyn_cast<TupleExpr>(e1)) {
    auto *te2 = cast<TupleExpr>(e2);

    // Easy checks: # of elements, element names.
    if (te1->getNumElements() != te2->getNumElements() ||
        te1->getElementNames() != te2->getElementNames()) {
      return false;
    }

    for (unsigned i = 0, n = te1->getNumElements(); i != n; ++i) {
      if (!areCertainlyEqualArgs(te1->getElement(i), te2->getElement(i)))
        return false;
    }

    return true;
  }

  // Otherwise, we have no idea if they are identical.
  return false;
}

/// Given two argument lists to the same SubscriptDecl (and thus are guaranteed
/// to have the same AST type) check to see if they are going to produce the
/// same value.
static bool areCertainlyEqualArgumentLists(const ArgumentList *l1,
                                           const ArgumentList *l2) {
  if (l1->size() != l2->size())
    return false;

  for (auto idx : indices(*l1)) {
    if (l1->getLabel(idx) != l2->getLabel(idx))
      return false;
    if (!areCertainlyEqualArgs(l1->getExpr(idx), l2->getExpr(idx)))
      return false;
  }
  return true;
}

static LValueOptions getBaseOptions(LValueOptions options,
                                    AccessStrategy strategy) {
  return (strategy.getKind() == AccessStrategy::Storage
            ? options.forProjectedBaseLValue()
            : options.forComputedBaseLValue());
}

static ArgumentSource emitBaseValueForAccessor(SILGenFunction &SGF,
                                               SILLocation loc, LValue &&dest,
                                               CanType baseFormalType,
                                               SILDeclRef accessor);

static SGFAccessKind getBaseAccessKind(SILGenModule &SGM,
                                       AbstractStorageDecl *member,
                                       SGFAccessKind accessKind,
                                       AccessStrategy strategy,
                                       CanType baseFormalType);

namespace {
  /// A helper class for implementing components that involve accessing
  /// storage.
  template <class Base>
  class AccessComponent : public Base {
  protected:
    // The VarDecl or SubscriptDecl being get/set.
    AbstractStorageDecl *Storage;

    /// The subscript argument list.  Useless
    ArgumentList *ArgListForDiagnostics;
    PreparedArguments Indices;

    /// AST type of the base expression, in case the accessor call
    /// requires re-abstraction.
    CanType BaseFormalType;

    struct AccessorArgs {
      ArgumentSource base;
      PreparedArguments Indices;
    };

    /// Returns a tuple of RValues holding the accessor value, base (retained if
    /// necessary), and subscript arguments, in that order.
    AccessorArgs
    prepareAccessorArgs(SILGenFunction &SGF, SILLocation loc,
                        ManagedValue base, SILDeclRef accessor) &&
    {
      AccessorArgs result;
      if (base) {
        // Borrow the base, because we may need it again to invoke other
        // accessors.
        result.base = SGF.prepareAccessorBaseArg(loc,
                                             base.formalAccessBorrow(SGF, loc),
                                             BaseFormalType,
                                             accessor);
      }

      if (!Indices.isNull())
        result.Indices = std::move(Indices);
      
      return result;
    }

    AccessComponent(PathComponent::KindTy kind,
                    AbstractStorageDecl *storage,
                    CanType baseFormalType,
                    LValueTypeData typeData,
                    ArgumentList *argListForDiagnostics,
                    PreparedArguments &&indices)
      : Base(typeData, kind), Storage(storage),
        ArgListForDiagnostics(argListForDiagnostics),
        Indices(std::move(indices)),
        BaseFormalType(baseFormalType)
    {
    }

    AccessComponent(const AccessComponent &copied,
                    SILGenFunction &SGF,
                    SILLocation loc)
      : Base(copied.getTypeData(), copied.getKind()),
        Storage(copied.Storage),
        ArgListForDiagnostics(copied.ArgListForDiagnostics),
        Indices(copied.Indices.copy(SGF, loc)) ,
        BaseFormalType(copied.BaseFormalType) {}

    bool doesAccessorMutateSelf(SILGenFunction &SGF,
                                SILDeclRef accessor) const {
      auto accessorSelf = SGF.SGM.Types.getConstantSelfParameter(
          SGF.getTypeExpansionContext(), accessor);
      return accessorSelf.getInterfaceType()
        && accessorSelf.isIndirectMutating();
    }
    
    void printBase(raw_ostream &OS, unsigned indent, StringRef name) const {
      OS.indent(indent) << name << "(" << Storage->getBaseName() << ")";
      if (ArgListForDiagnostics) {
        OS << " subscript_index:\n";
        ArgListForDiagnostics->dump(OS, 2);
      }
      OS << '\n';
    }
  };

  /// A helper class for implementing a component that involves
  /// calling accessors.
  template <class Base>
  class AccessorBasedComponent : public AccessComponent<Base> {
    using super = AccessComponent<Base>;

  protected:
    SILDeclRef Accessor;
    bool IsSuper;
    bool IsDirectAccessorUse;
    bool IsOnSelfParameter;
    SubstitutionMap Substitutions;
    llvm::Optional<ActorIsolation> ActorIso;

  public:
    AccessorBasedComponent(PathComponent::KindTy kind,
                           AbstractStorageDecl *decl, SILDeclRef accessor,
                           bool isSuper, bool isDirectAccessorUse,
                           SubstitutionMap substitutions,
                           CanType baseFormalType, LValueTypeData typeData,
                           ArgumentList *argListForDiagnostics,
                           PreparedArguments &&indices,
                           bool isOnSelfParameter = false,
                           llvm::Optional<ActorIsolation> actorIso = llvm::None)
        : super(kind, decl, baseFormalType, typeData, argListForDiagnostics,
                std::move(indices)),
          Accessor(accessor), IsSuper(isSuper),
          IsDirectAccessorUse(isDirectAccessorUse),
          IsOnSelfParameter(isOnSelfParameter), Substitutions(substitutions),
          ActorIso(actorIso) {}

    AccessorBasedComponent(const AccessorBasedComponent &copied,
                           SILGenFunction &SGF,
                           SILLocation loc)
      : super(copied, SGF, loc),
        Accessor(copied.Accessor),
        IsSuper(copied.IsSuper),
        IsDirectAccessorUse(copied.IsDirectAccessorUse),
        IsOnSelfParameter(copied.IsOnSelfParameter),
        Substitutions(copied.Substitutions),
        ActorIso(copied.ActorIso) {}

    AccessorDecl *getAccessorDecl() const {
      return cast<AccessorDecl>(Accessor.getFuncDecl());
    }
  };

  class GetterSetterComponent
    : public AccessorBasedComponent<LogicalPathComponent> {
  public:
    GetterSetterComponent(AbstractStorageDecl *decl, SILDeclRef accessor,
                          bool isSuper, bool isDirectAccessorUse,
                          SubstitutionMap substitutions, CanType baseFormalType,
                          LValueTypeData typeData,
                          ArgumentList *subscriptArgList,
                          PreparedArguments &&indices, bool isOnSelfParameter,
                          llvm::Optional<ActorIsolation> actorIso)
        : AccessorBasedComponent(
              GetterSetterKind, decl, accessor, isSuper, isDirectAccessorUse,
              substitutions, baseFormalType, typeData, subscriptArgList,
              std::move(indices), isOnSelfParameter, actorIso) {
      assert(getAccessorDecl()->isGetterOrSetter());
    }

    GetterSetterComponent(const GetterSetterComponent &copied,
                          SILGenFunction &SGF,
                          SILLocation loc)
      : AccessorBasedComponent(copied, SGF, loc)
    {
    }

    bool canRewriteSetAsPropertyWrapperInit(SILGenFunction &SGF) const {
      if (auto *VD = dyn_cast<VarDecl>(Storage)) {
        if (VD->isImplicit() || isa<ParamDecl>(VD))
          return false;

        // If this is not a wrapper property that can be initialized from
        // a value of the wrapped type, we can't perform the initialization.
        auto initInfo = VD->getPropertyWrapperInitializerInfo();
        if (!initInfo.hasInitFromWrappedValue())
          return false;

        auto *fnDecl = SGF.FunctionDC->getAsDecl();
        bool isAssignmentToSelfParamInInit =
            IsOnSelfParameter && isa<ConstructorDecl>(fnDecl) &&
            // Convenience initializers only contain assignments and not
            // initializations.
            !(cast<ConstructorDecl>(fnDecl)->isConvenienceInit());

        // Assignment to a wrapped property can only be re-written to initialization for
        // members of `self` in an initializer, and for local variables.
        if (!(isAssignmentToSelfParamInInit || VD->getDeclContext()->isLocalContext()))
          return false;

        // If this var isn't in a type context, assignment will always use the setter
        // if there is an initial value.
        if (!VD->getDeclContext()->isTypeContext() &&
            initInfo.getWrappedValuePlaceholder()->getOriginalWrappedValue())
          return false;

        // If this property wrapper uses autoclosure in it's initializer,
        // the argument types of the setter and initializer shall be
        // different, so we don't rewrite an assignment into an
        // initialization.
        return !initInfo.getWrappedValuePlaceholder()->isAutoClosure();
      }

      return false;
    }

    /// Whether an assignment 'x = y' can be re-written as a call to an
    /// init accessor declared by 'x'.
    bool canRewriteSetAsInitAccessor(SILGenFunction &SGF) const {
      auto *varDecl = dyn_cast<VarDecl>(Storage);
      if (!varDecl || varDecl->isStatic() ||
          varDecl->getDeclContext()->isLocalContext())
        return false;

      auto *fnDecl = SGF.FunctionDC->getAsDecl();
      bool isAssignmentToSelfParamInInit =
          IsOnSelfParameter && isa<ConstructorDecl>(fnDecl) &&
          // Convenience initializers only contain assignments and not
          // initializations.
          !(cast<ConstructorDecl>(fnDecl)->isConvenienceInit());

      // Assignment to a wrapped property can only be re-written to initialization for
      // members of `self` in an initializer, and for local variables.
      if (!isAssignmentToSelfParamInInit)
        return false;

      auto *initAccessor = varDecl->getAccessor(AccessorKind::Init);
      if (!initAccessor)
        return false;

      return true;
    }

    void emitAssignWithSetter(SILGenFunction &SGF, SILLocation loc,
                              LValue &&dest, ArgumentSource &&value) {
      assert(getAccessorDecl()->isSetter());
      SILDeclRef setter = Accessor;

      // Pull everything out of this that we'll need, because we're
      // about to modify the LValue and delete this component.
      auto subs = this->Substitutions;
      bool isSuper = this->IsSuper;
      bool isDirectAccessorUse = this->IsDirectAccessorUse;
      auto indices = std::move(this->Indices);
      auto baseFormalType = this->BaseFormalType;
      bool isOnSelfParameter = this->IsOnSelfParameter;

      // Drop this component from the l-value.
      dest.dropLastComponent(*this);

      return emitAssignWithSetter(SGF, loc, std::move(dest), baseFormalType,
                                  isSuper, setter, isDirectAccessorUse, subs,
                                  std::move(indices), std::move(value),
                                  isOnSelfParameter);
    }

    static void emitAssignWithSetter(SILGenFunction &SGF, SILLocation loc,
                                     LValue &&baseLV, CanType baseFormalType,
                                     bool isSuper, SILDeclRef setter,
                                     bool isDirectAccessorUse,
                                     SubstitutionMap subs,
                                     PreparedArguments &&indices,
                                     ArgumentSource &&value,
                                     bool isSelfParameter) {
      ArgumentSource self = [&] {
        if (!baseLV.isValid()) {
          return ArgumentSource();
        } else if (computeSelfParam(cast<FuncDecl>(setter.getDecl()))
                     .getParameterFlags().isInOut()) {
          return ArgumentSource(loc, std::move(baseLV));
        } else {
          return emitBaseValueForAccessor(SGF, loc, std::move(baseLV),
                                          baseFormalType, setter);
        }
      }();

      return SGF.emitSetAccessor(loc, setter, subs, std::move(self), isSuper,
                                 isDirectAccessorUse, std::move(indices),
                                 std::move(value), isSelfParameter);
    }

    /// Determine whether the backing variable for the given
    /// property (that has a wrapper) is visible from the
    /// current context.
    static bool isBackingVarVisible(VarDecl *field,
                                    DeclContext *fromDC){
      VarDecl *backingVar = field->getPropertyWrapperBackingProperty();
      return backingVar->isAccessibleFrom(fromDC);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      assert(getAccessorDecl()->isSetter());
      assert(!ActorIso && "no support for cross-actor set operations");
      SILDeclRef setter = Accessor;

      auto getSetterFRef = [&]() -> SILValue {
        auto setterInfo =
            SGF.getConstantInfo(SGF.getTypeExpansionContext(), setter);
        if (setter.hasDecl() && setter.getDecl()->shouldUseObjCDispatch()) {
          // Emit a thunk we might have to bridge arguments.
          auto foreignSetterThunk = setter.asForeign(false);
          return SGF
              .emitDynamicMethodRef(
                  loc, foreignSetterThunk,
                  SGF.SGM.Types
                      .getConstantInfo(SGF.getTypeExpansionContext(),
                                       foreignSetterThunk)
                      .SILFnType)
              .getValue();
        }

        return SGF.emitGlobalFunctionRef(loc, setter, setterInfo);
      };

      auto getSetterType = [&](SILValue setterFRef) {
        CanSILFunctionType setterTy =
            setterFRef->getType().castTo<SILFunctionType>();
        return setterTy->substGenericArgs(SGF.SGM.M, Substitutions,
                                          SGF.getTypeExpansionContext());
      };

      auto emitPartialInitAccessorApply =
          [&](AccessorDecl *initAccessor) -> SILValue {
        assert(initAccessor->isInitAccessor());
        auto initConstant = SGF.getAccessorDeclRef(initAccessor);
        SILValue initFRef = SGF.emitGlobalFunctionRef(loc, initConstant);

        // If there are no substitutions there is no need to emit partial
        // apply.
        if (Substitutions.empty())
          return initFRef;

        // Emit partial apply without argument to produce a substituted
        // init accessor reference.
        PartialApplyInst *initPAI = SGF.B.createPartialApply(
            loc, initFRef, Substitutions, ArrayRef<SILValue>(),
            ParameterConvention::Direct_Guaranteed);
        return SGF.emitManagedRValueWithCleanup(initPAI).getValue();
      };

      auto emitPartialSetterApply =
          [&](SILValue setterFRef,
              const SILFunctionConventions &setterConv) -> ManagedValue {
        // Emit captures for thI e setter
        SmallVector<SILValue, 4> capturedArgs;
        auto captureInfo = SGF.SGM.Types.getLoweredLocalCaptures(setter);
        if (!captureInfo.getCaptures().empty()) {
          SmallVector<ManagedValue, 4> captures;
          SGF.emitCaptures(loc, setter, CaptureEmission::AssignByWrapper,
                           captures);

          for (auto capture : captures)
            capturedArgs.push_back(capture.forward(SGF));
        } else {
          assert(base);

          SILValue capturedBase;
          unsigned argIdx = setterConv.getNumSILArguments() - 1;

          if (setterConv.getSILArgumentConvention(argIdx).isInoutConvention()) {
            capturedBase = base.getValue();
          } else if (base.getType().isAddress() &&
                     base.getType().getObjectType() ==
                         setterConv.getSILArgumentType(
                             argIdx, SGF.getTypeExpansionContext())) {
            // If the base is a reference and the setter expects a value, emit a
            // load. This pattern is emitted for property wrappers with a
            // nonmutating setter, for example.
            capturedBase = SGF.B.createTrivialLoadOr(
                loc, base.getValue(), LoadOwnershipQualifier::Copy);
          } else {
            capturedBase = base.copy(SGF, loc).forward(SGF);
          }

          capturedArgs.push_back(capturedBase);
        }

        PartialApplyInst *setterPAI = SGF.B.createPartialApply(
            loc, setterFRef, Substitutions, capturedArgs,
            ParameterConvention::Direct_Guaranteed);
        return SGF.emitManagedRValueWithCleanup(setterPAI);
      };

      auto emitValue =
          [&](VarDecl *field, Type fieldType, CanSILFunctionType setterTy,
              const SILFunctionConventions &setterConv) -> ManagedValue {
        // FIXME: This should use CallEmission instead of doing everything
        // manually.
        assert(value.isRValue());
        ManagedValue Mval =
            std::move(value).asKnownRValue(SGF).getAsSingleValue(SGF, loc);
        auto param = setterTy->getParameters()[0];
        SILType loweredSubstArgType = Mval.getType();
        if (param.isIndirectInOut()) {
          loweredSubstArgType = SILType::getPrimitiveAddressType(
              loweredSubstArgType.getASTType());
        }
        auto loweredSubstParamTy = SILType::getPrimitiveType(
            param.getArgumentType(SGF.SGM.M, setterTy,
                                  SGF.getTypeExpansionContext()),
            loweredSubstArgType.getCategory());
        // Handle reabstraction differences.
        if (Mval.getType() != loweredSubstParamTy) {
          Mval = SGF.emitSubstToOrigValue(
              loc, Mval, SGF.SGM.Types.getAbstractionPattern(field),
              fieldType->getCanonicalType());
        }

        // If we need the argument in memory, materialize an address.
        if (setterConv.getSILArgumentConvention(0).isIndirectConvention() &&
            !Mval.getType().isAddress()) {
          Mval = Mval.materialize(SGF, loc);
        }

        return Mval;
      };

      if (canRewriteSetAsInitAccessor(SGF)) {
        // Emit an assign_or_init with the allocating initializer function and the
        // setter function as arguments. DefiniteInitialization will then decide
        // between the two functions, depending if it's an init call or a
        // set call.
        VarDecl *field = cast<VarDecl>(Storage);
        auto FieldType = field->getValueInterfaceType();
        if (!Substitutions.empty()) {
          FieldType = FieldType.subst(Substitutions);
        }

        // Emit the init accessor function partially applied to the base.
        auto initFn = emitPartialInitAccessorApply(
            field->getOpaqueAccessor(AccessorKind::Init));

        // Emit the set accessor function partially applied to the base.
        auto setterFRef = getSetterFRef();
        auto setterTy = getSetterType(setterFRef);
        SILFunctionConventions setterConv(setterTy, SGF.SGM.M);
        auto setterFn = emitPartialSetterApply(setterFRef, setterConv);

        // Create the assign_or_init with the initializer and setter.
        auto value = emitValue(field, FieldType, setterTy, setterConv);
        SGF.B.createAssignOrInit(loc, base.getValue(), value.forward(SGF),
                                 initFn, setterFn.getValue(),
                                 AssignOrInitInst::Unknown);
        return;
      }

      if (canRewriteSetAsPropertyWrapperInit(SGF) &&
          !Storage->isStatic() &&
          isBackingVarVisible(cast<VarDecl>(Storage),
                              SGF.FunctionDC)) {
        // This is wrapped property. Instead of emitting a setter, emit an
        // assign_by_wrapper with the allocating initializer function and the
        // setter function as arguments. DefiniteInitialization will then decide
        // between the two functions, depending if it's an initialization or a
        // re-assignment.
        //
        VarDecl *field = cast<VarDecl>(Storage);
        VarDecl *backingVar = field->getPropertyWrapperBackingProperty();
        assert(backingVar);
        auto FieldType = field->getValueInterfaceType();
        auto ValType = backingVar->getValueInterfaceType();
        if (!Substitutions.empty()) {
          FieldType = FieldType.subst(Substitutions);
          ValType = ValType.subst(Substitutions);
        }

        auto typeData = getLogicalStorageTypeData(
            SGF.getTypeExpansionContext(), SGF.SGM, getTypeData().AccessKind,
            ValType->getCanonicalType());

        // Stores the address of the storage property.
        ManagedValue proj;

        // TODO: revist minimal
        SILType varStorageType = SGF.SGM.Types.getSubstitutedStorageType(
            TypeExpansionContext::minimal(), backingVar,
            ValType->getCanonicalType());

        if (!BaseFormalType) {
          proj =
              SGF.maybeEmitValueOfLocalVarDecl(backingVar, AccessKind::Write);
        } else if (BaseFormalType->mayHaveSuperclass()) {
          RefElementComponent REC(backingVar, LValueOptions(), varStorageType,
                                  typeData, /*actorIsolation=*/llvm::None);
          proj = std::move(REC).project(SGF, loc, base);
        } else {
          assert(BaseFormalType->getStructOrBoundGenericStruct());
          StructElementComponent SEC(backingVar, varStorageType, typeData,
                                     /*actorIsolation=*/llvm::None);
          proj = std::move(SEC).project(SGF, loc, base);
        }

        // The property wrapper backing initializer forms an instance of
        // the backing storage type from a wrapped value.
        SILDeclRef initConstant(
            field, SILDeclRef::Kind::PropertyWrapperBackingInitializer);
        SILValue initFRef = SGF.emitGlobalFunctionRef(loc, initConstant);

        PartialApplyInst *initPAI =
          SGF.B.createPartialApply(loc, initFRef,
                                   Substitutions, ArrayRef<SILValue>(),
                                   ParameterConvention::Direct_Guaranteed);
        ManagedValue initFn = SGF.emitManagedRValueWithCleanup(initPAI);

        // Create the allocating setter function. It captures the base address.
        auto setterFRef = getSetterFRef();
        auto setterTy = getSetterType(setterFRef);
        SILFunctionConventions setterConv(setterTy, SGF.SGM.M);
        auto setterFn = emitPartialSetterApply(setterFRef, setterConv);

        // Create the assign_by_wrapper with the initializer and setter.

        auto Mval = emitValue(field, FieldType, setterTy, setterConv);

        SGF.B.createAssignByWrapper(
            loc, Mval.forward(SGF), proj.forward(SGF), initFn.getValue(),
            setterFn.getValue(), AssignByWrapperInst::Unknown);

        return;
      }

      FormalEvaluationScope scope(SGF);
      // Pass in just the setter.
      auto args =
        std::move(*this).prepareAccessorArgs(SGF, loc, base, setter);

      return SGF.emitSetAccessor(loc, setter, Substitutions,
                                 std::move(args.base), IsSuper,
                                 IsDirectAccessorUse, std::move(args.Indices),
                                 std::move(value), IsOnSelfParameter);
    }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(isReadAccess(getAccessKind()) &&
             "shouldn't be using this path to call modify");
      return std::move(*this).LogicalPathComponent::project(SGF, loc, base);
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      assert(getAccessorDecl()->isGetter());

      SILDeclRef getter = Accessor;
      RValue rvalue;
      FormalEvaluationScope scope(SGF);

      auto args =
          std::move(*this).prepareAccessorArgs(SGF, loc, base, getter);

      rvalue = SGF.emitGetAccessor(
          loc, getter, Substitutions, std::move(args.base), IsSuper,
          IsDirectAccessorUse, std::move(args.Indices), c,
          IsOnSelfParameter, ActorIso);

      return rvalue;
    }
    
    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone = new GetterSetterComponent(*this, SGF, loc);
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      printBase(OS, indent, "GetterSetterComponent");
    }

    /// Compare 'this' lvalue and the 'rhs' lvalue (which is guaranteed to have
    /// the same dynamic PathComponent type as the receiver) to see if they are
    /// identical.  If so, there is a conflicting writeback happening, so emit a
    /// diagnostic.
    llvm::Optional<AccessStorage> getAccessStorage() const override {
      return AccessStorage{Storage, IsSuper,
                             Indices.isNull() ? nullptr : &Indices,
                             ArgListForDiagnostics };
    }
  };

  class MaterializeToTemporaryComponent final
      : public AccessComponent<LogicalPathComponent> {
    SubstitutionMap Substitutions;
    AccessStrategy ReadStrategy;
    AccessStrategy WriteStrategy;
    LValueOptions Options;
    bool IsSuper;
    bool IsOnSelfParameter;

  public:
    MaterializeToTemporaryComponent(AbstractStorageDecl *storage,
                                    bool isSuper, SubstitutionMap subs,
                                    LValueOptions options,
                                    AccessStrategy readStrategy,
                                    AccessStrategy writeStrategy,
                                    CanType baseFormalType,
                                    LValueTypeData typeData,
                                    ArgumentList *argListForDiagnostics,
                                    PreparedArguments &&indices,
                                    bool isOnSelfParameter)
      : AccessComponent(MaterializeToTemporaryKind, storage, baseFormalType,
                        typeData, argListForDiagnostics, std::move(indices)),
        Substitutions(subs),
        ReadStrategy(readStrategy), WriteStrategy(writeStrategy),
        Options(options), IsSuper(isSuper), IsOnSelfParameter(isOnSelfParameter)
        {}


    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      PreparedArguments clonedIndices = Indices.copy(SGF, loc);

      LogicalPathComponent *clone =
        new MaterializeToTemporaryComponent(Storage, IsSuper, Substitutions,
                                            Options,
                                            ReadStrategy, WriteStrategy,
                                            BaseFormalType, getTypeData(),
                                            ArgListForDiagnostics,
                                            std::move(clonedIndices),
                                            IsOnSelfParameter);
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext C) && override {
      LValue lv = std::move(*this).prepareLValue(SGF, loc, base,
                                                 SGFAccessKind::OwnedObjectRead,
                                                 ReadStrategy);
      return SGF.emitLoadOfLValue(loc, std::move(lv), C);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      LValue lv = std::move(*this).prepareLValue(SGF, loc, base,
                                                 SGFAccessKind::Write,
                                                 WriteStrategy);
      return SGF.emitAssignToLValue(loc, std::move(value), std::move(lv));
    }

    llvm::Optional<AccessStorage> getAccessStorage() const override {
      return AccessStorage{Storage, IsSuper,
                             Indices.isNull() ? nullptr : &Indices,
                             ArgListForDiagnostics};
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "MaterializeToTemporaryComponent\n";
    }

  private:
    LValue prepareLValue(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base, SGFAccessKind accessKind,
                         AccessStrategy strategy) && {
      LValue lv = [&] {
        if (!base) return LValue();
        auto baseAccessKind =
          getBaseAccessKind(SGF.SGM, Storage, accessKind, strategy,
                            BaseFormalType);
        return LValue::forValue(baseAccessKind, base, BaseFormalType);
      }();

      if (auto subscript = dyn_cast<SubscriptDecl>(Storage)) {
        lv.addMemberSubscriptComponent(SGF, loc, subscript, Substitutions,
                                       Options, IsSuper, accessKind, strategy,
                                       getSubstFormalType(),
                                       std::move(Indices),
                                       ArgListForDiagnostics);
      } else {
        auto var = cast<VarDecl>(Storage);
        if (base) {
          lv.addMemberVarComponent(SGF, loc, var, Substitutions, Options,
                                   IsSuper, accessKind, strategy,
                                   getSubstFormalType());
        } else {
          lv.addNonMemberVarComponent(SGF, loc, var, Substitutions, Options,
                                      accessKind, strategy,
                                      getSubstFormalType(),
                                      /*actorIsolation=*/llvm::None);
        }
      }
      
      if (lv.getTypeOfRValue() != getTypeOfRValue()) {
        lv.addOrigToSubstComponent(getTypeOfRValue());
      }

      return lv;
    }
  };

  /// A physical component which involves calling addressors.
  class AddressorComponent
      : public AccessorBasedComponent<PhysicalPathComponent> {
    SILType SubstFieldType;
  public:
     AddressorComponent(AbstractStorageDecl *decl, SILDeclRef accessor,
                        bool isSuper,
                        bool isDirectAccessorUse,
                        SubstitutionMap substitutions,
                        CanType baseFormalType, LValueTypeData typeData,
                        SILType substFieldType,
                        ArgumentList *argListForDiagnostics,
                        PreparedArguments &&indices, bool isOnSelfParameter)
      : AccessorBasedComponent(AddressorKind, decl, accessor, isSuper,
                               isDirectAccessorUse,
                               substitutions,
                               baseFormalType, typeData,
                               argListForDiagnostics, std::move(indices),
                               isOnSelfParameter),
        SubstFieldType(substFieldType)
    {
      assert(getAccessorDecl()->isAnyAddressor());
    }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(SGF.isInFormalEvaluationScope() &&
             "offsetting l-value for modification without writeback scope");

      ManagedValue addr;
      {
        FormalEvaluationScope scope(SGF);

        auto args =
            std::move(*this).prepareAccessorArgs(SGF, loc, base, Accessor);
        addr = SGF.emitAddressorAccessor(
            loc, Accessor, Substitutions, std::move(args.base), IsSuper,
            IsDirectAccessorUse, std::move(args.Indices),
            SubstFieldType, IsOnSelfParameter);
      }

      // Enter an unsafe access scope for the access.
      addr =
          enterAccessScope(SGF, loc, base, addr, getTypeData(), getAccessKind(),
                           SILAccessEnforcement::Unsafe, ActorIso);

      return addr;
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      printBase(OS, indent, "AddressorComponent");
    }
  };

  class EndApplyPseudoComponent : public WritebackPseudoComponent {
    CleanupHandle EndApplyHandle;
    AbstractStorageDecl *Storage;
    bool IsSuper;
    PreparedArguments PeekedIndices;
    ArgumentList *ArgListForDiagnostics;
  public:
    EndApplyPseudoComponent(const LValueTypeData &typeData,
                            CleanupHandle endApplyHandle,
                            AbstractStorageDecl *storage,
                            bool isSuper,
                            PreparedArguments &&peekedIndices,
                            ArgumentList *argListForDiagnostics)
      : WritebackPseudoComponent(typeData),
        EndApplyHandle(endApplyHandle),
        Storage(storage), IsSuper(isSuper),
        PeekedIndices(std::move(peekedIndices)),
        ArgListForDiagnostics(argListForDiagnostics) {}

  private:
    void writeback(SILGenFunction &SGF, SILLocation loc,
                   ManagedValue base,
                   MaterializedLValue materialized,
                   bool isFinal) override {
      // Just let the cleanup get emitted normally if the writeback is for
      // an unwind.
      if (!isFinal) return;

      SGF.Cleanups.popAndEmitCleanup(EndApplyHandle, CleanupLocation(loc),
                                     NotForUnwind);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "EndApplyPseudoComponent";
    }

    llvm::Optional<AccessStorage> getAccessStorage() const override {
      return AccessStorage{Storage, IsSuper,
                             PeekedIndices.isNull() ? nullptr : &PeekedIndices,
                             ArgListForDiagnostics};
    }
  };
}

static void pushEndApplyWriteback(SILGenFunction &SGF, SILLocation loc,
                                  CleanupHandle endApplyHandle,
                                  LValueTypeData typeData,
                                  ManagedValue base = ManagedValue(),
                                  AbstractStorageDecl *storage = nullptr,
                                  bool isSuper = false,
                                  PreparedArguments &&indices
                                    = PreparedArguments(),
                                  ArgumentList *argListForDiagnostics = nullptr) {
  std::unique_ptr<LogicalPathComponent>
    component(new EndApplyPseudoComponent(typeData, endApplyHandle,
                                          storage, isSuper, std::move(indices),
                                          argListForDiagnostics));
  pushWriteback(SGF, loc, std::move(component), /*for diagnostics*/ base,
                MaterializedLValue());
}

namespace {

  /// A physical component which involves calling coroutine accessors.
  class CoroutineAccessorComponent
      : public AccessorBasedComponent<PhysicalPathComponent> {
  public:
    CoroutineAccessorComponent(AbstractStorageDecl *decl, SILDeclRef accessor,
                               bool isSuper, bool isDirectAccessorUse,
                               SubstitutionMap substitutions,
                               CanType baseFormalType, LValueTypeData typeData,
                               ArgumentList *argListForDiagnostics,
                               PreparedArguments &&indices,
                               bool isOnSelfParameter)
        : AccessorBasedComponent(
              CoroutineAccessorKind, decl, accessor, isSuper,
              isDirectAccessorUse,
              substitutions, baseFormalType, typeData,
              argListForDiagnostics, std::move(indices), isOnSelfParameter) {}

    using AccessorBasedComponent::AccessorBasedComponent;

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(SGF.isInFormalEvaluationScope() &&
             "offsetting l-value for modification without writeback scope");

      ManagedValue result;

      auto args =
        std::move(*this).prepareAccessorArgs(SGF, loc, base, Accessor);
      auto peekedIndices = args.Indices.copyForDiagnostics();
      SmallVector<ManagedValue, 4> yields;
      auto endApplyHandle = SGF.emitCoroutineAccessor(
          loc, Accessor, Substitutions, std::move(args.base), IsSuper,
          IsDirectAccessorUse, std::move(args.Indices), yields,
          IsOnSelfParameter);

      // Push a writeback that ends the access.
      pushEndApplyWriteback(SGF, loc, endApplyHandle, getTypeData(),
                            base, Storage, IsSuper, std::move(peekedIndices),
                            ArgListForDiagnostics);

      auto decl = cast<AccessorDecl>(Accessor.getFuncDecl());

      // 'modify' always returns an address of the right type.
      if (decl->getAccessorKind() == AccessorKind::Modify) {
        assert(yields.size() == 1);
        return yields[0];
      }

      // 'read' returns a borrowed r-value, which might or might not be
      // an address of the right type.

      // Use the yield value directly if it's the right type.
      if (!getOrigFormalType().isTuple()) {
        assert(yields.size() == 1);
        auto value = yields[0];
        if (value.getType().isAddress() ||
            !isReadAccessResultAddress(getAccessKind()))
          return value;
      }

      // Otherwise, we need to make a temporary.
      // TODO: build a scalar tuple if possible.
      auto temporary =
        SGF.emitTemporary(loc, SGF.getTypeLowering(getTypeOfRValue()));
      auto yieldsAsArray = llvm::makeArrayRef(yields);
      copyBorrowedYieldsIntoTemporary(SGF, loc, yieldsAsArray,
                                      getOrigFormalType(), getSubstFormalType(),
                                      temporary.get());
      assert(yieldsAsArray.empty() && "didn't claim all yields");
      return temporary->getManagedAddress();
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      printBase(OS, indent, "CoroutineAccessorComponent");
    }
  };
}

static ManagedValue
makeBaseConsumableMaterializedRValue(SILGenFunction &SGF,
                                     SILLocation loc, ManagedValue base) {
  if (base.isLValue()) {
    auto tmp = SGF.emitTemporaryAllocation(loc, base.getType());
    SGF.B.createCopyAddr(loc, base.getValue(), tmp,
                         IsNotTake, IsInitialization);
    return SGF.emitManagedBufferWithCleanup(tmp);
  }

  bool isBorrowed = base.isPlusZeroRValueOrTrivial()
    && !base.getType().isTrivial(SGF.F);
  if (!base.getType().isAddress() || isBorrowed) {
    if (SGF.useLoweredAddresses()) {
      auto tmp = SGF.emitTemporaryAllocation(loc, base.getType());
      if (isBorrowed)
        base.copyInto(SGF, loc, tmp);
      else
        base.forwardInto(SGF, loc, tmp);
      return SGF.emitManagedBufferWithCleanup(tmp);
    } else {
      return base.copy(SGF, loc);
    }
  }

  return base;
}

static ManagedValue
emitUpcastToKeyPath(SILGenFunction &SGF, SILLocation loc,
                    KeyPathTypeKind typeKind, ManagedValue keyPath) {
  if (typeKind == KPTK_KeyPath) return keyPath;
  assert(typeKind == KPTK_WritableKeyPath ||
         typeKind == KPTK_ReferenceWritableKeyPath);

  auto derivedKeyPathTy = keyPath.getType().castTo<BoundGenericType>();
  auto baseKeyPathTy =
    BoundGenericType::get(SGF.getASTContext().getKeyPathDecl(),
                          Type(), derivedKeyPathTy->getGenericArgs())
      ->getCanonicalType();
  return SGF.B.createUpcast(loc, keyPath,
                            SILType::getPrimitiveObjectType(baseKeyPathTy));
}

namespace {
  class LogicalKeyPathApplicationComponent final
      : public LogicalPathComponent {
    KeyPathTypeKind TypeKind;
    ManagedValue KeyPath;
    Type BaseFormalType;
  public:
    LogicalKeyPathApplicationComponent(LValueTypeData typeData,
                                       KeyPathTypeKind typeKind,
                                       ManagedValue keyPath,
                                       Type baseFormalType)
      : LogicalPathComponent(typeData, LogicalKeyPathApplicationKind),
        TypeKind(typeKind), KeyPath(keyPath), BaseFormalType(baseFormalType) {
      assert(isReadAccess(getAccessKind()) ||
             typeKind == KPTK_WritableKeyPath ||
             typeKind == KPTK_ReferenceWritableKeyPath);
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext C) && override {
      assert(isReadAccess(getAccessKind()));
      FuncDecl *projectFn;

      auto keyPathValue = KeyPath;

      SmallVector<Type, 2> typeArgs;
      typeArgs.push_back(BaseFormalType);
      if (TypeKind == KPTK_AnyKeyPath) {
        projectFn = SGF.getASTContext().getGetAtAnyKeyPath();
      } else if (TypeKind == KPTK_PartialKeyPath) {
        projectFn = SGF.getASTContext().getGetAtPartialKeyPath();
      } else if (TypeKind == KPTK_KeyPath ||
                 TypeKind == KPTK_WritableKeyPath ||
                 TypeKind == KPTK_ReferenceWritableKeyPath) {
        projectFn = SGF.getASTContext().getGetAtKeyPath();

        auto keyPathTy = keyPathValue.getType().castTo<BoundGenericType>();
        assert(keyPathTy->getGenericArgs().size() == 2);
        assert(keyPathTy->getGenericArgs()[0]->getCanonicalType() ==
               BaseFormalType->getCanonicalType());
        typeArgs.push_back(keyPathTy->getGenericArgs()[1]);

        keyPathValue = emitUpcastToKeyPath(SGF, loc, TypeKind, keyPathValue);
      } else {
        llvm_unreachable("bad key path kind for this component");
      }

      auto subs = SubstitutionMap::get(projectFn->getGenericSignature(),
                                       ArrayRef<Type>(typeArgs),
                                       ArrayRef<ProtocolConformanceRef>());

      base = makeBaseConsumableMaterializedRValue(SGF, loc, base);

      return SGF.emitApplyOfLibraryIntrinsic(loc, projectFn, subs,
                                             {base, keyPathValue}, C);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      assert(!isReadAccess(getAccessKind()));

      auto keyPathValue = KeyPath;
      FuncDecl *setFn;
      if (TypeKind == KPTK_WritableKeyPath) {
        setFn = SGF.getASTContext().getSetAtWritableKeyPath();
        assert(base.isLValue());
      } else if (TypeKind == KPTK_ReferenceWritableKeyPath) {
        setFn = SGF.getASTContext().getSetAtReferenceWritableKeyPath();
        base = makeBaseConsumableMaterializedRValue(SGF, loc, base);
      } else {
        llvm_unreachable("bad writable type kind");
      }

      auto keyPathTy = keyPathValue.getType().castTo<BoundGenericType>();
      auto subs = SubstitutionMap::get(setFn->getGenericSignature(),
                                       keyPathTy->getGenericArgs(),
                                       ArrayRef<ProtocolConformanceRef>());

      auto origType = AbstractionPattern::getOpaque();
      auto loweredTy = SGF.getLoweredType(origType, value.getSubstRValueType());

      auto setValue =
        std::move(value).getAsSingleValue(SGF, origType, loweredTy);
      if (!setValue.getType().isAddress()) {
        setValue = setValue.materialize(SGF, loc);
      }

      SGF.emitApplyOfLibraryIntrinsic(loc, setFn, subs,
                                      {base, keyPathValue, setValue},
                                      SGFContext());
    }

    llvm::Optional<AccessStorage> getAccessStorage() const override {
      return llvm::None;
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation l) const override {
      llvm_unreachable("can't be cloned");
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "LogicalKeyPathApplicationComponent\n";
    }
  };

  /// A physical component which involves applying a key path.
  class PhysicalKeyPathApplicationComponent final
      : public PhysicalPathComponent {
    KeyPathTypeKind TypeKind;
    ManagedValue KeyPath;
  public:
    PhysicalKeyPathApplicationComponent(LValueTypeData typeData,
                                        KeyPathTypeKind typeKind,
                                        ManagedValue keyPath)
        : PhysicalPathComponent(typeData, PhysicalKeyPathApplicationKind,
                                /*actorIsolation=*/llvm::None),
          TypeKind(typeKind), KeyPath(keyPath) {
      assert(typeKind == KPTK_KeyPath ||
             typeKind == KPTK_WritableKeyPath ||
             typeKind == KPTK_ReferenceWritableKeyPath);
      assert(typeKind != KPTK_KeyPath || isReadAccess(getAccessKind()));
    }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(SGF.isInFormalEvaluationScope() &&
             "offsetting l-value for modification without writeback scope");

      bool isRead = isReadAccess(getAccessKind());

      // Set up the base and key path values correctly.
      auto keyPathValue = KeyPath;
      if (isRead) {
        keyPathValue = emitUpcastToKeyPath(SGF, loc, TypeKind, keyPathValue);
        base = makeBaseConsumableMaterializedRValue(SGF, loc, base);
      } else if (TypeKind == KPTK_WritableKeyPath) {
        // nothing to do
      } else if (TypeKind == KPTK_ReferenceWritableKeyPath) {
        base = makeBaseConsumableMaterializedRValue(SGF, loc, base);
      } else {
        llvm_unreachable("bad combination");
      }

      SILFunction *projectFn =
        SGF.SGM.getKeyPathProjectionCoroutine(isRead, TypeKind);
      auto projectFnRef = SGF.B.createManagedFunctionRef(loc, projectFn);
      auto projectFnType = projectFn->getLoweredFunctionType();

      auto keyPathTy = keyPathValue.getType().castTo<BoundGenericType>();
      auto subs = SubstitutionMap::get(
                                 projectFnType->getInvocationGenericSignature(),
                                 keyPathTy->getGenericArgs(), {});

      auto substFnType = projectFnType->substGenericArgs(
          SGF.SGM.M, subs, SGF.getTypeExpansionContext());

      // Perform the begin_apply.
      SmallVector<ManagedValue, 1> yields;
      auto cleanup =
        SGF.emitBeginApply(loc, projectFnRef, subs, { base, keyPathValue },
                           substFnType, ApplyOptions(), yields);

      // Push an operation to do the end_apply.
      pushEndApplyWriteback(SGF, loc, cleanup, getTypeData());

      assert(yields.size() == 1);
      return yields[0];
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "PhysicalKeyPathApplicationComponent\n";
    }
  };

  /// A translation component that performs \c unchecked_*_cast 's as-needed.
  class UncheckedConversionComponent final : public TranslationPathComponent {
  private:
    Type OrigType;

    /// \returns the type this component is trying to convert \b to
    CanType getTranslatedType() const {
      return getTypeData().SubstFormalType->getCanonicalType();
    }

    /// \returns the type this component is trying to convert \b from
    CanType getUntranslatedType() const {
      return OrigType->getRValueType()->getCanonicalType();
    }

    /// perform a conversion of ManagedValue -> ManagedValue
    ManagedValue doUncheckedConversion(SILGenFunction &SGF, SILLocation loc,
                                       ManagedValue val, CanType toType) {
      auto toTy = SGF.getLoweredType(toType);
      auto fromTy = val.getType();

      if (fromTy == toTy)
        return val; // nothing to do.

      // otherwise emit the right kind of cast based on whether it's an address.
      assert(fromTy.isAddress() == toTy.isAddress());

      if (toTy.isAddress())
        return SGF.B.createUncheckedAddrCast(loc, val, toTy);

      return SGF.B.createUncheckedBitCast(loc, val, toTy);
    }

    /// perform a conversion of RValue -> RValue
    RValue doUncheckedConversion(SILGenFunction &SGF, SILLocation loc,
                                 RValue &&rv, CanType toType) {
      auto val = std::move(rv).getAsSingleValue(SGF, loc);
      val = doUncheckedConversion(SGF, loc, val, toType);
      return RValue(SGF, loc, toType, val);
    }

  public:
    /// \param OrigType is the type we are converting \b from
    /// \param typeData will contain the type we are converting \b to
    UncheckedConversionComponent(LValueTypeData typeData, Type OrigType)
      : TranslationPathComponent(typeData, UncheckedConversionKind),
      OrigType(OrigType) {}

    bool isLoadingPure() const override { return true; }

    /// Used during write operations to convert the value prior to writing to
    /// the base.
    RValue untranslate(SILGenFunction &SGF, SILLocation loc,
                       RValue &&rv, SGFContext c) && override {
      return doUncheckedConversion(SGF, loc, std::move(rv),
                getUntranslatedType());
    }

    /// Used during read operations to convert the value after reading the base.
    RValue translate(SILGenFunction &SGF, SILLocation loc,
                     RValue &&rv, SGFContext c) && override {
      return doUncheckedConversion(SGF, loc, std::move(rv),
                getTranslatedType());
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      return std::make_unique<UncheckedConversionComponent>(getTypeData(),
                                                            OrigType);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "UncheckedConversionComponent"
                        << "\n\tfromType: " << getUntranslatedType()
                        << "\n\ttoType: " << getTranslatedType()
                        << "\n";
    }
  };
} // end anonymous namespace

RValue
TranslationPathComponent::get(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue base, SGFContext c) && {
  // Inline constructor.
  RValue baseVal = [&]() -> RValue {
    // If our base is an object, just put it into an RValue and return.
    if (base.getType().isObject()) {
      return RValue(SGF, loc, getSubstFormalType(), base);
    }

    // Otherwise, load the value and put it into an RValue.
    return RValue(SGF, loc, getSubstFormalType(),
                  SGF.emitLoad(loc, base.getValue(),
                               SGF.getTypeLowering(base.getType()),
                               SGFContext(), IsNotTake));
  }();

  // Map the base value to its substituted representation.
  return std::move(*this).translate(SGF, loc, std::move(baseVal), c);
}

void TranslationPathComponent::set(SILGenFunction &SGF, SILLocation loc,
                                   ArgumentSource &&valueSource,
                                   ManagedValue base) && {
  assert(base.getType().isAddress() &&
         "Only support setting bases that have addresses");
  RValue value = std::move(valueSource).getAsRValue(SGF);

  // Map the value to the original pattern.
  RValue newValue = std::move(*this).untranslate(SGF, loc, std::move(value));

  // Store to the base.
  std::move(newValue).assignInto(SGF, loc, base.getValue());
}

namespace {
  /// Remap an lvalue referencing a generic type to an lvalue of its
  /// substituted type in a concrete context.
  class OrigToSubstComponent : public TranslationPathComponent {
    AbstractionPattern OrigType;

  public:
    OrigToSubstComponent(const LValueTypeData &typeData,
                         AbstractionPattern origType)
      : TranslationPathComponent(typeData, OrigToSubstKind),
        OrigType(origType)
    {}

    virtual bool isLoadingPure() const override { return true; }

    RValue untranslate(SILGenFunction &SGF, SILLocation loc,
                       RValue &&rv, SGFContext c) && override {
      return SGF.emitSubstToOrigValue(loc, std::move(rv), OrigType,
                                      getSubstFormalType(), c);
    }

    RValue translate(SILGenFunction &SGF, SILLocation loc,
                     RValue &&rv, SGFContext c) && override {
      return SGF.emitOrigToSubstValue(loc, std::move(rv), OrigType,
                                      getSubstFormalType(), c);
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone
        = new OrigToSubstComponent(getTypeData(), OrigType);
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OrigToSubstComponent("
                        << getOrigFormalType() << ", "
                        << getSubstFormalType() << ", "
                        << getTypeOfRValue() << ")\n";
    }
  };

  /// Remap an lvalue referencing a concrete type to an lvalue of a
  /// generically-reabstracted type.
  class SubstToOrigComponent : public TranslationPathComponent {
  public:
    SubstToOrigComponent(const LValueTypeData &typeData)
      : TranslationPathComponent(typeData, SubstToOrigKind)
    {}

    virtual bool isLoadingPure() const override { return true; }

    RValue untranslate(SILGenFunction &SGF, SILLocation loc,
                       RValue &&rv, SGFContext c) && override {
      return SGF.emitOrigToSubstValue(loc, std::move(rv), getOrigFormalType(),
                                      getSubstFormalType(), c);
    }

    RValue translate(SILGenFunction &SGF, SILLocation loc,
                     RValue &&rv, SGFContext c) && override {
      return SGF.emitSubstToOrigValue(loc, std::move(rv), getOrigFormalType(),
                                      getSubstFormalType(), c);
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone
        = new SubstToOrigComponent(getTypeData());
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "SubstToOrigComponent("
                        << getOrigFormalType() << ", "
                        << getSubstFormalType() << ", "
                        << getTypeOfRValue() << ")\n";
    }
  };

  /// Remap a weak value to Optional<T>*, or unowned pointer to T*.
  class OwnershipComponent : public LogicalPathComponent {
  public:
    OwnershipComponent(LValueTypeData typeData)
      : LogicalPathComponent(typeData, OwnershipKind) {
    }

    virtual bool isLoadingPure() const override { return true; }

    llvm::Optional<AccessStorage> getAccessStorage() const override {
      return llvm::None;
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      assert(base && "ownership component must not be root of lvalue path");

      auto &TL = SGF.getTypeLowering(getTypeOfRValue());

      ManagedValue result;
      if (base.getType().isObject()) {
        result = SGF.emitConversionToSemanticRValue(loc, base, TL);
      } else {
        result = SGF.emitLoad(loc, base.getValue(), TL, SGFContext(),
                              IsNotTake);
      }

      return RValue(SGF, loc, getSubstFormalType(), result);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&valueSource, ManagedValue base) && override {
      assert(base && "ownership component must not be root of lvalue path");
      auto &TL = SGF.getTypeLowering(base.getType());

      auto value = std::move(valueSource).getAsSingleValue(SGF).forward(SGF);
      SGF.emitSemanticStore(loc, value, base.getValue(), TL,
                            IsNotInitialization);
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone = new OwnershipComponent(getTypeData());
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OwnershipComponent(...)\n";
    }
  };
} // end anonymous namespace

LValue LValue::forValue(SGFAccessKind accessKind, ManagedValue value,
                        CanType substFormalType) {
  if (value.getType().isObject()) {
    LValueTypeData typeData = getValueTypeData(accessKind, substFormalType,
                                               value.getValue());

    LValue lv;
    lv.add<ValueComponent>(value, llvm::None, typeData, /*isRValue=*/true);
    return lv;
  } else {
    // Treat an address-only value as an lvalue we only read from.
    if (!value.isLValue())
      value = ManagedValue::forLValue(value.getValue());
    return forAddress(accessKind, value, llvm::None,
                      AbstractionPattern(substFormalType), substFormalType);
  }
}

LValue LValue::forAddress(SGFAccessKind accessKind, ManagedValue address,
                          llvm::Optional<SILAccessEnforcement> enforcement,
                          AbstractionPattern origFormalType,
                          CanType substFormalType) {
  assert(address.isLValue());
  LValueTypeData typeData = {
    accessKind, origFormalType, substFormalType,
    address.getType().getASTType()
  };

  LValue lv;
  lv.add<ValueComponent>(address, enforcement, typeData);
  return lv;
}

void LValue::addMemberComponent(SILGenFunction &SGF, SILLocation loc,
                                AbstractStorageDecl *storage,
                                SubstitutionMap subs,
                                LValueOptions options,
                                bool isSuper,
                                SGFAccessKind accessKind,
                                AccessStrategy accessStrategy,
                                CanType formalRValueType,
                                PreparedArguments &&indices,
                                ArgumentList *argListForDiagnostics) {
  if (auto var = dyn_cast<VarDecl>(storage)) {
    assert(indices.isNull());
    addMemberVarComponent(SGF, loc, var, subs, options, isSuper,
                          accessKind, accessStrategy, formalRValueType);
  } else {
    auto subscript = cast<SubscriptDecl>(storage);
    addMemberSubscriptComponent(SGF, loc, subscript, subs, options, isSuper,
                                accessKind, accessStrategy, formalRValueType,
                                std::move(indices), argListForDiagnostics);
  }
}

void LValue::addOrigToSubstComponent(SILType loweredSubstType) {
  loweredSubstType = loweredSubstType.getObjectType();
  assert(getTypeOfRValue() != loweredSubstType &&
         "reabstraction component is unnecessary!");

  // Peephole away complementary reabstractions.
  assert(!Path.empty() && "adding translation component to empty l-value");
  if (Path.back()->getKind() == PathComponent::SubstToOrigKind) {
    // But only if the lowered type matches exactly.
    if (Path[Path.size()-2]->getTypeOfRValue() == loweredSubstType) {
      Path.pop_back();
      return;
    }
    // TODO: combine reabstractions; this doesn't matter all that much
    // for most things, but it can be dramatically better for function
    // reabstraction.
  }

  auto substFormalType = getSubstFormalType();
  LValueTypeData typeData = {
    getAccessKind(),
    AbstractionPattern(substFormalType),
    substFormalType,
    loweredSubstType.getASTType()
  };
  add<OrigToSubstComponent>(typeData, getOrigFormalType());
}

void LValue::addSubstToOrigComponent(AbstractionPattern origType,
                                     SILType loweredSubstType) {
  loweredSubstType = loweredSubstType.getObjectType();
  assert(getTypeOfRValue() != loweredSubstType &&
         "reabstraction component is unnecessary!");

  // Peephole away complementary reabstractions.
  assert(!Path.empty() && "adding translation component to empty l-value");
  if (Path.back()->getKind() == PathComponent::OrigToSubstKind) {
    // But only if the lowered type matches exactly.
    if (Path[Path.size()-2]->getTypeOfRValue() == loweredSubstType) {
      Path.pop_back();
      return;
    }
    // TODO: combine reabstractions; this doesn't matter all that much
    // for most things, but it can be dramatically better for function
    // reabstraction.
  }

  LValueTypeData typeData = {
    getAccessKind(),
    origType,
    getSubstFormalType(),
    loweredSubstType.getASTType()
  };
  add<SubstToOrigComponent>(typeData);
}

void LValue::dump() const {
  dump(llvm::errs());
}

void LValue::dump(raw_ostream &OS, unsigned indent) const {
  for (const auto &component : *this) {
    component->dump(OS, indent);
  }
}

LValue SILGenFunction::emitLValue(Expr *e, SGFAccessKind accessKind,
                                  LValueOptions options) {
  // Some lvalue nodes (namely BindOptionalExprs) require immediate evaluation
  // of their subexpression, so we must have a writeback scope open while
  // building an lvalue.
  assert(isInFormalEvaluationScope() && "must be in a formal evaluation scope");

  LValue r = SILGenLValue(*this).visit(e, accessKind, options);
  // If the final component has an abstraction change, introduce a
  // reabstraction component.
  auto substFormalType = r.getSubstFormalType();
  auto loweredSubstType = getLoweredType(substFormalType);
  if (r.getTypeOfRValue() != loweredSubstType.getObjectType()) {
    // Logical components always re-abstract back to the substituted
    // type.
    assert(r.isLastComponentPhysical());
    r.addOrigToSubstComponent(loweredSubstType);
  }
  return r;
}

static LValue visitRecInOut(SILGenLValue &SGL, Expr *e,
                            SGFAccessKind accessKind,
                            LValueOptions options, AbstractionPattern orig) {
  auto lv = SGL.visit(e, accessKind, options);
  // If necessary, handle reabstraction with a SubstToOrigComponent that handles
  // writeback in the original representation.
  if (orig.isValid()) {
    auto &origTL = SGL.SGF.getTypeLowering(orig, e->getType()->getRValueType());
    if (lv.getTypeOfRValue() != origTL.getLoweredType().getObjectType())
      lv.addSubstToOrigComponent(orig, origTL.getLoweredType().getObjectType());
  }

  return lv;
}

// Otherwise we have a non-lvalue type (references, values, metatypes,
// etc). These act as the root of a logical lvalue.
static ManagedValue visitRecNonInOutBase(SILGenLValue &SGL, Expr *e,
                                         SGFAccessKind accessKind,
                                         LValueOptions options,
                                         AbstractionPattern orig) {
  auto &SGF = SGL.SGF;

  // For an rvalue base, apply the reabstraction (if any) eagerly, since
  // there's no need for writeback.
  if (orig.isValid()) {
    return SGF.emitRValueAsOrig(
        e, orig, SGF.getTypeLowering(orig, e->getType()->getRValueType()));
  }

  // Ok, at this point we know that re-abstraction is not required.

  SGFContext ctx;

  if (auto *dre = dyn_cast<DeclRefExpr>(e)) {
    // Any reference to "self" can be done at +0 so long as it is a direct
    // access, since we know it is guaranteed.
    //
    // TODO: it would be great to factor this even lower into SILGen to the
    // point where we can see that the parameter is +0 guaranteed.  Note that
    // this handles the case in initializers where there is actually a stack
    // allocation for it as well.
    if (isa<ParamDecl>(dre->getDecl()) &&
        dre->getDecl()->getName() == SGF.getASTContext().Id_self &&
        dre->getDecl()->isImplicit()) {
      ctx = SGFContext::AllowGuaranteedPlusZero;
      if (SGF.SelfInitDelegationState != SILGenFunction::NormalSelf) {
        // This needs to be inlined since there is a Formal Evaluation Scope
        // in emitRValueForDecl that causing any borrow for this LValue to be
        // popped too soon.
        auto *vd = cast<ParamDecl>(dre->getDecl());
        CanType formalRValueType = dre->getType()->getCanonicalType();
        ManagedValue selfLValue =
          SGF.emitAddressOfLocalVarDecl(dre, vd, formalRValueType,
                                        SGFAccessKind::OwnedObjectRead);
        selfLValue = SGF.emitFormalEvaluationRValueForSelfInDelegationInit(
                            e, formalRValueType,
                            selfLValue.getLValueAddress(), ctx)
                         .getAsSingleValue(SGF, e);

        return selfLValue;
      }
    }

    if (auto *VD = dyn_cast<VarDecl>(dre->getDecl())) {
      // All let values are guaranteed to be held alive across their lifetime,
      // and won't change once initialized.  Any loaded value is good for the
      // duration of this expression evaluation.
      if (VD->isLet()) {
        ctx = SGFContext::AllowGuaranteedPlusZero;
      }
    }
  }
  
  if (SGF.SGM.Types.isIndirectPlusZeroSelfParameter(e->getType())) {
    ctx = SGFContext::AllowGuaranteedPlusZero;
  }

  ManagedValue mv = SGF.emitRValueAsSingleValue(e, ctx);
  if (mv.isPlusZeroRValueOrTrivial())
    return mv;

  // Any temporaries needed to materialize the lvalue must be destroyed when
  // at the end of the lvalue's formal evaluation scope.
  // e.g. for foo(self.bar)
  //   %self = load [copy] %ptr_self
  //   %rvalue = barGetter(%self)
  //   destroy_value %self // self must be released before calling foo.
  //   foo(%rvalue)
  SILValue value = mv.forward(SGF);
  return SGF.emitFormalAccessManagedRValueWithCleanup(CleanupLocation(e),
                                                      value);
}

LValue SILGenLValue::visitRec(Expr *e, SGFAccessKind accessKind,
                              LValueOptions options, AbstractionPattern orig) {
  // First see if we have an lvalue type. If we do, then quickly handle that and
  // return.
  if (e->getType()->is<LValueType>() || e->isSemanticallyInOutExpr()) {
    return visitRecInOut(*this, e, accessKind, options, orig);
  }
  
  // If the base is a load of a noncopyable type (or, eventually, when we have
  // a `borrow x` operator, the operator is used on the base here), we want to
  // apply the lvalue within a formal access to the original value instead of
  // an actual loaded copy.
  if (e->getType()->isPureMoveOnly()) {
    if (auto load = dyn_cast<LoadExpr>(e)) {
      LValue lv = visitRec(load->getSubExpr(), SGFAccessKind::BorrowedAddressRead,
                               options, orig);
      CanType formalType = getSubstFormalRValueType(e);
      LValueTypeData typeData{accessKind, AbstractionPattern(formalType),
                              formalType, lv.getTypeOfRValue().getASTType()};
      lv.add<BorrowValueComponent>(typeData);
      return lv;
    }
  }

  // Otherwise we have a non-lvalue type (references, values, metatypes,
  // etc). These act as the root of a logical lvalue. Compute the root value,
  // wrap it in a ValueComponent, and return it for our caller.
  ManagedValue rv = visitRecNonInOutBase(*this, e, accessKind, options, orig);
  CanType formalType = getSubstFormalRValueType(e);
  auto typeData = getValueTypeData(accessKind, formalType, rv.getValue());
  LValue lv;
  lv.add<ValueComponent>(rv, llvm::None, typeData, /*isRValue=*/true);
  return lv;
}

LValue SILGenLValue::visitExpr(Expr *e, SGFAccessKind accessKind,
                               LValueOptions options) {
  e->dump(llvm::errs());
  llvm::errs() << "\n";
  llvm_unreachable("unimplemented lvalue expr");
}

namespace {
  /// A CRTP class for emitting accesses.
  template <class Impl, class StorageType>
  struct AccessEmitter {
    SILGenFunction &SGF;
    StorageType *Storage;
    SubstitutionMap Subs;
    CanType FormalRValueType;
    SGFAccessKind AccessKind;

    Impl &asImpl() { return static_cast<Impl&>(*this); }

    AccessEmitter(SILGenFunction &SGF, StorageType *storage,
                  SubstitutionMap subs, SGFAccessKind accessKind,
                  CanType formalRValueType)
      : SGF(SGF), Storage(storage), Subs(subs),
        FormalRValueType(formalRValueType), AccessKind(accessKind) {}

    void emitUsingStrategy(AccessStrategy strategy) {
      switch (strategy.getKind()) {
      case AccessStrategy::Storage: {
        auto typeData =
            getPhysicalStorageTypeData(SGF.getTypeExpansionContext(), SGF.SGM,
                                       AccessKind, Storage, Subs,
                                       FormalRValueType);
        return asImpl().emitUsingStorage(typeData);
      }

      case AccessStrategy::DirectToAccessor:
        return asImpl()
            .emitUsingAccessor(strategy.getAccessor(),
                               /*isDirect=*/true);

      case AccessStrategy::DispatchToAccessor:
        return asImpl().emitUsingAccessor(strategy.getAccessor(),
                                          /*isDirect=*/false);

      case AccessStrategy::MaterializeToTemporary: {
        auto typeData = getLogicalStorageTypeData(
            SGF.getTypeExpansionContext(), SGF.SGM, AccessKind, FormalRValueType);
        return asImpl().emitUsingMaterialization(strategy.getReadStrategy(),
                                                 strategy.getWriteStrategy(),
                                                 typeData);
      }

      case AccessStrategy::DispatchToDistributedThunk:
        return asImpl().emitUsingDistributedThunk();
      }
      llvm_unreachable("unknown kind");
    }

    void emitUsingAccessor(AccessorKind accessorKind,
                           bool isDirect) {
      auto accessor =
          SGF.getAccessorDeclRef(Storage->getOpaqueAccessor(accessorKind));

      switch (accessorKind) {
      case AccessorKind::Set: {
        LLVM_FALLTHROUGH;
      }
      case AccessorKind::Get: {
        auto typeData = getLogicalStorageTypeData(
            SGF.getTypeExpansionContext(), SGF.SGM, AccessKind, FormalRValueType);
        return asImpl().emitUsingGetterSetter(accessor, isDirect, typeData);
      }

      case AccessorKind::Address:
      case AccessorKind::MutableAddress: {
        auto typeData =
            getPhysicalStorageTypeData(SGF.getTypeExpansionContext(), SGF.SGM,
                                       AccessKind, Storage, Subs,
                                       FormalRValueType);
        return asImpl().emitUsingAddressor(accessor, isDirect, typeData);
      }

      case AccessorKind::Read:
      case AccessorKind::Modify: {
        auto typeData =
            getPhysicalStorageTypeData(SGF.getTypeExpansionContext(), SGF.SGM,
                                       AccessKind, Storage, Subs,
                                       FormalRValueType);
        return asImpl().emitUsingCoroutineAccessor(accessor, isDirect,
                                                   typeData);
      }

      case AccessorKind::WillSet:
      case AccessorKind::DidSet:
        llvm_unreachable("cannot use accessor directly to perform an access");

      case AccessorKind::Init:
        llvm_unreachable("init accessor not yet implemented");
      }

      llvm_unreachable("bad kind");
    }
  };
}

static LValue emitLValueForNonMemberVarDecl(
    SILGenFunction &SGF, SILLocation loc, ConcreteDeclRef declRef,
    CanType formalRValueType, SGFAccessKind accessKind, LValueOptions options,
    AccessSemantics semantics, llvm::Optional<ActorIsolation> actorIso) {
  LValue lv;

  auto *var = cast<VarDecl>(declRef.getDecl());
  auto subs = declRef.getSubstitutions();
  if (!subs)
    subs = SGF.F.getForwardingSubstitutionMap();

  auto access = getFormalAccessKind(accessKind);
  auto strategy = var->getAccessStrategy(semantics, access,
                                         SGF.SGM.M.getSwiftModule(),
                                         SGF.F.getResilienceExpansion());

  lv.addNonMemberVarComponent(SGF, loc, var, subs,
                              options, accessKind, strategy, formalRValueType,
                              actorIso);

  return lv;
}

/// Map a SILGen access kind back to an AST access kind.
static AccessKind mapAccessKind(SGFAccessKind accessKind) {
  switch (accessKind) {
  case SGFAccessKind::IgnoredRead:
  case SGFAccessKind::BorrowedAddressRead:
  case SGFAccessKind::BorrowedObjectRead:
  case SGFAccessKind::OwnedAddressRead:
  case SGFAccessKind::OwnedObjectRead:
    return AccessKind::Read;

  case SGFAccessKind::Write:
    return AccessKind::Write;

  case SGFAccessKind::OwnedAddressConsume:
  case SGFAccessKind::OwnedObjectConsume:
  case SGFAccessKind::ReadWrite:
    return AccessKind::ReadWrite;
  }
  llvm_unreachable("covered switch");
}

void LValue::addNonMemberVarComponent(
    SILGenFunction &SGF, SILLocation loc, VarDecl *var, SubstitutionMap subs,
    LValueOptions options, SGFAccessKind accessKind, AccessStrategy strategy,
    CanType formalRValueType, llvm::Optional<ActorIsolation> actorIso) {
  struct NonMemberVarAccessEmitter :
      AccessEmitter<NonMemberVarAccessEmitter, VarDecl> {
    LValue &LV;
    SILLocation Loc;
    LValueOptions Options;
    llvm::Optional<ActorIsolation> ActorIso;

    NonMemberVarAccessEmitter(SILGenFunction &SGF, SILLocation loc,
                              VarDecl *var, SubstitutionMap subs,
                              SGFAccessKind accessKind,
                              CanType formalRValueType, LValueOptions options,
                              llvm::Optional<ActorIsolation> actorIso,
                              LValue &lv)
        : AccessEmitter(SGF, var, subs, accessKind, formalRValueType), LV(lv),
          Loc(loc), Options(options), ActorIso(actorIso) {}

    void emitUsingAddressor(SILDeclRef addressor, bool isDirect,
                            LValueTypeData typeData) {
      assert(!ActorIso);
      SILType storageType =
        SGF.getLoweredType(Storage->getType()).getAddressType();
      LV.add<AddressorComponent>(Storage, addressor,
                                 /*isSuper=*/false, isDirect, Subs,
                                 CanType(), typeData, storageType, nullptr,
                                 PreparedArguments(),
                                 /* isOnSelfParameter */ false);
    }

    void emitUsingCoroutineAccessor(SILDeclRef accessor, bool isDirect,
                                    LValueTypeData typeData) {
      assert(!ActorIso);
      LV.add<CoroutineAccessorComponent>(
          Storage, accessor,
          /*isSuper*/ false, isDirect, Subs, CanType(), typeData, nullptr,
          PreparedArguments(), /*isOnSelfParameter*/ false);
    }

    void emitUsingGetterSetter(SILDeclRef accessor,
                               bool isDirect,
                               LValueTypeData typeData) {
      LV.add<GetterSetterComponent>(
          Storage, accessor,
          /*isSuper=*/false, isDirect, Subs, CanType(), typeData,
          nullptr, PreparedArguments(),
          /*isOnSelfParameter=*/false,
          ActorIso);
    }

    void emitUsingMaterialization(AccessStrategy readStrategy,
                                  AccessStrategy writeStrategy,
                                  LValueTypeData typeData) {
      assert(!ActorIso);
      LV.add<MaterializeToTemporaryComponent>(
          Storage, /*super*/ false, Subs, Options, readStrategy,
          writeStrategy,
          /*base type*/ CanType(), typeData, nullptr, PreparedArguments(),
          /* isOnSelfParameter */ false);
    }

    void emitUsingStorage(LValueTypeData typeData) {
      // If it's a physical value (e.g. a local variable in memory), push its
      // address.

      // Check for a local (possibly captured) variable.
      auto astAccessKind = mapAccessKind(this->AccessKind);
      auto address = SGF.maybeEmitValueOfLocalVarDecl(Storage, astAccessKind);

      // The only other case that should get here is a global variable.
      if (!address) {
          address = SGF.emitGlobalVariableRef(Loc, Storage, ActorIso);
      } else {
        assert((!ActorIso || Storage->isTopLevelGlobal()) &&
               "local var should not be actor isolated!");
      }

      assert(address.isLValue() &&
             "Must have a physical copyable lvalue decl ref that "
             "evaluates to an address");

      llvm::Optional<SILAccessEnforcement> enforcement;
      if (!Storage->isLet()) {
        if (Options.IsNonAccessing) {
          enforcement = llvm::None;
        } else if (Storage->getDeclContext()->isLocalContext()) {
          enforcement = SGF.getUnknownEnforcement(Storage);
        } else if (Storage->getDeclContext()->isModuleScopeContext()) {
          enforcement = SGF.getDynamicEnforcement(Storage);
        } else {
          assert(Storage->getDeclContext()->isTypeContext() &&
                 !Storage->isInstanceMember());
          enforcement = SGF.getDynamicEnforcement(Storage);
        }
      }

      LV.add<ValueComponent>(address, enforcement, typeData,
                             /*isRValue=*/false, ActorIso);

      if (address.getType().is<ReferenceStorageType>())
        LV.add<OwnershipComponent>(typeData);
    }

    void emitUsingDistributedThunk() {
      llvm_unreachable("cannot dispatch non-member var via distributed thunk");
    }

  } emitter(SGF, loc, var, subs, accessKind, formalRValueType, options,
            actorIso, *this);

  emitter.emitUsingStrategy(strategy);
}

ManagedValue
SILGenFunction::maybeEmitValueOfLocalVarDecl(
    VarDecl *var, AccessKind accessKind) {
  // For local decls, use the address we allocated or the value if we have it.
  auto It = VarLocs.find(var);
  if (It != VarLocs.end()) {
    SILValue ptr = It->second.value;

    // If this has an address, return it.  By-value let bindings have no address.
    if (ptr->getType().isAddress())
      return ManagedValue::forLValue(ptr);

    // Otherwise, it is an RValue let.  Uses of it are borrows, but we don't
    // want to proactively emit a borrow here.
    // TODO: integrate this with how callers want these values so we can do
    // something more semantic than just forUnmanaged.
    return ManagedValue::forUnmanaged(ptr);
  }

  // Otherwise, it's non-local or not stored.
  return ManagedValue();
}

ManagedValue
SILGenFunction::emitAddressOfLocalVarDecl(SILLocation loc, VarDecl *var,
                                          CanType formalRValueType,
                                          SGFAccessKind accessKind) {
  assert(var->getDeclContext()->isLocalContext());
  assert(var->getImplInfo().isSimpleStored());
  AccessKind astAccessKind = mapAccessKind(accessKind);
  
  assert(!var->isAsyncLet() && "async let does not have an address");
  
  auto address = maybeEmitValueOfLocalVarDecl(var, astAccessKind);
  assert(address.isLValue());
  return address;
}

RValue SILGenFunction::emitRValueForNonMemberVarDecl(SILLocation loc,
                                                     ConcreteDeclRef declRef,
                                                     CanType formalRValueType,
                                                     AccessSemantics semantics,
                                                     SGFContext C) {
  // Any writebacks for this access are tightly scoped.
  FormalEvaluationScope scope(*this);

  auto *var = cast<VarDecl>(declRef.getDecl());
  // If the variable is part of an async let, get the result from the child
  // task.
  if (var->isAsyncLet()) {
    return RValue(*this, loc, formalRValueType,
                  emitReadAsyncLetBinding(loc, var));
  }

  // If our localValue is a closure captured box of a noncopyable type, project
  // it out eagerly and insert a no_consume_or_assign constraint.
  ManagedValue localValue = maybeEmitValueOfLocalVarDecl(var, AccessKind::Read);

  // If this VarDecl is represented as an address, emit it as an lvalue, then
  // perform a load to get the rvalue.
  if (localValue && localValue.isLValue()) {
    bool guaranteedValid = false;
    IsTake_t shouldTake = IsNotTake;

    // We should only end up in this path for local and global variables,
    // i.e. ones whose lifetime is assured for the duration of the evaluation.
    // Therefore, if the variable is a constant, the value is guaranteed
    // valid as well.
    if (var->isLet())
      guaranteedValid = true;

    // Protect the lvalue read with access markers. The !is<LValueType> assert
    // above ensures that the "LValue" is actually immutable, so we use an
    // unenforced access marker.
    SILValue destAddr = localValue.getLValueAddress();
    SILValue accessAddr = UnenforcedFormalAccess::enter(*this, loc, destAddr,
                                                        SILAccessKind::Read);

    if (accessAddr->getType().isMoveOnly() &&
        !isa<MarkMustCheckInst>(accessAddr)) {
      // When loading an rvalue, we should never need to modify the place
      // we're loading from.
      accessAddr = B.createMarkMustCheckInst(
          loc, accessAddr,
          MarkMustCheckInst::CheckKind::NoConsumeOrAssign);
    }

    auto propagateRValuePastAccess = [&](RValue &&rvalue) {
      // Check if a new begin_access was emitted and returned as the
      // RValue. This means that the load did not actually load. If so, then
      // fix the rvalue to begin_access operand. The end_access cleanup
      // doesn't change. FIXME: this can't happen with sil-opaque-values.
      if (accessAddr != destAddr && rvalue.isComplete()
          && rvalue.isPlusZero(*this) && !isa<TupleType>(rvalue.getType())) {
        auto mv = std::move(rvalue).getScalarValue();
        if (mv.getValue() == accessAddr)
          mv = std::move(mv).transform(
              cast<BeginAccessInst>(accessAddr)->getOperand());
        return RValue(*this, loc, formalRValueType, mv);
      }
      return std::move(rvalue);
    };
    // If we have self, see if we are in an 'init' delegation sequence. If so,
    // call out to the special delegation init routine. Otherwise, use the
    // normal RValue emission logic.
    if (var->getName() == getASTContext().Id_self &&
        SelfInitDelegationState != NormalSelf) {
      auto rvalue =
        emitRValueForSelfInDelegationInit(loc, formalRValueType, accessAddr, C);
      return propagateRValuePastAccess(std::move(rvalue));
    }

    // Avoid computing an abstraction pattern for local variables.
    // This is a slight compile-time optimization, but more importantly
    // it avoids problems where locals don't always have interface types.
    if (var->getDeclContext()->isLocalContext()) {
      auto &rvalueTL = getTypeLowering(formalRValueType);
      auto rvalue = RValue(*this, loc, formalRValueType,
                           emitLoad(loc, accessAddr, rvalueTL,
                                    C, shouldTake, guaranteedValid));

      return propagateRValuePastAccess(std::move(rvalue));
    }

    // Otherwise, do the full thing where we potentially bridge and
    // reabstract the declaration.
    auto origFormalType = SGM.Types.getAbstractionPattern(var);
    auto rvalue = RValue(*this, loc, formalRValueType,
                         emitLoad(loc, accessAddr, origFormalType,
                                  formalRValueType,
                                  getTypeLowering(formalRValueType),
                                  C, shouldTake, guaranteedValid));
    return propagateRValuePastAccess(std::move(rvalue));
  }

  // For local decls, use the address we allocated or the value if we have it.
  if (localValue) {
    // Mutable lvalue and address-only 'let's are LValues.
    assert(!localValue.getType().isAddress() &&
           "LValue cases should be handled above");

    SILValue Scalar = localValue.getUnmanagedValue();

    // For weak and unowned types, convert the reference to the right
    // pointer.
    if (Scalar->getType().is<ReferenceStorageType>()) {
      Scalar = emitConversionToSemanticRValue(loc, Scalar,
                                             getTypeLowering(formalRValueType));
      // emitConversionToSemanticRValue always produces a +1 strong result.
      return RValue(*this, loc, formalRValueType,
                    emitManagedRValueWithCleanup(Scalar));
    }

    // This is a let, so we can make guarantees, so begin the borrow scope.
    ManagedValue Result = emitManagedBeginBorrow(loc, Scalar);

    // If the client can't handle a +0 result, retain it to get a +1.
    // This is a 'let', so we can make guarantees.
    return RValue(*this, loc, formalRValueType,
                  C.isGuaranteedPlusZeroOk()
                    ? Result : Result.copyUnmanaged(*this, loc));
  }

  LValue lv = emitLValueForNonMemberVarDecl(
      *this, loc, declRef, formalRValueType, SGFAccessKind::OwnedObjectRead,
      LValueOptions(), semantics,
      /*actorIsolation=*/llvm::None);
  return emitLoadOfLValue(loc, std::move(lv), C);
}

LValue SILGenLValue::visitDiscardAssignmentExpr(DiscardAssignmentExpr *e,
                                                SGFAccessKind accessKind,
                                                LValueOptions options) {
  LValueTypeData typeData = getValueTypeData(SGF, accessKind, e);

  SILValue address = SGF.emitTemporaryAllocation(e,
                                               SILType::getPrimitiveObjectType(
                                                 typeData.TypeOfRValue));
  address = SGF.B.createMarkUninitialized(e, address,
                                          MarkUninitializedInst::Var);
  LValue lv;
  lv.add<ValueComponent>(SGF.emitManagedBufferWithCleanup(address), llvm::None,
                         typeData);
  return lv;
}


LValue SILGenLValue::visitDeclRefExpr(DeclRefExpr *e, SGFAccessKind accessKind,
                                      LValueOptions options) {
  llvm::Optional<ActorIsolation> actorIso;
  if (e->isImplicitlyAsync())
    actorIso = getActorIsolation(e->getDecl());

  return emitLValueForNonMemberVarDecl(SGF, e, e->getDeclRef(),
                                       getSubstFormalRValueType(e),
                                       accessKind, options,
                                       e->getAccessSemantics(),
                                       actorIso);
}

LValue SILGenLValue::visitOpaqueValueExpr(OpaqueValueExpr *e,
                                          SGFAccessKind accessKind,
                                          LValueOptions options) {
  // Handle an opaque lvalue that refers to an opened existential.
  auto known = SGF.OpaqueValueExprs.find(e);
  if (known != SGF.OpaqueValueExprs.end()) {
    // Dig the open-existential expression out of the list.
    OpenExistentialExpr *opened = known->second;
    SGF.OpaqueValueExprs.erase(known);

    // Do formal evaluation of the underlying existential lvalue.
    auto lv = visitRec(opened->getExistentialValue(), accessKind, options);
    lv = SGF.emitOpenExistentialLValue(
        opened, std::move(lv),
        CanArchetypeType(opened->getOpenedArchetype()),
        e->getType()->getWithoutSpecifierType()->getCanonicalType(),
        accessKind);
    return lv;
  }

  assert(SGF.OpaqueValues.count(e) && "Didn't bind OpaqueValueExpr");
  auto value = SGF.OpaqueValues[e];

  RegularLocation loc(e);
  LValue lv;
  lv.add<ValueComponent>(value.formalAccessBorrow(SGF, loc), llvm::None,
                         getValueTypeData(SGF, accessKind, e));
  return lv;
}

LValue SILGenLValue::visitPackElementExpr(PackElementExpr *e,
                                          SGFAccessKind accessKind,
                                          LValueOptions options) {
  auto refExpr = e->getPackRefExpr();

  auto substFormalType = e->getType()->getCanonicalType();

  if (auto declRefExpr = dyn_cast<DeclRefExpr>(refExpr)) {
    if (auto var = dyn_cast<VarDecl>(declRefExpr->getDecl())) {
      auto origFormalType = SGF.SGM.Types.getAbstractionPattern(var);
      auto elementTy =
        SGF.getLoweredType(origFormalType, substFormalType).getAddressType();
      SILValue packAddr =
        SGF.emitAddressOfLocalVarDecl(declRefExpr, var,
                                      substFormalType,
                                      accessKind)
           .getLValueAddress();
      auto packIndex = SGF.getInnermostPackExpansion()->ExpansionIndex;
      auto elementAddr =
        SGF.B.createPackElementGet(e, packIndex, packAddr, elementTy);
      return LValue::forAddress(
          accessKind, ManagedValue::forLValue(elementAddr),
          /*access enforcement*/ llvm::None, origFormalType, substFormalType);
    }
  }

  if (auto packExpr = dyn_cast<MaterializePackExpr>(refExpr)) {
    auto *activeExpansion = SGF.getInnermostPackExpansion();
    assert(activeExpansion->MaterializedPacks.count(packExpr) &&
           "didn't materialize pack before dynamic pack loop emission");

    auto elementTy =
      SGF.getLoweredType(substFormalType).getAddressType();
    auto tupleAddr = activeExpansion->MaterializedPacks.find(packExpr);
    auto packIndex = activeExpansion->ExpansionIndex;
    auto elementAddr =
      SGF.B.createTuplePackElementAddr(e, packIndex, tupleAddr->second, elementTy);
    return LValue::forAddress(accessKind, ManagedValue::forLValue(elementAddr),
                              /*access enforcement*/ llvm::None,
                              AbstractionPattern(substFormalType),
                              substFormalType);
  }

  SGF.SGM.diagnose(refExpr, diag::not_implemented,
                   "emission of 'each' for this kind of expression");
  auto loweredTy = SGF.getLoweredType(substFormalType).getAddressType();
  auto fakeAddr = ManagedValue::forLValue(SILUndef::get(loweredTy, SGF.F));
  return LValue::forAddress(
      accessKind, fakeAddr, /*access enforcement*/ llvm::None,
      AbstractionPattern(substFormalType), substFormalType);
}

LValue SILGenLValue::visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *e,
                                                   SGFAccessKind accessKind,
                                                   LValueOptions options) {
  SGF.emitIgnoredExpr(e->getLHS());
  return visitRec(e->getRHS(), accessKind, options);
}

static SGFAccessKind getBaseAccessKindForAccessor(SILGenModule &SGM,
                                                  AccessorDecl *accessor,
                                                  CanType baseFormalType) {
  if (accessor->isMutating())
    return SGFAccessKind::ReadWrite;

  auto declRef = SGM.getAccessorDeclRef(accessor, ResilienceExpansion::Minimal);
  if (SGM.shouldEmitSelfAsRValue(accessor, baseFormalType)) {
    return SGM.isNonMutatingSelfIndirect(declRef)
               ? SGFAccessKind::OwnedAddressRead
               : SGFAccessKind::OwnedObjectRead;
  } else {
    return SGM.isNonMutatingSelfIndirect(declRef)
               ? SGFAccessKind::BorrowedAddressRead
               : SGFAccessKind::BorrowedObjectRead;
  }
}

static SGFAccessKind getBaseAccessKindForStorage(SGFAccessKind accessKind) {
  // Assume that the member only partially projects the enclosing value,
  // so a write to the member is a read/write of the base.
  return (accessKind == SGFAccessKind::Write
            ? SGFAccessKind::ReadWrite : accessKind);
}

/// Return the appropriate access kind for the base l-value of a
/// particular member, which is being accessed in a particular way.
static SGFAccessKind getBaseAccessKind(SILGenModule &SGM,
                                       AbstractStorageDecl *member,
                                       SGFAccessKind accessKind,
                                       AccessStrategy strategy,
                                       CanType baseFormalType) {
  switch (strategy.getKind()) {
  case AccessStrategy::Storage:
    return getBaseAccessKindForStorage(accessKind);

  case AccessStrategy::MaterializeToTemporary: {
    assert(accessKind == SGFAccessKind::ReadWrite);
    auto writeBaseKind = getBaseAccessKind(SGM, member, SGFAccessKind::Write,
                                           strategy.getWriteStrategy(),
                                           baseFormalType);

    // Fast path for the common case that the write will need to mutate
    // the base.
    if (writeBaseKind == SGFAccessKind::ReadWrite)
      return writeBaseKind;

    auto readBaseKind = getBaseAccessKind(SGM, member,
                                          SGFAccessKind::OwnedAddressRead,
                                          strategy.getReadStrategy(),
                                          baseFormalType);

    // If they're the same kind, just use that.
    if (readBaseKind == writeBaseKind)
      return readBaseKind;

    // If either access is mutating, the total access is a read-write.
    if (!isReadAccess(readBaseKind) || !isReadAccess(writeBaseKind))
      return SGFAccessKind::ReadWrite;

    // Okay, we have two different kinds of read somehow for different
    // accessors of the same storage?
    return SGFAccessKind::OwnedObjectRead;
  }

  case AccessStrategy::DirectToAccessor:
  case AccessStrategy::DispatchToAccessor:
  case AccessStrategy::DispatchToDistributedThunk: {
    auto accessor = member->getOpaqueAccessor(strategy.getAccessor());
    return getBaseAccessKindForAccessor(SGM, accessor, baseFormalType);
  }
  }
  llvm_unreachable("bad access strategy");
}

static CanType getBaseFormalType(Expr *baseExpr) {
  return baseExpr->getType()->getWithoutSpecifierType()->getCanonicalType();
}

bool isCallToReplacedInDynamicReplacement(SILGenFunction &SGF,
                                          AbstractFunctionDecl *afd,
                                          bool &isObjCReplacementSelfCall);

static bool isCallToSelfOfCurrentFunction(SILGenFunction &SGF, LookupExpr *e) {
  return isa_and_nonnull<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()) &&
         e->getBase()->isSelfExprOf(
             cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()), false);
}

static bool isCurrentFunctionAccessor(SILGenFunction &SGF,
                                      AccessorKind accessorKind) {
  auto *contextAccessorDecl =
      dyn_cast_or_null<AccessorDecl>(SGF.FunctionDC->getAsDecl());
  return contextAccessorDecl &&
         contextAccessorDecl->getAccessorKind() == accessorKind;
}

LValue SILGenLValue::visitMemberRefExpr(MemberRefExpr *e,
                                        SGFAccessKind accessKind,
                                        LValueOptions options) {
  // MemberRefExpr can refer to type and function members, but the only case
  // that can be an lvalue is a VarDecl.
  VarDecl *var = cast<VarDecl>(e->getMember().getDecl());

  // A reference to an instance property in init accessor body
  // has to be remapped into an argument reference because all
  // of the properties from initialized/accesses lists are passed
  // to init accessors individually via arguments.
  if (isCurrentFunctionAccessor(SGF, AccessorKind::Init)) {
    if (auto *arg = SGF.isMappedToInitAccessorArgument(var)) {
      auto subs = e->getMember().getSubstitutions();
      return emitLValueForNonMemberVarDecl(
          SGF, e, ConcreteDeclRef(arg, subs), getSubstFormalRValueType(e),
          accessKind, options, AccessSemantics::Ordinary, llvm::None);
    }
  }

  auto accessSemantics = e->getAccessSemantics();
  AccessStrategy strategy =
    var->getAccessStrategy(accessSemantics,
                           getFormalAccessKind(accessKind),
                           SGF.SGM.M.getSwiftModule(),
                           SGF.F.getResilienceExpansion());

  bool isOnSelfParameter = isCallToSelfOfCurrentFunction(SGF, e);

  bool isContextRead = isCurrentFunctionAccessor(SGF, AccessorKind::Read);

  // If we are inside _read, calling self.get, and the _read we are inside of is
  // the same as the as self's variable and the current function is a
  // dynamic replacement directly call the implementation.
  if (isContextRead && isOnSelfParameter && strategy.hasAccessor() &&
      strategy.getAccessor() == AccessorKind::Get &&
      var->getOpaqueAccessor(AccessorKind::Read)) {
    bool isObjC = false;
    auto readAccessor =
        SGF.getAccessorDeclRef(var->getOpaqueAccessor(AccessorKind::Read));
    if (isCallToReplacedInDynamicReplacement(
            SGF, readAccessor.getAbstractFunctionDecl(), isObjC)) {
      accessSemantics = AccessSemantics::DirectToImplementation;
      strategy = var->getAccessStrategy(
          accessSemantics,
          getFormalAccessKind(accessKind),
          SGF.SGM.M.getSwiftModule(),
          SGF.F.getResilienceExpansion());
    }
  }
  
  LValue lv = visitRec(e->getBase(),
                       getBaseAccessKind(SGF.SGM, var, accessKind, strategy,
                                         getBaseFormalType(e->getBase())),
                       getBaseOptions(options, strategy));
  assert(lv.isValid());

  llvm::Optional<ActorIsolation> actorIso;
  if (e->isImplicitlyAsync())
    actorIso = getActorIsolation(var);

  CanType substFormalRValueType = getSubstFormalRValueType(e);
  lv.addMemberVarComponent(SGF, e, var, e->getMember().getSubstitutions(),
                           options, e->isSuper(), accessKind, strategy,
                           substFormalRValueType, isOnSelfParameter, actorIso);
  return lv;
}

namespace {

/// A CRTP class for emitting member accesses.
template <class Impl, class StorageType>
struct MemberStorageAccessEmitter : AccessEmitter<Impl, StorageType> {
  using super = AccessEmitter<Impl, StorageType>;
  using super::SGF;
  using super::Storage;
  using super::Subs;
  using super::FormalRValueType;
  LValue &LV;
  LValueOptions Options;
  SILLocation Loc;
  bool IsSuper;
  bool IsOnSelfParameter; // Is self the self parameter in context.
  CanType BaseFormalType;
  ArgumentList *ArgListForDiagnostics;
  PreparedArguments Indices;
  // If any, holds the actor we must switch to when performing the access.
  llvm::Optional<ActorIsolation> ActorIso;

  MemberStorageAccessEmitter(
      SILGenFunction &SGF, SILLocation loc, StorageType *storage,
      SubstitutionMap subs, bool isSuper, SGFAccessKind accessKind,
      CanType formalRValueType, LValueOptions options, LValue &lv,
      ArgumentList *argListForDiagnostics, PreparedArguments &&indices,
      bool isSelf = false, llvm::Optional<ActorIsolation> actorIso = llvm::None)
      : super(SGF, storage, subs, accessKind, formalRValueType), LV(lv),
        Options(options), Loc(loc), IsSuper(isSuper), IsOnSelfParameter(isSelf),
        BaseFormalType(lv.getSubstFormalType()),
        ArgListForDiagnostics(argListForDiagnostics),
        Indices(std::move(indices)), ActorIso(actorIso) {}

  void emitUsingAddressor(SILDeclRef addressor, bool isDirect,
                          LValueTypeData typeData) {
    assert(!ActorIso);
    SILType varStorageType = SGF.SGM.Types.getSubstitutedStorageType(
        SGF.getTypeExpansionContext(), Storage, FormalRValueType);

    LV.add<AddressorComponent>(Storage, addressor, IsSuper, isDirect, Subs,
                               BaseFormalType, typeData, varStorageType,
                               ArgListForDiagnostics, std::move(Indices),
                               IsOnSelfParameter);
    
  }

  void emitUsingCoroutineAccessor(SILDeclRef accessor, bool isDirect,
                                  LValueTypeData typeData) {
    assert(!ActorIso);
    LV.add<CoroutineAccessorComponent>(
        Storage, accessor, IsSuper, isDirect, Subs, BaseFormalType, typeData,
        ArgListForDiagnostics, std::move(Indices), IsOnSelfParameter);
  }

  void emitUsingGetterSetter(SILDeclRef accessor,
                             bool isDirect,
                             LValueTypeData typeData) {
    LV.add<GetterSetterComponent>(
        Storage, accessor, IsSuper, isDirect, Subs,
        BaseFormalType, typeData, ArgListForDiagnostics, std::move(Indices),
        IsOnSelfParameter, ActorIso);
  }

  void emitUsingMaterialization(AccessStrategy readStrategy,
                                AccessStrategy writeStrategy,
                                LValueTypeData typeData) {
    assert(!ActorIso);
    LV.add<MaterializeToTemporaryComponent>(
        Storage, IsSuper, Subs, Options, readStrategy, writeStrategy,
        BaseFormalType, typeData, ArgListForDiagnostics, std::move(Indices),
        IsOnSelfParameter);
  }
};
} // end anonymous namespace

void LValue::addMemberVarComponent(
    SILGenFunction &SGF, SILLocation loc, VarDecl *var, SubstitutionMap subs,
    LValueOptions options, bool isSuper, SGFAccessKind accessKind,
    AccessStrategy strategy, CanType formalRValueType, bool isOnSelfParameter,
    llvm::Optional<ActorIsolation> actorIso) {
  struct MemberVarAccessEmitter
      : MemberStorageAccessEmitter<MemberVarAccessEmitter, VarDecl> {
    using MemberStorageAccessEmitter::MemberStorageAccessEmitter;

    void emitUsingStorage(LValueTypeData typeData) {
      // For static variables, emit a reference to the global variable backing
      // them.
      // FIXME: This has to be dynamically looked up for classes, and
      // dynamically instantiated for generics.
      if (Storage->isStatic()) {
        // FIXME: this implicitly drops the earlier components, but maybe
        // we ought to evaluate them for side-effects even during the
        // formal access?
        LV.Path.clear();
        LV.addNonMemberVarComponent(SGF, Loc, Storage, Subs, Options,
                                    typeData.getAccessKind(),
                                    AccessStrategy::getStorage(),
                                    FormalRValueType,
                                    ActorIso);
        return;
      }

      // Otherwise, it's a physical member.
      SILType varStorageType = SGF.SGM.Types.getSubstitutedStorageType(
          SGF.getTypeExpansionContext(), Storage, FormalRValueType);

      if (BaseFormalType->mayHaveSuperclass()) {
        LV.add<RefElementComponent>(Storage, Options, varStorageType,
                                    typeData, ActorIso);
      } else {
        assert(BaseFormalType->getStructOrBoundGenericStruct());
        LV.add<StructElementComponent>(Storage, varStorageType, typeData,
                                       ActorIso);
      }

      // If the member has weak or unowned storage, convert it away.
      if (varStorageType.is<ReferenceStorageType>()) {
        LV.add<OwnershipComponent>(typeData);
      }
    }

    void emitUsingDistributedThunk() {
      auto *var = cast<VarDecl>(Storage);
      SILDeclRef accessor(var->getAccessor(AccessorKind::Get),
                          SILDeclRef::Kind::Func,
                          /*isForeign=*/false, /*isDistributed=*/true);

      auto typeData = getLogicalStorageTypeData(
          SGF.getTypeExpansionContext(), SGF.SGM, AccessKind, FormalRValueType);

      asImpl().emitUsingGetterSetter(accessor, /*isDirect=*/true, typeData);
    }

  } emitter(SGF, loc, var, subs, isSuper, accessKind,
            formalRValueType, options, *this,
            /*indices for diags*/ nullptr, /*indices*/ PreparedArguments(),
            isOnSelfParameter, actorIso);

  emitter.emitUsingStrategy(strategy);
}

LValue SILGenLValue::visitSubscriptExpr(SubscriptExpr *e,
                                        SGFAccessKind accessKind,
                                        LValueOptions options) {
  auto decl = cast<SubscriptDecl>(e->getDecl().getDecl());
  auto subs = e->getDecl().getSubstitutions();


  auto accessSemantics = e->getAccessSemantics();
  auto strategy =
    decl->getAccessStrategy(accessSemantics,
                            getFormalAccessKind(accessKind),
                            SGF.SGM.M.getSwiftModule(),
                            SGF.F.getResilienceExpansion());

  bool isOnSelfParameter = isCallToSelfOfCurrentFunction(SGF, e);
  bool isContextRead = isCurrentFunctionAccessor(SGF, AccessorKind::Read);

  // If we are inside _read, calling self.get, and the _read we are inside of is
  // the same as the as self's variable and the current function is a
  // dynamic replacement directly call the implementation.
  if (isContextRead && isOnSelfParameter && strategy.hasAccessor() &&
      strategy.getAccessor() == AccessorKind::Get &&
      decl->getOpaqueAccessor(AccessorKind::Read)) {
    bool isObjC = false;
    auto readAccessor =
        SGF.getAccessorDeclRef(decl->getOpaqueAccessor(AccessorKind::Read));
    if (isCallToReplacedInDynamicReplacement(
            SGF, readAccessor.getAbstractFunctionDecl(), isObjC)) {
      accessSemantics = AccessSemantics::DirectToImplementation;
      strategy = decl->getAccessStrategy(
          accessSemantics,
          getFormalAccessKind(accessKind),
          SGF.SGM.M.getSwiftModule(),
          SGF.F.getResilienceExpansion());
    }
  }

  LValue lv = visitRec(e->getBase(),
                       getBaseAccessKind(SGF.SGM, decl, accessKind, strategy,
                                         getBaseFormalType(e->getBase())),
                       getBaseOptions(options, strategy));
  assert(lv.isValid());

  // Now that the base components have been resolved, check the isolation for
  // this subscript decl.
  llvm::Optional<ActorIsolation> actorIso;
  if (e->isImplicitlyAsync())
    actorIso = getActorIsolation(decl);

  auto *argList = e->getArgs();
  auto indices = SGF.prepareSubscriptIndices(e, decl, subs, strategy, argList);

  CanType formalRValueType = getSubstFormalRValueType(e);
  lv.addMemberSubscriptComponent(SGF, e, decl, subs,
                                 options, e->isSuper(), accessKind, strategy,
                                 formalRValueType, std::move(indices),
                                 argList, isOnSelfParameter, actorIso);
  return lv;
}

LValue SILGenLValue::visitKeyPathApplicationExpr(KeyPathApplicationExpr *e,
                                                 SGFAccessKind accessKind,
                                                 LValueOptions options) {
  auto keyPathExpr = e->getKeyPath();
  auto keyPathKind =
    *keyPathExpr->getType()->getAnyNominal()->getKeyPathTypeKind();

  // Determine the base access strategy based on the strategy of this access.
  SGFAccessKind subAccess;
  if (!isReadAccess(accessKind)) {
    // The only read-write accesses we allow are with WritableKeyPath and
    // ReferenceWritableKeyPath.
    assert(keyPathKind == KPTK_WritableKeyPath ||
           keyPathKind == KPTK_ReferenceWritableKeyPath);
    subAccess = (keyPathKind == KPTK_ReferenceWritableKeyPath
                    ? SGFAccessKind::BorrowedAddressRead
                    : SGFAccessKind::ReadWrite);
  } else {
    // For all the other kinds, we want the emit the base as an address
    // r-value; we don't support key paths for storage with mutating read
    // operations.
    subAccess = SGFAccessKind::BorrowedAddressRead;
  }

  // For now, just ignore any options we were given.
  LValueOptions subOptions = LValueOptions();

  // Do formal evaluation of the base l-value.
  // The base should be reabstracted to the maximal abstraction pattern.
  LValue lv = visitRec(e->getBase(), subAccess, subOptions,
                       AbstractionPattern::getOpaque());

  // Emit the key path r-value.
  auto keyPath = SGF.emitRValueAsSingleValue(keyPathExpr);

  // The result will end up projected at the maximal abstraction level too.
  auto substFormalType = e->getType()->getRValueType()->getCanonicalType();

  bool useLogical = [&] {
    switch (accessKind) {
    // Use the physical 'read' pattern for these unless we're working with
    // AnyKeyPath or PartialKeyPath, which only have getters.
    case SGFAccessKind::BorrowedAddressRead:
    case SGFAccessKind::BorrowedObjectRead:
      return (keyPathKind == KPTK_AnyKeyPath ||
              keyPathKind == KPTK_PartialKeyPath);

    case SGFAccessKind::OwnedObjectRead:
    case SGFAccessKind::OwnedAddressRead:
    case SGFAccessKind::IgnoredRead: // should we use physical for this?
    case SGFAccessKind::Write:
      return true;

    case SGFAccessKind::ReadWrite:
    case SGFAccessKind::OwnedAddressConsume:
    case SGFAccessKind::OwnedObjectConsume:
      return false;
    }
    llvm_unreachable("bad access kind");
  }();

  if (useLogical) {
    auto typeData = getLogicalStorageTypeData(
        SGF.getTypeExpansionContext(), SGF.SGM, accessKind, substFormalType);

    Type baseFormalType = e->getBase()->getType()->getRValueType();
    lv.add<LogicalKeyPathApplicationComponent>(typeData, keyPathKind, keyPath,
                                               baseFormalType);

    // TODO: make LogicalKeyPathApplicationComponent expect/produce values
    // in the opaque AbstractionPattern and push an OrigToSubstComponent here
    // so it can be peepholed.
  } else {
    auto typeData = getAbstractedTypeData(
        TypeExpansionContext::minimal(), SGF.SGM, accessKind,
        AbstractionPattern::getOpaque(), substFormalType);

    lv.add<PhysicalKeyPathApplicationComponent>(typeData, keyPathKind, keyPath);

    // Reabstract to the substituted abstraction level if necessary.
    auto substResultSILTy = SGF.getLoweredType(substFormalType);
    if (typeData.TypeOfRValue != substResultSILTy.getASTType()) {
      lv.addOrigToSubstComponent(substResultSILTy);
    }
  }

  return lv;
}

void LValue::addMemberSubscriptComponent(
    SILGenFunction &SGF, SILLocation loc, SubscriptDecl *decl,
    SubstitutionMap subs, LValueOptions options, bool isSuper,
    SGFAccessKind accessKind, AccessStrategy strategy, CanType formalRValueType,
    PreparedArguments &&indices, ArgumentList *argListForDiagnostics,
    bool isOnSelfParameter, llvm::Optional<ActorIsolation> actorIso) {
  struct MemberSubscriptAccessEmitter
      : MemberStorageAccessEmitter<MemberSubscriptAccessEmitter,
                                   SubscriptDecl> {
    using MemberStorageAccessEmitter::MemberStorageAccessEmitter;

    void emitUsingStorage(LValueTypeData typeData) {
      llvm_unreachable("subscripts never have storage");
    }

    void emitUsingDistributedThunk() {
      llvm_unreachable("subscripts cannot be dispatch via distributed thunk");
    }
  } emitter(SGF, loc, decl, subs, isSuper, accessKind, formalRValueType,
            options, *this, argListForDiagnostics, std::move(indices),
            isOnSelfParameter, actorIso);

  emitter.emitUsingStrategy(strategy);
}

bool LValue::isObviouslyNonConflicting(const LValue &other,
                                       SGFAccessKind selfAccess,
                                       SGFAccessKind otherAccess) {
  // Reads never conflict with reads.
  if (isReadAccess(selfAccess) && isReadAccess(otherAccess))
    return true;

  // We can cover more cases here.
  return false;
}

LValue SILGenLValue::visitTupleElementExpr(TupleElementExpr *e,
                                           SGFAccessKind accessKind,
                                           LValueOptions options) {
  unsigned index = e->getFieldNumber();
  LValue lv = visitRec(e->getBase(),
                       getBaseAccessKindForStorage(accessKind),
                       options.forProjectedBaseLValue());

  auto baseTypeData = lv.getTypeData();
  LValueTypeData typeData = {
    accessKind,
    baseTypeData.OrigFormalType.getTupleElementType(index),
    cast<TupleType>(baseTypeData.SubstFormalType).getElementType(index),
    cast<TupleType>(baseTypeData.TypeOfRValue).getElementType(index)
  };

  lv.add<TupleElementComponent>(index, typeData);
  return lv;
}

LValue SILGenLValue::visitOpenExistentialExpr(OpenExistentialExpr *e,
                                              SGFAccessKind accessKind,
                                              LValueOptions options) {
  // If the opaque value is not an lvalue, open the existential immediately.
  if (!e->getOpaqueValue()->getType()->is<LValueType>()) {
    return SGF.emitOpenExistentialExpr<LValue>(e,
                                               [&](Expr *subExpr) -> LValue {
                                                 return visitRec(subExpr,
                                                                 accessKind,
                                                                 options);
                                               });
  }

  // Record the fact that we're opening this existential. The actual
  // opening operation will occur when we see the OpaqueValueExpr.
  bool inserted = SGF.OpaqueValueExprs.insert({e->getOpaqueValue(), e}).second;
  (void)inserted;
  assert(inserted && "already have this opened existential?");

  // Visit the subexpression.
  LValue lv = visitRec(e->getSubExpr(), accessKind, options);

  // Sanity check that we did see the OpaqueValueExpr.
  assert(SGF.OpaqueValueExprs.count(e->getOpaqueValue()) == 0 &&
         "opened existential not removed?");
  return lv;
}

static LValueTypeData
getOptionalObjectTypeData(SILGenFunction &SGF, SGFAccessKind accessKind,
                          const LValueTypeData &baseTypeData) {
  return {
      accessKind,
      baseTypeData.OrigFormalType.getOptionalObjectType(),
      baseTypeData.SubstFormalType.getOptionalObjectType(),
      baseTypeData.TypeOfRValue.getOptionalObjectType(),
  };
}

LValue SILGenLValue::visitForceValueExpr(ForceValueExpr *e,
                                         SGFAccessKind accessKind,
                                         LValueOptions options) {
  // Like BindOptional, this is a read even if we only write to the result.
  // (But it's unnecessary to use a force this way!)
  LValue lv = visitRec(e->getSubExpr(),
                       getBaseAccessKindForStorage(accessKind),
                       options.forComputedBaseLValue());
  LValueTypeData typeData =
    getOptionalObjectTypeData(SGF, accessKind, lv.getTypeData());
  bool isImplicitUnwrap = e->isImplicit() &&
    e->isForceOfImplicitlyUnwrappedOptional(); 
  lv.add<ForceOptionalObjectComponent>(typeData, isImplicitUnwrap);
  return lv;
}

LValue SILGenLValue::visitBindOptionalExpr(BindOptionalExpr *e,
                                           SGFAccessKind accessKind,
                                           LValueOptions options) {
  // Binding reads the base even if we then only write to the result.
  auto baseAccessKind = getBaseAccessKindForStorage(accessKind);

  // We're going to take the address of the base.
  // TODO: deal more efficiently with an object-preferring access.
  baseAccessKind = getAddressAccessKind(baseAccessKind);

  // Do formal evaluation of the base l-value.
  LValue optLV = visitRec(e->getSubExpr(), baseAccessKind,
                          options.forComputedBaseLValue());

  LValueTypeData optTypeData = optLV.getTypeData();
  LValueTypeData valueTypeData =
    getOptionalObjectTypeData(SGF, accessKind, optTypeData);

  // The chaining operator immediately begins a formal access to the
  // base l-value.  In concrete terms, this means we can immediately
  // evaluate the base down to an address.
  ManagedValue optAddr = SGF.emitAddressOfLValue(e, std::move(optLV));

  // Bind the value, branching to the destination address if there's no
  // value there.
  SGF.emitBindOptionalAddress(e, optAddr, e->getDepth());

  // Project out the payload on the success branch.  We can just use a
  // naked ValueComponent here; this is effectively a separate l-value.
  ManagedValue valueAddr =
    getAddressOfOptionalValue(SGF, e, optAddr, valueTypeData);
  LValue valueLV;
  valueLV.add<ValueComponent>(valueAddr, llvm::None, valueTypeData);
  return valueLV;
}

LValue SILGenLValue::visitInOutExpr(InOutExpr *e, SGFAccessKind accessKind,
                                    LValueOptions options) {
  return visitRec(e->getSubExpr(), accessKind, options);
}

LValue SILGenLValue::visitConsumeExpr(ConsumeExpr *e, SGFAccessKind accessKind,
                                      LValueOptions options) {
  // Do formal evaluation of the base l-value.
  LValue baseLV = visitRec(e->getSubExpr(), SGFAccessKind::ReadWrite,
                           options.forComputedBaseLValue());

  ManagedValue addr = SGF.emitAddressOfLValue(e, std::move(baseLV));

  // Now create the temporary and move our value into there.
  auto temp =
      SGF.emitFormalAccessTemporary(e, SGF.F.getTypeLowering(addr.getType()));
  auto toAddr = temp->getAddressForInPlaceInitialization(SGF, e);

  // If we have a move only type, we use a copy_addr that will be handled by the
  // address move only checker. If we have a copyable type, we need to use a
  // mark_unresolved_move_addr to ensure that the move operator checker performs
  // the relevant checking.
  if (addr.getType().isMoveOnly()) {
    SGF.B.createCopyAddr(e, addr.getValue(), toAddr, IsNotTake,
                         IsInitialization);
  } else {
    SGF.B.createMarkUnresolvedMoveAddr(e, addr.getValue(), toAddr);
  }
  temp->finishInitialization(SGF);

  // Now return the temporary in a value component.
  return LValue::forValue(SGFAccessKind::BorrowedAddressRead,
                          temp->getManagedAddress(),
                          toAddr->getType().getASTType());
}

LValue SILGenLValue::visitCopyExpr(CopyExpr *e, SGFAccessKind accessKind,
                                   LValueOptions options) {
  // Do formal evaluation of the base l-value.
  LValue baseLV = visitRec(e->getSubExpr(), SGFAccessKind::BorrowedAddressRead,
                           options.forComputedBaseLValue());

  ManagedValue addr = SGF.emitAddressOfLValue(e, std::move(baseLV));

  // Now create the temporary and copy our value into there using an explicit
  // copy_value. This ensures that the rest of the move checker views this as a
  // liveness requiring use rather than a copy that must be eliminated.
  auto temp =
      SGF.emitFormalAccessTemporary(e, SGF.F.getTypeLowering(addr.getType()));
  auto toAddr = temp->getAddressForInPlaceInitialization(SGF, e);
  SGF.B.createExplicitCopyAddr(e, addr.getValue(), toAddr, IsNotTake,
                               IsInitialization);
  temp->finishInitialization(SGF);

  // Now return the temporary in a value component.
  return LValue::forValue(SGFAccessKind::BorrowedAddressRead,
                          temp->getManagedAddress(),
                          toAddr->getType().getASTType());
}

LValue SILGenLValue::visitABISafeConversionExpr(ABISafeConversionExpr *e,
                                    SGFAccessKind accessKind,
                                    LValueOptions options) {
  LValue lval = visitRec(e->getSubExpr(), accessKind, options);
  auto typeData = getValueTypeData(SGF, accessKind, e);

  auto OrigType = e->getSubExpr()->getType();

  lval.add<UncheckedConversionComponent>(typeData, OrigType);

  return lval;
}

/// Emit an lvalue that refers to the given property.  This is
/// designed to work with ManagedValue 'base's that are either +0 or +1.
LValue SILGenFunction::emitPropertyLValue(SILLocation loc, ManagedValue base,
                                          CanType baseFormalType,
                                          VarDecl *ivar,
                                          LValueOptions options,
                                          SGFAccessKind accessKind,
                                          AccessSemantics semantics) {
  SILGenLValue sgl(*this);
  LValue lv;

  auto baseType = base.getType().getASTType();
  auto subMap = baseType->getContextSubstitutionMap(
      SGM.M.getSwiftModule(), ivar->getDeclContext());

  AccessStrategy strategy =
    ivar->getAccessStrategy(semantics,
                            getFormalAccessKind(accessKind),
                            SGM.M.getSwiftModule(),
                            F.getResilienceExpansion());

  auto baseAccessKind =
    getBaseAccessKind(SGM, ivar, accessKind, strategy, baseFormalType);

  LValueTypeData baseTypeData =
    getValueTypeData(baseAccessKind, baseFormalType, base.getValue());

  // Refer to 'self' as the base of the lvalue.
  lv.add<ValueComponent>(base, llvm::None, baseTypeData,
                         /*isRValue=*/!base.isLValue());

  auto substFormalType = ivar->getValueInterfaceType().subst(subMap)
    ->getCanonicalType();

  lv.addMemberVarComponent(*this, loc, ivar, subMap, options, /*super*/ false,
                           accessKind, strategy, substFormalType);
  return lv;
}

// This is emitLoad that will handle re-abstraction and bridging for the client.
ManagedValue SILGenFunction::emitLoad(SILLocation loc, SILValue addr,
                                      AbstractionPattern origFormalType,
                                      CanType substFormalType,
                                      const TypeLowering &rvalueTL,
                                      SGFContext C, IsTake_t isTake,
                                      bool isAddressGuaranteed) {
  assert(addr->getType().isAddress());
  SILType addrRValueType = addr->getType().getReferenceStorageReferentType();

  // Fast path: the types match exactly.
  if (addrRValueType == rvalueTL.getLoweredType().getAddressType()) {
    return emitLoad(loc, addr, rvalueTL, C, isTake, isAddressGuaranteed);
  }

  // Otherwise, we need to reabstract or bridge.
  auto conversion =
    origFormalType.isClangType()
      ? Conversion::getBridging(Conversion::BridgeFromObjC,
                                origFormalType.getType(),
                                substFormalType, rvalueTL.getLoweredType())
      : Conversion::getOrigToSubst(origFormalType, substFormalType,
                                   rvalueTL.getLoweredType());

  return emitConvertedRValue(loc, conversion, C,
      [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
    return SGF.emitLoad(loc, addr, getTypeLowering(addrRValueType),
                        C, isTake, isAddressGuaranteed);
  });
}

/// Load an r-value out of the given address.
///
/// \param rvalueTL - the type lowering for the type-of-rvalue
///   of the address
/// \param isAddrGuaranteed - true if the value in this address
///   is guaranteed to be valid for the duration of the current
///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
ManagedValue SILGenFunction::emitLoad(SILLocation loc, SILValue addr,
                                      const TypeLowering &rvalueTL,
                                      SGFContext C, IsTake_t isTake,
                                      bool isAddrGuaranteed) {
  SILType addrType = addr->getType();
  // Get the lowering for the address type.  We can avoid a re-lookup
  // in the very common case of this being equivalent to the r-value
  // type.
  auto &addrTL = (addrType == rvalueTL.getLoweredType().getAddressType()
                      ? rvalueTL
                      : getTypeLowering(addrType));

  // Never do a +0 load together with a take.
  bool isPlusZeroOk = (isTake == IsNotTake &&
                       (isAddrGuaranteed ? C.isGuaranteedPlusZeroOk()
                                          : C.isImmediatePlusZeroOk()));

  if (rvalueTL.isAddressOnly() && silConv.useLoweredAddresses()) {
    // If the client is cool with a +0 rvalue, the decl has an address-only
    // type, and there are no conversions, then we can return this as a +0
    // address RValue.
    if (isPlusZeroOk) {
      SILType rvalueType = rvalueTL.getLoweredType();
      SILType addrType = addrTL.getLoweredType();
      if (!rvalueType.isMoveOnlyWrapped() && addrType.isMoveOnlyWrapped()) {
        SILValue value = B.createMoveOnlyWrapperToCopyableAddr(loc, addr);
        return ManagedValue::forUnmanaged(value);
      }
      if (rvalueTL.getLoweredType() == addrTL.getLoweredType()) {
        return ManagedValue::forUnmanaged(addr);
      }
    }

    // Copy the address-only value.
    return B.bufferForExpr(
        loc, rvalueTL.getLoweredType(), rvalueTL, C, [&](SILValue newAddr) {
          // If our dest address is a move only value from C, we can not reuse
          // rvalueTL here.
          SILType newAddrType = newAddr->getType();
          auto &newAddrTL = newAddrType.isMoveOnlyWrapped()
                                ? getTypeLowering(newAddrType)
                                : rvalueTL;
          return emitSemanticLoadInto(loc, addr, addrTL, newAddr, newAddrTL,
                                      isTake, IsInitialization);
        });
  }

  // Ok, this is something loadable.  If this is a non-take access at plus zero,
  // we can perform a +0 load of the address instead of materializing a +1
  // value.
  if (isPlusZeroOk && addrTL.getLoweredType() == rvalueTL.getLoweredType()) {
    return B.createLoadBorrow(loc, ManagedValue::forUnmanaged(addr));
  }

  // Load the loadable value, and retain it if we aren't taking it.
  SILValue loadedV = emitSemanticLoad(loc, addr, addrTL, rvalueTL, isTake);
  return emitManagedRValueWithCleanup(loadedV);
}

/// Load an r-value out of the given address.
///
/// \param rvalueTL - the type lowering for the type-of-rvalue
///   of the address
/// \param isAddressGuaranteed - true if the value in this address
///   is guaranteed to be valid for the duration of the current
///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
ManagedValue SILGenFunction::emitFormalAccessLoad(SILLocation loc,
                                                  SILValue addr,
                                                  const TypeLowering &rvalueTL,
                                                  SGFContext C, IsTake_t isTake,
                                                  bool isAddressGuaranteed) {
  // Get the lowering for the address type.  We can avoid a re-lookup
  // in the very common case of this being equivalent to the r-value
  // type.
  auto &addrTL = (addr->getType() == rvalueTL.getLoweredType().getAddressType()
                      ? rvalueTL
                      : getTypeLowering(addr->getType()));

  // Never do a +0 load together with a take.
  bool isPlusZeroOk =
      (isTake == IsNotTake && (isAddressGuaranteed ? C.isGuaranteedPlusZeroOk()
                                                 : C.isImmediatePlusZeroOk()));

  if (rvalueTL.isAddressOnly() && silConv.useLoweredAddresses()) {
    // If the client is cool with a +0 rvalue, the decl has an address-only
    // type, and there are no conversions, then we can return this as a +0
    // address RValue.
    if (isPlusZeroOk && rvalueTL.getLoweredType() == addrTL.getLoweredType())
      return ManagedValue::forUnmanaged(addr);

    // Copy the address-only value.
    return B.formalAccessBufferForExpr(
        loc, rvalueTL.getLoweredType(), rvalueTL, C,
        [&](SILValue addressForCopy) {
          // If our dest address is a move only value from C, we can not reuse
          // rvalueTL here.
          SILType addressForCopyType = addressForCopy->getType();
          auto &addressForCopyTL = addressForCopyType.isMoveOnlyWrapped()
                                       ? getTypeLowering(addressForCopyType)
                                       : rvalueTL;
          return emitSemanticLoadInto(loc, addr, addrTL, addressForCopy,
                                      addressForCopyTL, isTake,
                                      IsInitialization);
        });
  }

  // Ok, this is something loadable.  If this is a non-take access at plus zero,
  // we can perform a +0 load of the address instead of materializing a +1
  // value.
  if (isPlusZeroOk && addrTL.getLoweredType() == rvalueTL.getLoweredType()) {
    return B.createFormalAccessLoadBorrow(loc,
                                          ManagedValue::forUnmanaged(addr));
  }

  // Load the loadable value, and retain it if we aren't taking it.
  SILValue loadedV = emitSemanticLoad(loc, addr, addrTL, rvalueTL, isTake);
  return emitFormalAccessManagedRValueWithCleanup(loc, loadedV);
}

static void emitUnloweredStoreOfCopy(SILGenBuilder &B, SILLocation loc,
                                     SILValue value, SILValue addr,
                                     IsInitialization_t isInit) {
  if (isInit) {
    B.emitStoreValueOperation(loc, value, addr, StoreOwnershipQualifier::Init);
  } else {
    B.createAssign(loc, value, addr, AssignOwnershipQualifier::Unknown);
  }
}

SILValue SILGenFunction::emitConversionToSemanticRValue(SILLocation loc,
                                                        SILValue src,
                                                  const TypeLowering &valueTL) {
  auto storageType = src->getType();
  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    /* Address-only storage types are handled with their underlying type. */ \
    llvm_unreachable("address-only pointers are handled elsewhere");
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  case ReferenceOwnership::Name:                                               \
    return B.createStrongCopy##Name##Value(loc, src);
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  case ReferenceOwnership::Name: {                                             \
    /* For loadable reference storage types, we need to generate a strong */   \
    /* retain and strip the box. */                                            \
    assert(storageType.castTo<Name##StorageType>()->isLoadable(                \
        ResilienceExpansion::Maximal));                                        \
    return B.createStrongCopy##Name##Value(loc, src);                          \
  }
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case ReferenceOwnership::Name: {                                             \
    /* For static reference storage types, we need to strip the box and */     \
    /* then do an (unsafe) retain. */                                          \
    return B.createStrongCopy##Name##Value(loc, src);                          \
  }
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("impossible");
}

ManagedValue SILGenFunction::emitConversionToSemanticRValue(
    SILLocation loc, ManagedValue src, const TypeLowering &valueTL) {
  auto swiftStorageType = src.getType().castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    /* Address-only storage types are handled with their underlying type. */ \
    llvm_unreachable("address-only pointers are handled elsewhere");
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  case ReferenceOwnership::Name:                                               \
    /* Generate a strong retain and strip the box. */                          \
    return B.createStrongCopy##Name##Value(loc, src);
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case ReferenceOwnership::Name:                                               \
    /* Strip the box and then do an (unsafe) retain. */                        \
    return B.createStrongCopy##Name##Value(loc, src);
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("impossible");
}

/// Given that the type-of-rvalue differs from the type-of-storage,
/// and given that the type-of-rvalue is loadable, produce a +1 scalar
/// of the type-of-rvalue.
static SILValue emitLoadOfSemanticRValue(SILGenFunction &SGF,
                                         SILLocation loc,
                                         SILValue src,
                                         const TypeLowering &valueTL,
                                         IsTake_t isTake) {
  auto storageType = src->getType();
  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    return SGF.B.createLoad##Name(loc, src, isTake);
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name)          \
  {                                                                            \
    /* For loadable types, we need to strip the box. */                        \
    /* If we are not performing a take, use a load_borrow. */                  \
    if (!isTake) {                                                             \
      SILValue value = SGF.B.createLoadBorrow(loc, src);                       \
      SILValue strongValue = SGF.B.createStrongCopy##Name##Value(loc, value);  \
      SGF.B.createEndBorrow(loc, value);                                       \
      return strongValue;                                                      \
    }                                                                          \
    /* Otherwise perform a load take and destroy the stored value. */          \
    auto value =                                                               \
        SGF.B.emitLoadValueOperation(loc, src, LoadOwnershipQualifier::Take);  \
    SILValue strongValue = SGF.B.createStrongCopy##Name##Value(loc, value);    \
    SGF.B.createDestroyValue(loc, value);                                      \
    return strongValue;                                                        \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For loadable types, we need to strip the box. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    if (!type->isLoadable(ResilienceExpansion::Maximal)) { \
      return SGF.B.createLoad##Name(loc, src, isTake); \
    } \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name) \
  }
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case ReferenceOwnership::Name: {                                             \
    /* For static reference storage types, we need to strip the box. */        \
    auto value = SGF.B.createLoad(loc, src, LoadOwnershipQualifier::Trivial);  \
    return SGF.B.createStrongCopy##Name##Value(loc, value);                    \
  }
#include "swift/AST/ReferenceStorage.def"
#undef ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER
  }
  llvm_unreachable("unhandled ownership");
}

/// Given that the type-of-rvalue differs from the type-of-storage,
/// store a +1 value (possibly not a scalar) of the type-of-rvalue
/// into the given address.
static void emitStoreOfSemanticRValue(SILGenFunction &SGF,
                                      SILLocation loc,
                                      SILValue value,
                                      SILValue dest,
                                      const TypeLowering &valueTL,
                                      IsInitialization_t isInit) {
  auto storageType = dest->getType();
  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    SGF.B.createStore##Name(loc, value, dest, isInit); \
    /* store doesn't take ownership of the input, so cancel it out. */ \
    SGF.B.emitDestroyValueOperation(loc, value); \
    return; \
  }
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name) \
  { \
    auto typedValue = SGF.B.createRefTo##Name(loc, value, \
                                              storageType.getObjectType()); \
    auto copiedVal = SGF.B.createCopyValue(loc, typedValue); \
    emitUnloweredStoreOfCopy(SGF.B, loc, copiedVal, dest, isInit); \
    SGF.B.emitDestroyValueOperation(loc, value); \
    return; \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For loadable types, we need to enter the box by */ \
    /* turning the strong retain into an type-specific retain. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    /* FIXME: resilience */ \
    if (!type->isLoadable(ResilienceExpansion::Maximal)) { \
      SGF.B.createStore##Name(loc, value, dest, isInit); \
      /* store doesn't take ownership of the input, so cancel it out. */ \
      SGF.B.emitDestroyValueOperation(loc, value); \
      return; \
    } \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name) \
  }
#define UNCHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For static reference storage types, we need to enter the box and */ \
    /* release the strong retain. */ \
    auto typedValue = SGF.B.createRefTo##Name(loc, value, \
                                              storageType.getObjectType()); \
    emitUnloweredStoreOfCopy(SGF.B, loc, typedValue, dest, isInit); \
    SGF.B.emitDestroyValueOperation(loc, value); \
    return; \
  }
#include "swift/AST/ReferenceStorage.def"
#undef ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER
  }
  llvm_unreachable("impossible");
}

/// Load a value of the type-of-rvalue out of the given address as a
/// scalar.  The type-of-rvalue must be loadable.
SILValue SILGenFunction::emitSemanticLoad(SILLocation loc,
                                          SILValue src,
                                          const TypeLowering &srcTL,
                                          const TypeLowering &rvalueTL,
                                          IsTake_t isTake) {
  assert(srcTL.getLoweredType().getAddressType() == src->getType());
  assert(rvalueTL.isLoadable() || !silConv.useLoweredAddresses());

  SILType srcType = srcTL.getLoweredType();
  SILType rvalueType = rvalueTL.getLoweredType();

  // Easy case: the types match exactly.
  if (srcType == rvalueType) {
    return srcTL.emitLoadOfCopy(B, loc, src, isTake);
  }

  // Harder case: the srcTL and the rvalueTL match without move only.
  if (srcType.removingMoveOnlyWrapper() ==
      rvalueType.removingMoveOnlyWrapper()) {
    // Ok, we know that one must be move only and the other must not be. Thus we
    // perform one of two things:
    //
    // 1. If our source address is move only and our rvalue type is not move
    // only, lets perform a load [copy] and a moveonly_to_copyable. We just need
    // to insert something so that the move only checker knows that this copy of
    // the move only address must be a last use.
    //
    // 2. If our dest value type is move only and our rvalue type is not move
    // only, then we perform a load [copy] + copyable_to_moveonly.
    SILValue newCopy = srcTL.emitLoadOfCopy(B, loc, src, isTake);
    if (newCopy->getType().isMoveOnlyWrapped()) {
      return B.createOwnedMoveOnlyWrapperToCopyableValue(loc, newCopy);
    }

    return B.createOwnedCopyableToMoveOnlyWrapperValue(loc, newCopy);
  }

  return emitLoadOfSemanticRValue(*this, loc, src, rvalueTL, isTake);
}

/// Load a value of the type-of-reference out of the given address
/// and into the destination address.
void SILGenFunction::emitSemanticLoadInto(SILLocation loc,
                                          SILValue src,
                                          const TypeLowering &srcTL,
                                          SILValue dest,
                                          const TypeLowering &destTL,
                                          IsTake_t isTake,
                                          IsInitialization_t isInit) {
  assert(srcTL.getLoweredType().getAddressType() == src->getType());
  assert(destTL.getLoweredType().getAddressType() == dest->getType());

  // Easy case: the types match.
  if (srcTL.getLoweredType() == destTL.getLoweredType()) {
    B.createCopyAddr(loc, src, dest, isTake, isInit);
    return;
  }

  // Then see if our source address was a moveonlywrapped type and our dest was
  // not. In such a case, just cast away the move only and perform a
  // copy_addr. We are going to error on this later after SILGen.
  if (srcTL.getLoweredType().removingMoveOnlyWrapper() ==
      destTL.getLoweredType().removingMoveOnlyWrapper()) {
    // In such a case, for now emit B.createCopyAddr. In the future, insert the
    // address version of moveonly_to_copyable.
    if (src->getType().isMoveOnlyWrapped()) {
      // If we have a take, we are performing an owned usage of our src addr. If
      // we aren't taking, then we can use guaranteed.
      if (isTake) {
        src = B.createMoveOnlyWrapperToCopyableAddr(loc, src);
      } else {
        src = B.createMoveOnlyWrapperToCopyableAddr(loc, src);
      }
    }
    if (dest->getType().isMoveOnlyWrapped()) {
      if (isInit) {
        dest = B.createMoveOnlyWrapperToCopyableAddr(loc, dest);
      } else {
        dest = B.createMoveOnlyWrapperToCopyableAddr(loc, dest);
      }
    }
    B.createCopyAddr(loc, src, dest, isTake, isInit);
    return;
  }

  auto rvalue = emitLoadOfSemanticRValue(*this, loc, src, srcTL, isTake);
  emitUnloweredStoreOfCopy(B, loc, rvalue, dest, isInit);
}

/// Store an r-value into the given address as an initialization.
void SILGenFunction::emitSemanticStore(SILLocation loc,
                                       SILValue rvalue,
                                       SILValue dest,
                                       const TypeLowering &destTL,
                                       IsInitialization_t isInit) {
  assert(destTL.getLoweredType().getAddressType() == dest->getType());

  if (rvalue->getType().isMoveOnlyWrapped()) {

    // If our rvalue is a moveonlywrapped value, insert a moveonly_to_copyable
    // instruction. We rely on the relevant checkers at the SIL level to
    // validate that this is safe to do. SILGen is just leaving in crumbs to be
    // checked.
    if (rvalue->getType().isObject())
      rvalue = B.createOwnedMoveOnlyWrapperToCopyableValue(loc, rvalue);
    else
      rvalue = B.createMoveOnlyWrapperToCopyableAddr(loc, rvalue);
  }

  // If our dest is a moveonlywrapped address, unwrap it.
  if (dest->getType().isMoveOnlyWrapped()) {
    dest = B.createMoveOnlyWrapperToCopyableAddr(loc, dest);
  }

  // Easy case: the types match.
  if (rvalue->getType().getObjectType() == dest->getType().getObjectType()) {
    assert(!silConv.useLoweredAddresses() ||
           (dest->getType().isAddressOnly(F) == rvalue->getType().isAddress()));
    if (rvalue->getType().isAddress()) {
      B.createCopyAddr(loc, rvalue, dest, IsTake, isInit);
    } else {
      emitUnloweredStoreOfCopy(B, loc, rvalue, dest, isInit);
    }
    return;
  }

  auto &rvalueTL = getTypeLowering(rvalue->getType());
  emitStoreOfSemanticRValue(*this, loc, rvalue, dest, rvalueTL, isInit);
}

/// Convert a semantic rvalue to a value of storage type.
SILValue SILGenFunction::emitConversionFromSemanticValue(SILLocation loc,
                                                         SILValue semanticValue,
                                                         SILType storageType) {
  auto &destTL = getTypeLowering(storageType);
  (void)destTL;
  // Easy case: the types match.
  if (semanticValue->getType() == storageType) {
    return semanticValue;
  }

  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();
  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    llvm_unreachable("address-only types are never loadable");
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    SILValue value = B.createRefTo##Name(loc, semanticValue, storageType); \
    value = B.createCopyValue(loc, value); \
    B.emitDestroyValueOperation(loc, semanticValue); \
    return value; \
  }
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For loadable types, place into a box. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    assert(type->isLoadable(ResilienceExpansion::Maximal)); \
    (void) type; \
    SILValue value = B.createRefTo##Name(loc, semanticValue, storageType); \
    value = B.createCopyValue(loc, value); \
    B.emitDestroyValueOperation(loc, semanticValue); \
    return value; \
  }
#define UNCHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For static reference storage types, place into a box. */ \
    SILValue value = B.createRefTo##Name(loc, semanticValue, storageType); \
    B.emitDestroyValueOperation(loc, semanticValue); \
    return value; \
  }
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("impossible");
}

static void emitTsanInoutAccess(SILGenFunction &SGF, SILLocation loc,
                                ManagedValue address) {
  assert(address.getType().isAddress());
  SILValue accessFnArgs[] = {address.getValue()};

  SGF.B.createBuiltin(loc, SGF.getASTContext().getIdentifier("tsanInoutAccess"),
                      SGF.SGM.Types.getEmptyTupleType(), {}, accessFnArgs);
}

/// Produce a physical address that corresponds to the given l-value
/// component.
static ManagedValue drillIntoComponent(SILGenFunction &SGF,
                                       SILLocation loc,
                                       PathComponent &&component,
                                       ManagedValue base,
                                       TSanKind tsanKind) {
  bool isRValue = component.isRValue();
  ManagedValue addr = std::move(component).project(SGF, loc, base);

  if ((SGF.getModule().getOptions().Sanitizers & SanitizerKind::Thread) &&
      tsanKind == TSanKind::InoutAccess && !isRValue) {
    emitTsanInoutAccess(SGF, loc, addr);
  }

  return addr;
}

/// Find the last component of the given lvalue and derive a base
/// location for it.
static PathComponent &&
drillToLastComponent(SILGenFunction &SGF,
                     SILLocation loc,
                     LValue &&lv,
                     ManagedValue &addr,
                     TSanKind tsanKind = TSanKind::None) {
  assert(lv.begin() != lv.end() &&
         "lvalue must have at least one component");

  for (auto i = lv.begin(), e = lv.end() - 1; i != e; ++i) {
    addr = drillIntoComponent(SGF, loc, std::move(**i), addr, tsanKind);
  }

  return std::move(**(lv.end() - 1));
}

static ArgumentSource emitBaseValueForAccessor(SILGenFunction &SGF,
                                               SILLocation loc, LValue &&lvalue,
                                               CanType baseFormalType,
                                               SILDeclRef accessor) {
  ManagedValue base;
  PathComponent &&component =
    drillToLastComponent(SGF, loc, std::move(lvalue), base);
  base = drillIntoComponent(SGF, loc, std::move(component), base,
                            TSanKind::None);

  return SGF.prepareAccessorBaseArg(loc, base, baseFormalType, accessor);
}

RValue SILGenFunction::emitLoadOfLValue(SILLocation loc, LValue &&src,
                                        SGFContext C, bool isBaseGuaranteed) {
  assert(isReadAccess(src.getAccessKind()));
  ExecutorBreadcrumb prevExecutor;
  RValue result;
  {
    // Any writebacks should be scoped to after the load.
    FormalEvaluationScope scope(*this);

    // We shouldn't need to re-abstract here, but we might have to bridge.
    // This should only happen if we have a global variable of NSString type.
    auto origFormalType = src.getOrigFormalType();
    auto substFormalType = src.getSubstFormalType();
    auto &rvalueTL = getTypeLowering(src.getTypeOfRValue());

    ManagedValue addr;
    PathComponent &&component =
        drillToLastComponent(*this, loc, std::move(src), addr);

    // If the last component is physical, drill down and load from it.
    if (component.isPhysical()) {
      auto projection = std::move(component).project(*this, loc, addr);
      if (projection.getType().isAddress()) {
        auto actorIso = component.asPhysical().takeActorIsolation();

        // If the load must happen in the context of an actor, do a hop first.
        prevExecutor = emitHopToTargetActor(loc, actorIso, addr);
        projection =
            emitLoad(loc, projection.getValue(), origFormalType,
                     substFormalType, rvalueTL, C, IsNotTake, isBaseGuaranteed);
      } else if (isReadAccessResultOwned(src.getAccessKind()) &&
          !projection.isPlusOneOrTrivial(*this)) {

        // Before we copy, if we have a move only wrapped value, unwrap the
        // value using a guaranteed moveonlywrapper_to_copyable.
        if (projection.getType().isMoveOnlyWrapped()) {
          // We are assuming we always get a guaranteed value here.
          assert(projection.getValue()->getOwnershipKind() ==
                 OwnershipKind::Guaranteed);
          // We use SILValues here to ensure we get a tight scope around our
          // copy.
          projection =
              B.createGuaranteedMoveOnlyWrapperToCopyableValue(loc, projection);
        }

        projection = projection.copy(*this, loc);
      }

      result = RValue(*this, loc, substFormalType, projection);
    } else {
      // If the last component is logical, emit a get.
      result = std::move(component.asLogical()).get(*this, loc, addr, C);
    }
  } // End the evaluation scope before any hop back to the current executor.

  // If we hopped to the target's executor, then we need to hop back.
  prevExecutor.emit(*this, loc);
  return result;
}

static AbstractionPattern
getFormalStorageAbstractionPattern(SILGenFunction &SGF, AbstractStorageDecl *field) {
  if (auto var = dyn_cast<VarDecl>(field)) {
    auto origType = SGF.SGM.Types.getAbstractionPattern(var);
    return origType.getReferenceStorageReferentType();
  }
  auto sub = cast<SubscriptDecl>(field);
  return SGF.SGM.Types.getAbstractionPattern(sub);
}

/// Produce a singular RValue for a load from the specified property.  This is
/// designed to work with RValue ManagedValue bases that are either +0 or +1.
RValue SILGenFunction::emitRValueForStorageLoad(
    SILLocation loc, ManagedValue base, CanType baseFormalType,
    bool isSuper, AbstractStorageDecl *storage,
    PreparedArguments &&subscriptIndices,
    SubstitutionMap substitutions,
    AccessSemantics semantics, Type propTy, SGFContext C,
    bool isBaseGuaranteed) {
  AccessStrategy strategy =
    storage->getAccessStrategy(semantics, AccessKind::Read,
                               SGM.M.getSwiftModule(),
                               F.getResilienceExpansion());

  // If we should call an accessor of some kind, do so.
  if (strategy.getKind() != AccessStrategy::Storage) {
    auto accessKind = SGFAccessKind::OwnedObjectRead;

    LValue lv = [&] {
      if (!base) return LValue();

      auto baseAccess = getBaseAccessKind(SGM, storage, accessKind,
                                          strategy, baseFormalType);
      return LValue::forValue(baseAccess, base, baseFormalType);
    }();

    lv.addMemberComponent(*this, loc, storage, substitutions, LValueOptions(),
                          isSuper, accessKind, strategy,
                          propTy->getCanonicalType(),
                          std::move(subscriptIndices),
                          /*index for diagnostics*/ nullptr);

    return emitLoadOfLValue(loc, std::move(lv), C, isBaseGuaranteed);
  }

  assert(isa<VarDecl>(storage) && "only properties should have storage");
  auto field = cast<VarDecl>(storage);
  assert(field->hasStorage() &&
         "Cannot directly access value without storage");

  // For static variables, emit a reference to the global variable backing
  // them.
  // FIXME: This has to be dynamically looked up for classes, and
  // dynamically instantiated for generics.
  if (field->isStatic()) {
    auto baseMeta = base.getType().castTo<MetatypeType>().getInstanceType();
    (void)baseMeta;
    assert(!baseMeta->is<BoundGenericType>() &&
           "generic static stored properties not implemented");
    if (field->getDeclContext()->getSelfClassDecl() &&
        field->hasStorage())
      // FIXME: don't need to check hasStorage, already done above
      assert(field->isFinal() && "non-final class stored properties not implemented");

    return emitRValueForDecl(loc, field, propTy, semantics, C);
  }


  // rvalue MemberRefExprs are produced in two cases: when accessing a 'let'
  // decl member, and when the base is a (non-lvalue) struct.
  assert(baseFormalType->getAnyNominal() &&
         base.getType().getASTType()->getAnyNominal() &&
         "The base of an rvalue MemberRefExpr should be an rvalue value");

  // If the accessed field is stored, emit a StructExtract on the base.

  auto substFormalType = propTy->getCanonicalType();
  auto &lowering = getTypeLowering(substFormalType);

  // Check for an abstraction difference.
  AbstractionPattern origFormalType =
    getFormalStorageAbstractionPattern(*this, field);
  bool hasAbstractionChange = false;
  auto &abstractedTL = getTypeLowering(origFormalType, substFormalType);
  if (!origFormalType.isExactType(substFormalType)) {
    hasAbstractionChange =
        (abstractedTL.getLoweredType() != lowering.getLoweredType());
  }

  // If the base is a reference type, just handle this as loading the lvalue.
  ManagedValue result;
  if (baseFormalType->hasReferenceSemantics()) {
    LValue LV = emitPropertyLValue(loc, base, baseFormalType, field,
                                   LValueOptions(),
                                   SGFAccessKind::OwnedObjectRead,
                                   AccessSemantics::DirectToStorage);
    auto loaded = emitLoadOfLValue(loc, std::move(LV), C, isBaseGuaranteed);
    // If we don't have to reabstract, the load is sufficient.
    if (!hasAbstractionChange)
      return loaded;

    // Otherwise, bring the component up to +1 so we can reabstract it.
    result = std::move(loaded).getAsSingleValue(*this, loc)
                              .copyUnmanaged(*this, loc);
  } else if (!base.getType().isAddress()) {
    // For non-address-only structs, we emit a struct_extract sequence.
    result = B.createStructExtract(loc, base, field);

    if (result.getType().is<ReferenceStorageType>()) {
      // For weak and unowned types, convert the reference to the right
      // pointer, producing a +1.
      result = emitConversionToSemanticRValue(loc, result, lowering);

    } else if (hasAbstractionChange ||
               (!C.isImmediatePlusZeroOk() &&
                !(C.isGuaranteedPlusZeroOk() && isBaseGuaranteed))) {
      // If we have an abstraction change or if we have to produce a result at
      // +1, then copy the value. If we know that our base will stay alive for
      // the entire usage of this value, we can borrow the value at +0 for a
      // guaranteed consumer. Otherwise, since we do not have enough information
      // to know if the base's lifetime last's as long as our use of the access,
      // we can only emit at +0 for immediate clients.
      result = result.copyUnmanaged(*this, loc);
    }
  } else {
    // Create a tiny unenforced access scope around a load from local memory. No
    // cleanup is necessary since we directly emit the load here. This will
    // probably go away with opaque values.
    UnenforcedAccess access;
    SILValue accessAddress =
      access.beginAccess(*this, loc, base.getValue(), SILAccessKind::Read);

    // For address-only sequences, the base is in memory.  Emit a
    // struct_element_addr to get to the field, and then load the element as an
    // rvalue.
    SILValue ElementPtr = B.createStructElementAddr(loc, accessAddress, field);

    result = emitLoad(loc, ElementPtr, abstractedTL,
                      hasAbstractionChange ? SGFContext() : C, IsNotTake);
    access.endAccess(*this);
  }

  // If we're accessing this member with an abstraction change, perform that
  // now.
  if (hasAbstractionChange)
    result =
        emitOrigToSubstValue(loc, result, origFormalType, substFormalType, C);
  return RValue(*this, loc, substFormalType, result);
}

ManagedValue SILGenFunction::emitAddressOfLValue(SILLocation loc,
                                                 LValue &&src,
                                                 TSanKind tsanKind) {
  // Write is only included in this list because of property behaviors.
  // It's obviously unreasonable, though.
  assert(src.getAccessKind() == SGFAccessKind::IgnoredRead ||
         src.getAccessKind() == SGFAccessKind::BorrowedAddressRead ||
         src.getAccessKind() == SGFAccessKind::OwnedAddressRead ||
         src.getAccessKind() == SGFAccessKind::ReadWrite ||
         src.getAccessKind() == SGFAccessKind::Write);

  ManagedValue addr;
  PathComponent &&component =
    drillToLastComponent(*this, loc, std::move(src), addr, tsanKind);

  addr = drillIntoComponent(*this, loc, std::move(component), addr, tsanKind);
  assert(addr.getType().isAddress() &&
         "resolving lvalue did not give an address");
  return ManagedValue::forLValue(addr.getValue());
}

ManagedValue SILGenFunction::emitBorrowedLValue(SILLocation loc,
                                                LValue &&src,
                                                TSanKind tsanKind) {
  assert(src.getAccessKind() == SGFAccessKind::IgnoredRead ||
         src.getAccessKind() == SGFAccessKind::BorrowedAddressRead ||
         src.getAccessKind() == SGFAccessKind::BorrowedObjectRead);
  bool isIgnored = src.getAccessKind() == SGFAccessKind::IgnoredRead;

  ManagedValue base;
  PathComponent &&component =
    drillToLastComponent(*this, loc, std::move(src), base, tsanKind);

  auto value =
    drillIntoComponent(*this, loc, std::move(component), base, tsanKind);

  // If project() returned an owned value, and the caller cares, borrow it.
  if (value.hasCleanup() && !isIgnored)
    value = value.formalAccessBorrow(*this, loc);
  return value;
}

ManagedValue SILGenFunction::emitConsumedLValue(SILLocation loc, LValue &&src,
                                                TSanKind tsanKind) {
  assert(isConsumeAccess(src.getAccessKind()));

  ManagedValue base;
  PathComponent &&component =
      drillToLastComponent(*this, loc, std::move(src), base, tsanKind);

  auto value =
      drillIntoComponent(*this, loc, std::move(component), base, tsanKind);

  return value;
}

LValue
SILGenFunction::emitOpenExistentialLValue(SILLocation loc,
                                          LValue &&lv,
                                          CanArchetypeType openedArchetype,
                                          CanType formalRValueType,
                                          SGFAccessKind accessKind) {
  assert(!formalRValueType->hasLValueType());
  LValueTypeData typeData = {
    accessKind, AbstractionPattern::getOpaque(), formalRValueType,
    getLoweredType(formalRValueType).getASTType()
  };

  // Open up the existential.
  auto rep = lv.getTypeOfRValue()
    .getPreferredExistentialRepresentation();
  switch (rep) {
  case ExistentialRepresentation::Opaque:
  case ExistentialRepresentation::Boxed: {
    lv.add<OpenOpaqueExistentialComponent>(openedArchetype, typeData);
    break;
  }
  case ExistentialRepresentation::Metatype:
  case ExistentialRepresentation::Class: {
    lv.add<OpenNonOpaqueExistentialComponent>(openedArchetype, typeData);
    break;
  }
  case ExistentialRepresentation::None:
    llvm_unreachable("cannot open non-existential");
  }

  return std::move(lv);
}

static bool trySetterPeephole(SILGenFunction &SGF, SILLocation loc,
                              ArgumentSource &&src, LValue &&dest) {
  // The last component must be a getter/setter.
  // TODO: allow reabstraction here, too.
  auto &component = **(dest.end() - 1);
  if (component.getKind() != PathComponent::GetterSetterKind)
    return false;

  // We cannot apply the peephole if the l-value includes an
  // open-existential component because we need to make sure that
  // the opened archetype is available everywhere during emission.
  // TODO: should we instead just immediately open the existential
  // during emitLValue and simply leave the opened address in the LValue?
  // Or is there some reasonable way to detect that this is happening
  // and avoid affecting cases where it is not necessary?
  for (auto &componentPtr : dest) {
    if (componentPtr->isOpenExistential())
      return false;
  }

  auto &setterComponent = static_cast<GetterSetterComponent&>(component);
  if (setterComponent.canRewriteSetAsPropertyWrapperInit(SGF) ||
      setterComponent.canRewriteSetAsInitAccessor(SGF))
    return false;

  setterComponent.emitAssignWithSetter(SGF, loc, std::move(dest),
                                       std::move(src));
  return true;;
}

void SILGenFunction::emitAssignToLValue(SILLocation loc, RValue &&src,
                                        LValue &&dest) {
  emitAssignToLValue(loc, ArgumentSource(loc, std::move(src)), std::move(dest));
}

namespace {

} // end anonymous namespace

/// Checks if the last component of the LValue refers to the given var decl.
static bool referencesVar(PathComponent const& comp, VarDecl *var) {
  if (comp.getKind() != PathComponent::RefElementKind)
    return false;

  return static_cast<RefElementComponent const&>(comp).getField() == var;
}

void SILGenFunction::emitAssignToLValue(SILLocation loc,
                                        ArgumentSource &&src,
                                        LValue &&dest) {
  // Enter a FormalEvaluationScope so that formal access to independent LValue
  // components do not overlap. Furthermore, use an ArgumentScope to force
  // cleanup of materialized LValues immediately, before evaluating the next
  // LValue. For example: (x[i], x[j]) = a, b
  ArgumentScope argScope(*this, loc);

  // If this is an assignment to a distributed actor's actorSystem, push
  // a clean-up to also initialize its id.
  if (LLVM_UNLIKELY(DistActorCtorContext) &&
      referencesVar(**(dest.end()-1), DistActorCtorContext->getSystemVar()))
    Cleanups.pushCleanup<InitializeDistActorIdentity>(*DistActorCtorContext);

  // If the last component is a getter/setter component, use a special
  // generation pattern that allows us to peephole the emission of the RHS.
  if (trySetterPeephole(*this, loc, std::move(src), std::move(dest))) {
    argScope.pop();
    return;
  }

  // Otherwise, force the RHS now to preserve evaluation order.
  auto srcLoc = src.getLocation();
  RValue srcValue = std::move(src).getAsRValue(*this);

  // Peephole: instead of materializing and then assigning into a
  // translation component, untransform the value first.
  while (dest.isLastComponentTranslation()) {
    srcValue = std::move(dest.getLastTranslationComponent())
                 .untranslate(*this, loc, std::move(srcValue));
    dest.dropLastTranslationComponent();
  }

  src = ArgumentSource(srcLoc, std::move(srcValue));

  // Resolve all components up to the last, keeping track of value-type logical
  // properties we need to write back to.
  ManagedValue destAddr;
  PathComponent &&component =
    drillToLastComponent(*this, loc, std::move(dest), destAddr);
  
  // Write to the tail component.
  if (component.isPhysical()) {
    std::move(component.asPhysical()).set(*this, loc, std::move(src), destAddr);
  } else {
    std::move(component.asLogical()).set(*this, loc, std::move(src), destAddr);
  }

  // The writeback scope closing will propagate the value back up through the
  // writeback chain.
  argScope.pop();
}

void SILGenFunction::emitCopyLValueInto(SILLocation loc, LValue &&src,
                                        Initialization *dest) {
  auto skipPeephole = [&]{
    auto loaded = emitLoadOfLValue(loc, std::move(src), SGFContext(dest));
    if (!loaded.isInContext())
      std::move(loaded).forwardInto(*this, loc, dest);
  };
  
  // If the source is a physical lvalue, the destination is a single address,
  // and there's no semantic conversion necessary, do a copy_addr from the
  // lvalue into the destination.
  if (!src.isPhysical())
    return skipPeephole();
  if (!dest->canPerformInPlaceInitialization())
    return skipPeephole();
  auto destAddr = dest->getAddressForInPlaceInitialization(*this, loc);
  assert(src.getTypeOfRValue().getASTType()
           == destAddr->getType().getASTType());

  auto srcAddr = emitAddressOfLValue(loc, std::move(src)).getUnmanagedValue();

  UnenforcedAccess access;
  SILValue accessAddress =
    access.beginAccess(*this, loc, destAddr, SILAccessKind::Modify);

  if (srcAddr->getType().isMoveOnlyWrapped())
    srcAddr = B.createMoveOnlyWrapperToCopyableAddr(loc, srcAddr);
  if (accessAddress->getType().isMoveOnlyWrapped())
    accessAddress = B.createMoveOnlyWrapperToCopyableAddr(loc, accessAddress);

  B.createCopyAddr(loc, srcAddr, accessAddress, IsNotTake, IsInitialization);
  access.endAccess(*this);

  dest->finishInitialization(*this);
}

void SILGenFunction::emitAssignLValueToLValue(SILLocation loc, LValue &&src,
                                              LValue &&dest) {
  // Only perform the peephole if both operands are physical, there's no
  // semantic conversion necessary, and exclusivity enforcement
  // is not enabled. The peephole interferes with exclusivity enforcement
  // because it causes the formal accesses to the source and destination to
  // overlap.
  bool peepholeConflict =
      !src.isObviouslyNonConflicting(dest, SGFAccessKind::OwnedObjectRead,
                                     SGFAccessKind::Write);

  if (peepholeConflict || !src.isPhysical() || !dest.isPhysical()) {
    RValue loaded = emitLoadOfLValue(loc, std::move(src), SGFContext());
    emitAssignToLValue(loc, std::move(loaded), std::move(dest));
    return;
  }

  // If this is an assignment to a distributed actor's actorSystem, push
  // a clean-up to also initialize its id.
  // FIXME: in what situations does this lvalue to lvalue assign happen?
  if (LLVM_UNLIKELY(DistActorCtorContext) &&
      referencesVar(**(dest.end()-1), DistActorCtorContext->getSystemVar()))
    Cleanups.pushCleanup<InitializeDistActorIdentity>(*DistActorCtorContext);

  auto &rvalueTL = getTypeLowering(src.getTypeOfRValue());

  auto srcAddr = emitAddressOfLValue(loc, std::move(src)).getUnmanagedValue();
  auto destAddr = emitAddressOfLValue(loc, std::move(dest)).getUnmanagedValue();

  if (srcAddr->getType() == destAddr->getType()) {
    B.createCopyAddr(loc, srcAddr, destAddr, IsNotTake, IsNotInitialization);
  } else {
    // If there's a semantic conversion necessary, do a load then assign.
    auto loaded = emitLoad(loc, srcAddr, rvalueTL, SGFContext(), IsNotTake);
    loaded.assignInto(*this, loc, destAddr);
  }
}
