//===--- SILGenApply.cpp - Constructs call sites for SILGen ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ArgumentScope.h"
#include "ArgumentSource.h"
#include "Callee.h"
#include "Conversion.h"
#include "ExecutorBreadcrumb.h"
#include "FormalEvaluation.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "ResultPlan.h"
#include "Scope.h"
#include "SpecializedEmitter.h"
#include "Varargs.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/Effects.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/ExternalUnion.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Unicode.h"
#include "swift/SIL/AbstractionPatternGenerators.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/Support/Compiler.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                             Utility Functions
//===----------------------------------------------------------------------===//

SubstitutionMap SILGenModule::mapSubstitutionsForWitnessOverride(
                                              AbstractFunctionDecl *original,
                                              AbstractFunctionDecl *overridden,
                                              SubstitutionMap subs) {
  // Substitute the 'Self' type of the base protocol.
  auto origProto = cast<ProtocolDecl>(original->getDeclContext());
  Type origProtoSelfType = origProto->getSelfInterfaceType();
  auto baseProto = cast<ProtocolDecl>(overridden->getDeclContext());
  return SubstitutionMap::getProtocolSubstitutions(
      baseProto, origProtoSelfType.subst(subs),
      subs.lookupConformance(origProtoSelfType->getCanonicalType(), baseProto));
}

/// Return the abstraction pattern to use when calling a function value.
static AbstractionPattern
getIndirectApplyAbstractionPattern(SILGenFunction &SGF,
                                   AbstractionPattern pattern,
                                   CanFunctionType fnType) {
  assert(fnType);
  switch (fnType->getRepresentation()) {
  case FunctionTypeRepresentation::Swift:
  case FunctionTypeRepresentation::Thin:
    return pattern;

  case FunctionTypeRepresentation::CFunctionPointer:
  case FunctionTypeRepresentation::Block: {
    // C and block function parameters and results are implicitly
    // bridged to a foreign type.
    auto silRep =
        SILFunctionTypeRepresentation(fnType->getExtInfo().getRepresentation());
    auto bridgedType = SGF.SGM.Types.getBridgedFunctionType(
        pattern, fnType, Bridgeability::Full, silRep);
    pattern.rewriteType(CanGenericSignature(), bridgedType);
    return pattern;
  }
  }
  llvm_unreachable("bad representation");
}

/// Return the formal type for the partial-apply result type of a
/// dynamic method invocation.
static CanFunctionType
getPartialApplyOfDynamicMethodFormalType(SILGenModule &SGM, SILDeclRef member,
                                         ConcreteDeclRef memberRef) {
  auto memberCI =
      SGM.Types.getConstantInfo(TypeExpansionContext::minimal(), member);

  // Construct a non-generic version of the formal type.
  // This works because we're only using foreign members, where presumably
  // substitution doesn't matter.
  CanAnyFunctionType completeMethodTy = memberCI.LoweredType;
  if (auto genericFnType = dyn_cast<GenericFunctionType>(completeMethodTy)) {
    completeMethodTy = cast<FunctionType>(
      genericFnType->substGenericArgs(memberRef.getSubstitutions())
                   ->getCanonicalType());
  }

  // Adjust the parameters by removing the self parameter, which we
  // will be partially applying.
  auto params = completeMethodTy.getParams().drop_back();

  // Adjust the result type to replace dynamic-self with AnyObject.
  CanType resultType = completeMethodTy.getResult();
  if (auto fnDecl = dyn_cast<FuncDecl>(member.getDecl())) {
    if (fnDecl->hasDynamicSelfResult()) {
      auto anyObjectTy = SGM.getASTContext().getAnyObjectType();
      resultType = resultType->replaceCovariantResultType(anyObjectTy, 0)
                             ->getCanonicalType();
    }
  }

  // Adjust the ExtInfo by using a Swift representation.
  auto extInfo = completeMethodTy->getExtInfo()
                   .withRepresentation(FunctionTypeRepresentation::Swift);

  auto fnType = CanFunctionType::get(params, resultType, extInfo);
  return fnType;
}

/// Retrieve the type to use for a method found via dynamic lookup.
static SILType
getDynamicMethodLoweredType(SILModule &M,
                            SILDeclRef constant,
                            CanAnyFunctionType substMemberTy) {
  assert(constant.isForeign);
  auto objcFormalTy = substMemberTy.withExtInfo(
      substMemberTy->getExtInfo()
          .intoBuilder()
          .withSILRepresentation(SILFunctionTypeRepresentation::ObjCMethod)
          .build());
  return SILType::getPrimitiveObjectType(
      M.Types.getUncachedSILFunctionTypeForConstant(
          TypeExpansionContext::minimal(), constant, objcFormalTy));
}

/// Check if we can perform a dynamic dispatch on a super method call.
static bool canUseStaticDispatch(SILGenFunction &SGF,
                                 SILDeclRef constant) {
  auto *funcDecl = cast<AbstractFunctionDecl>(constant.getDecl());

  if (funcDecl->isFinal())
    return true;
  
  // Native initializing entry points are always statically dispatched.
  if (constant.kind == SILDeclRef::Kind::Initializer
      && !constant.isForeign)
    return true;
  
  // Extension methods currently must be statically dispatched, unless they're
  // @objc or dynamic.
  if (isa<ExtensionDecl>(funcDecl->getDeclContext()) && !constant.isForeign)
    return true;

  // We cannot form a direct reference to a method body defined in
  // Objective-C.
  if (constant.isForeign)
    return false;

  // If we cannot form a direct reference due to resilience constraints,
  // we have to dynamic dispatch.
  if (SGF.F.isSerialized())
    return false;

  // If the method is defined in the same module, we can reference it
  // directly.
  auto thisModule = SGF.SGM.M.getSwiftModule();
  if (thisModule == funcDecl->getModuleContext())
    return true;

  // Otherwise, we must dynamic dispatch.
  return false;
}

static SILValue getOriginalSelfValue(SILValue selfValue) {
  if (auto *TTOI = dyn_cast<ThickToObjCMetatypeInst>(selfValue))
    selfValue = TTOI->getOperand();

  if (auto *BBI = dyn_cast<BeginBorrowInst>(selfValue))
    selfValue = BBI->getOperand();

  while (auto *UI = dyn_cast<UpcastInst>(selfValue))
    selfValue = UI->getOperand();

  if (auto *UTBCI = dyn_cast<UncheckedTrivialBitCastInst>(selfValue))
    selfValue = UTBCI->getOperand();

  return selfValue;
}

/// Borrow self and then upcast self to its original type. If self is a
/// metatype, we just return the original metatype since metatypes are trivial.
static ManagedValue borrowedCastToOriginalSelfType(SILGenFunction &SGF,
                                                   SILLocation loc,
                                                   ManagedValue self) {
  SILValue originalSelf = getOriginalSelfValue(self.getValue());
  SILType originalSelfType = originalSelf->getType();

  // If we have a metatype, then we just return the original self value since
  // metatypes are trivial, so we can avoid ownership concerns.
  if (originalSelfType.is<AnyMetatypeType>()) {
    assert(originalSelfType.isTrivial(SGF.F) &&
           "Metatypes should always be trivial");
    return ManagedValue::forUnmanaged(originalSelf);
  }

  // Otherwise, we have a non-metatype. Use a borrow+unchecked_ref_cast.
  return SGF.B.createUncheckedRefCast(loc, self.formalAccessBorrow(SGF, loc),
                                      originalSelfType);
}

static ManagedValue convertOwnershipConventionGivenParamInfo(
    SILGenFunction &SGF, SILParameterInfo param,
    llvm::Optional<AnyFunctionType::Param> origParam, ManagedValue value,
    SILLocation loc, bool isForCoroutine) {
  bool isOwned = false;
  if (origParam) {
    isOwned |= origParam->isOwned();
  }

  // If we have a moveonlywrapped type that is trivial when unwrapped, then we
  // at an ABI level our parameter will be passed as direct_unowned. We want to
  // consume this value though if we have an owned parameter.
  auto valueType = value.getType();
  if (isOwned && valueType.isMoveOnlyWrapped() &&
      valueType.removingMoveOnlyWrapper().isTrivial(SGF.F)) {
    if (value.getOwnershipKind() == OwnershipKind::Guaranteed) {
      value = value.copyUnmanaged(SGF, loc);
      return SGF.B.createOwnedMoveOnlyWrapperToCopyableValue(loc, value);
    }
  }

  if (param.isConsumed() &&
      value.getOwnershipKind() == OwnershipKind::Guaranteed) {
    return value.copyUnmanaged(SGF, loc);
  }

  // If we are emitting arguments for a coroutine, we need to borrow owned
  // values to ensure that they are live over the entire closure invocation. If
  // we do not have a coroutine, then we have an immediate non-consuming use so
  // no borrow is necessary.
  if (isForCoroutine && value.getOwnershipKind() == OwnershipKind::Owned) {
    if (param.isDirectGuaranteed() || (!SGF.silConv.useLoweredAddresses() &&
                                       param.isIndirectInGuaranteed())) {
      return value.formalAccessBorrow(SGF, loc);
    }
  }

  return value;
}

static void convertOwnershipConventionsGivenParamInfos(
    SILGenFunction &SGF, ArrayRef<SILParameterInfo> params,
    ArrayRef<ManagedValue> values, SILLocation loc, bool isForCoroutine,
    llvm::SmallVectorImpl<ManagedValue> &outVar) {
  assert(params.size() == values.size() &&
         "Different number of params from arguments");
  llvm::transform(indices(params), std::back_inserter(outVar),
                  [&](unsigned i) -> ManagedValue {
                    return convertOwnershipConventionGivenParamInfo(
                        SGF, params[i], llvm::None /*orig param*/, values[i],
                        loc, isForCoroutine);
                  });
}

static bool shouldApplyBackDeploymentThunk(ValueDecl *decl, ASTContext &ctx,
                                           ResilienceExpansion expansion) {
  auto backDeployBeforeVersion = decl->getBackDeployedBeforeOSVersion(ctx);
  if (!backDeployBeforeVersion)
    return false;

  // If the context of the application is inlinable then we must always call the
  // back deployment thunk since we can't predict the deployment targets of
  // other modules.
  if (expansion != ResilienceExpansion::Maximal)
    return true;

  // In resilient function bodies skip calling the back deployment thunk when
  // the deployment target is high enough that the ABI implementation of the
  // back deployed function is guaranteed to be available.
  auto deploymentAvailability = AvailabilityContext::forDeploymentTarget(ctx);
  auto declAvailability =
      AvailabilityContext(VersionRange::allGTE(*backDeployBeforeVersion));

  if (deploymentAvailability.isContainedIn(declAvailability))
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
//                                   Callee
//===----------------------------------------------------------------------===//

namespace {

/// Abstractly represents a callee, which may be a constant or function value,
/// and knows how to perform dynamic dispatch and reference the appropriate
/// entry point at any valid uncurry level.
class Callee {
public:
  enum class Kind {
    /// An indirect function value.
    IndirectValue,

    /// A direct standalone function call, referenceable by a FunctionRefInst.
    StandaloneFunction,

    /// A direct standalone function call, referenceable by a
    /// PreviousDynamicFunctionRefInst.
    StandaloneFunctionDynamicallyReplaceableImpl,

    /// Enum case constructor call.
    EnumElement,

    /// A method call using class method dispatch.
    ClassMethod,

    /// A method call using super method dispatch.
    SuperMethod,

    /// A method call using protocol witness table dispatch.
    WitnessMethod,

    /// A method call using dynamic lookup.
    DynamicMethod,
  };

  Kind kind;

  // Move, don't copy.
  Callee(const Callee &) = delete;
  Callee &operator=(const Callee &) = delete;

private:
  /// An IndirectValue callee represents something like a swift closure or a c
  /// function pointer where we have /no/ information at all on what the callee
  /// is. This contrasts with a class method, where we may not know the exact
  /// method that is being called, but we have some information from the type
  /// system that we have an actual method.
  ///
  /// *NOTE* This will never be non-null if Constant is non-null.
  ManagedValue IndirectValue;

  /// If we are trying to call a specific method or function, this field is set
  /// to the decl ref information for that callee.
  ///
  /// *NOTE* This should never be non-null if IndirectValue is non-null.
  SILDeclRef Constant;

  /// The abstraction pattern of the callee.
  AbstractionPattern OrigFormalInterfaceType;

  /// The callee's formal type with substitutions applied.
  CanFunctionType SubstFormalInterfaceType;

  /// The substitutions applied to OrigFormalInterfaceType to produce
  /// SubstFormalInterfaceType, substituted into the current type expansion
  /// context.
  SubstitutionMap Substitutions;

  /// The list of values captured by our callee.
  llvm::Optional<SmallVector<ManagedValue, 2>> Captures;

  // The pointer back to the AST node that produced the callee.
  SILLocation Loc;

  static CanFunctionType
  getSubstFormalInterfaceType(CanAnyFunctionType substFormalType,
                              SubstitutionMap subs) {
    if (auto *gft = substFormalType->getAs<GenericFunctionType>()) {
      return cast<FunctionType>(
        gft->substGenericArgs(subs)
          ->getCanonicalType());
    }

    return cast<FunctionType>(substFormalType);
  }

  /// Constructor for Callee::forIndirect.
  Callee(ManagedValue indirectValue,
         AbstractionPattern origFormalType,
         CanFunctionType substFormalType,
         SILLocation l)
    : kind(Kind::IndirectValue),
      IndirectValue(indirectValue),
      OrigFormalInterfaceType(origFormalType),
      SubstFormalInterfaceType(substFormalType),
      Loc(l)
  {}

  /// Constructor for Callee::forDirect.
  Callee(SILGenFunction &SGF, SILDeclRef standaloneFunction,
         AbstractionPattern origFormalType, CanAnyFunctionType substFormalType,
         SubstitutionMap subs, SubstitutionMap formalSubs, SILLocation l,
         bool callDynamicallyReplaceableImpl = false)
      : kind(callDynamicallyReplaceableImpl
                 ? Kind::StandaloneFunctionDynamicallyReplaceableImpl
                 : Kind::StandaloneFunction),
        Constant(standaloneFunction),
        OrigFormalInterfaceType(origFormalType.withSubstitutions(subs)),
        SubstFormalInterfaceType(
            getSubstFormalInterfaceType(substFormalType, formalSubs)),
        Substitutions(subs), Loc(l) {}

  /// Constructor called by all for* factory methods except forDirect and
  /// forIndirect.
  Callee(Kind methodKind, SILGenFunction &SGF, SILDeclRef methodName,
         AbstractionPattern origFormalType, CanAnyFunctionType substFormalType,
         SubstitutionMap subs, SILLocation l)
      : kind(methodKind), Constant(methodName),
        // FIXME: use .withSubstitutions(subs) here when we figure out how
        // to provide the right substitutions for overrides
        OrigFormalInterfaceType(origFormalType),
        SubstFormalInterfaceType(
            getSubstFormalInterfaceType(substFormalType, subs)),
        Substitutions(subs), Loc(l) {}

public:

  static Callee forIndirect(ManagedValue indirectValue,
                            AbstractionPattern origFormalType,
                            CanFunctionType substFormalType,
                            SILLocation l) {
    return Callee(indirectValue, origFormalType, substFormalType, l);
  }
  static Callee forDirect(SILGenFunction &SGF, SILDeclRef c,
                          SubstitutionMap subs,
                          SILLocation l,
                          bool callPreviousDynamicReplaceableImpl = false) {
    auto &ci = SGF.getConstantInfo(SGF.getTypeExpansionContext(), c);
    return Callee(
        SGF, c, ci.FormalPattern, ci.FormalType,
        subs.mapIntoTypeExpansionContext(SGF.getTypeExpansionContext()),
        subs,
        l,
        callPreviousDynamicReplaceableImpl);
  }

  static Callee forEnumElement(SILGenFunction &SGF, SILDeclRef c,
                               SubstitutionMap subs,
                               SILLocation l) {
    assert(isa<EnumElementDecl>(c.getDecl()));
    auto &ci = SGF.getConstantInfo(SGF.getTypeExpansionContext(), c);
    return Callee(
        Kind::EnumElement, SGF, c, ci.FormalPattern, ci.FormalType,
        subs.mapIntoTypeExpansionContext(SGF.getTypeExpansionContext()), l);
  }
  static Callee forClassMethod(SILGenFunction &SGF,
                               SILDeclRef c, SubstitutionMap subs,
                               SILLocation l) {
    auto base = c.getOverriddenVTableEntry();
    auto &baseCI = SGF.getConstantInfo(SGF.getTypeExpansionContext(), base);
    auto &derivedCI = SGF.getConstantInfo(SGF.getTypeExpansionContext(), c);
    return Callee(
        Kind::ClassMethod, SGF, c, baseCI.FormalPattern, derivedCI.FormalType,
        subs.mapIntoTypeExpansionContext(SGF.getTypeExpansionContext()), l);
  }
  static Callee forSuperMethod(SILGenFunction &SGF,
                               SILDeclRef c, SubstitutionMap subs,
                               SILLocation l) {
    auto &ci = SGF.getConstantInfo(SGF.getTypeExpansionContext(), c);
    return Callee(
        Kind::SuperMethod, SGF, c, ci.FormalPattern, ci.FormalType,
        subs.mapIntoTypeExpansionContext(SGF.getTypeExpansionContext()), l);
  }
  static Callee forWitnessMethod(SILGenFunction &SGF,
                                 CanType protocolSelfType,
                                 SILDeclRef c,
                                 SubstitutionMap subs,
                                 SILLocation l) {
    // Find a witness that has an entry in the witness table.
    if (!c.requiresNewWitnessTableEntry()) {
      // Retrieve the constant that has an entry in the witness table.
      auto original = cast<AbstractFunctionDecl>(c.getDecl());
      c = c.getOverriddenWitnessTableEntry();
      c = c.asForeign(c.getDecl()->isObjC());
      auto overridden = cast<AbstractFunctionDecl>(c.getDecl());

      // Substitute the 'Self' type of the base protocol.
      subs = SILGenModule::mapSubstitutionsForWitnessOverride(original,
                                                              overridden,
                                                              subs);
    }

    auto &ci = SGF.getConstantInfo(SGF.getTypeExpansionContext(), c);
    return Callee(
        Kind::WitnessMethod, SGF, c, ci.FormalPattern, ci.FormalType,
        subs.mapIntoTypeExpansionContext(SGF.getTypeExpansionContext()), l);
  }
  static Callee forDynamic(SILGenFunction &SGF,
                           SILDeclRef c, SubstitutionMap constantSubs,
                           CanAnyFunctionType substFormalType,
                           SubstitutionMap subs, SILLocation l) {
    auto &ci = SGF.getConstantInfo(SGF.getTypeExpansionContext(), c);
    AbstractionPattern origFormalType = ci.FormalPattern;

    // Replace the original self type with the partially-applied subst type.
    auto origFormalFnType = cast<AnyFunctionType>(origFormalType.getType());
    if (auto genericFnType = dyn_cast<GenericFunctionType>(origFormalFnType)) {
      // If we have a generic function type, substitute it.  This is normally
      // a huge no-no, but the partial-application hacks we're doing here
      // really kindof mandate it, and it works out because we're always using
      // a foreign function.  If/when we support native dynamic functions,
      // this will stop working and we will need a completely different
      // approach.
      origFormalFnType =
        cast<FunctionType>(genericFnType->substGenericArgs(constantSubs)
                                        ->getCanonicalType());
    }
    origFormalType.rewriteType(CanGenericSignature(), origFormalFnType);

    return Callee(
        Kind::DynamicMethod, SGF, c, origFormalType, substFormalType,
        subs.mapIntoTypeExpansionContext(SGF.getTypeExpansionContext()), l);
  }

  Callee(Callee &&) = default;
  Callee &operator=(Callee &&) = default;

  void setCaptures(SmallVectorImpl<ManagedValue> &&captures) {
    Captures = std::move(captures);
  }
  
  ArrayRef<ManagedValue> getCaptures() const {
    if (Captures)
      return *Captures;
    return {};
  }
  
  bool hasCaptures() const {
    return Captures.has_value();
  }

  AbstractionPattern getOrigFormalType() const {
    return OrigFormalInterfaceType;
  }

  CanFunctionType getSubstFormalType() const {
    return SubstFormalInterfaceType;
  }

  bool requiresSelfValueForDispatch() const {
    switch (kind) {
    case Kind::IndirectValue:
    case Kind::StandaloneFunction:
    case Kind::StandaloneFunctionDynamicallyReplaceableImpl:
    case Kind::EnumElement:
      return false;
    case Kind::WitnessMethod:
      if (Constant.isForeign)
        return true;
      return false;
    case Kind::ClassMethod:
    case Kind::SuperMethod:
    case Kind::DynamicMethod:
      return true;
    }

    llvm_unreachable("Unhandled Kind in switch.");
  }

  EnumElementDecl *getEnumElementDecl() {
    assert(kind == Kind::EnumElement);
    return cast<EnumElementDecl>(Constant.getDecl());
  }

  ValueDecl *getDecl() {
    return Constant.getDecl();
  }

  CalleeTypeInfo createCalleeTypeInfo(SILGenFunction &SGF,
                                      llvm::Optional<SILDeclRef> constant,
                                      SILType formalFnType) const & {
    CalleeTypeInfo result;

    result.substFnType =
        formalFnType.castTo<SILFunctionType>()->substGenericArgs(
            SGF.SGM.M, Substitutions, SGF.getTypeExpansionContext());

    if (!constant || !constant->isForeign)
      return result;

    auto func = cast<AbstractFunctionDecl>(constant->getDecl());
    result.foreign = ForeignInfo{
        func->getImportAsMemberStatus(),
        func->getForeignErrorConvention(),
        func->getForeignAsyncConvention(),
    };

    // Remove the metatype "self" parameter by making this a static member.
    if (isa_and_nonnull<clang::CXXConstructorDecl>(
            constant->getDecl()->getClangDecl()))
      result.foreign.self.setStatic();

    return result;
  }

  ManagedValue getFnValue(SILGenFunction &SGF,
                          llvm::Optional<ManagedValue> borrowedSelf) const & {
    llvm::Optional<SILDeclRef> constant = llvm::None;

    if (Constant)
      constant = Constant;

    switch (kind) {
    case Kind::IndirectValue:
      assert(Substitutions.empty());
      return IndirectValue;
    case Kind::EnumElement:
    case Kind::StandaloneFunction: {
      auto constantInfo =
          SGF.getConstantInfo(SGF.getTypeExpansionContext(), *constant);
      SILValue ref = SGF.emitGlobalFunctionRef(Loc, *constant, constantInfo);
      return ManagedValue::forUnmanaged(ref);
    }
    case Kind::StandaloneFunctionDynamicallyReplaceableImpl: {
      auto constantInfo =
          SGF.getConstantInfo(SGF.getTypeExpansionContext(), *constant);
      SILValue ref =
          SGF.emitGlobalFunctionRef(Loc, *constant, constantInfo, true);
      return ManagedValue::forUnmanaged(ref);
    }
    case Kind::ClassMethod: {
      auto methodTy = SGF.SGM.Types.getConstantOverrideType(
          SGF.getTypeExpansionContext(), *constant);

      // Otherwise, do the dynamic dispatch inline.
      ArgumentScope S(SGF, Loc);

      SILValue methodVal;
      if (!constant->isForeign) {
        methodVal = SGF.emitClassMethodRef(
            Loc, borrowedSelf->getValue(), *constant, methodTy);
      } else {
        methodVal = SGF.B.createObjCMethod(
            Loc, borrowedSelf->getValue(), *constant,
            SILType::getPrimitiveObjectType(methodTy));
      }
      S.pop();
      return ManagedValue::forUnmanaged(methodVal);
    }
    case Kind::SuperMethod: {
      ArgumentScope S(SGF, Loc);
      ManagedValue castValue = borrowedCastToOriginalSelfType(
        SGF, Loc, *borrowedSelf);

      auto base = constant->getOverriddenVTableEntry();
      auto constantInfo = SGF.SGM.Types.getConstantOverrideInfo(
          SGF.getTypeExpansionContext(), *constant, base);

      ManagedValue fn;
      if (!constant->isForeign) {
        fn = SGF.B.createSuperMethod(Loc, castValue, *constant,
                                     constantInfo.getSILType());
      } else {
        fn = SGF.B.createObjCSuperMethod(Loc, castValue, *constant,
                                         constantInfo.getSILType());
      }
      S.pop();
      return fn;
    }
    case Kind::WitnessMethod: {
      if (auto func = constant->getFuncDecl()) {
        if (func->isDistributed() && isa<ProtocolDecl>(func->getDeclContext())) {
          // If we're calling cross-actor, we must always use a distributed thunk
          if (!isSameActorIsolated(func, SGF.FunctionDC)) {
            // We must adjust the constant to use a distributed thunk.
            constant = constant->asDistributed();
          }
        }
      }

      auto constantInfo =
          SGF.getConstantInfo(SGF.getTypeExpansionContext(), *constant);

      // TODO:  substOpaqueTypesWithUnderlyingTypes ...
      auto proto = cast<ProtocolDecl>(Constant.getDecl()->getDeclContext());
      auto selfType = proto->getSelfInterfaceType()->getCanonicalType();
      auto lookupType = selfType.subst(Substitutions)->getCanonicalType();
      auto conformance = Substitutions.lookupConformance(selfType, proto);

      ArgumentScope S(SGF, Loc);

      SILValue fn;
      if (!constant->isForeign) {
        fn = SGF.B.createWitnessMethod(
          Loc, lookupType, conformance, *constant,
          constantInfo.getSILType());
      } else {
        fn = SGF.B.createObjCMethod(Loc, borrowedSelf->getValue(),
                                    *constant, constantInfo.getSILType());
      }
      S.pop();
      return ManagedValue::forUnmanaged(fn);
    }
    case Kind::DynamicMethod: {
      auto closureType = getDynamicMethodLoweredType(
          SGF.SGM.M, *constant, getSubstFormalType());

      ArgumentScope S(SGF, Loc);
      SILValue fn = SGF.B.createObjCMethod(
          Loc, borrowedSelf->getValue(), *constant,
          closureType);
      S.pop();
      return ManagedValue::forUnmanaged(fn);
    }
    }
    llvm_unreachable("unhandled kind");
  }

  CalleeTypeInfo getTypeInfo(SILGenFunction &SGF) const & {
    llvm::Optional<SILDeclRef> constant = llvm::None;

    if (Constant)
      constant = Constant;

    switch (kind) {
    case Kind::IndirectValue:
      assert(Substitutions.empty());
      return createCalleeTypeInfo(SGF, constant, IndirectValue.getType());

    case Kind::StandaloneFunctionDynamicallyReplaceableImpl:
    case Kind::StandaloneFunction: {
      auto constantInfo =
          SGF.getConstantInfo(SGF.getTypeExpansionContext(), *constant);
      return createCalleeTypeInfo(SGF, constant, constantInfo.getSILType());
    }
    case Kind::EnumElement: {
      // Emit a direct call to the element constructor thunk.
      auto constantInfo =
          SGF.getConstantInfo(SGF.getTypeExpansionContext(), *constant);
      return createCalleeTypeInfo(SGF, constant, constantInfo.getSILType());
    }
    case Kind::ClassMethod: {
      auto constantInfo = SGF.SGM.Types.getConstantOverrideInfo(
          SGF.getTypeExpansionContext(), *constant);
      return createCalleeTypeInfo(SGF, constant, constantInfo.getSILType());
    }
    case Kind::SuperMethod: {
      auto base = constant->getOverriddenVTableEntry();
      auto constantInfo = SGF.SGM.Types.getConstantOverrideInfo(
          SGF.getTypeExpansionContext(), *constant, base);
      return createCalleeTypeInfo(SGF, constant, constantInfo.getSILType());
    }
    case Kind::WitnessMethod: {
      if (auto func = constant->getFuncDecl()) {
        if (func->isDistributed() && isa<ProtocolDecl>(func->getDeclContext())) {
          // If we're calling cross-actor, we must always use a distributed thunk
          if (!isSameActorIsolated(func, SGF.FunctionDC)) {
            /// We must adjust the constant to use a distributed thunk.
            constant = constant->asDistributed();
          }
        }
      }

      auto constantInfo =
          SGF.getConstantInfo(SGF.getTypeExpansionContext(), *constant);
      return createCalleeTypeInfo(SGF, constant, constantInfo.getSILType());
    }
    case Kind::DynamicMethod: {
      auto formalType = getDynamicMethodLoweredType(
          SGF.SGM.M, *constant, getSubstFormalType());
      return createCalleeTypeInfo(SGF, constant, formalType);
    }
    }
    llvm_unreachable("unhandled kind");
  }

  SubstitutionMap getSubstitutions() const {
    return Substitutions;
  }

  SILDeclRef getMethodName() const {
    return Constant;
  }

  /// Return a specialized emission function if this is a function with a known
  /// lowering, such as a builtin, or return null if there is no specialized
  /// emitter.
  llvm::Optional<SpecializedEmitter>
  getSpecializedEmitter(SILGenModule &SGM) const {
    switch (kind) {
    case Kind::StandaloneFunction: {
      return SpecializedEmitter::forDecl(SGM, Constant);
    }
    case Kind::EnumElement:
    case Kind::IndirectValue:
    case Kind::ClassMethod:
    case Kind::SuperMethod:
    case Kind::WitnessMethod:
    case Kind::DynamicMethod:
    case Kind::StandaloneFunctionDynamicallyReplaceableImpl:
      return llvm::None;
    }
    llvm_unreachable("bad callee kind");
  }
};

} // end anonymous namespace

/// Is this a call to the dynamically replaced function inside of a
/// '@_dynamicReplacement(for:)' function.
bool isCallToReplacedInDynamicReplacement(SILGenFunction &SGF,
                                          AbstractFunctionDecl *afd,
                                          bool &isObjCReplacementSelfCall) {
  if (auto *func =
          dyn_cast_or_null<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl())) {
    if (func->getDynamicallyReplacedDecl() == afd) {
      isObjCReplacementSelfCall = afd->isObjC();
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
//                           SILGenApply ASTVisitor
//===----------------------------------------------------------------------===//

/// For ObjC init methods, we generate a shared-linkage Swift allocating entry
/// point that does the [[T alloc] init] dance. We want to use this native
/// thunk where we expect to be calling an allocating entry point for an ObjC
/// constructor.
static bool isConstructorWithGeneratedAllocatorThunk(ValueDecl *vd) {
  return vd->isObjC() && isa<ConstructorDecl>(vd);
}

namespace {

/// An ASTVisitor for decomposing a nesting of ApplyExprs into an initial
/// Callee and a list of CallSites. The CallEmission class below uses these
/// to generate the actual SIL call.
///
/// Formally, an ApplyExpr in the AST always has a single argument, which may
/// be of tuple type, possibly empty. Also, some callees have a formal type
/// which is curried -- for example, methods have type Self -> Arg -> Result.
///
/// However, SIL functions take zero or more parameters and the natural entry
/// point of a method takes Self as an additional argument, rather than
/// returning a partial application.
///
/// Therefore, nested ApplyExprs applied to a constant are flattened into a
/// single call of the most uncurried entry point fitting the call site.
/// This avoids intermediate closure construction.
///
/// For example, a method reference 'self.method' decomposes into curry thunk
/// as the callee, with a single call site '(self)'.
///
/// On the other hand, a call of a method 'self.method(x)(y)' with a function
/// return type decomposes into the method's natural entry point as the callee,
/// and two call sites, first '(x, self)' then '(y)'.
class SILGenApply : public Lowering::ExprVisitor<SILGenApply> {
public:
  /// The SILGenFunction that we are emitting SIL into.
  SILGenFunction &SGF;

  /// The apply callee that abstractly represents the entry point that is being
  /// called.
  llvm::Optional<Callee> applyCallee;

  /// The lvalue or rvalue representing the argument source of self.
  ArgumentSource selfParam;

  SelfApplyExpr *selfApply = nullptr;
  ApplyExpr *callSite = nullptr;
  Expr *sideEffect = nullptr;

  SILGenApply(SILGenFunction &SGF)
    : SGF(SGF)
  {}

  void setCallee(Callee &&c) {
    assert(!applyCallee && "already set callee!");
    applyCallee.emplace(std::move(c));
  }

  void setSideEffect(Expr *sideEffectExpr) {
    assert(!sideEffect && "already set side effect!");
    sideEffect = sideEffectExpr;
  }

  void setSelfParam(ArgumentSource &&theSelfParam) {
    assert(!selfParam && "already set this!");
    selfParam = std::move(theSelfParam);
  }

  SelfApplyExpr *getAsMethodSelfApply(Expr *e) {
    auto *SAE = dyn_cast<SelfApplyExpr>(e);
    if (!SAE)
      return nullptr;
    if (isa<AutoClosureExpr>(SAE->getFn()))
      return nullptr;
    return SAE;
  }

  void decompose(ApplyExpr *e) {
    if (auto *SAE = getAsMethodSelfApply(e)) {
      selfApply = SAE;

      visit(selfApply->getFn());
      return;
    }

    callSite = e;

    if (auto *SAE = getAsMethodSelfApply(e->getFn())) {
      selfApply = SAE;

      if (selfApply->getBase()->isSuperExpr()) {
        applySuper(selfApply);
        return;
      }

      if (applyInitDelegation(selfApply))
        return;

      visit(selfApply->getFn());
      return;
    }

    visit(e->getFn());
  }

  /// Fall back to an unknown, indirect callee.
  void visitExpr(Expr *e) {
    // TODO: preserve the function pointer at its original abstraction level
    // when loading from memory.

    ManagedValue fn = SGF.emitRValueAsSingleValue(e);
    auto substType = cast<FunctionType>(e->getType()->getCanonicalType());
    auto origType = AbstractionPattern(substType);
    // When calling an C or block function, there's implicit bridging.
    origType = getIndirectApplyAbstractionPattern(SGF, origType, substType);

    setCallee(Callee::forIndirect(fn, origType, substType, e));
  }

  static constexpr unsigned metatypeRepPair(MetatypeRepresentation a,
                                            MetatypeRepresentation b) {
    return assert(unsigned(a) < 256 && unsigned(b) < 256
                  && "MetatypeRepresentation got too big for its britches"),
      unsigned(a) << 8 | unsigned(b);
  }

  /// Idempotently convert a metatype to a thick or objc metatype, depending
  /// on what allocation mechanism we need for a given class hierarchy.
  std::pair<ManagedValue, SILType>
  convertToMetatypeForAllocRefDynamic(ManagedValue selfMeta,
                                      SILLocation loc,
                                      bool usesObjCAllocation) {
    auto givenMetatype = selfMeta.getType().castTo<AnyMetatypeType>();
    CanType instanceType = givenMetatype.getInstanceType();

    auto destMetatypeRep = usesObjCAllocation
      ? MetatypeRepresentation::ObjC
      : MetatypeRepresentation::Thick;

    // If we are already the right rep, just return.
    auto givenMetatypeRep = givenMetatype->getRepresentation();
    if (givenMetatypeRep == destMetatypeRep) {
      return {selfMeta, SGF.getLoweredType(instanceType)};
    }

    CanAnyMetatypeType destMetatype;
    if (isa<MetatypeType>(givenMetatype)) {
      destMetatype =
          CanMetatypeType::get(instanceType, destMetatypeRep);
    } else {
      destMetatype = CanExistentialMetatypeType::get(instanceType,
                                                        destMetatypeRep);
    }
    // Metatypes are trivial and thus do not have a cleanup. Only if we
    // convert them to an object do they become non-trivial.
    assert(!selfMeta.hasCleanup());
    SILValue convertedValue;
    switch (metatypeRepPair(givenMetatypeRep, destMetatypeRep)) {
    case metatypeRepPair(MetatypeRepresentation::Thick,
                         MetatypeRepresentation::ObjC):
      convertedValue = SGF.B.emitThickToObjCMetatype(
        loc, selfMeta.getValue(),
        SILType::getPrimitiveObjectType(destMetatype));
      break;
    
    case metatypeRepPair(MetatypeRepresentation::ObjC,
                         MetatypeRepresentation::Thick):
      convertedValue = SGF.B.emitObjCToThickMetatype(
        loc, selfMeta.getValue(),
        SILType::getPrimitiveObjectType(destMetatype));
      break;

    default:
      llvm_unreachable("shouldn't happen");
    }

    auto result = ManagedValue::forUnmanaged(convertedValue);
    return {result, SGF.getLoweredType(instanceType)};
  }

  /// Given a metatype value for the type, allocate an Objective-C
  /// object (with alloc_ref_dynamic) of that type.
  ///
  /// \returns the self object.
  ManagedValue allocateObject(ManagedValue selfMeta,
                              SILLocation loc,
                              bool usesObjCAllocation) {
    // Convert to the necessary metatype representation, if needed.
    ManagedValue selfMetaConverted;
    SILType instanceType;
    std::tie(selfMetaConverted, instanceType) =
       convertToMetatypeForAllocRefDynamic(selfMeta, loc, usesObjCAllocation);

    // Allocate the object.
    return SGF.B.createAllocRefDynamic(loc, selfMetaConverted, instanceType,
                                       usesObjCAllocation, {}, {});
  }

  void processProtocolMethod(DeclRefExpr *e, AbstractFunctionDecl *afd,
                             ProtocolDecl *proto) {
    ArgumentSource selfValue = selfApply->getBase();

    auto subs = e->getDeclRef().getSubstitutions();

    SILDeclRef::Kind kind = SILDeclRef::Kind::Func;
    if (isa<ConstructorDecl>(afd)) {
      if (proto->isObjC()) {
        SILLocation loc = selfApply->getBase();

        // For Objective-C initializers, we only have an initializing
        // initializer. We need to allocate the object ourselves.
        kind = SILDeclRef::Kind::Initializer;

        auto metatype = std::move(selfValue).getAsSingleValue(SGF);
        auto allocated = allocateObject(metatype, loc, /*objc*/ true);
        auto allocatedType = allocated.getType().getASTType();
        selfValue =
            ArgumentSource(loc, RValue(SGF, loc, allocatedType, allocated));
      } else {
        // For non-Objective-C initializers, we have an allocating
        // initializer to call.
        kind = SILDeclRef::Kind::Allocator;
      }
    }

    SILDeclRef constant(afd, kind);
    constant = constant.asForeign(afd->isObjC());

    // Prepare the callee.
    Callee theCallee = Callee::forWitnessMethod(
        SGF, selfValue.getSubstRValueType(),
        constant, subs, e);

    setSelfParam(std::move(selfValue));
    setCallee(std::move(theCallee));
  }

  bool isClassMethod(DeclRefExpr *e, AbstractFunctionDecl *afd) {
    if (e->getAccessSemantics() != AccessSemantics::Ordinary)
      return false;

    if (getMethodDispatch(afd) == MethodDispatch::Static)
      return false;

    if (auto ctor = dyn_cast<ConstructorDecl>(afd)) {
      // Non-required initializers are statically dispatched.
      if (!ctor->isRequired())
        return false;

      // @objc dynamic initializers are statically dispatched (we're
      // calling the allocating entry point, which is a thunk that
      // does the dynamic dispatch for us).
      if (ctor->shouldUseObjCDispatch())
        return false;

      // Required constructors are statically dispatched when the 'self'
      // value is statically derived.
      assert(selfApply->getBase()->getType()->is<AnyMetatypeType>());
      if (selfApply->getBase()->isStaticallyDerivedMetatype())
        return false;
    }

    // Ok, we're dynamically dispatched.
    return true;
  }

  void processClassMethod(DeclRefExpr *e, AbstractFunctionDecl *afd) {
    assert(!afd->hasBackDeployedAttr() &&
           "cannot back deploy dynamically dispatched methods");

    ArgumentSource selfArgSource(selfApply->getBase());
    setSelfParam(std::move(selfArgSource));

    // Directly dispatch to calls of the replaced function inside of
    // '@_dynamicReplacement(for:)' methods.
    bool isObjCReplacementCall = false;
    if (SGF.getOptions()
            .EnableDynamicReplacementCanCallPreviousImplementation &&
        isCallToReplacedInDynamicReplacement(SGF, afd, isObjCReplacementCall) &&
        selfApply->getBase()->isSelfExprOf(
            cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()), false)) {
      auto constant = SILDeclRef(afd).asForeign(
          !isObjCReplacementCall && requiresForeignEntryPoint(e->getDecl()));
      auto subs = e->getDeclRef().getSubstitutions();
      if (isObjCReplacementCall)
        setCallee(Callee::forDirect(SGF, constant, subs, e));
      else
        setCallee(Callee::forDirect(
            SGF,
            SILDeclRef(cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl())),
            subs, e, true));
      return;
    }

    SILDeclRef constant = SILDeclRef(afd);
    if (auto distributedThunk = afd->getDistributedThunk()) {
      constant = SILDeclRef(distributedThunk).asDistributed();
    } else {
      constant = constant.asForeign(requiresForeignEntryPoint(afd));
    }

    auto subs = e->getDeclRef().getSubstitutions();

    bool isObjCDirect = false;
    if (auto objcDecl = dyn_cast_or_null<clang::ObjCMethodDecl>(
            afd->getClangDecl())) {
      isObjCDirect = objcDecl->isDirectMethod();
    }

    // Methods on unsafe foreign objects are always called directly.
    bool isUFO = isa_and_nonnull<ClassDecl>(afd->getDeclContext()) &&
        cast<ClassDecl>(afd->getDeclContext())->isForeignReferenceType();
    if (isObjCDirect || isUFO) {
      setCallee(Callee::forDirect(SGF, constant, subs, e));
    } else {
      setCallee(Callee::forClassMethod(SGF, constant, subs, e));
    }
  }

  SILDeclRef getDeclRefForStaticDispatchApply(DeclRefExpr *e) {
    auto *afd = cast<AbstractFunctionDecl>(e->getDecl());
    auto &ctx = SGF.getASTContext();

    // A call to a `distributed` function may need to go through a thunk.
    if (callSite && callSite->shouldApplyDistributedThunk()) {
      if (auto distributedThunk = afd->getDistributedThunk())
        return SILDeclRef(distributedThunk).asDistributed();
    }

    // A call to `@backDeployed` function may need to go through a thunk.
    if (shouldApplyBackDeploymentThunk(afd, ctx,
                                       SGF.F.getResilienceExpansion())) {
      return SILDeclRef(afd).asBackDeploymentKind(
          SILDeclRef::BackDeploymentKind::Thunk);
    }

    return SILDeclRef(afd).asForeign(
        !isConstructorWithGeneratedAllocatorThunk(afd) &&
        requiresForeignEntryPoint(afd));
  }

  //
  // Known callees.
  //
  void visitDeclRefExpr(DeclRefExpr *e) {
    auto subs = e->getDeclRef().getSubstitutions();

    // If this is a direct reference to a vardecl, just emit its value directly.
    // Recursive references to callable declarations are allowed.
    if (isa<VarDecl>(e->getDecl())) {
      visitExpr(e);
      return;
    }

    // Enum case constructor references are open-coded.
    if (auto *eed = dyn_cast<EnumElementDecl>(e->getDecl())) {
      if (selfApply) {
        ArgumentSource selfArgSource(selfApply->getBase());
        setSelfParam(std::move(selfArgSource));
      }

      setCallee(Callee::forEnumElement(SGF, SILDeclRef(eed), subs, e));
      return;
    }

    // Ok, we have a constructor or a function.
    auto *afd = cast<AbstractFunctionDecl>(e->getDecl());

    // Witness method or @objc protocol dispatch.
    if (auto *proto = dyn_cast<ProtocolDecl>(afd->getDeclContext())) {
      processProtocolMethod(e, afd, proto);
      return;
    }

    // VTable class method or @objc class method dispatch.
    if (isClassMethod(e, afd)) {
      processClassMethod(e, afd);
      return;
    }

    // Otherwise, we have a statically-dispatched call.
    SILDeclRef constant = getDeclRefForStaticDispatchApply(e);
    auto captureInfo = SGF.SGM.Types.getLoweredLocalCaptures(constant);
    SGF.SGM.Types.setCaptureTypeExpansionContext(constant, SGF.SGM.M);
    
    if (afd->getDeclContext()->isLocalContext() &&
        !captureInfo.hasGenericParamCaptures())
      subs = SubstitutionMap();

    // Check whether we have to dispatch to the original implementation of a
    // dynamically_replaceable inside of a dynamic_replacement(for:) function.
    ApplyExpr *thisCallSite = (selfApply ? selfApply : callSite);
    bool isObjCReplacementSelfCall = false;
    auto *unaryArg = thisCallSite->getArgs()->getUnaryExpr();
    bool isSelfCallToReplacedInDynamicReplacement =
        SGF.getOptions()
            .EnableDynamicReplacementCanCallPreviousImplementation &&
        isCallToReplacedInDynamicReplacement(
            SGF, cast<AbstractFunctionDecl>(constant.getDecl()),
            isObjCReplacementSelfCall) &&
        (afd->getDeclContext()->isModuleScopeContext() ||
         (unaryArg && unaryArg->isSelfExprOf(
             cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()), false)));

    if (isSelfCallToReplacedInDynamicReplacement && !isObjCReplacementSelfCall)
      setCallee(Callee::forDirect(
          SGF,
          SILDeclRef(cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()),
                     constant.kind),
          subs, e, true));
    else
      setCallee(Callee::forDirect(SGF, constant, subs, e));

    if (selfApply) {
      // This is a statically-dispatched method with a 'self' parameter.
      ArgumentSource selfArgSource(selfApply->getBase());
      setSelfParam(std::move(selfArgSource));
    }

    // If the decl ref requires captures, emit the capture params.
    if (!captureInfo.getCaptures().empty()) {
      SmallVector<ManagedValue, 4> captures;
      SGF.emitCaptures(e, SILDeclRef(afd),
                       CaptureEmission::ImmediateApplication,
                       captures);
      applyCallee->setCaptures(std::move(captures));
    }
  }
  
  void visitMemberRefExpr(MemberRefExpr *e) {
    assert(isa<VarDecl>(e->getMember().getDecl()));

    // Any writebacks for this access are tightly scoped.
    FormalEvaluationScope scope(SGF);

    LValue lv = SGF.emitLValue(e, SGFAccessKind::OwnedObjectRead);
    if (lv.isLastComponentTranslation())
      lv.dropLastTranslationComponent();

    ManagedValue fn = SGF.emitLoadOfLValue(e, std::move(lv), SGFContext())
      .getAsSingleValue(SGF, e);
    auto substType = cast<FunctionType>(lv.getSubstFormalType());
    auto origType = lv.getOrigFormalType();
    // When calling an C or block function, there's implicit bridging.
    origType = getIndirectApplyAbstractionPattern(SGF, origType, substType);

    setCallee(Callee::forIndirect(fn, origType, substType, e));
  }
  
  void visitAbstractClosureExpr(AbstractClosureExpr *e) {
    SILDeclRef constant(e);

    SGF.SGM.Types.setCaptureTypeExpansionContext(constant, SGF.SGM.M);
    // Emit the closure body.
    SGF.SGM.emitClosure(e);

    // If we're in top-level code, we don't need to physically capture script
    // globals, but we still need to mark them as escaping so that DI can flag
    // uninitialized uses.
    if (&SGF == SGF.SGM.TopLevelSGF) {
      SGF.SGM.emitMarkFunctionEscapeForTopLevelCodeGlobals(e,e->getCaptureInfo());
    }
    
    // A directly-called closure can be emitted as a direct call instead of
    // really producing a closure object.

    auto captureInfo = SGF.SGM.M.Types.getLoweredLocalCaptures(constant);

    SubstitutionMap subs;
    if (captureInfo.hasGenericParamCaptures())
      subs = SGF.getForwardingSubstitutionMap();

    setCallee(Callee::forDirect(SGF, constant, subs, e));
    
    // If the closure requires captures, emit them.
    if (!captureInfo.getCaptures().empty()) {
      SmallVector<ManagedValue, 4> captures;
      SGF.emitCaptures(e, constant, CaptureEmission::ImmediateApplication,
                       captures);
      applyCallee->setCaptures(std::move(captures));
    }
  }
  
  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *e) {
    auto subs = e->getDeclRef().getSubstitutions();

    // FIXME: We might need to go through ObjC dispatch for references to
    // constructors imported from Clang (which won't have a direct entry point)
    // or to delegate to a designated initializer.
    setCallee(Callee::forDirect(SGF,
                SILDeclRef(e->getDecl(), SILDeclRef::Kind::Initializer),
                           subs, e));
  }

  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *e) {
    setSideEffect(e->getLHS());
    visit(e->getRHS());
  }

  void visitFunctionConversionExpr(FunctionConversionExpr *e) {
    // FIXME: Check whether this function conversion requires us to build a
    // thunk.
    visit(e->getSubExpr());
  }

  void visitCovariantFunctionConversionExpr(CovariantFunctionConversionExpr *e){
    // FIXME: These expressions merely adjust the result type for DynamicSelf
    // in an unchecked, ABI-compatible manner. They shouldn't prevent us form
    // forming a complete call.
    visitExpr(e);
  }

  void visitIdentityExpr(IdentityExpr *e) {
    visit(e->getSubExpr());
  }

  void applySuper(SelfApplyExpr *apply) {
    // Load the 'super' argument.
    Expr *arg = apply->getBase();
    RValue super;
    CanType superFormalType = arg->getType()->getCanonicalType();

    // The callee for a super call has to be either a method or constructor.
    // There might be one level of conversion in between.
    Expr *fn = apply->getFn();
    if (auto fnConv = dyn_cast<FunctionConversionExpr>(fn))
      fn = fnConv->getSubExpr();
    SubstitutionMap substitutions;
    SILDeclRef constant;
    if (auto *ctorRef = dyn_cast<OtherConstructorDeclRefExpr>(fn)) {
      constant = SILDeclRef(ctorRef->getDecl(), SILDeclRef::Kind::Initializer)
        .asForeign(requiresForeignEntryPoint(ctorRef->getDecl()));

      if (ctorRef->getDeclRef().isSpecialized())
        substitutions = ctorRef->getDeclRef().getSubstitutions();

      assert(SGF.SelfInitDelegationState ==
             SILGenFunction::WillSharedBorrowSelf);
      SGF.SelfInitDelegationState = SILGenFunction::WillExclusiveBorrowSelf;
      super = SGF.emitRValue(arg);
      assert(SGF.SelfInitDelegationState ==
             SILGenFunction::DidExclusiveBorrowSelf);

      // We know that we have a single ManagedValue rvalue for self.
      ManagedValue superMV = std::move(super).getScalarValue();

      // Check if super is not the same as our base type. This means that we
      // performed an upcast, and we must have consumed the special cleanup
      // we installed.  Install a new special cleanup.
      if (superMV.getValue() != SGF.InitDelegationSelf.getValue()) {
        SILValue underlyingSelf = SGF.InitDelegationSelf.getValue();
        SGF.InitDelegationSelf = ManagedValue::forUnmanaged(underlyingSelf);
        CleanupHandle newWriteback = SGF.enterOwnedValueWritebackCleanup(
            SGF.InitDelegationLoc.value(), SGF.InitDelegationSelfBox,
            superMV.forward(SGF));
        SGF.SuperInitDelegationSelf =
            ManagedValue(superMV.getValue(), newWriteback);
        super = RValue(SGF, SGF.InitDelegationLoc.value(), superFormalType,
                       SGF.SuperInitDelegationSelf);
      }

    } else if (auto *declRef = dyn_cast<DeclRefExpr>(fn)) {
      assert(isa<FuncDecl>(declRef->getDecl()) && "non-function super call?!");
      // FIXME(backDeploy): Handle calls to back deployed methods on super?
      constant = SILDeclRef(declRef->getDecl())
        .asForeign(requiresForeignEntryPoint(declRef->getDecl()));

      if (declRef->getDeclRef().isSpecialized())
        substitutions = declRef->getDeclRef().getSubstitutions();
      super = SGF.emitRValue(arg);
    } else {
      llvm_unreachable("invalid super callee");
    }

    assert(super.isComplete() && "At this point super should be a complete "
                                 "rvalue that is not in any special states");
    ArgumentSource superArgSource(arg, std::move(super));
    if (!canUseStaticDispatch(SGF, constant)) {
      // ObjC super calls require dynamic dispatch.
      setCallee(Callee::forSuperMethod(SGF, constant, substitutions, fn));
    } else {
      // Native Swift super calls to final methods are direct.
      setCallee(Callee::forDirect(SGF, constant, substitutions, fn));
    }

    setSelfParam(std::move(superArgSource));
  }

  /// Walk the given \c selfArg expression that produces the appropriate
  /// `self` for a call, applying the same transformations to the provided
  /// \c selfValue (which might be a metatype).
  ///
  /// This is used for initializer delegation, so it covers only the narrow
  /// subset of expressions used there.
  ManagedValue emitCorrespondingSelfValue(ManagedValue selfValue,
                                          Expr *selfArg) {
    SILLocation loc = selfArg;
    auto resultTy = selfArg->getType()->getCanonicalType();
    while (true) {
      // Handle archetype-to-super and derived-to-base upcasts.
      if (isa<ArchetypeToSuperExpr>(selfArg) ||
          isa<DerivedToBaseExpr>(selfArg)) {
        selfArg = cast<ImplicitConversionExpr>(selfArg)->getSubExpr();
        continue;
      }

      // Skip over loads.
      if (auto load = dyn_cast<LoadExpr>(selfArg)) {
        selfArg = load->getSubExpr();
        resultTy = resultTy->getRValueType()->getCanonicalType();
        continue;
      }

      // Skip over inout expressions.
      if (auto inout = dyn_cast<InOutExpr>(selfArg)) {
        selfArg = inout->getSubExpr();
        resultTy = resultTy->getInOutObjectType()->getCanonicalType();
        continue;
      }

      // Declaration references terminate the search.
      if (isa<DeclRefExpr>(selfArg))
        break;

      llvm_unreachable("unhandled conversion for metatype value");
    }
    assert(isa<DeclRefExpr>(selfArg) &&
           "unexpected expr kind in self argument of initializer delegation");
  
    // If the 'self' value is a metatype, update the target type
    // accordingly.
    SILType loweredResultTy;
    auto selfMetaTy = selfValue.getType().getAs<AnyMetatypeType>();
    if (selfMetaTy) {
      loweredResultTy = SILType::getPrimitiveObjectType(
        CanMetatypeType::get(resultTy, selfMetaTy->getRepresentation()));
    } else {
      loweredResultTy = SGF.getLoweredLoadableType(resultTy);
    }
    
    if (loweredResultTy != selfValue.getType()) {
      // Introduce dynamic Self if necessary. A class initializer receives
      // a metatype argument that's formally the non-dynamic base class type
      // (though always dynamically of Self type),
      // but when invoking a protocol initializer, we need to pass it as
      // dynamic Self.
      if (!selfValue.getType().getASTType()->hasDynamicSelfType()
          && loweredResultTy.getASTType()->hasDynamicSelfType()) {
        assert(selfMetaTy);
        selfValue = SGF.emitManagedRValueWithCleanup(
            SGF.B.createUncheckedReinterpretCast(loc, selfValue.forward(SGF),
                                                 loweredResultTy));
      } else {
        selfValue = SGF.emitManagedRValueWithCleanup(
            SGF.B.createUpcast(loc, selfValue.forward(SGF), loweredResultTy));
      }
    }
    return selfValue;
  }

  /// Try to emit the given application as initializer delegation.
  bool applyInitDelegation(SelfApplyExpr *expr) {
    // Dig out the constructor we're delegating to.
    Expr *fn = expr->getFn();
    auto ctorRef = dyn_cast<OtherConstructorDeclRefExpr>(
                     fn->getSemanticsProvidingExpr());
    if (!ctorRef)
      return false;

    // Determine whether we'll need to use an allocating constructor (vs. the
    // initializing constructor).
    auto nominal = ctorRef->getDecl()->getDeclContext()
                     ->getSelfNominalTypeDecl();
    bool useAllocatingCtor;

    // Value types only have allocating initializers.
    if (isa<StructDecl>(nominal) || isa<EnumDecl>(nominal))
      useAllocatingCtor = true;
    // Protocols only witness allocating initializers, except for @objc
    // protocols, which only witness initializing initializers.
    else if (auto proto = dyn_cast<ProtocolDecl>(nominal)) {
      useAllocatingCtor = !proto->isObjC();
    // Factory initializers are effectively "allocating" initializers with no
    // corresponding initializing entry point.
    } else if (ctorRef->getDecl()->isFactoryInit()) {
       useAllocatingCtor = true;
    // If we're emitting a class initializer's non-allocating entry point and
    // delegating to an initializer exposed to Objective-C, use the initializing
    // entry point to avoid replacing an existing allocated object.
    } else if (!SGF.AllocatorMetatype && ctorRef->getDecl()->isObjC()) {
      useAllocatingCtor = false;
    // In general, though, class initializers self.init-delegate to each other
    // via their allocating entry points.
    } else {
      assert(isa<ClassDecl>(nominal)
             && "some new kind of init context we haven't implemented");
      useAllocatingCtor = !requiresForeignEntryPoint(ctorRef->getDecl());
    }

    // Load the 'self' argument.
    Expr *arg = expr->getBase();
    ManagedValue self;
    CanType selfFormalType = arg->getType()->getCanonicalType();

    // If we're using the allocating constructor, we need to pass along the
    // metatype.
    if (useAllocatingCtor) {
      selfFormalType = CanMetatypeType::get(
          selfFormalType->getInOutObjectType()->getCanonicalType());

      // If the initializer is a C function imported as a member,
      // there is no 'self' parameter. Mark it undef.
      if (ctorRef->getDecl()->isImportAsMember()) {
        self = SGF.emitUndef(selfFormalType);
      } else if (SGF.AllocatorMetatype) {
        self = emitCorrespondingSelfValue(
            ManagedValue::forUnmanaged(SGF.AllocatorMetatype), arg);
      } else {
        self = ManagedValue::forUnmanaged(SGF.emitMetatypeOfValue(expr, arg));
      }
    } else {
      // If we haven't allocated "self" yet at this point, do so.
      if (SGF.AllocatorMetatype) {
        bool usesObjCAllocation;
        if (auto clazz = dyn_cast<ClassDecl>(nominal)) {
          usesObjCAllocation = usesObjCAllocator(clazz);
        } else {
          // In the protocol extension case, we should only be here if the callee
          // initializer is @objc.
          usesObjCAllocation = true;
        }
        
        self = allocateObject(
                      ManagedValue::forUnmanaged(SGF.AllocatorMetatype), arg,
                      usesObjCAllocation);

        // Perform any adjustments needed to 'self'.
        self = emitCorrespondingSelfValue(self, arg);
      } else {
        assert(SGF.SelfInitDelegationState ==
               SILGenFunction::WillSharedBorrowSelf);
        SGF.SelfInitDelegationState = SILGenFunction::WillExclusiveBorrowSelf;
        self = SGF.emitRValueAsSingleValue(arg);
        assert(SGF.SelfInitDelegationState ==
               SILGenFunction::DidExclusiveBorrowSelf);
      }
    }

    auto subs = ctorRef->getDeclRef().getSubstitutions();
    ArgumentSource selfArgSource(arg, RValue(SGF, expr, selfFormalType, self));

    SILDeclRef constant(ctorRef->getDecl(),
                        useAllocatingCtor
                         ? SILDeclRef::Kind::Allocator
                         : SILDeclRef::Kind::Initializer);

    bool isObjCReplacementSelfCall = false;
    bool isSelfCallToReplacedInDynamicReplacement =
        SGF.getOptions()
            .EnableDynamicReplacementCanCallPreviousImplementation &&
        isCallToReplacedInDynamicReplacement(
            SGF, cast<AbstractFunctionDecl>(constant.getDecl()),
            isObjCReplacementSelfCall) &&
        arg->isSelfExprOf(
            cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()), false);

    if (!isObjCReplacementSelfCall) {
      if (useAllocatingCtor) {
        constant =
            constant.asForeign(requiresForeignEntryPoint(ctorRef->getDecl()));
      } else {
        // Note: if we ever implement delegating from one designated initializer
        // to another, this won't be correct; that should do a direct dispatch.
        constant = constant.asForeign(ctorRef->getDecl()->isObjC());
      }
    }

    // Determine the callee. This is normally the allocating
    // entry point, unless we're delegating to an ObjC initializer.
    if (isa<ProtocolDecl>(ctorRef->getDecl()->getDeclContext())) {
      // Look up the witness for the constructor.
      setCallee(Callee::forWitnessMethod(
          SGF, self.getType().getASTType(),
          constant, subs, expr));
    } else if ((useAllocatingCtor || constant.isForeign) &&
               !isSelfCallToReplacedInDynamicReplacement &&
               ((constant.isForeign && !useAllocatingCtor) ||
                getMethodDispatch(ctorRef->getDecl()) == MethodDispatch::Class)) {
      // Dynamic dispatch to the initializer.
      Scope S(SGF, expr);
      setCallee(Callee::forClassMethod(
          SGF, constant, subs, fn));
    } else {
      // Directly call the peer constructor.
      if (isObjCReplacementSelfCall ||
          !isSelfCallToReplacedInDynamicReplacement)
        setCallee(Callee::forDirect(SGF, constant, subs, fn));
      else
        setCallee(Callee::forDirect(
            SGF,
            SILDeclRef(cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()),
                       constant.kind),
            subs, fn, true));
    }

    setSelfParam(std::move(selfArgSource));

    return true;
  }

  Callee getCallee() {
    assert(applyCallee && "did not find callee?!");
    return std::move(*applyCallee);
  }

  /// \returns true if the conversion is from an async function to
  /// the same type but with a global actor added. For example this:
  ///     () async -> ()  ==>  @MainActor () async -> ()
  /// will return true. In all other cases, returns false.
  static bool addsGlobalActorToAsyncFn(FunctionConversionExpr *fce) {
    CanType oldTy = fce->getSubExpr()->getType()->getCanonicalType();
    CanType newTy = fce->getType()->getCanonicalType();

    if (auto oldFnTy = dyn_cast<AnyFunctionType>(oldTy)) {
      if (auto newFnTy = dyn_cast<AnyFunctionType>(newTy)) {
        // old type MUST be async
        if (!oldFnTy->hasEffect(EffectKind::Async))
          return false;

        // old type MUST NOT have a global actor
        if (oldFnTy->hasGlobalActor())
          return false;

        // new type MUST have a global actor
        if (!newFnTy->hasGlobalActor())
          return false;

        // see if adding the global actor to the old type yields the new type.
        auto globalActor = newFnTy->getGlobalActor();
        auto addedActor = oldFnTy->getExtInfo().withGlobalActor(globalActor);

        return oldFnTy->withExtInfo(addedActor) == newFnTy;
      }
    }

    return false;
  }

  /// Ignore parentheses and implicit conversions.
  static Expr *ignoreParensAndImpConversions(Expr *expr) {
    while (true) {
      if (auto ice = dyn_cast<ImplicitConversionExpr>(expr)) {
        expr = ice->getSubExpr();
        continue;
      }

      // Simple optional-to-optional conversions.  This doesn't work
      // for the full generality of OptionalEvaluationExpr, but it
      // works given that we check the result for certain forms.
      if (auto eval = dyn_cast<OptionalEvaluationExpr>(expr)) {
        if (auto inject = dyn_cast<InjectIntoOptionalExpr>(eval->getSubExpr())) {

          auto nextSubExpr = inject->getSubExpr();

          // skip over a specific, known no-op function conversion, if it exists
          if (auto funcConv = dyn_cast<FunctionConversionExpr>(nextSubExpr)) {
            if (addsGlobalActorToAsyncFn(funcConv))
              nextSubExpr = funcConv->getSubExpr();
          }

          if (auto bind = dyn_cast<BindOptionalExpr>(nextSubExpr)) {
            if (bind->getDepth() == 0)
              return bind->getSubExpr();
          }
        }
      }

      auto valueProviding = expr->getValueProvidingExpr();
      if (valueProviding != expr) {
        expr = valueProviding;
        continue;
      }

      return expr;
    }
  }

  void visitForceValueExpr(ForceValueExpr *e) {
    // If this application is a dynamic member reference that is forced to
    // succeed with the '!' operator, emit it as a direct invocation of the
    // method we found.
    if (emitForcedDynamicMemberRef(e))
      return;

    visitExpr(e);
  }

  /// If this application forces a dynamic member reference with !, emit
  /// a direct reference to the member.
  bool emitForcedDynamicMemberRef(ForceValueExpr *e) {
    // Check whether the argument is a dynamic member reference.
    auto arg = ignoreParensAndImpConversions(e->getSubExpr());

    auto openExistential = dyn_cast<OpenExistentialExpr>(arg);
    if (openExistential)
      arg = openExistential->getSubExpr();

    auto dynamicMemberRef = dyn_cast<DynamicMemberRefExpr>(arg);
    if (!dynamicMemberRef)
      return false;

    // Since we'll be collapsing this call site, make sure there's another
    // call site that will actually perform the invocation.
    if (callSite == nullptr)
      return false;

    // Only @objc methods can be forced.
    auto memberRef = dynamicMemberRef->getMember();
    auto *fd = dyn_cast<FuncDecl>(memberRef.getDecl());
    if (!fd || !fd->isObjC())
      return false;

    FormalEvaluationScope writebackScope(SGF);

    // Local function that actually emits the dynamic member reference.
    auto emitDynamicMemberRef = [&] {
      // We found it. Emit the base.
      ArgumentSource baseArgSource(dynamicMemberRef->getBase(),
                                   SGF.emitRValue(dynamicMemberRef->getBase()));

      // Determine the type of the method we referenced, by replacing the
      // class type of the 'Self' parameter with AnyObject.
      auto member = SILDeclRef(fd).asForeign();

      auto substFormalType = cast<FunctionType>(dynamicMemberRef->getType()
                                                    ->getCanonicalType()
                                                    .getOptionalObjectType());
      auto substSelfType = dynamicMemberRef->getBase()->getType()->getCanonicalType();
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      CanFunctionType::ExtInfo info;
      substFormalType = CanFunctionType::get(
          {AnyFunctionType::Param(substSelfType)}, substFormalType, info);

      setCallee(Callee::forDynamic(SGF, member,
                                   memberRef.getSubstitutions(),
                                   substFormalType, {}, e));
      setSelfParam(std::move(baseArgSource));
    };

    // When we have an open existential, open it and then emit the
    // member reference.
    if (openExistential) {
      SGF.emitOpenExistentialExpr(openExistential,
                                  [&](Expr*) { emitDynamicMemberRef(); });
    } else {
      emitDynamicMemberRef();
    }
    return true;
  }
};

} // end anonymous namespace

// TODO: move onto SGF directly and reuse in SILGenDistributed and other places
static PreparedArguments emitStringLiteralArgs(SILGenFunction &SGF, SILLocation E,
                                           StringRef Str, SGFContext C,
                                        StringLiteralExpr::Encoding encoding) {
  uint64_t Length;
  bool isASCII = SGF.getASTContext().isASCIIString(Str);
  StringLiteralInst::Encoding instEncoding;
  switch (encoding) {
  case StringLiteralExpr::UTF8:
    instEncoding = StringLiteralInst::Encoding::UTF8;
    Length = Str.size();
    break;

  case StringLiteralExpr::OneUnicodeScalar: {
    SILType Int32Ty = SILType::getBuiltinIntegerType(32, SGF.getASTContext());
    SILValue UnicodeScalarValue =
        SGF.B.createIntegerLiteral(E, Int32Ty,
                                   unicode::extractFirstUnicodeScalar(Str));

    AnyFunctionType::Param param(Int32Ty.getASTType());
    PreparedArguments args(llvm::ArrayRef<AnyFunctionType::Param>{param});
    args.add(E, RValue(SGF, E, Int32Ty.getASTType(),
                       ManagedValue::forUnmanaged(UnicodeScalarValue)));
    return args;
  }
  }

  // The string literal provides the data.
  auto *string = SGF.B.createStringLiteral(E, Str, instEncoding);

  // The length is lowered as an integer_literal.
  auto WordTy = SILType::getBuiltinWordType(SGF.getASTContext());
  auto *lengthInst = SGF.B.createIntegerLiteral(E, WordTy, Length);

  // The 'isascii' bit is lowered as an integer_literal.
  auto Int1Ty = SILType::getBuiltinIntegerType(1, SGF.getASTContext());
  auto *isASCIIInst = SGF.B.createIntegerLiteral(E, Int1Ty, isASCII);

  ManagedValue EltsArray[] = {
    ManagedValue::forUnmanaged(string),
    ManagedValue::forUnmanaged(lengthInst),
    ManagedValue::forUnmanaged(isASCIIInst)
  };

  AnyFunctionType::Param TypeEltsArray[] = {
    AnyFunctionType::Param(EltsArray[0].getType().getASTType()),
    AnyFunctionType::Param(EltsArray[1].getType().getASTType()),
    AnyFunctionType::Param(EltsArray[2].getType().getASTType())
  };

  ArrayRef<ManagedValue> Elts;
  ArrayRef<AnyFunctionType::Param> TypeElts;
  switch (instEncoding) {
  case StringLiteralInst::Encoding::UTF8:
    Elts = EltsArray;
    TypeElts = TypeEltsArray;
    break;

  case StringLiteralInst::Encoding::Bytes:
  case StringLiteralInst::Encoding::ObjCSelector:
    llvm_unreachable("these cannot be formed here");
  }

  PreparedArguments args(TypeElts);
  for (unsigned i = 0, e = Elts.size(); i != e; ++i) {
    args.add(E, RValue(SGF, Elts[i], CanType(TypeElts[i].getPlainType())));
  }
  return args;
}

/// Emit a raw apply operation, performing no additional lowering of
/// either the arguments or the result.
static void emitRawApply(SILGenFunction &SGF,
                         SILLocation loc,
                         ManagedValue fn,
                         SubstitutionMap subs,
                         ArrayRef<ManagedValue> args,
                         CanSILFunctionType substFnType,
                         ApplyOptions options,
                         ArrayRef<SILValue> indirectResultAddrs,
                         SmallVectorImpl<SILValue> &rawResults,
                         ExecutorBreadcrumb prevExecutor) {
  SILFunctionConventions substFnConv(substFnType, SGF.SGM.M);
  // Get the callee value.
  bool isConsumed = substFnType->isCalleeConsumed();
  bool isUnowned = substFnType->isCalleeUnowned();
  SILValue fnValue =
      isUnowned ? fn.getValue()
                : isConsumed ? fn.forward(SGF)
                             : fn.formalAccessBorrow(SGF, loc).getValue();

  SmallVector<SILValue, 4> argValues;

  // Add the buffers for the indirect results if needed.
#ifndef NDEBUG
  assert(indirectResultAddrs.size() == substFnConv.getNumIndirectSILResults());
  unsigned resultIdx = 0;
  for (auto indResultTy :
       substFnConv.getIndirectSILResultTypes(SGF.getTypeExpansionContext())) {
    assert(indResultTy == indirectResultAddrs[resultIdx++]->getType());
  }
#endif
  argValues.append(indirectResultAddrs.begin(), indirectResultAddrs.end());

  auto inputParams = substFnType->getParameters();
  assert(inputParams.size() == args.size());

  // Gather the arguments.
  for (auto i : indices(args)) {
    SILValue argValue;
    auto inputTy =
        substFnConv.getSILType(inputParams[i], SGF.getTypeExpansionContext());

    if (inputParams[i].isConsumed()) {
      argValue = args[i].forward(SGF);
      if (argValue->getType().isMoveOnlyWrapped() &&
          !inputTy.isMoveOnlyWrapped()) {
        if (argValue->getType().isObject())
          argValue =
              SGF.B.createOwnedMoveOnlyWrapperToCopyableValue(loc, argValue);
        else
          argValue = SGF.B.createMoveOnlyWrapperToCopyableAddr(loc, argValue);
      }
    } else {
      ManagedValue arg = args[i];

      // Move only is not represented in the Swift level type system, so if we
      // have a move only value, convert it to a non-move only value. The
      // move/is no escape checkers will ensure that it is legal to do this or
      // will error. At this point we just want to make sure that the emitted
      // types line up.
      if (arg.getType().isMoveOnlyWrapped()) {
        if (!inputTy.isMoveOnlyWrapped()) {
          // We need to borrow so that we can convert from $@moveOnly T -> $T.
          // Use a formal access borrow to ensure that we have tight scopes like
          // we do when we borrow fn.
          if (arg.getType().isObject()) {
            if (!arg.isPlusZero() ||
                arg.getOwnershipKind() != OwnershipKind::Guaranteed)
              arg = arg.formalAccessBorrow(SGF, loc);

            arg =
                SGF.B.createGuaranteedMoveOnlyWrapperToCopyableValue(loc, arg);
          } else {
            arg = ManagedValue::forBorrowedAddressRValue(
                SGF.B.createMoveOnlyWrapperToCopyableAddr(loc, arg.getValue()));
          }
        }
      }

      argValue = arg.getValue();
    }

#ifndef NDEBUG
    if (argValue->getType() != inputTy) {
      auto &out = llvm::errs();
      out << "TYPE MISMATCH IN ARGUMENT " << i << " OF APPLY AT ";
      printSILLocationDescription(out, loc, SGF.getASTContext());
      out << "  argument value: ";
      argValue->print(out);
      out << "  argument type: ";
      argValue->getType().print(out);
      out << "\n";
      out << "  parameter type: ";
      inputTy.print(out);
      out << "\n";
      abort();
    }
#endif
    argValues.push_back(argValue);
  }

  auto resultType = substFnConv.getSILResultType(SGF.getTypeExpansionContext());

  // If the function is a coroutine, we need to use 'begin_apply'.
  if (substFnType->isCoroutine()) {
    assert(!substFnType->hasErrorResult());
    auto apply = SGF.B.createBeginApply(loc, fnValue, subs, argValues);
    for (auto result : apply->getAllResults())
      rawResults.push_back(result);
    return;
  }

  if (!substFnType->isAsync())
    options -= ApplyFlags::DoesNotAwait;
  if (!substFnType->hasErrorResult())
    options -= ApplyFlags::DoesNotThrow;

  // If we don't have an error result, we can make a simple 'apply'.
  if (!substFnType->hasErrorResult()) {
    auto result = SGF.B.createApply(loc, fnValue, subs, argValues, options);
    rawResults.push_back(result);

  // Otherwise, we need to create a try_apply.
  } else {
    SILBasicBlock *normalBB = SGF.createBasicBlock();
    auto result = normalBB->createPhiArgument(resultType, OwnershipKind::Owned);
    rawResults.push_back(result);

    SILBasicBlock *errorBB =
      SGF.getTryApplyErrorDest(loc, substFnType, prevExecutor,
                               substFnType->getErrorResult(),
                               options.contains(ApplyFlags::DoesNotThrow));

    options -= ApplyFlags::DoesNotThrow;
    SGF.B.createTryApply(loc, fnValue, subs, argValues,
                         normalBB, errorBB, options);
    SGF.B.emitBlock(normalBB);
  }
}

static bool hasUnownedInnerPointerResult(CanSILFunctionType fnType) {
  for (auto result : fnType->getResults()) {
    if (result.getConvention() == ResultConvention::UnownedInnerPointer)
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
//                  Argument Emission for Builtin Initializer
//===----------------------------------------------------------------------===//
static inline PreparedArguments
buildBuiltinLiteralArgs(SILGenFunction &SGF, SGFContext C,
                        StringLiteralExpr *stringLiteral) {
  return emitStringLiteralArgs(SGF, stringLiteral, stringLiteral->getValue(), C,
                           stringLiteral->getEncoding());
}

static inline PreparedArguments
buildBuiltinLiteralArgs(SILGenFunction &SGF, SGFContext C,
                        NilLiteralExpr *nilLiteral) {
  CanType ty = SGF.getASTContext().TheEmptyTupleType;
  PreparedArguments builtinLiteralArgs;
  builtinLiteralArgs.emplace(AnyFunctionType::Param(ty));
  builtinLiteralArgs.add(nilLiteral, RValue(SGF, {}, ty));
  return builtinLiteralArgs;
}

static inline PreparedArguments
buildBuiltinLiteralArgs(SILGenFunction &SGF, SGFContext C,
                        BooleanLiteralExpr *booleanLiteral) {
  PreparedArguments builtinLiteralArgs;
  auto i1Ty = SILType::getBuiltinIntegerType(1, SGF.getASTContext());
  SILValue boolValue = SGF.B.createIntegerLiteral(booleanLiteral, i1Ty,
                                                  booleanLiteral->getValue());
  ManagedValue boolManaged = ManagedValue::forUnmanaged(boolValue);
  CanType ty = boolManaged.getType().getASTType()->getCanonicalType();
  builtinLiteralArgs.emplace(AnyFunctionType::Param(ty));
  builtinLiteralArgs.add(booleanLiteral, RValue(SGF, {boolManaged}, ty));
  return builtinLiteralArgs;
}

static inline PreparedArguments
buildBuiltinLiteralArgs(SILGenFunction &SGF, SGFContext C,
                        IntegerLiteralExpr *integerLiteral) {
  PreparedArguments builtinLiteralArgs;
  ManagedValue integerManaged =
      ManagedValue::forUnmanaged(SGF.B.createIntegerLiteral(
          integerLiteral,
          SILType::getBuiltinIntegerLiteralType(SGF.getASTContext()),
          integerLiteral->getRawValue()));
  CanType ty = integerManaged.getType().getASTType();
  builtinLiteralArgs.emplace(AnyFunctionType::Param(ty));
  builtinLiteralArgs.add(integerLiteral, RValue(SGF, {integerManaged}, ty));
  return builtinLiteralArgs;
}

static inline PreparedArguments
buildBuiltinLiteralArgs(SILGenFunction &SGF, SGFContext C,
                        FloatLiteralExpr *floatLiteral) {
  PreparedArguments builtinLiteralArgs;
  auto *litTy = floatLiteral->getBuiltinType()->castTo<BuiltinFloatType>();
  ManagedValue floatManaged =
      ManagedValue::forUnmanaged(SGF.B.createFloatLiteral(
          floatLiteral,
          SILType::getBuiltinFloatType(litTy->getFPKind(), SGF.getASTContext()),
          floatLiteral->getValue()));

  CanType ty = floatManaged.getType().getASTType();
  builtinLiteralArgs.emplace(AnyFunctionType::Param(ty));
  builtinLiteralArgs.add(floatLiteral, RValue(SGF, {floatManaged}, ty));
  return builtinLiteralArgs;
}

static inline PreparedArguments
buildBuiltinLiteralArgs(SILGenFunction &SGF, SGFContext C,
                        RegexLiteralExpr *expr) {
  auto &ctx = SGF.getASTContext();
  // %0 = string_literal <regex text>
  auto strLiteralArgs = emitStringLiteralArgs(SGF, expr, expr->getRegexText(), C,
                                          StringLiteralExpr::Encoding::UTF8);
  // %1 = function_ref String.init(
  //   _builtinStringLiteral:utf8CodeUnitCount:isASCII:)
  // %2 = apply %1(%0, ..., ...) -> $String
  auto strInitDecl = ctx.getStringBuiltinInitDecl(ctx.getStringDecl());
  RValue string = SGF.emitApplyAllocatingInitializer(
      expr, strInitDecl, std::move(strLiteralArgs),
      /*overriddenSelfType*/ Type(), SGFContext());

  // The version of the regex string.
  // %3 = integer_literal $Builtin.IntLiteral <version>
  auto versionIntLiteral =
      ManagedValue::forUnmanaged(SGF.B.createIntegerLiteral(
          expr, SILType::getBuiltinIntegerLiteralType(SGF.getASTContext()),
          expr->getVersion()));

  using Param = AnyFunctionType::Param;
  auto builtinIntTy = versionIntLiteral.getType().getASTType();
  PreparedArguments versionIntBuiltinArgs(ArrayRef<Param>{Param(builtinIntTy)});
  versionIntBuiltinArgs.add(
      expr, RValue(SGF, {versionIntLiteral}, builtinIntTy));

  // %4 = function_ref Int.init(_builtinIntegerLiteral: Builtin.IntLiteral)
  // %5 = apply %5(%3, ...) -> $Int
  auto intLiteralInit = ctx.getIntBuiltinInitDecl(ctx.getIntDecl());
  RValue versionInt = SGF.emitApplyAllocatingInitializer(
      expr, intLiteralInit, std::move(versionIntBuiltinArgs),
      /*overriddenSelfType*/ Type(), SGFContext());

  PreparedArguments args(ArrayRef<Param>{Param(ctx.getStringType()),
                                         Param(ctx.getIntType())});
  args.add(expr, std::move(string));
  args.add(expr, std::move(versionInt));
  return args;
}

static inline PreparedArguments
buildBuiltinLiteralArgs(SILGenFunction &SGF, SGFContext C,
                        MagicIdentifierLiteralExpr *magicLiteral) {
  ASTContext &ctx = SGF.getASTContext();
  SourceLoc loc = magicLiteral->getStartLoc();

  switch (magicLiteral->getKind()) {
  case MagicIdentifierLiteralExpr::FileIDSpelledAsFile:
  case MagicIdentifierLiteralExpr::FileID: {
    std::string value = loc.isValid() ? SGF.getMagicFileIDString(loc) : "";
    return emitStringLiteralArgs(SGF, magicLiteral, value, C,
                             magicLiteral->getStringEncoding());
  }

  case MagicIdentifierLiteralExpr::FilePathSpelledAsFile:
  case MagicIdentifierLiteralExpr::FilePath: {
    StringRef value = loc.isValid() ? SGF.getMagicFilePathString(loc) : "";
    return emitStringLiteralArgs(SGF, magicLiteral, value, C,
                             magicLiteral->getStringEncoding());
  }

  case MagicIdentifierLiteralExpr::Function: {
    StringRef value = loc.isValid() ? SGF.getMagicFunctionString() : "";
    return emitStringLiteralArgs(SGF, magicLiteral, value, C,
                             magicLiteral->getStringEncoding());
  }

  case MagicIdentifierLiteralExpr::Line:
  case MagicIdentifierLiteralExpr::Column: {
    SourceLoc Loc = magicLiteral->getStartLoc();
    unsigned Value = 0;
    if (Loc.isValid()) {
      Value = magicLiteral->getKind() == MagicIdentifierLiteralExpr::Line
                  ? ctx.SourceMgr.getPresumedLineAndColumnForLoc(Loc).first
                  : ctx.SourceMgr.getPresumedLineAndColumnForLoc(Loc).second;
    }

    auto silTy = SILType::getBuiltinIntegerLiteralType(ctx);
    auto ty = silTy.getASTType();
    SILValue integer = SGF.B.createIntegerLiteral(magicLiteral, silTy, Value);
    ManagedValue integerManaged = ManagedValue::forUnmanaged(integer);
    PreparedArguments builtinLiteralArgs;
    builtinLiteralArgs.emplace(AnyFunctionType::Param(ty));
    builtinLiteralArgs.add(magicLiteral, RValue(SGF, {integerManaged}, ty));
    return builtinLiteralArgs;
  }
  case MagicIdentifierLiteralExpr::DSOHandle:
    llvm_unreachable("handled elsewhere");
  }
  llvm_unreachable("covered switch");
}

static inline PreparedArguments buildBuiltinLiteralArgs(SILGenFunction &SGF,
                                                        SGFContext C,
                                                        LiteralExpr *literal) {
  if (auto stringLiteral = dyn_cast<StringLiteralExpr>(literal)) {
    return buildBuiltinLiteralArgs(SGF, C, stringLiteral);
  } else if (auto nilLiteral = dyn_cast<NilLiteralExpr>(literal)) {
    return buildBuiltinLiteralArgs(SGF, C, nilLiteral);
  } else if (auto booleanLiteral = dyn_cast<BooleanLiteralExpr>(literal)) {
    return buildBuiltinLiteralArgs(SGF, C, booleanLiteral);
  } else if (auto integerLiteral = dyn_cast<IntegerLiteralExpr>(literal)) {
    return buildBuiltinLiteralArgs(SGF, C, integerLiteral);
  } else if (auto floatLiteral = dyn_cast<FloatLiteralExpr>(literal)) {
    return buildBuiltinLiteralArgs(SGF, C, floatLiteral);
  } else if (auto regexLiteral = dyn_cast<RegexLiteralExpr>(literal)) {
    return buildBuiltinLiteralArgs(SGF, C, regexLiteral);
  } else {
    return buildBuiltinLiteralArgs(
        SGF, C, cast<MagicIdentifierLiteralExpr>(literal));
  }
}

ManagedValue SILGenFunction::emitStringLiteral(SILLocation loc,
                                           StringRef text,
                                           StringLiteralExpr::Encoding encoding,
                                           SGFContext ctx) {
  auto &C = getASTContext();

  // === Prepare the arguments
  auto args = emitStringLiteralArgs(*this, loc, text, ctx, encoding);

  // === Find the constructor
  auto strInitDecl = C.getStringBuiltinInitDecl(C.getStringDecl());

  RValue r = emitApplyAllocatingInitializer(loc, strInitDecl, std::move(args),
                                 /*overriddenSelfType*/ Type(), ctx);

  return std::move(r).getScalarValue();
}

//===----------------------------------------------------------------------===//
//                             Argument Emission
//===----------------------------------------------------------------------===//

/// Count the number of SILParameterInfos that are needed in order to
/// pass the given argument.
static unsigned getFlattenedValueCount(AbstractionPattern origType) {
  // The count is always 1 unless the original type is a tuple.
  if (!origType.isTuple())
    return 1;

  // Add up the elements.
  unsigned count = 0;
  for (auto elt : origType.getTupleElementTypes()) {
    // Expansion components turn into a single pack parameter.
    if (elt.isPackExpansion()) {
      count++;

    // Recursively expand scalar components.
    } else {
      count += getFlattenedValueCount(elt);
    }
  }
  return count;
}

/// Count the number of SILParameterInfos that are needed in order to
/// pass the given argument.
static unsigned getFlattenedValueCount(AbstractionPattern origType,
                                       ImportAsMemberStatus foreignSelf) {
  // C functions imported as static methods don't consume any real arguments.
  if (foreignSelf.isStatic())
    return 0;

  return getFlattenedValueCount(origType);
}

namespace {

/// The original argument expression for some sort of complex
/// argument emission.
class OriginalArgument {
  llvm::PointerIntPair<Expr*, 1, bool> ExprAndIsIndirect;

public:
  OriginalArgument() = default;
  OriginalArgument(Expr *expr, bool indirect)
    : ExprAndIsIndirect(expr, indirect) {}

  Expr *getExpr() const { return ExprAndIsIndirect.getPointer(); }
  bool isIndirect() const { return ExprAndIsIndirect.getInt(); }
};
  
/// A possibly-discontiguous slice of function parameters claimed by a
/// function application.
class ClaimedParamsRef {
public:
  static constexpr const unsigned NoSkip = (unsigned)-1;
private:
  ArrayRef<SILParameterInfo> Params;
  
  // The index of the param excluded from this range, if any, or ~0.
  unsigned SkipParamIndex;
  
  void checkParams() {
    // The parameters should already have been substituted into the caller's
    // context, and had the SILFunctionType substitutions applied, before we
    // queue them up to be claimed.
#ifndef NDEBUG
    for (auto param : Params) {
      assert(!param.getInterfaceType()->hasTypeParameter()
             && "params should be substituted into context");
    }
#endif
  }
  
  friend struct ParamLowering;
  explicit ClaimedParamsRef(ArrayRef<SILParameterInfo> params,
                            unsigned skip)
    : Params(params), SkipParamIndex(skip)
  {
    checkParams();
    // Eagerly chop a skipped parameter off either end.
    if (SkipParamIndex == 0) {
      Params = Params.slice(1);
      SkipParamIndex = NoSkip;
    }
    assert(!hasSkip() || SkipParamIndex < Params.size());
  }
  
  bool hasSkip() const {
    return SkipParamIndex != (unsigned)NoSkip;
  }
public:
  ClaimedParamsRef() : Params({}), SkipParamIndex(-1) {}
  explicit ClaimedParamsRef(ArrayRef<SILParameterInfo> params)
    : Params(params), SkipParamIndex(NoSkip)
  {
    checkParams();
  }
  
  struct iterator {
    using iterator_category = std::random_access_iterator_tag;
    using value_type = SILParameterInfo;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;    

    const SILParameterInfo *Base;
    unsigned I, SkipParamIndex;
    
    iterator(const SILParameterInfo *Base,
             unsigned I, unsigned SkipParamIndex)
      : Base(Base), I(I), SkipParamIndex(SkipParamIndex)
    {}
    
    iterator &operator++() {
      ++I;
      if (I == SkipParamIndex)
        ++I;
      return *this;
    }
    iterator operator++(int) {
      iterator old(*this);
      ++*this;
      return old;
    }
    iterator &operator--() {
      --I;
      if (I == SkipParamIndex)
        --I;
      return *this;
    }
    iterator operator--(int) {
      iterator old(*this);
      --*this;
      return old;
    }
    
    const SILParameterInfo &operator*() const {
      return Base[I];
    }
    const SILParameterInfo *operator->() const {
      return Base + I;
    }
    
    bool operator==(iterator other) const {
      return Base == other.Base && I == other.I
          && SkipParamIndex == other.SkipParamIndex;
    }
    
    bool operator!=(iterator other) const {
      return !(*this == other);
    }
    
    iterator operator+(std::ptrdiff_t distance) const {
      if (distance > 0)
        return goForward(distance);
      if (distance < 0)
        return goBackward(distance);
      return *this;
    }
    iterator operator-(std::ptrdiff_t distance) const {
      if (distance > 0)
        return goBackward(distance);
      if (distance < 0)
        return goForward(distance);
      return *this;
    }
    std::ptrdiff_t operator-(iterator other) const {
      assert(Base == other.Base && SkipParamIndex == other.SkipParamIndex);
      auto baseDistance = (std::ptrdiff_t)I - (std::ptrdiff_t)other.I;
      if (std::min(I, other.I) < SkipParamIndex &&
          std::max(I, other.I) > SkipParamIndex)
        return baseDistance - 1;
      return baseDistance;
    }
    
    iterator goBackward(unsigned distance) const {
      auto result = *this;
      if (I > SkipParamIndex && I <= SkipParamIndex + distance)
        result.I -= (distance + 1);
      result.I -= distance;
      return result;
    }
    
    iterator goForward(unsigned distance) const {
      auto result = *this;
      if (I < SkipParamIndex && I + distance >= SkipParamIndex)
        result.I += distance + 1;
      result.I += distance;
      return result;
    }
  };
  
  iterator begin() const {
    return iterator{Params.data(), 0, SkipParamIndex};
  }
  
  iterator end() const {
    return iterator{Params.data(), (unsigned)Params.size(), SkipParamIndex};
  }
  
  unsigned size() const {
    return Params.size() - (hasSkip() ? 1 : 0);
  }
  
  bool empty() const { return size() == 0; }
  
  SILParameterInfo front() const { return *begin(); }
  
  ClaimedParamsRef slice(unsigned start) const {
    if (start >= SkipParamIndex)
      return ClaimedParamsRef(Params.slice(start + 1), NoSkip);
    return ClaimedParamsRef(Params.slice(start),
                            hasSkip() ? SkipParamIndex - start : NoSkip);
  }
  ClaimedParamsRef slice(unsigned start, unsigned count) const {
    if (start >= SkipParamIndex)
      return ClaimedParamsRef(Params.slice(start + 1, count), NoSkip);
    unsigned newSkip = SkipParamIndex;
    if (hasSkip())
      newSkip -= start;
    
    if (newSkip < count)
      return ClaimedParamsRef(Params.slice(start, count+1), newSkip);
    return ClaimedParamsRef(Params.slice(start, count), NoSkip);
  }
};

/// A delayed argument.  Call arguments are evaluated in two phases:
/// a formal evaluation phase and a formal access phase.  The primary
/// example of this is an l-value that is passed by reference, where
/// the access to the l-value does not begin until the formal access
/// phase, but there are other examples, generally relating to pointer
/// conversions.
///
/// A DelayedArgument represents the part of evaluating an argument
/// that's been delayed until the formal access phase.
class DelayedArgument {
public:
  enum KindTy {
    /// This is a true inout argument.
    InOut,

    LastLVKindWithoutExtra = InOut,

    /// The l-value needs to be converted to a pointer type.
    LValueToPointer,

    /// An array l-value needs to be converted to a pointer type.
    LValueArrayToPointer,

    LastLVKind = LValueArrayToPointer,

    /// An array r-value needs to be converted to a pointer type.
    RValueArrayToPointer,

    /// A string r-value needs to be converted to a pointer type.
    RValueStringToPointer,

    /// A function conversion needs to occur.
    FunctionConversion,

    LastRVKind = FunctionConversion,

    /// This is an immutable borrow from an l-value.
    BorrowedLValue,

    /// A default argument that needs to be evaluated.
    DefaultArgument,

    /// This is a consume of an l-value. It acts like a BorrowedLValue, but we
    /// use a deinit access scope.
    ConsumedLValue,
  };

private:
  KindTy Kind;

  struct LValueStorage {
    LValue LV;
    SILLocation Loc;

    LValueStorage(LValue &&lv, SILLocation loc) : LV(std::move(lv)), Loc(loc) {}
  };
  struct RValueStorage {
    ManagedValue RV;

    RValueStorage(ManagedValue rv) : RV(rv) {}
  };
  struct DefaultArgumentStorage {
    SILLocation loc;
    ConcreteDeclRef defaultArgsOwner;
    unsigned destIndex;
    CanType resultType;
    AbstractionPattern origResultType;
    ClaimedParamsRef paramsToEmit;
    SILFunctionTypeRepresentation functionRepresentation;
    
    DefaultArgumentStorage(SILLocation loc,
                           ConcreteDeclRef defaultArgsOwner,
                           unsigned destIndex,
                           CanType resultType,
                           AbstractionPattern origResultType,
                           ClaimedParamsRef paramsToEmit,
                           SILFunctionTypeRepresentation functionRepresentation)
      : loc(loc), defaultArgsOwner(defaultArgsOwner), destIndex(destIndex),
        resultType(resultType), origResultType(origResultType),
        paramsToEmit(paramsToEmit),
        functionRepresentation(functionRepresentation)
    {}
  };
  struct BorrowedLValueStorage {
    LValue LV;
    SILLocation Loc;
    AbstractionPattern OrigParamType;
    ClaimedParamsRef ParamsToEmit;
  };

  struct ConsumedLValueStorage {
    LValue LV;
    SILLocation Loc;
    AbstractionPattern OrigParamType;
    ClaimedParamsRef ParamsToEmit;
  };

  using ValueMembers =
      ExternalUnionMembers<RValueStorage, LValueStorage, DefaultArgumentStorage,
                           BorrowedLValueStorage, ConsumedLValueStorage>;
  static ValueMembers::Index getValueMemberIndexForKind(KindTy kind) {
    switch (kind) {
    case InOut:
    case LValueToPointer:
    case LValueArrayToPointer:
      return ValueMembers::indexOf<LValueStorage>();
    case RValueArrayToPointer:
    case RValueStringToPointer:
    case FunctionConversion:
      return ValueMembers::indexOf<RValueStorage>();
    case DefaultArgument:
      return ValueMembers::indexOf<DefaultArgumentStorage>();
    case BorrowedLValue:
      return ValueMembers::indexOf<BorrowedLValueStorage>();
    case ConsumedLValue:
      return ValueMembers::indexOf<ConsumedLValueStorage>();
    }
    llvm_unreachable("bad kind");
  }

  /// Storage for either the l-value or the r-value.
  ExternalUnion<KindTy, ValueMembers, getValueMemberIndexForKind> Value;

  LValueStorage &LV() { return Value.get<LValueStorage>(Kind); }
  const LValueStorage &LV() const { return Value.get<LValueStorage>(Kind); }
  RValueStorage &RV() { return Value.get<RValueStorage>(Kind); }
  const RValueStorage &RV() const { return Value.get<RValueStorage>(Kind); }

  /// The original argument expression, which will be emitted down
  /// to the point from which the l-value or r-value was generated.
  OriginalArgument Original;

  using PointerAccessInfo = SILGenFunction::PointerAccessInfo;
  using ArrayAccessInfo = SILGenFunction::ArrayAccessInfo;
  
  using ExtraMembers =
    ExternalUnionMembers<void,
                         ArrayAccessInfo,
                         PointerAccessInfo>;
  static ExtraMembers::Index getExtraMemberIndexForKind(KindTy kind) {
    switch (kind) {
    case LValueToPointer:
      return ExtraMembers::indexOf<PointerAccessInfo>();
    case LValueArrayToPointer:
    case RValueArrayToPointer:
      return ExtraMembers::indexOf<ArrayAccessInfo>();
    default:
      return ExtraMembers::indexOf<void>();
    }
  }

  ExternalUnion<KindTy, ExtraMembers, getExtraMemberIndexForKind> Extra;

public:
  DelayedArgument(KindTy kind, LValue &&lv, SILLocation loc)
      : Kind(kind) {
    assert(kind <= LastLVKindWithoutExtra &&
           "this constructor should only be used for simple l-value kinds");
    Value.emplace<LValueStorage>(Kind, std::move(lv), loc);
  }

  DelayedArgument(KindTy kind, ManagedValue rv, OriginalArgument original)
      : Kind(kind), Original(original) {
    Value.emplace<RValueStorage>(Kind, rv);
  }

  DelayedArgument(SILGenFunction::PointerAccessInfo pointerInfo,
                  LValue &&lv, SILLocation loc, OriginalArgument original)
      : Kind(LValueToPointer), Original(original) {
    Value.emplace<LValueStorage>(Kind, std::move(lv), loc);
    Extra.emplace<PointerAccessInfo>(Kind, pointerInfo);
  }

  DelayedArgument(SILGenFunction::ArrayAccessInfo arrayInfo,
                  LValue &&lv, SILLocation loc, OriginalArgument original)
      : Kind(LValueArrayToPointer), Original(original) {
    Value.emplace<LValueStorage>(Kind, std::move(lv), loc);
    Extra.emplace<ArrayAccessInfo>(Kind, arrayInfo);
  }

  DelayedArgument(KindTy kind,
                  SILGenFunction::ArrayAccessInfo arrayInfo,
                  ManagedValue rv, OriginalArgument original)
      : Kind(kind), Original(original) {
    Value.emplace<RValueStorage>(Kind, rv);
    Extra.emplace<ArrayAccessInfo>(Kind, arrayInfo);
  }

  DelayedArgument(LValue &&lv, SILLocation loc,
                  AbstractionPattern origResultType, ClaimedParamsRef params,
                  bool isBorrowed = true)
      : Kind(isBorrowed ? BorrowedLValue : ConsumedLValue) {
    if (isBorrowed) {
      Value.emplaceAggregate<BorrowedLValueStorage>(Kind, std::move(lv), loc,
                                                    origResultType, params);
    } else {
      Value.emplaceAggregate<ConsumedLValueStorage>(Kind, std::move(lv), loc,
                                                    origResultType, params);
    }
  }

  DelayedArgument(SILLocation loc,
                  ConcreteDeclRef defaultArgsOwner,
                  unsigned destIndex,
                  CanType resultType,
                  AbstractionPattern origResultType,
                  ClaimedParamsRef params,
                  SILFunctionTypeRepresentation functionTypeRepresentation)
    : Kind(DefaultArgument) {
    Value.emplace<DefaultArgumentStorage>(Kind, loc, defaultArgsOwner,
                                          destIndex,
                                          resultType,
                                          origResultType, params,
                                          functionTypeRepresentation);
  }

  DelayedArgument(DelayedArgument &&other)
      : Kind(other.Kind), Original(other.Original) {
    Value.moveConstruct(Kind, std::move(other.Value));
    Extra.moveConstruct(Kind, std::move(other.Extra));
  }

  DelayedArgument &operator=(DelayedArgument &&other) {
    Value.moveAssign(Kind, other.Kind, std::move(other.Value));
    Extra.moveAssign(Kind, other.Kind, std::move(other.Extra));
    Kind = other.Kind;
    Original = other.Original;
    return *this;
  }

  ~DelayedArgument() {
    Extra.destruct(Kind);
    Value.destruct(Kind);
  }

  bool isSimpleInOut() const { return Kind == InOut; }
  SILLocation getInOutLocation() const {
    assert(isSimpleInOut());
    return LV().Loc;
  }

  void emit(SILGenFunction &SGF, SmallVectorImpl<ManagedValue> &args,
            size_t &argIndex) {
    switch (Kind) {
    case InOut:
      args[argIndex++] = emitInOut(SGF);
      return;
    case LValueToPointer:
    case LValueArrayToPointer:
    case RValueArrayToPointer:
    case RValueStringToPointer:
    case FunctionConversion:
      args[argIndex++] = finishOriginalArgument(SGF);
      return;
    case DefaultArgument:
      emitDefaultArgument(SGF, Value.get<DefaultArgumentStorage>(Kind),
                          args, argIndex);
      return;
    case BorrowedLValue:
      emitBorrowedLValue(SGF, Value.get<BorrowedLValueStorage>(Kind),
                         args, argIndex);
      return;
    case ConsumedLValue:
      emitConsumedLValue(SGF, Value.get<ConsumedLValueStorage>(Kind), args,
                         argIndex);
      return;
    }
    llvm_unreachable("bad kind");
  }

private:
  ManagedValue emitInOut(SILGenFunction &SGF) {
    return emitAddress(SGF, AccessKind::ReadWrite);
  }

  ManagedValue emitBorrowIndirect(SILGenFunction &SGF) {
    return emitAddress(SGF, AccessKind::Read);
  }

  ManagedValue emitBorrowDirect(SILGenFunction &SGF) {
    ManagedValue address = emitAddress(SGF, AccessKind::Read);
    return SGF.B.createLoadBorrow(LV().Loc, address);
  }

  ManagedValue emitAddress(SILGenFunction &SGF, AccessKind accessKind) {
    auto tsanKind =
      (accessKind == AccessKind::Read ? TSanKind::None : TSanKind::InoutAccess);
    return SGF.emitAddressOfLValue(LV().Loc, std::move(LV().LV), tsanKind);
  }

  /// Replay the original argument expression.
  ManagedValue finishOriginalArgument(SILGenFunction &SGF) {
    auto results = finishOriginalExpr(SGF, Original.getExpr());
    auto value = results.first; // just let the owner go

    if (Original.isIndirect() && !value.getType().isAddress()) {
      value = value.materialize(SGF, Original.getExpr());
    }

    return value;
  }
  
  void emitDefaultArgument(SILGenFunction &SGF,
                           const DefaultArgumentStorage &info,
                           SmallVectorImpl<ManagedValue> &args,
                           size_t &argIndex);

  void emitBorrowedLValue(SILGenFunction &SGF,
                          BorrowedLValueStorage &info,
                          SmallVectorImpl<ManagedValue> &args,
                          size_t &argIndex);

  void emitConsumedLValue(SILGenFunction &SGF, ConsumedLValueStorage &info,
                          SmallVectorImpl<ManagedValue> &args,
                          size_t &argIndex);

  // (value, owner)
  std::pair<ManagedValue, ManagedValue>
  finishOriginalExpr(SILGenFunction &SGF, Expr *expr) {

    // This needs to handle all of the recursive cases from
    // ArgEmission::maybeEmitDelayed.

    expr = expr->getSemanticsProvidingExpr();

    // Handle injections into optionals.
    if (auto inject = dyn_cast<InjectIntoOptionalExpr>(expr)) {
      auto ownedValue =
        finishOriginalExpr(SGF, inject->getSubExpr());
      auto &optionalTL = SGF.getTypeLowering(expr->getType());

      auto optValue = SGF.emitInjectOptional(inject, optionalTL, SGFContext(),
                              [&](SGFContext ctx) { return ownedValue.first; });
      return {optValue, ownedValue.second};
    }

    // Handle try!.
    if (auto forceTry = dyn_cast<ForceTryExpr>(expr)) {
      // Handle throws from the accessor?  But what if the writeback throws?
      SILGenFunction::ForceTryEmission emission(SGF, forceTry);
      return finishOriginalExpr(SGF, forceTry->getSubExpr());
    }

    // Handle optional evaluations.
    if (auto optEval = dyn_cast<OptionalEvaluationExpr>(expr)) {
      return finishOptionalEvaluation(SGF, optEval);
    }

    // Done with the recursive cases.  Make sure we handled everything.
    assert(isa<InOutToPointerExpr>(expr) ||
           isa<ArrayToPointerExpr>(expr) ||
           isa<StringToPointerExpr>(expr) ||
           isa<FunctionConversionExpr>(expr));

    switch (Kind) {
    case InOut:
    case BorrowedLValue:
    case ConsumedLValue:
    case DefaultArgument:
      llvm_unreachable("no original expr to finish in these cases");

    case LValueToPointer:
      return {SGF.emitLValueToPointer(LV().Loc, std::move(LV().LV),
                                      Extra.get<PointerAccessInfo>(Kind)),
              /*owner*/ ManagedValue()};

    case LValueArrayToPointer:
      return SGF.emitArrayToPointer(LV().Loc, std::move(LV().LV),
                                    Extra.get<ArrayAccessInfo>(Kind));

    case RValueArrayToPointer: {
      auto pointerExpr = cast<ArrayToPointerExpr>(expr);
      auto optArrayValue = RV().RV;
      auto arrayValue = emitBindOptionals(SGF, optArrayValue,
                                          pointerExpr->getSubExpr());
      return SGF.emitArrayToPointer(pointerExpr, arrayValue,
                                    Extra.get<ArrayAccessInfo>(Kind));
    }

    case RValueStringToPointer: {
      auto pointerExpr = cast<StringToPointerExpr>(expr);
      auto optStringValue = RV().RV;
      auto stringValue =
        emitBindOptionals(SGF, optStringValue, pointerExpr->getSubExpr());
      return SGF.emitStringToPointer(pointerExpr, stringValue,
                                     pointerExpr->getType());
    }
    case FunctionConversion: {
      auto funcConv = cast<FunctionConversionExpr>(expr);
      auto optFuncValue = RV().RV;
      auto funcValue =
        emitBindOptionals(SGF, optFuncValue, funcConv->getSubExpr());
      return {SGF.emitTransformedValue(funcConv, funcValue,
                          funcConv->getSubExpr()->getType()->getCanonicalType(),
                          funcConv->getType()->getCanonicalType(),
                          SGFContext()),
              ManagedValue()};
    }
    }
    llvm_unreachable("bad kind");
  }

  ManagedValue emitBindOptionals(SILGenFunction &SGF, ManagedValue optValue,
                                 Expr *expr) {
    expr = expr->getSemanticsProvidingExpr();
    auto bind = dyn_cast<BindOptionalExpr>(expr);

    // If we don't find a bind, the value isn't optional.
    if (!bind) return optValue;

    // Recurse.
    optValue = emitBindOptionals(SGF, optValue, bind->getSubExpr());

    // Check whether the value is non-nil and if the value is not-nil, return
    // the unwrapped value.
    return SGF.emitBindOptional(bind, optValue, bind->getDepth());
  }

  std::pair<ManagedValue, ManagedValue>
  finishOptionalEvaluation(SILGenFunction &SGF, OptionalEvaluationExpr *eval) {
    SmallVector<ManagedValue, 2> results;

    SGF.emitOptionalEvaluation(eval, eval->getType(), results, SGFContext(),
      [&](SmallVectorImpl<ManagedValue> &results, SGFContext C) {
        // Recurse.
        auto values = finishOriginalExpr(SGF, eval->getSubExpr());

        // Our primary result is the value.
        results.push_back(values.first);

        // Our secondary result is the owner, if we have one.
        if (auto owner = values.second) results.push_back(owner);
      });

    assert(results.size() == 1 || results.size() == 2);

    ManagedValue value = results[0];

    ManagedValue owner;
    if (results.size() == 2) {
      owner = results[1];

      // Create a new value-dependence here if the primary result is
      // trivial.
      auto &valueTL = SGF.getTypeLowering(value.getType());
      if (valueTL.isTrivial()) {
        SILValue dependentValue =
          SGF.B.createMarkDependence(eval, value.forward(SGF),
                                     owner.getValue());
        value = SGF.emitManagedRValueWithCleanup(dependentValue, valueTL);
      }
    }

    return {value, owner};
  }
};

} // end anonymous namespace

/// Perform the formal-access phase of call argument emission by emitting
/// all of the delayed arguments.
static void emitDelayedArguments(SILGenFunction &SGF,
                                 MutableArrayRef<DelayedArgument> delayedArgs,
                         MutableArrayRef<SmallVector<ManagedValue, 4>> args) {
  assert(!delayedArgs.empty());

  SmallVector<std::pair<SILValue, SILLocation>, 4> emittedInoutArgs;
  auto delayedNext = delayedArgs.begin();

  // The assumption we make is that 'args' and 'delayedArgs' were built
  // up in parallel, with empty spots being dropped into 'args'
  // wherever there's a delayed argument to insert.
  //
  // Note that this also begins the formal accesses in evaluation order.
  for (auto &siteArgs : args) {
    // NB: siteArgs.size() may change during iteration
    for (size_t i = 0; i < siteArgs.size(); ) {
      auto &siteArg = siteArgs[i];
      
      if (siteArg) {
        ++i;
        continue;
      }

      assert(delayedNext != delayedArgs.end());
      auto &delayedArg = *delayedNext;

      // Emit the delayed argument and replace it in the arguments array.
      delayedArg.emit(SGF, siteArgs, i);

      // Remember all the simple inouts we emitted so we can perform
      // a basic inout-aliasing analysis.
      // This should be completely obviated by static enforcement.
      if (delayedArg.isSimpleInOut()) {
        emittedInoutArgs.push_back({siteArg.getValue(),
                                    delayedArg.getInOutLocation()});
      }

      if (++delayedNext == delayedArgs.end())
        goto done;
    }
  }

  llvm_unreachable("ran out of null arguments before we ran out of inouts");

done:

  // Check to see if we have multiple inout arguments which obviously
  // alias.  Note that we could do this in a later SILDiagnostics pass
  // as well: this would be stronger (more equivalences exposed) but
  // would have worse source location information.
  for (auto i = emittedInoutArgs.begin(), e = emittedInoutArgs.end();
         i != e; ++i) {
    for (auto j = emittedInoutArgs.begin(); j != i; ++j) {
      if (!RValue::areObviouslySameValue(i->first, j->first)) continue;

      SGF.SGM.diagnose(i->second, diag::inout_argument_alias)
        .highlight(i->second.getSourceRange());
      SGF.SGM.diagnose(j->second, diag::previous_inout_alias)
        .highlight(j->second.getSourceRange());
    }
  }
}

namespace {
/// Container to hold the result of a search for the storage reference
/// when determining to emit a borrow.
struct StorageRefResult {
private:
  Expr *storageRef;
  Expr *transitiveRoot;

public:
  // Represents an empty result
  StorageRefResult() : storageRef(nullptr), transitiveRoot(nullptr) {}
  bool isEmpty() const { return transitiveRoot == nullptr; }
  operator bool() const { return !isEmpty(); }

  /// The root of the expression that accesses the storage in \c storageRef.
  /// When in doubt, this is probably what you want, as it includes the
  /// entire expression tree involving the reference.
  Expr *getTransitiveRoot() const { return transitiveRoot; }

  /// The direct storage reference that was discovered.
  Expr *getStorageRef() const { return storageRef; }

  StorageRefResult(Expr *storageRef, Expr *transitiveRoot)
      : storageRef(storageRef), transitiveRoot(transitiveRoot) {
    assert(storageRef && transitiveRoot && "use the zero-arg init for empty");
  }

  // Initializes a storage reference where the base matches the ref.
  StorageRefResult(Expr *storageRef)
      : StorageRefResult(storageRef, storageRef) {}

  StorageRefResult withTransitiveRoot(StorageRefResult refResult) const {
    return withTransitiveRoot(refResult.transitiveRoot);
  }

  StorageRefResult withTransitiveRoot(Expr *newRoot) const {
    return StorageRefResult(storageRef, newRoot);
  }
};
} // namespace

static StorageRefResult findStorageReferenceExprForBorrow(Expr *e) {
  e = e->getSemanticsProvidingExpr();

  // These are basically defined as the cases implemented by SILGenLValue.

  // Direct storage references.
  if (auto dre = dyn_cast<DeclRefExpr>(e)) {
    if (isa<VarDecl>(dre->getDecl()))
      return dre;
  } else if (auto mre = dyn_cast<MemberRefExpr>(e)) {
    if (isa<VarDecl>(mre->getDecl().getDecl()))
      return mre;
  } else if (isa<SubscriptExpr>(e)) {
    return e;
  } else if (isa<OpaqueValueExpr>(e)) {
    return e;
  } else if (isa<KeyPathApplicationExpr>(e)) {
    return e;

  // Transitive storage references.  Look through these to see if the
  // sub-expression is a storage reference, but don't return the
  // sub-expression.
  } else if (auto tue = dyn_cast<TupleElementExpr>(e)) {
    if (auto result = findStorageReferenceExprForBorrow(tue->getBase()))
      return result.withTransitiveRoot(tue);

  } else if (auto fve = dyn_cast<ForceValueExpr>(e)) {
    if (auto result = findStorageReferenceExprForBorrow(fve->getSubExpr()))
      return result.withTransitiveRoot(fve);

  } else if (auto boe = dyn_cast<BindOptionalExpr>(e)) {
    if (auto result = findStorageReferenceExprForBorrow(boe->getSubExpr()))
      return result.withTransitiveRoot(boe);

  } else if (auto oe = dyn_cast<OpenExistentialExpr>(e)) {
    if (findStorageReferenceExprForBorrow(oe->getExistentialValue()))
      if (auto result = findStorageReferenceExprForBorrow(oe->getSubExpr()))
        return result.withTransitiveRoot(oe);

  } else if (auto bie = dyn_cast<DotSyntaxBaseIgnoredExpr>(e)) {
    if (auto result = findStorageReferenceExprForBorrow(bie->getRHS()))
      return result.withTransitiveRoot(bie);

  } else if (auto te = dyn_cast<AnyTryExpr>(e)) {
    if (auto result = findStorageReferenceExprForBorrow(te->getSubExpr()))
      return result.withTransitiveRoot(te);

  } else if (auto ioe = dyn_cast<InOutExpr>(e)) {
    if (auto result = findStorageReferenceExprForBorrow(ioe->getSubExpr()))
      return result.withTransitiveRoot(ioe);
  }

  return StorageRefResult();
}

Expr *ArgumentSource::findStorageReferenceExprForMoveOnly(
    SILGenFunction &SGF, StorageReferenceOperationKind kind) && {
  if (!isExpr())
    return nullptr;

  auto argExpr = asKnownExpr();

  // If there's a load around the outer part of this arg expr, look past it.
  bool sawLoad = false;
  if (auto *li = dyn_cast<LoadExpr>(argExpr)) {
    argExpr = li->getSubExpr();
    sawLoad = true;
  }

  // If we have a subscript, strip it off and make sure that our base is
  // something that we can process. If we do and we succeed below, we return the
  // subscript instead.
  SubscriptExpr *subscriptExpr = nullptr;
  if ((subscriptExpr = dyn_cast<SubscriptExpr>(argExpr))) {
    auto *decl = cast<SubscriptDecl>(subscriptExpr->getDecl().getDecl());
    if (decl->getReadImpl() != ReadImplKind::Read) {
      subscriptExpr = nullptr;
    } else {
      argExpr = subscriptExpr->getBase();
    }

    // If there's a load on the base of the subscript expr, look past it.
    if (auto *li = dyn_cast<LoadExpr>(argExpr)) {
      argExpr = li->getSubExpr();
    }
  }

  // If we're consuming instead, then the load _must_ have been there.
  if (kind == StorageReferenceOperationKind::Consume && !sawLoad)
    return nullptr;

  auto result = ::findStorageReferenceExprForBorrow(argExpr);

  if (!result)
    return nullptr;

  // We want to perform a borrow/consume if the first piece of storage being
  // referenced is a move-only type.

  VarDecl *storage = nullptr;
  Type type;
  if (auto dre = dyn_cast<DeclRefExpr>(result.getStorageRef())) {
    storage = dyn_cast<VarDecl>(dre->getDecl());
    type = dre->getType();
  } else if (auto mre = dyn_cast<MemberRefExpr>(result.getStorageRef())) {
    storage = dyn_cast<VarDecl>(mre->getDecl().getDecl());
    type = mre->getType();
  }

  if (!storage)
      return nullptr;
  assert(type);

  SILType ty =
      SGF.getLoweredType(type->getWithoutSpecifierType()->getCanonicalType());
  bool isMoveOnly = ty.isPureMoveOnly();
  if (auto *pd = dyn_cast<ParamDecl>(storage)) {
      isMoveOnly |= pd->getSpecifier() == ParamSpecifier::Borrowing;
      isMoveOnly |= pd->getSpecifier() == ParamSpecifier::Consuming;
  }
  if (!isMoveOnly)
      return nullptr;

  // It makes sense to borrow any kind of storage we refer to at this stage,
  // but SILGenLValue does not currently handle some kinds of references well.
  //
  // When rejecting to do the LValue-style borrow here, it'll end up going thru
  // the RValue-style emission, after which the extra copy will get eliminated.
  //
  // If we did not see a LoadExpr around the argument expression, then only
  // do the borrow if the storage is non-local.
  // FIXME: I don't have a principled reason for why this matters and hope that
  // we can fix the AST we're working with.
  if (!sawLoad && storage->getDeclContext()->isLocalContext())
      return nullptr;

  // Claim the value of this argument since we found a storage reference that
  // has a move only base.
  (void)std::move(*this).asKnownExpr();

  // If we saw a subscript expr and the base of the subscript expr passed our
  // tests above, we can emit the call to the subscript directly as a borrowed
  // lvalue. Return the subscript expr here so that we emit it appropriately.
  if (subscriptExpr)
    return subscriptExpr;

  return result.getTransitiveRoot();
}

Expr *
ArgumentSource::findStorageReferenceExprForBorrowExpr(SILGenFunction &SGF) && {
  if (!isExpr())
    return nullptr;

  // We support two patterns:
  //
  // (load_expr (borrow_expr))
  //   *or*
  // (paren_expr (load_expr (borrow_expr)))
  //
  // The first happens if a borrow is used on a non-self argument. The second
  // happens if we pass self as a borrow.
  auto *argExpr = asKnownExpr();

  if (auto *parenExpr = dyn_cast<ParenExpr>(argExpr))
    argExpr = parenExpr->getSubExpr();

  auto *li = dyn_cast<LoadExpr>(argExpr);
  if (!li)
    return nullptr;
  auto *borrowExpr = dyn_cast<BorrowExpr>(li->getSubExpr());
  if (!borrowExpr)
    return nullptr;

  Expr *lvExpr = ::findStorageReferenceExprForBorrow(borrowExpr->getSubExpr())
                     .getTransitiveRoot();

  // Claim the value of this argument.
  if (lvExpr) {
    (void)std::move(*this).asKnownExpr();
  }

  return lvExpr;
}

Expr *ArgumentSource::findStorageReferenceExprForBorrow() && {
  if (!isExpr()) return nullptr;

  auto argExpr = asKnownExpr();
  auto *lvExpr =
      ::findStorageReferenceExprForBorrow(argExpr).getTransitiveRoot();

  // Claim the value of this argument if we found a storage reference.
  if (lvExpr) {
    (void) std::move(*this).asKnownExpr();
  }

  return lvExpr;
}

namespace {

class ArgEmitter {
  SILGenFunction &SGF;
  SILLocation ApplyLoc;
  SILFunctionTypeRepresentation Rep;
  bool IsYield;
  bool IsForCoroutine;
  ForeignInfo Foreign;
  ClaimedParamsRef ParamInfos;
  SmallVectorImpl<ManagedValue> &Args;

  /// Track any delayed arguments that are emitted.  Each corresponds
  /// in order to a "hole" (a null value) in Args.
  SmallVectorImpl<DelayedArgument> &DelayedArguments;

public:
  ArgEmitter(SILGenFunction &SGF, SILLocation applyLoc,
             SILFunctionTypeRepresentation Rep,
             bool isYield, bool isForCoroutine, ClaimedParamsRef paramInfos,
             SmallVectorImpl<ManagedValue> &args,
             SmallVectorImpl<DelayedArgument> &delayedArgs,
             const ForeignInfo &foreign)
      : SGF(SGF), ApplyLoc(applyLoc),
        Rep(Rep), IsYield(isYield), IsForCoroutine(isForCoroutine),
        Foreign(foreign), ParamInfos(paramInfos), Args(args),
        DelayedArguments(delayedArgs) {}

  // origParamType is a parameter type.
  void
  emitSingleArg(ArgumentSource &&arg, AbstractionPattern origParamType,
                llvm::Optional<AnyFunctionType::Param> param = llvm::None) {
    // If this is delayed default argument, prepare to emit the default argument
    // generator later.
    if (arg.isDelayedDefaultArg()) {
      auto substParamType = arg.getSubstRValueType();
      auto defArg = std::move(arg).asKnownDefaultArg();

      auto numParams = getFlattenedValueCount(origParamType,
                                              ImportAsMemberStatus());
      DelayedArguments.emplace_back(defArg,
                                    defArg->getDefaultArgsOwner(),
                                    defArg->getParamIndex(),
                                    substParamType, origParamType,
                                    claimNextParameters(numParams),
                                    Rep);
      Args.push_back(ManagedValue());

      maybeEmitForeignArgument();
      return;
    }
    emit(std::move(arg), origParamType, param);
    maybeEmitForeignArgument();
  }

  // origFormalType is a function type.
  void emitPreparedArgs(PreparedArguments &&args,
                        AbstractionPattern origFormalType) {
    assert(args.isValid());
    auto params = args.getParams();
    auto argSources = std::move(args).getSources();

    maybeEmitForeignArgument();

    size_t numOrigFormalParams =
      origFormalType.isTypeParameter()
        ? argSources.size()
        : origFormalType.getNumFunctionParams();
    size_t nextArgSourceIndex = 0;

    for (size_t i = 0; i != numOrigFormalParams; ++i) {
      auto origFormalParamType = origFormalType.getFunctionParamType(i);

      // If the next pattern is not a pack expansion, just emit it as a
      // single argument.
      if (!origFormalParamType.isPackExpansion()) {
        emitSingleArg(std::move(argSources[nextArgSourceIndex]),
                      origFormalParamType, params[nextArgSourceIndex]);
        ++nextArgSourceIndex;
        // Otherwise we need to emit a pack argument.
      } else {
        auto numComponents =
          origFormalParamType.getNumPackExpandedComponents();

        auto argSourcesSlice =
          argSources.slice(nextArgSourceIndex, numComponents);
        emitPackArg(argSourcesSlice, origFormalParamType);
        nextArgSourceIndex += numComponents;
      }
    }

    assert(nextArgSourceIndex == argSources.size());
  }

private:
  void emit(ArgumentSource &&arg, AbstractionPattern origParamType,
            llvm::Optional<AnyFunctionType::Param> origParam = llvm::None) {
    if (!arg.hasLValueType()) {
      // If the unsubstituted function type has a parameter of tuple type,
      // explode the tuple value.
      if (origParamType.isTuple()) {
        emitExpanded(std::move(arg), origParamType);
        return;
      }
    }

    // Okay, everything else will be passed as a single value, one
    // way or another.

    // If this is a discarded foreign static 'self' parameter, force the
    // argument and discard it.
    if (Foreign.self.isStatic()) {
      std::move(arg).getAsRValue(SGF);
      return;
    }

    // Adjust for the foreign error or async argument if necessary.
    maybeEmitForeignArgument();

    // The substituted parameter type.  Might be different from the
    // substituted argument type by abstraction and/or bridging.
    auto paramSlice = claimNextParameters(1);
    SILParameterInfo param = paramSlice.front();
    assert(arg.hasLValueType() == param.isIndirectInOut());

    // Make sure we use the same value category for these so that we
    // can hereafter just use simple equality checks to test for
    // abstraction.
    auto substArgType = arg.getSubstRValueType();
    SILType loweredSubstArgType = SGF.getLoweredType(substArgType);
    if (param.isIndirectInOut()) {
      loweredSubstArgType =
        SILType::getPrimitiveAddressType(loweredSubstArgType.getASTType());
    }
    SILType loweredSubstParamType = SILType::getPrimitiveType(
                param.getInterfaceType(),
                loweredSubstArgType.getCategory());
    bool isShared = false;
    bool isOwned = false;
    if (origParam) {
      isOwned |= origParam->isOwned();
      isShared |= origParam->isShared();
    }

    // If the caller takes the argument indirectly, the argument has an
    // inout type.
    if (param.isIndirectInOut()) {
      emitInOut(std::move(arg), loweredSubstArgType, loweredSubstParamType,
                origParamType, substArgType);
      return;
    }

    // If we have a guaranteed +0 parameter...
    if (param.isGuaranteed() || isShared) {
      // And this is a yield, emit a borrowed r-value.
      if (IsYield) {
        if (tryEmitBorrowed(std::move(arg), loweredSubstArgType,
                            loweredSubstParamType, origParamType, paramSlice))
          return;
      }

      if (tryEmitBorrowExpr(std::move(arg), loweredSubstArgType,
                            loweredSubstParamType, origParamType, paramSlice))
        return;

      // If we have a guaranteed paramter, see if we have a move only type and
      // can emit it borrow.
      //
      // We check for move only in tryEmitBorrowedMoveOnly.
      if (tryEmitBorrowedMoveOnly(std::move(arg), loweredSubstArgType,
                                  loweredSubstParamType, origParamType,
                                  paramSlice))
        return;
    }

    if (param.isConsumed() || isOwned) {
      if (tryEmitConsumedMoveOnly(std::move(arg), loweredSubstArgType,
                                  loweredSubstParamType, origParamType,
                                  paramSlice))
        return;
    }

    if (SGF.silConv.isSILIndirect(param)) {
      emitIndirect(std::move(arg), loweredSubstArgType, origParamType, param);
      return;
    }

    // Okay, if the original parameter is passed directly, then we
    // just need to handle abstraction differences and bridging.
    emitDirect(std::move(arg), loweredSubstArgType, origParamType, param,
               origParam);
  }

  ClaimedParamsRef claimNextParameters(unsigned count) {
    assert(count <= ParamInfos.size());
    auto slice = ParamInfos.slice(0, count);
    ParamInfos = ParamInfos.slice(count);
    return slice;
  }

  /// Emit an argument as an expanded tuple.
  void emitExpanded(ArgumentSource &&arg, AbstractionPattern origParamType) {
    assert(!arg.isLValue() && "argument is l-value but parameter is tuple?");

    // If the original parameter type is a vanishing tuple, we want to emit
    // this as if the argument source was wrapped in an extra level of
    // tuple literal.
    bool origTupleVanishes = origParamType.doesTupleVanish();

    auto substType = arg.getSubstRValueType();

    // If we're working with an r-value, just expand it out and emit
    // all the elements individually.
    // FIXME: this code is not doing the right thing with packs
    if (arg.isRValue()) {
      if (CanTupleType substArgType = dyn_cast<TupleType>(substType)) {
        // The original type isn't necessarily a tuple.
        if (!origParamType.matchesTuple(substArgType))
          origParamType = origParamType.getTupleElementType(0);

        assert(origParamType.matchesTuple(substArgType));

        auto loc = arg.getKnownRValueLocation();
        SmallVector<RValue, 4> elts;
        std::move(arg).asKnownRValue(SGF).extractElements(elts);
        for (auto i : indices(substArgType.getElementTypes())) {
          emit({ loc, std::move(elts[i]) },
               origParamType.getTupleElementType(i));
        }
        return;
      }

      auto loc = arg.getKnownRValueLocation();
      SmallVector<RValue, 1> elts;
      std::move(arg).asKnownRValue(SGF).extractElements(elts);
      emit({ loc, std::move(elts[0]) },
           origParamType.getTupleElementType(0));
      return;
    }

    // Otherwise, we're working with an expression.
    Expr *e = std::move(arg).asKnownExpr();

    // If the source expression is a tuple literal, we can break it
    // up directly.  We can also do this if the orig type is a vanishing
    // tuple, because we want to treat that like it was the sole element
    // of a tuple.  Note that vanishing tuples take priority: the
    // singleton element could itself be a tuple.
    auto tupleExpr = dyn_cast<TupleExpr>(e);
    if (origTupleVanishes || tupleExpr) {
      auto getElementExpr = [&](unsigned index) {
        assert(!origTupleVanishes || index == 0);
        return (origTupleVanishes ? e : tupleExpr->getElement(index));
      };
      origParamType.forEachTupleElement(substType,
                                        [&](TupleElementGenerator &elt) {
        if (!elt.isOrigPackExpansion()) {
          emit(getElementExpr(elt.getSubstIndex()), elt.getOrigType());
          return;
        }

        auto substEltTypes = elt.getSubstTypes();
        SmallVector<ArgumentSource, 4> eltArgs;
        eltArgs.reserve(substEltTypes.size());
        for (auto i : elt.getSubstIndexRange()) {
          eltArgs.emplace_back(getElementExpr(i));
        }
        emitPackArg(eltArgs, elt.getOrigType());
      });
      return;
    }

    if (IsYield) {
      if (auto result = findStorageReferenceExprForBorrow(e)) {
        emitExpandedBorrowed(result.getTransitiveRoot(), origParamType);
        return;
      }
    }

    // Fall back to the r-value case.
    emitExpanded({ e, SGF.emitRValue(e) }, origParamType);
  }

  void emitIndirect(ArgumentSource &&arg,
                    SILType loweredSubstArgType,
                    AbstractionPattern origParamType,
                    SILParameterInfo param) {
    auto contexts = getRValueEmissionContexts(loweredSubstArgType, param);
    ManagedValue result;

    // If no abstraction is required, try to honor the emission contexts.
    if (!contexts.RequiresReabstraction) {
      auto loc = arg.getLocation();

      // Peephole certain argument emissions.
      if (arg.isExpr()) {
        auto expr = std::move(arg).asKnownExpr();

        // Try the peepholes.
        if (maybeEmitDelayed(expr, OriginalArgument(expr, /*indirect*/ true)))
          return;

        // Otherwise, just use the default logic.
        result = SGF.emitRValueAsSingleValue(expr, contexts.FinalContext);
      } else {
        result = std::move(arg).getAsSingleValue(SGF, contexts.FinalContext);
      }

      // If it's not already in memory, put it there.
      if (!result.getType().isAddress()) {
        // If we have a move only wrapped type, we need to unwrap before we
        // materialize. We will forward as appropriate so it will show up as a
        // consuming use or a guaranteed use as appropriate.
        if (result.getType().isMoveOnlyWrapped()) {
          if (result.isPlusOne(SGF)) {
            result =
                SGF.B.createOwnedMoveOnlyWrapperToCopyableValue(loc, result);
          } else {
            result = SGF.B.createGuaranteedMoveOnlyWrapperToCopyableValue(
                loc, result);
          }
        }
        result = result.materialize(SGF, loc);
      }

    // Otherwise, simultaneously emit and reabstract.
    } else {
      result = std::move(arg).materialize(SGF, origParamType,
                                          SGF.getSILInterfaceType(param));
    }

    Args.push_back(result);
  }

  void emitInOut(ArgumentSource &&arg,
                 SILType loweredSubstArgType, SILType loweredSubstParamType,
                 AbstractionPattern origType, CanType substType) {
    SILLocation loc = arg.getLocation();

    LValue lv = [&]{
      // If the argument is already lowered to an LValue, it must be the
      // receiver of a self argument, which will be the first inout.
      if (arg.isLValue()) {
        return std::move(arg).asKnownLValue();
      } else {
        auto *e = cast<InOutExpr>(std::move(arg).asKnownExpr()->
                                  getSemanticsProvidingExpr());
        return SGF.emitLValue(e->getSubExpr(), SGFAccessKind::ReadWrite);
      }
    }();
    
    if (loweredSubstParamType.hasAbstractionDifference(Rep,
                                                       loweredSubstArgType)) {
      lv.addSubstToOrigComponent(origType, loweredSubstParamType);
    }

    // Leave an empty space in the ManagedValue sequence and
    // remember that we had an inout argument.
    DelayedArguments.emplace_back(DelayedArgument::InOut, std::move(lv), loc);
    Args.push_back(ManagedValue());
    return;
  }

  bool tryEmitBorrowed(ArgumentSource &&arg, SILType loweredSubstArgType,
                       SILType loweredSubstParamType,
                       AbstractionPattern origParamType,
                       ClaimedParamsRef paramsSlice) {
    assert(paramsSlice.size() == 1);

    // Try to find an expression we can emit as an l-value.
    auto lvExpr = std::move(arg).findStorageReferenceExprForBorrow();
    if (!lvExpr) return false;

    emitBorrowed(lvExpr, loweredSubstArgType, loweredSubstParamType,
                 origParamType, paramsSlice);
    return true;
  }

  bool tryEmitBorrowedMoveOnly(ArgumentSource &&arg,
                               SILType loweredSubstArgType,
                               SILType loweredSubstParamType,
                               AbstractionPattern origParamType,
                               ClaimedParamsRef paramsSlice) {
    assert(paramsSlice.size() == 1);

    // Try to find an expression we can emit as a borrowed l-value.
    auto lvExpr = std::move(arg).findStorageReferenceExprForMoveOnly(
        SGF, ArgumentSource::StorageReferenceOperationKind::Borrow);
    if (!lvExpr)
      return false;

    emitBorrowed(lvExpr, loweredSubstArgType, loweredSubstParamType,
                 origParamType, paramsSlice);

    return true;
  }

  bool tryEmitBorrowExpr(ArgumentSource &&arg, SILType loweredSubstArgType,
                         SILType loweredSubstParamType,
                         AbstractionPattern origParamType,
                         ClaimedParamsRef paramsSlice) {
    assert(paramsSlice.size() == 1);

    // Try to find an expression we can emit as a borrowed l-value.
    auto lvExpr = std::move(arg).findStorageReferenceExprForBorrowExpr(SGF);
    if (!lvExpr)
      return false;

    emitBorrowed(lvExpr, loweredSubstArgType, loweredSubstParamType,
                 origParamType, paramsSlice);

    return true;
  }

  void emitBorrowed(Expr *arg, SILType loweredSubstArgType,
                    SILType loweredSubstParamType,
                    AbstractionPattern origParamType,
                    ClaimedParamsRef claimedParams) {
    auto emissionKind = SGFAccessKind::BorrowedObjectRead;
    for (auto param : claimedParams) {
      assert(!param.isConsumed());
      if (param.isIndirectInGuaranteed() && SGF.silConv.useLoweredAddresses()) {
        emissionKind = SGFAccessKind::BorrowedAddressRead;
        break;
      }
    }

    LValue argLV = SGF.emitLValue(arg, emissionKind);

    if (loweredSubstParamType.hasAbstractionDifference(Rep,
                                                       loweredSubstArgType)) {
      argLV.addSubstToOrigComponent(origParamType, loweredSubstParamType);
    }

    DelayedArguments.emplace_back(std::move(argLV), arg, origParamType,
                                  claimedParams);
    Args.push_back(ManagedValue());
  }

  void emitExpandedBorrowed(Expr *arg, AbstractionPattern origParamType) {
    CanType substArgType = arg->getType()->getCanonicalType();
    auto count = getFlattenedValueCount(origParamType);
    auto claimedParams = claimNextParameters(count);

    SILType loweredSubstArgType = SGF.getLoweredType(substArgType);
    SILType loweredSubstParamType =
      SGF.getLoweredType(origParamType, substArgType);

    return emitBorrowed(arg, loweredSubstArgType, loweredSubstParamType,
                        origParamType, claimedParams);
  }

  bool tryEmitConsumedMoveOnly(ArgumentSource &&arg,
                               SILType loweredSubstArgType,
                               SILType loweredSubstParamType,
                               AbstractionPattern origParamType,
                               ClaimedParamsRef paramsSlice) {
    assert(paramsSlice.size() == 1);

    // Try to find an expression we can emit as a consumed l-value.
    auto lvExpr = std::move(arg).findStorageReferenceExprForMoveOnly(
        SGF, ArgumentSource::StorageReferenceOperationKind::Consume);
    if (!lvExpr)
      return false;

    emitConsumed(lvExpr, loweredSubstArgType, loweredSubstParamType,
                 origParamType, paramsSlice);

    return true;
  }

  void emitConsumed(Expr *arg, SILType loweredSubstArgType,
                    SILType loweredSubstParamType,
                    AbstractionPattern origParamType,
                    ClaimedParamsRef claimedParams) {
    auto emissionKind = SGFAccessKind::OwnedAddressConsume;

    LValue argLV = SGF.emitLValue(arg, emissionKind);

    if (loweredSubstParamType.hasAbstractionDifference(Rep,
                                                       loweredSubstArgType)) {
      argLV.addSubstToOrigComponent(origParamType, loweredSubstParamType);
    }

    DelayedArguments.emplace_back(std::move(argLV), arg, origParamType,
                                  claimedParams, false /*is borrowed*/);
    Args.push_back(ManagedValue());
  }

  void emitExpandedConsumed(Expr *arg, AbstractionPattern origParamType) {
    CanType substArgType = arg->getType()->getCanonicalType();
    auto count = getFlattenedValueCount(origParamType);
    auto claimedParams = claimNextParameters(count);

    SILType loweredSubstArgType = SGF.getLoweredType(substArgType);
    SILType loweredSubstParamType =
        SGF.getLoweredType(origParamType, substArgType);

    return emitConsumed(arg, loweredSubstArgType, loweredSubstParamType,
                        origParamType, claimedParams);
  }

  void
  emitDirect(ArgumentSource &&arg, SILType loweredSubstArgType,
             AbstractionPattern origParamType, SILParameterInfo param,
             llvm::Optional<AnyFunctionType::Param> origParam = llvm::None) {
    ManagedValue value;
    auto loc = arg.getLocation();

    auto convertOwnershipConvention = [&](ManagedValue value) {
      return convertOwnershipConventionGivenParamInfo(
          SGF, param, origParam, value, loc, IsForCoroutine);
    };

    auto contexts = getRValueEmissionContexts(loweredSubstArgType, param);
    if (contexts.RequiresReabstraction) {
      auto conversion = [&] {
        switch (getSILFunctionLanguage(Rep)) {
        case SILFunctionLanguage::Swift:
          return Conversion::getSubstToOrig(origParamType,
                                            arg.getSubstRValueType(),
                                            param.getSILStorageInterfaceType());
        case SILFunctionLanguage::C:
          return Conversion::getBridging(Conversion::BridgeToObjC,
             arg.getSubstRValueType(),
             origParamType.getType(),
             param.getSILStorageInterfaceType());
        }
        llvm_unreachable("bad language");
      }();
      value = emitConvertedArgument(std::move(arg), conversion,
                                    contexts.FinalContext);
      Args.push_back(convertOwnershipConvention(value));
      return;
    }

    // Peephole certain argument emissions.
    if (arg.isExpr()) {
      auto expr = std::move(arg).asKnownExpr();

      // Try the peepholes.
      if (maybeEmitDelayed(expr, OriginalArgument(expr, /*indirect*/ false)))
        return;

      // Any borrows from any rvalue accesses, we want to be cleaned up at this
      // point.
      FormalEvaluationScope S(SGF);

      // Otherwise, just use the default logic.
      value = SGF.emitRValueAsSingleValue(expr, contexts.FinalContext);

      // We want any borrows done by the ownership-convention adjustment to
      // happen outside of the formal-evaluation scope we pushed for the
      // expression evaluation, but any copies to be done inside of it.
      // Copies are only done if the parameter is consumed.
      if (!param.isConsumed())
        S.pop();

      Args.push_back(convertOwnershipConvention(value));
      return;
    }

    value = std::move(arg).getAsSingleValue(SGF, contexts.FinalContext);
    Args.push_back(convertOwnershipConvention(value));
  }

  void emitPackArg(MutableArrayRef<ArgumentSource> args,
                   AbstractionPattern origExpansionType) {
    // Adjust for the foreign error or async argument if necessary.
    maybeEmitForeignArgument();

    // The substituted parameter type.  Might be different from the
    // substituted argument type by abstraction and/or bridging.
    auto paramSlice = claimNextParameters(1);
    SILParameterInfo param = paramSlice.front();
    assert(param.isPack() && "emitting pack argument into non-pack parameter");
    auto packTy = cast<SILPackType>(param.getInterfaceType());

    // TODO: if there's one argument, and it's a pack of the right
    // type, try to simply forward it instead of allocating a new pack.

    // Otherwise, allocate a pack.
    auto pack = SGF.emitTemporaryPackAllocation(ApplyLoc,
                                    SILType::getPrimitiveObjectType(packTy));

    // We can't support inout pack expansions yet because SIL can't have
    // a dynamic set of accesses in flight at once.
    // TODO: we *could* support this if there aren't any expansions in the
    // arguments
    if (param.getConvention() == ParameterConvention::Pack_Inout) {
      SGF.SGM.diagnose(ApplyLoc, diag::not_implemented,
                       "inout pack argument emission");
      Args.push_back(ManagedValue::forLValue(pack));
      return;
    }

    auto formalPackType = getFormalPackType(args);

    bool consumed = param.getConvention() == ParameterConvention::Pack_Owned;
    emitIndirectIntoPack(args, origExpansionType, pack, formalPackType,
                         consumed);
  }

  CanPackType getFormalPackType(ArrayRef<ArgumentSource> args) {
    SmallVector<CanType, 8> elts;
    for (auto &arg : args) {
      elts.push_back(arg.getSubstRValueType());
    }
    return CanPackType::get(SGF.getASTContext(), elts);
  }

  void emitIndirectIntoPack(MutableArrayRef<ArgumentSource> args,
                            AbstractionPattern origExpansionType,
                            SILValue packAddr,
                            CanPackType formalPackType,
                            bool consumed) {
    auto packTy = packAddr->getType().castTo<SILPackType>();
    assert(packTy->getNumElements() == args.size() &&
           "wrong pack shape for arguments");

    SmallVector<CleanupHandle, 8> eltCleanups;

    for (auto i : indices(args)) {
      ArgumentSource &&arg = std::move(args[i]);
      auto expectedEltTy = packTy->getSILElementType(i);

      bool isPackExpansion = expectedEltTy.is<PackExpansionType>();

      auto cleanup = CleanupHandle::invalid();
      if (isPackExpansion) {
        cleanup =
          emitPackExpansionIntoPack(std::move(arg), origExpansionType,
                                    expectedEltTy, consumed,
                                    packAddr, formalPackType, i);
      } else {
        cleanup =
          emitScalarIntoPack(std::move(arg),
                             origExpansionType.getPackExpansionPatternType(),
                             expectedEltTy, consumed,
                             packAddr, formalPackType, i);
      }
      if (consumed && cleanup.isValid()) eltCleanups.push_back(cleanup);
    }

    if (!consumed) {
      Args.push_back(ManagedValue::forBorrowedAddressRValue(packAddr));
    } else if (eltCleanups.empty()) {
      Args.push_back(ManagedValue::forTrivialAddressRValue(packAddr));
    } else if (eltCleanups.size() == 1) {
      Args.push_back(ManagedValue::forOwnedAddressRValue(packAddr,
                                                         eltCleanups[0]));
    } else {
      // Args implicitly expects there to be <= 1 cleanup per argument,
      // so to make this work, we need to deactivate all the existing
      // cleanups and push a unified cleanup to destroy those values via
      // the pack.
      for (auto cleanup : eltCleanups)
        SGF.Cleanups.forwardCleanup(cleanup);
      auto packCleanup =
        SGF.enterDestroyPackCleanup(packAddr, formalPackType);
      Args.push_back(ManagedValue::forOwnedAddressRValue(packAddr,
                                                         packCleanup));
    }
  }

  CleanupHandle emitPackExpansionIntoPack(ArgumentSource &&arg,
                                          AbstractionPattern origExpansionType,
                                          SILType expectedParamType,
                                          bool consumed,
                                          SILValue packAddr,
                                          CanPackType formalPackType,
                                          unsigned packComponentIndex) {
    // TODO: we'll need to handle already-emitted packs for things like
    // subscripts
    assert(arg.isExpr() && "emitting a non-expression pack expansion");
    auto expr = std::move(arg).asKnownExpr();

    auto expansionExpr =
      cast<PackExpansionExpr>(expr->getSemanticsProvidingExpr());

    // TODO: try to borrow the existing elements if we can do that within
    // the limitations of the SIL representation.

    // Allocate a tuple with a single component: the expected expansion type.
    auto tupleTy =
      SILType::getPrimitiveObjectType(
        TupleType::get(TupleTypeElt(expectedParamType.getASTType()),
                       SGF.getASTContext())->getCanonicalType());
    auto &tupleTL = SGF.getTypeLowering(tupleTy);

    auto tupleAddr = SGF.emitTemporaryAllocation(expansionExpr, tupleTy);

    auto openedElementEnv = expansionExpr->getGenericEnvironment();
    SGF.emitDynamicPackLoop(expansionExpr, formalPackType,
                            packComponentIndex, openedElementEnv,
                            [&](SILValue indexWithinComponent,
                                SILValue packExpansionIndex,
                                SILValue packIndex) {
      auto partialCleanup = CleanupHandle::invalid();
      if (!tupleTL.isTrivial()) {
        partialCleanup =
          SGF.enterPartialDestroyPackCleanup(packAddr, formalPackType,
                                             packComponentIndex,
                                             indexWithinComponent);
      }

      // FIXME: push a partial-array cleanup to destroy the previous
      // elements in this slice of the pack (then pop it before we exit
      // this scope).

      // Turn pack archetypes in the pattern type of the lowered pack
      // expansion type into opened element archetypes.  These AST-level
      // manipulations should work fine on SIL types since we're not
      // changing any interesting structure.
      SILType expectedElementType = [&] {
        auto loweredPatternType =
          expectedParamType.castTo<PackExpansionType>().getPatternType();
        auto loweredElementType =
          openedElementEnv->mapContextualPackTypeIntoElementContext(
            loweredPatternType);
        return SILType::getPrimitiveAddressType(loweredElementType);
      }();

      // Project the tuple element.  This projection uses the
      // pack expansion index because the tuple is only for the
      // projection elements.
      auto eltAddr =
        SGF.B.createTuplePackElementAddr(expansionExpr,
                                         packExpansionIndex, tupleAddr,
                                         expectedElementType);
      auto &eltTL = SGF.getTypeLowering(eltAddr->getType());

      // Evaluate the pattern expression into that address.
      auto patternExpr = expansionExpr->getPatternExpr();
      auto bufferInit = SGF.useBufferAsTemporary(eltAddr, eltTL);
      Initialization *innermostInit = bufferInit.get();

      // Wrap it in a ConversionInitialization if required.
      llvm::Optional<ConvertingInitialization> convertingInit;
      auto substPatternType = patternExpr->getType()->getCanonicalType();
      auto loweredPatternTy = SGF.getLoweredRValueType(substPatternType);
      if (loweredPatternTy != expectedElementType.getASTType()) {
        convertingInit.emplace(
            Conversion::getSubstToOrig(
                origExpansionType.getPackExpansionPatternType(),
                substPatternType, expectedElementType),
            SGFContext(innermostInit));
        innermostInit = &*convertingInit;
      }

      SGF.emitExprInto(patternExpr, innermostInit);

      // Deactivate any cleanup associated with that value.  In later
      // iterations of this loop, we're managing this with our
      // partial-array cleanup; after the loop, we're managing this
      // with our full-tuple cleanup.
      bufferInit->getManagedAddress().forward(SGF);

      // Store the element address into the pack.
      SGF.B.createPackElementSet(expansionExpr, eltAddr, packIndex,
                                 packAddr);

      // Deactivate the partial cleanup before we go out of scope.
      if (partialCleanup.isValid())
        SGF.Cleanups.forwardCleanup(partialCleanup);
    });

    // If the tuple is trivial, we don't need a cleanup.
    if (tupleTL.isTrivial())
      return CleanupHandle::invalid();

    // Otherwise, push a full-tuple cleanup for it.
    return SGF.enterDestroyCleanup(tupleAddr);
  }

  CleanupHandle emitScalarIntoPack(ArgumentSource &&arg,
                                   AbstractionPattern origFormalType,
                                   SILType expectedParamType,
                                   bool consumed,
                                   SILValue packAddr,
                                   CanPackType formalPackType,
                                   unsigned packComponentIndex) {
    auto loc = arg.getLocation();

    // TODO: make an effort to borrow the value if the parameter
    // isn't consumed

    // Create a temporary.
    auto &paramTL = SGF.getTypeLowering(expectedParamType);
    auto eltInit = SGF.emitTemporary(loc, paramTL);

    // Emit the argument into the temporary within a scope.

    FullExpr scope(SGF.Cleanups, CleanupLocation(loc));

    std::move(arg).forwardInto(SGF, origFormalType, eltInit.get(), paramTL);

    // Deactivate the destroy cleanup for the temporary itself within the
    // scope, then pop the scope and recreate the cleanup.
    auto eltAddr = eltInit->getManagedAddress().forward(SGF);
    scope.pop();
    auto cleanup = (paramTL.isTrivial()
                      ? CleanupHandle::invalid()
                      : SGF.enterDestroyCleanup(eltAddr));

    // Store the address of the temporary into the pack.
    auto packIndex = SGF.B.createScalarPackIndex(loc, packComponentIndex,
                                                 formalPackType);
    SGF.B.createPackElementSet(loc, eltAddr, packIndex, packAddr);

    return cleanup;
  }

  bool maybeEmitDelayed(Expr *expr, OriginalArgument original) {
    expr = expr->getSemanticsProvidingExpr();

    // Delay accessing inout-to-pointer arguments until the call.
    if (auto inoutToPointer = dyn_cast<InOutToPointerExpr>(expr)) {
      return emitDelayedConversion(inoutToPointer, original);
    }

    // Delay accessing array-to-pointer arguments until the call.
    if (auto arrayToPointer = dyn_cast<ArrayToPointerExpr>(expr)) {
      return emitDelayedConversion(arrayToPointer, original);
    }

    // Delay accessing string-to-pointer arguments until the call.
    if (auto stringToPointer = dyn_cast<StringToPointerExpr>(expr)) {
      return emitDelayedConversion(stringToPointer, original);
    }
    
    // Delay function conversions involving the opened Self type of an
    // existential whose opening is itself delayed.
    //
    // This comes up when invoking protocol methods on an existential that
    // have covariant arguments of function type with Self arguments, e.g.:
    //
    //    protocol P {
    //      mutating func foo(_: (Self) -> Void)
    //    }
    //
    //    func bar(x: inout P) {
    //      x.foo { y in return }
    //    }
    //
    // Although the type-erased method is presented as formally taking an
    // argument of the existential type P, it still has a conversion thunk to
    // perform type erasure on the argument coming from the underlying
    // implementation. Since the `self` argument is inout, it isn't formally
    // opened until late when formal accesses begin, so this closure conversion
    // must also be deferred until after that occurs.
    if (auto funcConv = dyn_cast<FunctionConversionExpr>(expr)) {
      auto destTy = funcConv->getType()->castTo<AnyFunctionType>();
      auto srcTy = funcConv->getSubExpr()->getType()->castTo<AnyFunctionType>();
      
      if (destTy->hasOpenedExistential()
          && !srcTy->hasOpenedExistential()
          && destTy->getRepresentation() == srcTy->getRepresentation()) {
        return emitDelayedConversion(funcConv, original);
      }
    }

    // Any recursive cases we handle here need to be handled in
    // DelayedArgument::finishOriginalExpr.

    // Handle optional evaluations.
    if (auto optional = dyn_cast<OptionalEvaluationExpr>(expr)) {
      // The validity of just recursing here depends on the fact
      // that we only return true for the specific conversions above,
      // which are constrained by the ASTVerifier to only appear in
      // specific forms.
      return maybeEmitDelayed(optional->getSubExpr(), original);
    }

    // Handle injections into optionals.
    if (auto inject = dyn_cast<InjectIntoOptionalExpr>(expr)) {
      return maybeEmitDelayed(inject->getSubExpr(), original);
    }

    // Handle try! expressions.
    if (auto forceTry = dyn_cast<ForceTryExpr>(expr)) {
      // Any expressions in the l-value must be routed appropriately.
      SILGenFunction::ForceTryEmission emission(SGF, forceTry);

      return maybeEmitDelayed(forceTry->getSubExpr(), original);
    }

    return false;
  }

  bool emitDelayedConversion(InOutToPointerExpr *pointerExpr,
                             OriginalArgument original) {
    auto info = SGF.getPointerAccessInfo(pointerExpr->getType());
    LValueOptions options;
    options.IsNonAccessing = pointerExpr->isNonAccessing();
    LValue lv = SGF.emitLValue(pointerExpr->getSubExpr(), info.AccessKind,
                               options);
    DelayedArguments.emplace_back(info, std::move(lv), pointerExpr, original);
    Args.push_back(ManagedValue());
    return true;
  }

  bool emitDelayedConversion(ArrayToPointerExpr *pointerExpr,
                             OriginalArgument original) {
    auto arrayExpr = pointerExpr->getSubExpr();

    // If the source of the conversion is an inout, emit the l-value
    // but delay the formal access.
    if (arrayExpr->isSemanticallyInOutExpr()) {
      auto info = SGF.getArrayAccessInfo(pointerExpr->getType(),
                                         arrayExpr->getType()->getInOutObjectType());
      LValueOptions options;
      options.IsNonAccessing = pointerExpr->isNonAccessing();
      LValue lv = SGF.emitLValue(arrayExpr, info.AccessKind, options);
      DelayedArguments.emplace_back(info, std::move(lv), pointerExpr,
                                    original);
      Args.push_back(ManagedValue());
      return true;
    }

    // Otherwise, it's an r-value conversion.
    auto info = SGF.getArrayAccessInfo(pointerExpr->getType(),
                                       arrayExpr->getType());

    auto rvalueExpr = lookThroughBindOptionals(arrayExpr);
    ManagedValue value = SGF.emitRValueAsSingleValue(rvalueExpr);
    DelayedArguments.emplace_back(DelayedArgument::RValueArrayToPointer,
                                  info, value, original);
    Args.push_back(ManagedValue());
    return true;
  }

  /// Emit an rvalue-array-to-pointer conversion as a delayed argument.
  bool emitDelayedConversion(StringToPointerExpr *pointerExpr,
                             OriginalArgument original) {
    auto rvalueExpr = lookThroughBindOptionals(pointerExpr->getSubExpr());
    ManagedValue value = SGF.emitRValueAsSingleValue(rvalueExpr);
    DelayedArguments.emplace_back(DelayedArgument::RValueStringToPointer,
                                  value, original);
    Args.push_back(ManagedValue());
    return true;
  }
  
  bool emitDelayedConversion(FunctionConversionExpr *funcConv,
                             OriginalArgument original) {
    auto rvalueExpr = lookThroughBindOptionals(funcConv->getSubExpr());
    ManagedValue value = SGF.emitRValueAsSingleValue(rvalueExpr);
    DelayedArguments.emplace_back(DelayedArgument::FunctionConversion,
                                  value, original);
    Args.push_back(ManagedValue());
    return true;
  }

  static Expr *lookThroughBindOptionals(Expr *expr) {
    while (true) {
      expr = expr->getSemanticsProvidingExpr();
      if (auto bind = dyn_cast<BindOptionalExpr>(expr)) {
        expr = bind->getSubExpr();
      } else {
        return expr;
      }
    }
  }

  ManagedValue emitConvertedArgument(ArgumentSource &&arg,
                                     Conversion conversion,
                                     SGFContext C) {
    auto loc = arg.getLocation();
    Scope scope(SGF, loc);

    // TODO: honor C here.
    auto result = std::move(arg).getConverted(SGF, conversion);

    return scope.popPreservingValue(result);
  }
  
  void maybeEmitForeignArgument() {
    bool keepGoing = true;
    while (keepGoing) {
      keepGoing = false;
      if (Foreign.async &&
          Foreign.async->completionHandlerParamIndex() == Args.size()) {
        SILParameterInfo param = claimNextParameters(1).front();
        (void)param;

        // Leave a placeholder in the position. We'll fill this in with a block
        // capturing the current continuation right before we invoke the
        // function.
        // (We can't do this immediately, because evaluating other arguments
        // may require suspending the async task, which is not allowed while its
        // continuation is active.)
        Args.push_back(ManagedValue::forInContext());
        keepGoing = true;
      }
      if (Foreign.error &&
          Foreign.error->getErrorParameterIndex() == Args.size()) {
        SILParameterInfo param = claimNextParameters(1).front();
        assert(param.getConvention() == ParameterConvention::Direct_Unowned);
        (void)param;

        // Leave a placeholder in the position.
        Args.push_back(ManagedValue::forInContext());
        keepGoing = true;
      }
    }
  }

  struct EmissionContexts {
    /// The context for emitting the r-value.
    SGFContext FinalContext;
    /// If the context requires reabstraction
    bool RequiresReabstraction;
  };
  EmissionContexts getRValueEmissionContexts(SILType loweredArgType,
                                             SILParameterInfo param) {
    bool requiresReabstraction = loweredArgType.getASTType()
      != param.getInterfaceType();
    // If the parameter is consumed, we have to emit at +1.
    if (param.isConsumed()) {
      return {SGFContext(), requiresReabstraction};
    }

    // Otherwise, we can emit the final value at +0 (but only with a
    // guarantee that the value will survive).
    //
    // TODO: we can pass at +0 (immediate) to an unowned parameter
    // if we know that there will be no arbitrary side-effects
    // between now and the call.
    return {SGFContext::AllowGuaranteedPlusZero, requiresReabstraction};
  }
};

void DelayedArgument::emitDefaultArgument(SILGenFunction &SGF,
                                          const DefaultArgumentStorage &info,
                                          SmallVectorImpl<ManagedValue> &args,
                                          size_t &argIndex) {
  auto value = SGF.emitApplyOfDefaultArgGenerator(info.loc,
                                                  info.defaultArgsOwner,
                                                  info.destIndex,
                                                  info.resultType);

  SmallVector<ManagedValue, 4> loweredArgs;
  SmallVector<DelayedArgument, 4> delayedArgs;
  auto emitter = ArgEmitter(SGF, info.loc, info.functionRepresentation,
                            /*yield*/ false, /*coroutine*/ false,
                            info.paramsToEmit, loweredArgs,
                            delayedArgs, ForeignInfo{});

  emitter.emitSingleArg(ArgumentSource(info.loc, std::move(value)),
                        info.origResultType);
  assert(delayedArgs.empty());
  
  // Splice the emitted default argument into the argument list.
  if (loweredArgs.size() == 1) {
    args[argIndex++] = loweredArgs.front();
  } else {
    args.erase(args.begin() + argIndex);
    args.insert(args.begin() + argIndex,
                loweredArgs.begin(), loweredArgs.end());
    argIndex += loweredArgs.size();
  }
}

static void emitBorrowedLValueRecursive(SILGenFunction &SGF,
                                        SILLocation loc,
                                        ManagedValue value,
                                        AbstractionPattern origParamType,
                                        ClaimedParamsRef &params,
                                        MutableArrayRef<ManagedValue> args,
                                        size_t &argIndex) {
  // Recurse into tuples.
  if (origParamType.isTuple()) {
    size_t count = origParamType.getNumTupleElements();
    for (size_t i = 0; i != count; ++i) {
      // Drill down to the element, either by address or by scalar extraction.
      ManagedValue eltValue;
      if (value.getType().isAddress()) {
        eltValue = SGF.B.createTupleElementAddr(loc, value, i);
      } else {
        eltValue = SGF.B.createTupleExtract(loc, value, i);
      }

      // Recurse.
      auto origEltType = origParamType.getTupleElementType(i);
      emitBorrowedLValueRecursive(SGF, loc, eltValue, origEltType,
                                  params, args, argIndex);
    }

    return;
  }

  // Claim the next parameter.
  auto param = params.front();
  params = params.slice(1);

  // Load if necessary.
  if (value.getType().isAddress()) {
    if (!param.isIndirectInGuaranteed() || !SGF.silConv.useLoweredAddresses()) {
      if (value.getType().isMoveOnly()) {
        // We use a formal access load [copy] instead of a load_borrow here
        // since in the case where we have a second parameter that is consuming,
        // we want to avoid emitting invalid SIL and instead allow for the
        // exclusivity checker to emit an error due to the conflicting access
        // scopes.
        //
        // NOTE: We are not actually hurting codegen here since due to the way
        // scopes are layered, the destroy_value of the load [copy] is
        // guaranteed to be within the access scope meaning that it is easy for
        // SIL passes like the move only checker to convert this to a
        // load_borrow.
        value = SGF.B.createFormalAccessLoadCopy(loc, value);
        // Strip off the cleanup from the load [copy] since we do not want the
        // cleanup to be forwarded.
        value = ManagedValue::forUnmanaged(value.getValue());
      } else {
        value = SGF.B.createFormalAccessLoadBorrow(loc, value);
      }
    }
  }

  // TODO: This does not take into account resilience, we should probably use
  // getArgumentType()... but we do not have the SILFunctionType here...
  assert(param.getInterfaceType() == value.getType().getASTType());

  // If we have an indirect_guaranteed argument, move this using store_borrow
  // into an alloc_stack.
  if (SGF.silConv.useLoweredAddresses() &&
      param.isIndirectInGuaranteed() && value.getType().isObject()) {
    SILValue alloca = SGF.emitTemporaryAllocation(loc, value.getType());
    value = SGF.emitFormalEvaluationManagedStoreBorrow(loc, value.getValue(),
                                                       alloca);
  }

  args[argIndex++] = value;
}

void DelayedArgument::emitBorrowedLValue(SILGenFunction &SGF,
                                         BorrowedLValueStorage &info,
                                         SmallVectorImpl<ManagedValue> &args,
                                         size_t &argIndex) {
  // Begin the access.
  auto value = SGF.emitBorrowedLValue(info.Loc, std::move(info.LV));
  ClaimedParamsRef params = info.ParamsToEmit;

  // We inserted exactly one space in the argument array, so fix that up
  // to have the right number of spaces.
  if (params.size() == 0) {
    args.erase(args.begin() + argIndex);
    return;
  } else if (params.size() > 1) {
    args.insert(args.begin() + argIndex + 1, params.size() - 1, ManagedValue());
  }

  // Recursively expand.
  emitBorrowedLValueRecursive(SGF, info.Loc, value, info.OrigParamType,
                              params, args, argIndex);

  // That should drain all the parameters.
  assert(params.empty());
}

static void emitConsumedLValueRecursive(SILGenFunction &SGF, SILLocation loc,
                                        ManagedValue value,
                                        AbstractionPattern origParamType,
                                        ClaimedParamsRef &params,
                                        MutableArrayRef<ManagedValue> args,
                                        size_t &argIndex) {
  // Recurse into tuples.
  if (origParamType.isTuple()) {
    SGF.B.emitDestructureOperation(
        loc, value, [&](unsigned eltIndex, ManagedValue eltValue) {
          auto origEltType = origParamType.getTupleElementType(eltIndex);
          // Recurse.
          emitConsumedLValueRecursive(SGF, loc, eltValue, origEltType, params,
                                      args, argIndex);
        });
    return;
  }

  // Claim the next parameter.
  auto param = params.front();
  params = params.slice(1);

  // Load if necessary.
  if (value.getType().isAddress()) {
    if (!param.isIndirectIn() || !SGF.silConv.useLoweredAddresses()) {
      value = SGF.B.createFormalAccessLoadTake(loc, value);

      // If our value is a moveonlywrapped type, unwrap it using owned so that
      // we consume it.
      if (value.getType().isMoveOnlyWrapped()) {
        value = SGF.B.createOwnedMoveOnlyWrapperToCopyableValue(loc, value);
      }
    }
  }

  assert(param.getInterfaceType() == value.getType().getASTType());
  args[argIndex++] = value;
}

void DelayedArgument::emitConsumedLValue(SILGenFunction &SGF,
                                         ConsumedLValueStorage &info,
                                         SmallVectorImpl<ManagedValue> &args,
                                         size_t &argIndex) {
  // Begin the access.
  ManagedValue value = SGF.emitConsumedLValue(info.Loc, std::move(info.LV));
  ClaimedParamsRef params = info.ParamsToEmit;

  // We inserted exactly one space in the argument array, so fix that up
  // to have the right number of spaces.
  if (params.size() == 0) {
    args.erase(args.begin() + argIndex);
    return;
  } else if (params.size() > 1) {
    args.insert(args.begin() + argIndex + 1, params.size() - 1, ManagedValue());
  }

  // Recursively expand.
  emitConsumedLValueRecursive(SGF, info.Loc, value, info.OrigParamType, params,
                              args, argIndex);

  // That should drain all the parameters.
  assert(params.empty());
}

} // end anonymous namespace

namespace {
/// Cleanup to destroy an uninitialized box.
class DeallocateUninitializedBox : public Cleanup {
  SILValue box;
public:
  DeallocateUninitializedBox(SILValue box) : box(box) {}

  void emit(SILGenFunction &SGF, CleanupLocation l, ForUnwind_t forUnwind) override {
    auto theBox = box;
    if (SGF.getASTContext().SILOpts.supportsLexicalLifetimes(SGF.getModule())) {
      if (auto *bbi = cast<BeginBorrowInst>(theBox)) {
        SGF.B.createEndBorrow(l, bbi);
        theBox = bbi->getOperand();
      }
    }
    SGF.B.createDeallocBox(l, theBox);
  }

  void dump(SILGenFunction &SGF) const override {
#ifndef NDEBUG
    llvm::errs() << "DeallocateUninitializedBox "
                 << "State:" << getState() << " "
                 << "Box: " << box << "\n";
#endif
  }
};
} // end anonymous namespace

CleanupHandle SILGenFunction::enterDeallocBoxCleanup(SILValue box) {
  Cleanups.pushCleanup<DeallocateUninitializedBox>(box);
  return Cleanups.getTopCleanup();
}

/// This is an initialization for a box.
class BoxInitialization : public SingleBufferInitialization {
  SILValue box;
  SILValue addr;
  CleanupHandle uninitCleanup;
  CleanupHandle initCleanup;

public:
  BoxInitialization(SILValue box, SILValue addr,
                    CleanupHandle uninitCleanup,
                    CleanupHandle initCleanup)
    : box(box), addr(addr),
      uninitCleanup(uninitCleanup),
      initCleanup(initCleanup) {}

  void finishInitialization(SILGenFunction &SGF) override {
    SingleBufferInitialization::finishInitialization(SGF);
    SGF.Cleanups.setCleanupState(uninitCleanup, CleanupState::Dead);
    if (initCleanup.isValid())
        SGF.Cleanups.setCleanupState(initCleanup, CleanupState::Active);
  }

  SILValue getAddressForInPlaceInitialization(SILGenFunction &SGF,
                                              SILLocation loc) override {
    return addr;
  }

  bool isInPlaceInitializationOfGlobal() const override {
    return false;
  }

  ManagedValue getManagedBox() const {
    return ManagedValue(box, initCleanup);
  }
};

namespace {

/// A structure for conveniently claiming sets of uncurried parameters.
struct ParamLowering {
  ArrayRef<SILParameterInfo> Params;
  unsigned ClaimedForeignSelf = -1;
  SILFunctionTypeRepresentation Rep;
  SILFunctionConventions fnConv;
  TypeExpansionContext typeExpansionContext;

  ParamLowering(CanSILFunctionType fnType, SILGenFunction &SGF)
      : Params(fnType->getUnsubstitutedType(SGF.SGM.M)->getParameters()),
        Rep(fnType->getRepresentation()), fnConv(fnType, SGF.SGM.M),
        typeExpansionContext(SGF.getTypeExpansionContext()) {}

  ClaimedParamsRef claimParams(AbstractionPattern origFormalType,
                               ArrayRef<AnyFunctionType::Param> substParams,
                               const ForeignInfo &foreign) {
    unsigned count = 0;
    if (!foreign.self.isStatic()) {
      size_t numOrigFormalParams =
        (origFormalType.isTypeParameter()
           ? substParams.size()
           : origFormalType.getNumFunctionParams());

      size_t nextSubstParamIndex = 0;
      for (size_t i = 0; i != numOrigFormalParams; ++i) {
        auto origParamType = origFormalType.getFunctionParamType(i);
        if (origParamType.isPackExpansion()) {
          count++;
          nextSubstParamIndex += origParamType.getNumPackExpandedComponents();
        } else {
          auto substParam = substParams[nextSubstParamIndex++];
          if (substParam.isInOut()) {
            count += 1;
          } else {
            count += getFlattenedValueCount(origParamType,
                                            ImportAsMemberStatus());
          }
        }
      }
      assert(nextSubstParamIndex == substParams.size());
    }

    if (foreign.error)
      ++count;
    if (foreign.async)
      ++count;

    if (foreign.self.isImportAsMember()) {
      // Claim only the self parameter.
      assert(ClaimedForeignSelf == (unsigned)-1 &&
             "already claimed foreign self?!");
      if (foreign.self.isStatic()) {
        // Imported as a static method, no real self param to claim.
        return {};
      }
      ClaimedForeignSelf = foreign.self.getSelfIndex();
      return ClaimedParamsRef(Params[ClaimedForeignSelf],
                              ClaimedParamsRef::NoSkip);
    }

    if (ClaimedForeignSelf != (unsigned)-1) {
      assert(count + 1 == Params.size() &&
             "not claiming all params after foreign self?!");
      auto result = Params;
      Params = {};
      return ClaimedParamsRef(result, ClaimedForeignSelf);
    }

    assert(count <= Params.size());
    auto result = Params.slice(Params.size() - count, count);
    Params = Params.slice(0, Params.size() - count);
    return ClaimedParamsRef(result, (unsigned)-1);
  }

  ArrayRef<SILParameterInfo>
  claimCaptureParams(ArrayRef<ManagedValue> captures) {
    auto firstCapture = Params.size() - captures.size();
#ifndef NDEBUG
    assert(Params.size() >= captures.size() && "more captures than params?!");
    for (unsigned i = 0; i < captures.size(); ++i) {
      assert(fnConv.getSILType(Params[i + firstCapture],
                               typeExpansionContext) == captures[i].getType() &&
             "capture doesn't match param type");
    }
#endif

    auto result = Params.slice(firstCapture, captures.size());
    Params = Params.slice(0, firstCapture);
    return result;
  }

  ~ParamLowering() {
    assert(Params.empty() && "didn't consume all the parameters");
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                                  CallSite
//===----------------------------------------------------------------------===//

namespace {

/// An application of possibly unevaluated arguments in the form of an
/// ArgumentSource to a Callee.
class CallSite {
public:
  SILLocation Loc;

private:
  PreparedArguments Args;
  /// Is this a 'rethrows' function that is known not to throw?
  bool NoThrows;
  /// Is this a 'reasync' function that is known not to 'await'?
  bool NoAsync;

public:
  CallSite(SILLocation loc, PreparedArguments &&args,
           bool isNoThrows=false, bool isNoAsync=false)
      : Loc(loc), Args(std::move(args)),
        NoThrows(isNoThrows), NoAsync(isNoAsync) {
    assert(Args.isValid());
  }

  /// Return the substituted, unlowered AST parameter types of the argument.
  ArrayRef<AnyFunctionType::Param> getParams() const { return Args.getParams(); }

  bool isNoThrows() const { return NoThrows; }

  bool isNoAsync() const { return NoAsync; }

  /// Evaluate arguments and begin any inout formal accesses.
  void emit(SILGenFunction &SGF, AbstractionPattern origFormalType,
            CanSILFunctionType substFnType, ParamLowering &lowering,
            SmallVectorImpl<ManagedValue> &args,
            SmallVectorImpl<DelayedArgument> &delayedArgs,
            const ForeignInfo &foreign) && {
    auto params = lowering.claimParams(origFormalType, getParams(), foreign);

    ArgEmitter emitter(SGF, Loc, lowering.Rep, /*yield*/ false,
                       /*isForCoroutine*/ substFnType->isCoroutine(), params,
                       args, delayedArgs, foreign);
    emitter.emitPreparedArgs(std::move(Args), origFormalType);
  }

  /// Take the arguments for special processing, in place of the above.
  PreparedArguments &&forward() && {
    return std::move(Args);
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                                CallEmission
//===----------------------------------------------------------------------===//

namespace {

/// Once the Callee and CallSites have been prepared by SILGenApply,
/// generate SIL for a fully-formed call.
///
/// The lowered function type of the callee defines an abstraction pattern
/// for evaluating argument values of tuple type directly into explosions of
/// scalars where possible.
///
/// If there are more call sites than the natural uncurry level, they are
/// have to be applied recursively to each intermediate callee.
///
/// Also inout formal access and parameter and result conventions are
/// handled here, with some special logic required for calls with +0 self.
class CallEmission {
  SILGenFunction &SGF;

  llvm::Optional<CallSite> selfArg;
  llvm::Optional<CallSite> callSite;

  Callee callee;
  FormalEvaluationScope initialWritebackScope;
  llvm::Optional<ActorIsolation> implicitActorHopTarget;
  bool implicitlyThrows;

public:
  /// Create an emission for a call of the given callee.
  CallEmission(SILGenFunction &SGF, Callee &&callee,
               FormalEvaluationScope &&writebackScope)
      : SGF(SGF), callee(std::move(callee)),
        initialWritebackScope(std::move(writebackScope)),
        implicitActorHopTarget(llvm::None), implicitlyThrows(false) {}

  /// A factory method for decomposing the apply expr \p e into a call
  /// emission.
  static CallEmission forApplyExpr(SILGenFunction &SGF, ApplyExpr *e);

  /// Add a level of function application by passing in its possibly
  /// unevaluated arguments and their formal type.
  void addCallSite(CallSite &&site) {
    // Append to the main argument list if we have uncurry levels remaining.
    assert(!callSite.has_value());
    callSite = std::move(site);
  }

  /// Add a level of function application by passing in its possibly
  /// unevaluated arguments and their formal type
  template<typename...T>
  void addCallSite(T &&...args) {
    addCallSite(CallSite{std::forward<T>(args)...});
  }

  void addSelfParam(SILLocation loc,
                    ArgumentSource &&self,
                    AnyFunctionType::Param selfParam) {
    assert(!selfArg.has_value());

    PreparedArguments preparedSelf(llvm::ArrayRef<AnyFunctionType::Param>{selfParam});
    preparedSelf.addArbitrary(std::move(self));

    selfArg = CallSite(loc, std::move(preparedSelf));
  }

  /// Is this a fully-applied enum element constructor call?
  bool isEnumElementConstructor() {
    return (callee.kind == Callee::Kind::EnumElement);
  }

  /// Sets a flag that indicates whether this call be treated as being 
  /// implicitly async, i.e., it requires a hop_to_executor prior to 
  /// invoking the sync callee, etc.
  void
  setImplicitlyAsync(llvm::Optional<ActorIsolation> implicitActorHopTarget) {
    this->implicitActorHopTarget = implicitActorHopTarget;
  }

  /// Sets a flag that indicates whether this call be treated as being
  /// implicitly throws, i.e., the call may be delegating to a proxy function
  /// which actually is throwing, regardless whether or not the actual target
  /// function can throw or not.
  void setImplicitlyThrows(bool flag) { implicitlyThrows = flag; }

  CleanupHandle applyCoroutine(SmallVectorImpl<ManagedValue> &yields);

  RValue apply(SGFContext C = SGFContext()) {
    initialWritebackScope.verify();

    // Emit the first level of call.
    auto value = applyFirstLevelCallee(C);

    // End of the initial writeback scope.
    // FIXME: Unnecessary?
    initialWritebackScope.verify();
    initialWritebackScope.pop();

    return value;
  }

  // Movable, but not copyable.
  CallEmission(CallEmission &&e) = default;

private:
  CallEmission(const CallEmission &) = delete;
  CallEmission &operator=(const CallEmission &) = delete;

  /// Emit all of the arguments for a normal apply. This means an apply that
  /// is not:
  ///
  /// 1. A specialized emitter (e.g. an emitter for a builtin).
  /// 2. A partially applied super method.
  /// 3. An enum element constructor.
  ///
  /// It is though all other initial calls and subsequent callees that we feed
  /// the first callee into.
  ///
  /// This returns whether or not any arguments were able to throw in
  /// ApplyOptions.
  ApplyOptions emitArgumentsForNormalApply(
      AbstractionPattern origFormalType, CanSILFunctionType substFnType,
      const ForeignInfo &foreign, SmallVectorImpl<ManagedValue> &uncurriedArgs,
      llvm::Optional<SILLocation> &uncurriedLoc);

  RValue
  applySpecializedEmitter(SpecializedEmitter &specializedEmitter, SGFContext C);

  RValue applyEnumElementConstructor(SGFContext C);

  RValue applyNormalCall(SGFContext C);

  RValue applyFirstLevelCallee(SGFContext C);
};

} // end anonymous namespace

namespace {
/// Cleanup to end a coroutine application.
class EndCoroutineApply : public Cleanup {
  SILValue ApplyToken;
public:
  EndCoroutineApply(SILValue applyToken) : ApplyToken(applyToken) {}

  void emit(SILGenFunction &SGF, CleanupLocation l, ForUnwind_t forUnwind) override {
    if (forUnwind) {
      SGF.B.createAbortApply(l, ApplyToken);
    } else {
      SGF.B.createEndApply(l, ApplyToken);
    }
  }

  void dump(SILGenFunction &SGF) const override {
#ifndef NDEBUG
    llvm::errs() << "EndCoroutineApply "
                 << "State:" << getState() << " "
                 << "Token: " << ApplyToken << "\n";
#endif
  }
};
}

CleanupHandle
CallEmission::applyCoroutine(SmallVectorImpl<ManagedValue> &yields) {
  auto origFormalType = callee.getOrigFormalType();

  // Get the callee type information.
  auto calleeTypeInfo = callee.getTypeInfo(SGF);

  SmallVector<ManagedValue, 4> uncurriedArgs;
  llvm::Optional<SILLocation> uncurriedLoc;

  // Evaluate the arguments.
  ApplyOptions options = emitArgumentsForNormalApply(
      origFormalType, calleeTypeInfo.substFnType,
      calleeTypeInfo.foreign, uncurriedArgs,
      uncurriedLoc);

  // Now evaluate the callee.
  llvm::Optional<ManagedValue> borrowedSelf;
  if (callee.requiresSelfValueForDispatch()) {
    borrowedSelf = uncurriedArgs.back();
  }

  auto fnValue = callee.getFnValue(SGF, borrowedSelf);

  return SGF.emitBeginApply(uncurriedLoc.value(), fnValue,
                            callee.getSubstitutions(), uncurriedArgs,
                            calleeTypeInfo.substFnType, options, yields);
}

CleanupHandle
SILGenFunction::emitBeginApply(SILLocation loc, ManagedValue fn,
                               SubstitutionMap subs,
                               ArrayRef<ManagedValue> args,
                               CanSILFunctionType substFnType,
                               ApplyOptions options,
                               SmallVectorImpl<ManagedValue> &yields) {
  // Emit the call.
  SmallVector<SILValue, 4> rawResults;
  emitRawApply(*this, loc, fn, subs, args, substFnType, options,
               /*indirect results*/ {}, rawResults, ExecutorBreadcrumb());

  auto token = rawResults.pop_back_val();
  auto yieldValues = llvm::makeArrayRef(rawResults);

  // Push a cleanup to end the application.
  // TODO: destroy all the arguments at exactly this point?
  Cleanups.pushCleanup<EndCoroutineApply>(token);
  auto endApplyHandle = getTopCleanup();

  // Manage all the yielded values.
  auto yieldInfos = substFnType->getYields();
  assert(yieldValues.size() == yieldInfos.size());
  bool useLoweredAddresses = silConv.useLoweredAddresses();
  for (auto i : indices(yieldValues)) {
    auto value = yieldValues[i];
    auto info = yieldInfos[i];
    if (info.isIndirectInOut()) {
      yields.push_back(ManagedValue::forLValue(value));
    } else if (info.isConsumed()) {
      !useLoweredAddresses && value->getType().isTrivial(getFunction())
          ? yields.push_back(ManagedValue::forTrivialRValue(value))
          : yields.push_back(emitManagedRValueWithCleanup(value));
    } else if (info.isGuaranteed()) {
      !useLoweredAddresses && value->getType().isTrivial(getFunction())
          ? yields.push_back(ManagedValue::forTrivialRValue(value))
          : yields.push_back(ManagedValue::forBorrowedRValue(value));
    } else {
      yields.push_back(ManagedValue::forTrivialRValue(value));
    }
  }

  return endApplyHandle;
}

RValue CallEmission::applyFirstLevelCallee(SGFContext C) {
  // Check for a specialized emitter.
  if (auto emitter = callee.getSpecializedEmitter(SGF.SGM)) {
    return applySpecializedEmitter(emitter.value(), C);
  }

  if (isEnumElementConstructor()) {
    return applyEnumElementConstructor(C);
  }

  return applyNormalCall(C);
}

RValue CallEmission::applyNormalCall(SGFContext C) {
  // We use the context emit-into initialization only for the
  // outermost call.
  SGFContext uncurriedContext = C;

  auto formalType = callee.getSubstFormalType();
  auto origFormalType = callee.getOrigFormalType();

  // Get the callee type information.
  auto calleeTypeInfo = callee.getTypeInfo(SGF);

  calleeTypeInfo.origFormalType = origFormalType;

  // In C language modes, substitute the type of the AbstractionPattern
  // so that we won't see type parameters down when we try to form bridging
  // conversions.
  if (calleeTypeInfo.substFnType->getLanguage() == SILFunctionLanguage::C) {
    if (auto genericFnType =
          dyn_cast<GenericFunctionType>(origFormalType.getType())) {
      auto fnType = genericFnType->substGenericArgs(callee.getSubstitutions());
      origFormalType.rewriteType(CanGenericSignature(),
                                 fnType->getCanonicalType());
    }
  }

  // Initialize the rest of the call info.
  calleeTypeInfo.origResultType = origFormalType.getFunctionResultType();
  calleeTypeInfo.substResultType = formalType.getResult();

  if (selfArg.has_value() && callSite.has_value()) {
    calleeTypeInfo.origFormalType =
        calleeTypeInfo.origFormalType->getFunctionResultType();
    calleeTypeInfo.origResultType =
      calleeTypeInfo.origResultType->getFunctionResultType();
    calleeTypeInfo.substResultType =
      cast<FunctionType>(calleeTypeInfo.substResultType).getResult();
  }

  ResultPlanPtr resultPlan = ResultPlanBuilder::computeResultPlan(
      SGF, calleeTypeInfo, callSite->Loc, uncurriedContext);

  ArgumentScope argScope(SGF, callSite->Loc);

  // Emit the arguments.
  SmallVector<ManagedValue, 4> uncurriedArgs;
  llvm::Optional<SILLocation> uncurriedLoc;
  CanFunctionType formalApplyType;

  // *NOTE* We pass in initial options as a reference so that we can pass to
  // emitApply if any of the arguments could have thrown.
  ApplyOptions options = emitArgumentsForNormalApply(
      origFormalType, calleeTypeInfo.substFnType,
      calleeTypeInfo.foreign, uncurriedArgs,
      uncurriedLoc);

  // Now evaluate the callee.
  llvm::Optional<ManagedValue> borrowedSelf;
  if (callee.requiresSelfValueForDispatch()) {
    borrowedSelf = uncurriedArgs.back();
  }

  auto mv = callee.getFnValue(SGF, borrowedSelf);

  // Emit the uncurried call.
  return SGF.emitApply(
      std::move(resultPlan), std::move(argScope), uncurriedLoc.value(), mv,
      callee.getSubstitutions(), uncurriedArgs, calleeTypeInfo, options,
      uncurriedContext, implicitActorHopTarget);
}

static void emitPseudoFunctionArguments(SILGenFunction &SGF,
                                        SILLocation applyLoc,
                                        AbstractionPattern origFnType,
                                        CanFunctionType substFnType,
                                        SmallVectorImpl<ManagedValue> &outVals,
                                        PreparedArguments &&args);

RValue CallEmission::applyEnumElementConstructor(SGFContext C) {
  SGFContext uncurriedContext = C;

  // Get the callee type information.
  //
  // Enum payloads are always stored at the abstraction level of the
  // unsubstituted payload type. This means that unlike with specialized
  // emitters above, enum constructors use the AST-level abstraction
  // pattern, to ensure that function types in payloads are re-abstracted
  // correctly.
  auto formalType = callee.getSubstFormalType();
  auto origFormalType = callee.getOrigFormalType();

  // We have a fully-applied enum element constructor: open-code the
  // construction.
  EnumElementDecl *element = callee.getEnumElementDecl();

  SILLocation uncurriedLoc = selfArg->Loc;

  origFormalType = origFormalType.getFunctionResultType();
  CanType formalResultType = formalType.getResult();

  // Ignore metatype argument
  SmallVector<ManagedValue, 0> metatypeVal;
  emitPseudoFunctionArguments(SGF, uncurriedLoc,
                              AbstractionPattern(formalType),
                              formalType, metatypeVal,
                              std::move(*selfArg).forward());
  assert(metatypeVal.size() == 1);


  // Get the payload argument.
  ArgumentSource payload;
  if (element->hasAssociatedValues()) {
    SmallVector<ManagedValue, 4> argVals;
    auto resultFnType = cast<FunctionType>(formalResultType);

    emitPseudoFunctionArguments(SGF, uncurriedLoc,
                                AbstractionPattern(resultFnType),
                                resultFnType, argVals,
                                std::move(*callSite).forward());

    // We need to implode a tuple rvalue for enum construction. This is
    // essentially an implosion of the internal arguments of a pseudo case
    // constructor, so we can drop the parameter flags.
    auto payloadTy = AnyFunctionType::composeTuple(
        SGF.getASTContext(), resultFnType->getParams(),
        ParameterFlagHandling::IgnoreNonEmpty);

    auto arg = RValue(SGF, argVals, payloadTy->getCanonicalType());
    payload = ArgumentSource(uncurriedLoc, std::move(arg));
    formalResultType = cast<FunctionType>(formalResultType).getResult();
    origFormalType = origFormalType.getFunctionResultType();
  } else {
    assert(!callSite.has_value());
  }

  ManagedValue resultMV = SGF.emitInjectEnum(
      uncurriedLoc, std::move(payload),
      SGF.getLoweredType(formalResultType),
      element, uncurriedContext);

  return RValue(SGF, uncurriedLoc, formalResultType, resultMV);
}

RValue
CallEmission::applySpecializedEmitter(SpecializedEmitter &specializedEmitter,
                                      SGFContext C) {
  // We use the context emit-into initialization only for the
  // outermost call.
  SGFContext uncurriedContext = C;

  ManagedValue mv;

  // Get the callee type information. We want to emit the arguments as
  // fully-substituted values because that's what the specialized emitters
  // expect.
  auto formalType = callee.getSubstFormalType();
  auto origFormalType = AbstractionPattern(formalType);
  auto substFnType = SGF.getSILFunctionType(
      SGF.getTypeExpansionContext(), origFormalType, formalType);

  CanType formalResultType = formalType.getResult();

  // If we have an early emitter, just let it take over for the
  // uncurried call site.
  if (specializedEmitter.isEarlyEmitter()) {
    auto emitter = specializedEmitter.getEarlyEmitter();

    assert(!selfArg.has_value());
    assert(!formalType->getExtInfo().isThrowing());
    SILLocation uncurriedLoc = callSite->Loc;

    // We should be able to enforce that these arguments are
    // always still expressions.
    PreparedArguments args = std::move(*callSite).forward();
    ManagedValue resultMV =
        emitter(SGF, uncurriedLoc, callee.getSubstitutions(),
                std::move(args), uncurriedContext);
    return RValue(SGF, uncurriedLoc, formalResultType, resultMV);
  }

  llvm::Optional<ResultPlanPtr> resultPlan;
  llvm::Optional<ArgumentScope> argScope;
  llvm::Optional<CalleeTypeInfo> calleeTypeInfo;
  SILLocation loc = callSite->Loc;
  SILFunctionConventions substConv(substFnType, SGF.SGM.M);

  // If we have a named builtin and have an indirect out parameter, compute a
  // result plan/arg scope before we prepare arguments.
  if (!specializedEmitter.isLateEmitter() &&
      substConv.hasIndirectSILResults()) {
    calleeTypeInfo.emplace(callee.getTypeInfo(SGF));

    calleeTypeInfo->origResultType = origFormalType.getFunctionResultType();
    calleeTypeInfo->substResultType = callee.getSubstFormalType().getResult();

    resultPlan.emplace(ResultPlanBuilder::computeResultPlan(
        SGF, *calleeTypeInfo, loc, uncurriedContext));
    argScope.emplace(SGF, loc);
  }

  // Emit the arguments.
  SmallVector<ManagedValue, 4> uncurriedArgs;
  llvm::Optional<SILLocation> uncurriedLoc;
  CanFunctionType formalApplyType;
  emitArgumentsForNormalApply(origFormalType, substFnType, ForeignInfo{},
                              uncurriedArgs, uncurriedLoc);

  // If we have a late emitter, now that we have emitted our arguments, call the
  // emitter.
  if (specializedEmitter.isLateEmitter()) {
    auto emitter = specializedEmitter.getLateEmitter();
    ManagedValue mv = emitter(SGF, loc, callee.getSubstitutions(),
                              uncurriedArgs, uncurriedContext);
    return RValue(SGF, loc, formalResultType, mv);
  }

  // Otherwise, we must have a named builtin.
  assert(specializedEmitter.isNamedBuiltin());
  auto builtinName = specializedEmitter.getBuiltinName();

  // Prepare our raw args.
  SmallVector<SILValue, 4> rawArgs;

  // First get the indirect result addrs and add them to rawArgs. We want to be
  // able to handle them specially later as well, so we keep them in two arrays.
  if (resultPlan.has_value())
    (*resultPlan)->gatherIndirectResultAddrs(SGF, loc, rawArgs);

  // Then add all arguments to our array, copying them if they are not at +1
  // yet.
  for (auto arg : uncurriedArgs) {
    // Nonescaping closures can't be forwarded so we pass them +0.
    auto argFnTy = arg.getType().getAs<SILFunctionType>();
    if (argFnTy && argFnTy->isTrivialNoEscape()) {
      rawArgs.push_back(arg.getValue());
    } else {
      // Named builtins are by default assumed to take other arguments at +1,
      // as Owned or Trivial. Named builtins that don't follow this convention
      // must use a specialized emitter.
      auto maybePlusOne = arg.ensurePlusOne(SGF, loc);
      rawArgs.push_back(maybePlusOne.forward(SGF));
    }
  }

  SILValue rawResult = SGF.B.createBuiltin(
      loc, builtinName,
      substConv.getSILResultType(SGF.getTypeExpansionContext()),
      callee.getSubstitutions(), rawArgs);

  if (argScope.has_value())
    std::move(argScope)->pop();

  // If we have a direct result, it will consist of a single value even if
  // formally we have multiple values. We could handle this better today by
  // using multiple return values instead of a tuple.
  SmallVector<ManagedValue, 1> directResultsArray;
  if (!substConv.hasIndirectSILResults()) {
    directResultsArray.push_back(SGF.emitManagedRValueWithCleanup(rawResult));
  }

  ArrayRef<ManagedValue> directResultsFinal(directResultsArray);

  // Then finish our value.
  if (resultPlan.has_value()) {
    return std::move(*resultPlan)
        ->finish(SGF, loc, directResultsFinal, SILValue());
  } else {
    return RValue(
        SGF, *uncurriedLoc, formalResultType, directResultsFinal[0]);
  }
}

ApplyOptions CallEmission::emitArgumentsForNormalApply(
    AbstractionPattern origFormalType, CanSILFunctionType substFnType,
    const ForeignInfo &foreign, SmallVectorImpl<ManagedValue> &uncurriedArgs,
    llvm::Optional<SILLocation> &uncurriedLoc) {
  ApplyOptions options;

  SmallVector<SmallVector<ManagedValue, 4>, 2> args;
  SmallVector<DelayedArgument, 2> delayedArgs;

  args.reserve(selfArg.has_value() ? 2 : 1);
  {
    ParamLowering paramLowering(substFnType, SGF);

    assert((!foreign.error && !foreign.async)
           || !selfArg.has_value()
           || (selfArg.has_value() && substFnType->hasSelfParam()));

    if (callSite->isNoThrows()) {
      options |= ApplyFlags::DoesNotThrow;
    }
    if (callSite->isNoAsync()) {
      options |= ApplyFlags::DoesNotAwait;
    }

    // Collect the captures, if any.
    if (callee.hasCaptures()) {
      (void)paramLowering.claimCaptureParams(callee.getCaptures());
      args.push_back({});
      args.back().append(callee.getCaptures().begin(),
                         callee.getCaptures().end());
    }

    // Collect the arguments to the uncurried call.
    if (selfArg.has_value()) {
      args.push_back({});
      
      // Claim the foreign "self" with the self param.
      auto siteForeign = ForeignInfo{foreign.self, {}, {}};
      std::move(*selfArg).emit(SGF, origFormalType, substFnType, paramLowering,
                               args.back(), delayedArgs,
                               siteForeign);

      origFormalType = origFormalType.getFunctionResultType();
    }

    args.push_back({});

    // Claim the foreign error and/or async arguments when claiming the formal
    // params.
    auto siteForeignError = ForeignInfo{{}, foreign.error, foreign.async};
    // Claim the method formal params.
    std::move(*callSite).emit(SGF, origFormalType, substFnType, paramLowering,
                              args.back(), delayedArgs, siteForeignError);
  }

  uncurriedLoc = callSite->Loc;

  // Emit any delayed arguments: formal accesses to inout arguments, etc.
  if (!delayedArgs.empty()) {
    emitDelayedArguments(SGF, delayedArgs, args);
  }

  // Uncurry the arguments in calling convention order.
  for (auto &argSet : llvm::reverse(args))
    uncurriedArgs.append(argSet.begin(), argSet.end());
  args = {};

  // Move the foreign "self" argument into position.
  if (foreign.self.isInstance()) {
    auto selfArg = uncurriedArgs.back();
    std::move_backward(uncurriedArgs.begin() + foreign.self.getSelfIndex(),
                       uncurriedArgs.end() - 1, uncurriedArgs.end());
    uncurriedArgs[foreign.self.getSelfIndex()] = selfArg;
  }

  return options;
}

CallEmission CallEmission::forApplyExpr(SILGenFunction &SGF, ApplyExpr *e) {
  // Set up writebacks for the call(s).
  FormalEvaluationScope writebacks(SGF);

  SILGenApply apply(SGF);

  // Decompose the call site.
  apply.decompose(e);

  // Evaluate and discard the side effect if present.
  if (apply.sideEffect)
    SGF.emitRValue(apply.sideEffect);

  // Build the call.
  // Pass the writeback scope on to CallEmission so it can thread scopes through
  // nested calls.
  CallEmission emission(SGF, apply.getCallee(), std::move(writebacks));

  // Apply 'self' if provided.
  if (apply.selfParam) {
    auto substFormalType = apply.getCallee().getSubstFormalType();
    auto origSelfParam = substFormalType->getParams().back();
    auto origSelfParamFlags = origSelfParam.getParameterFlags();

    auto newFlags = ParameterTypeFlags();
    if (apply.selfParam.isLValue()) {
      newFlags = newFlags.withInOut(true);
    } else {
      // Transfer to new flags shared or owned if we do not have an lvalue.
      //
      // TODO: I wonder if we can reuse more of the orig self param flags here.
      if (origSelfParamFlags.isOwned())
        newFlags = newFlags.withOwned(true);
      else if (origSelfParamFlags.isShared())
        newFlags = newFlags.withShared(true);
    }

    AnyFunctionType::Param selfParam(apply.selfParam.getSubstRValueType(),
                                     Identifier(), newFlags);
    emission.addSelfParam(e, std::move(apply.selfParam), selfParam);
  }

  // Apply arguments from the actual call site.
  if (auto *call = apply.callSite) {
    auto fnTy = call->getFn()->getType()->castTo<FunctionType>();
    PreparedArguments preparedArgs(fnTy->getParams(), call->getArgs());
    emission.addCallSite(call, std::move(preparedArgs), call->isNoThrows(),
                         call->isNoAsync());

    // For an implicitly-async call, record the target of the actor hop.
    if (auto target = call->isImplicitlyAsync())
      emission.setImplicitlyAsync(target);
  }

  return emission;
}

bool SILGenModule::shouldEmitSelfAsRValue(FuncDecl *fn, CanType selfType) {
  if (fn->isStatic())
    return true;

  switch (fn->getSelfAccessKind()) {
  case SelfAccessKind::Mutating:
    return false;
  case SelfAccessKind::NonMutating:
  case SelfAccessKind::LegacyConsuming:
  case SelfAccessKind::Consuming:
  case SelfAccessKind::Borrowing:
    return true;
  }
  llvm_unreachable("bad self-access kind");
}

bool SILGenModule::isNonMutatingSelfIndirect(SILDeclRef methodRef) {
  auto method = methodRef.getFuncDecl();
  if (method->isStatic())
    return false;

  assert(method->getDeclContext()->isTypeContext());
  assert(!method->isMutating());

  auto fnType = M.Types.getConstantFunctionType(TypeExpansionContext::minimal(),
                                                methodRef);
  auto importAsMember = method->getImportAsMemberStatus();

  SILParameterInfo self;
  if (importAsMember.isImportAsMember()) {
    self = fnType->getParameters()[importAsMember.getSelfIndex()];
  } else {
    self = fnType->getSelfParameter();
  }
  return self.isFormalIndirect();
}

namespace {
/// Cleanup to insert fix_lifetime and destroy
class FixLifetimeDestroyCleanup : public Cleanup {
  SILValue val;

public:
  FixLifetimeDestroyCleanup(SILValue val) : val(val) {}

  void emit(SILGenFunction &SGF, CleanupLocation l,
            ForUnwind_t forUnwind) override {
    SGF.B.emitFixLifetime(l, val);
    SGF.B.emitDestroyOperation(l, val);
  }

  void dump(SILGenFunction &SGF) const override {
#ifndef NDEBUG
    llvm::errs() << "FixLifetimeDestroyCleanup "
                 << "State:" << getState() << " "
                 << "Value: " << val << "\n";
#endif
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                           Top Level Entrypoints
//===----------------------------------------------------------------------===//

/// Emit a function application, assuming that the arguments have been
/// lowered appropriately for the abstraction level but that the
/// result does need to be turned back into something matching a
/// formal type.
RValue SILGenFunction::emitApply(
    ResultPlanPtr &&resultPlan, ArgumentScope &&argScope, SILLocation loc,
    ManagedValue fn, SubstitutionMap subs, ArrayRef<ManagedValue> args,
    const CalleeTypeInfo &calleeTypeInfo, ApplyOptions options,
    SGFContext evalContext,
    llvm::Optional<ActorIsolation> implicitActorHopTarget) {
  auto substFnType = calleeTypeInfo.substFnType;

  // Create the result plan.
  SmallVector<SILValue, 4> indirectResultAddrs;
  resultPlan->gatherIndirectResultAddrs(*this, loc, indirectResultAddrs);

  // If the function returns an inner pointer, we'll need to lifetime-extend
  // the 'self' parameter.
  SILValue lifetimeExtendedSelf;
  bool hasAlreadyLifetimeExtendedSelf = false;
  if (hasUnownedInnerPointerResult(substFnType)) {
    auto selfMV = args.back();
    lifetimeExtendedSelf = selfMV.getValue();

    switch (substFnType->getParameters().back().getConvention()) {
    case ParameterConvention::Direct_Owned:
      // If the callee will consume the 'self' parameter, let's retain it so we
      // can keep it alive.
      lifetimeExtendedSelf =
          B.emitCopyValueOperation(loc, lifetimeExtendedSelf);
      break;
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Direct_Unowned:
      // We'll manually manage the argument's lifetime after the
      // call. Disable its cleanup, forcing a copy if it was emitted +0.
      if (selfMV.hasCleanup()) {
        selfMV.forwardCleanup(*this);
      } else {
        lifetimeExtendedSelf = selfMV.copyUnmanaged(*this, loc).forward(*this);
      }
      break;

    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Owned:
    case ParameterConvention::Pack_Inout:
      // We may need to support this at some point, but currently only imported
      // objc methods are returns_inner_pointer.
      llvm_unreachable("indirect self argument to method that"
                       " returns_inner_pointer?!");
    }
  }

  // If there's a foreign error or async parameter, fill it in.
  ManagedValue errorTemp;
  if (auto foreignAsync = calleeTypeInfo.foreign.async) {
    unsigned completionIndex = foreignAsync->completionHandlerParamIndex();

    // Ram the emitted completion into the argument list, over the placeholder
    // we left during the first pass.
    auto &completionArgSlot = const_cast<ManagedValue &>(args[completionIndex]);

    auto origFormalType = *calleeTypeInfo.origFormalType;
    completionArgSlot = resultPlan->emitForeignAsyncCompletionHandler(
        *this, origFormalType, loc);
  }
  if (auto foreignError = calleeTypeInfo.foreign.error) {
    unsigned errorParamIndex =
        foreignError->getErrorParameterIndex();

    // Ram the emitted error into the argument list, over the placeholder
    // we left during the first pass.
    auto &errorArgSlot = const_cast<ManagedValue &>(args[errorParamIndex]);

    std::tie(errorTemp, errorArgSlot) =
        resultPlan->emitForeignErrorArgument(*this, loc).value();
  }

  // Emit the raw application.
  GenericSignature genericSig =
    fn.getType().castTo<SILFunctionType>()->getInvocationGenericSignature();

  // When calling a closure that's defined in a generic context but does not
  // capture any generic parameters, we will have substitutions, but the
  // function type won't have a generic signature. Drop the substitutions in
  // this case.
  if (genericSig.isNull()) {
    subs = SubstitutionMap();

  // Otherwise, the substitutions should match the generic signature.
  } else {
    assert(genericSig.getCanonicalSignature() ==
           subs.getGenericSignature().getCanonicalSignature());
  }

  ExecutorBreadcrumb breadcrumb;

  // The presence of `implicitActorHopTarget` indicates that the callee is a
  // synchronous function isolated to an actor other than our own.
  // Such functions require the caller to hop to the callee's executor
  // prior to invoking the callee.
  if (implicitActorHopTarget) {
    assert(F.isAsync() && "cannot hop_to_executor in a non-async func!");

    SILValue executor;
    switch (*implicitActorHopTarget) {
    case ActorIsolation::ActorInstance:
      if (unsigned paramIndex =
              implicitActorHopTarget->getActorInstanceParameter()) {
        executor = emitLoadActorExecutor(loc, args[paramIndex-1]);
      } else {
        executor = emitLoadActorExecutor(loc, args.back());
      }
      break;

    case ActorIsolation::GlobalActor:
    case ActorIsolation::GlobalActorUnsafe:
      executor = emitLoadGlobalActorExecutor(
          implicitActorHopTarget->getGlobalActor());
      break;

    case ActorIsolation::Unspecified:
    case ActorIsolation::Independent:
      llvm_unreachable("Not isolated");
      break;
    }

    breadcrumb = emitHopToTargetExecutor(loc, executor);
  } else if (ExpectedExecutor &&
             (substFnType->isAsync() || calleeTypeInfo.foreign.async)) {
    // Otherwise, if we're in an actor method ourselves, and we're calling into
    // any sort of async function, we'll want to make sure to hop back to our
    // own executor afterward, since the callee could have made arbitrary hops
    // out of our isolation domain.
    breadcrumb = ExecutorBreadcrumb(true);
  }

  SILValue rawDirectResult;
  {
    SmallVector<SILValue, 1> rawDirectResults;
    emitRawApply(*this, loc, fn, subs, args, substFnType, options,
                 indirectResultAddrs, rawDirectResults, breadcrumb);
    assert(rawDirectResults.size() == 1);
    rawDirectResult = rawDirectResults[0];
  }

  // For objc async calls, lifetime extend the args until the result plan which
  // generates `await_async_continuation`.
  // Lifetime is extended by creating unmanaged copies here and by pushing the
  // cleanups required just before the result plan is generated.
  SmallVector<SILValue, 8> unmanagedCopies;
  if (calleeTypeInfo.foreign.async) {
    for (auto arg : args) {
      if (arg.hasCleanup()) {
        unmanagedCopies.push_back(arg.unmanagedCopy(*this, loc));
      }
    }
    // similarly, we defer the emission of the breadcrumb until the result
    // plan's finish method is called, because it must happen in the
    // successors of the `await_async_continuation` terminator.
    resultPlan->deferExecutorBreadcrumb(std::move(breadcrumb));

  } else {
    // In the ordinary case, we hop back to the current executor
    breadcrumb.emit(*this, loc);
  }

  // Pop the argument scope.
  argScope.pop();

  if (substFnType->isNoReturnFunction(SGM.M, getTypeExpansionContext()))
    loc.markAutoGenerated();

  // Explode the direct results.
  SILFunctionConventions substFnConv(substFnType, SGM.M);
  SmallVector<ManagedValue, 4> directResults;
  auto addManagedDirectResult = [&](SILValue result,
                                    const SILResultInfo &resultInfo) {
    auto &resultTL = getTypeLowering(resultInfo.getReturnValueType(
        SGM.M, substFnType, getTypeExpansionContext()));

    switch (resultInfo.getConvention()) {
    case ResultConvention::Indirect:
      assert(!substFnConv.isSILIndirect(resultInfo) &&
             "indirect direct result?");
      break;

    case ResultConvention::Pack:
      break;

    case ResultConvention::Owned:
      break;

    // For autoreleased results, the reclaim is implicit, so the value is
    // effectively +1.
    case ResultConvention::Autoreleased:
      break;

    // Autorelease the 'self' value to lifetime-extend it.
    case ResultConvention::UnownedInnerPointer:
      assert(lifetimeExtendedSelf &&
             "did not save lifetime-extended self param");
      if (!hasAlreadyLifetimeExtendedSelf) {
        B.createAutoreleaseValue(loc, lifetimeExtendedSelf,
                                 B.getDefaultAtomicity());
        hasAlreadyLifetimeExtendedSelf = true;
      }
      LLVM_FALLTHROUGH;

    case ResultConvention::Unowned:
      // Unretained. Retain the value.
      result = resultTL.emitCopyValue(B, loc, result);
      break;
    }

    directResults.push_back(emitManagedRValueWithCleanup(result, resultTL));
  };

  auto directSILResults = substFnConv.getDirectSILResults();
  if (directSILResults.empty()) {
    // Nothing to do.
  } else if (substFnConv.getNumDirectSILResults() == 1) {
    addManagedDirectResult(rawDirectResult, *directSILResults.begin());
  } else {
    auto directSILResultsIter = directSILResults.begin();
    // Finally add our managed direct results.
    B.emitDestructureValueOperation(
        loc, rawDirectResult, [&](unsigned index, SILValue v) {
          auto directResult = *directSILResultsIter;
          ++directSILResultsIter;
          assert(directResult.getConvention() == ResultConvention::Owned ||
                 directResult.getConvention() == ResultConvention::Unowned ||
                 !substFnConv.useLoweredAddresses());
          addManagedDirectResult(v, directResult);
        });
  }

  SILValue bridgedForeignError;
  // If there was a foreign error convention, consider it.
  // TODO: maybe this should happen after managing the result if it's
  // not a result-checking convention?
  if (auto foreignError = calleeTypeInfo.foreign.error) {
    bool doesNotThrow = options.contains(ApplyFlags::DoesNotThrow);
    bridgedForeignError =
        emitForeignErrorCheck(loc, directResults, errorTemp, doesNotThrow,
                              *foreignError, calleeTypeInfo.foreign.async);
  }

  // For objc async calls, push cleanups to be used on
  // both result and throw paths prior to finishing the result plan.
  if (calleeTypeInfo.foreign.async) {
    for (auto unmanagedCopy : unmanagedCopies) {
      Cleanups.pushCleanup<FixLifetimeDestroyCleanup>(unmanagedCopy);
    }
  } else {
    assert(unmanagedCopies.empty());
  }

  auto directResultsArray = makeArrayRef(directResults);
  RValue result = resultPlan->finish(*this, loc, directResultsArray,
                                     bridgedForeignError);
  assert(directResultsArray.empty() && "didn't claim all direct results");

  return result;
}

RValue SILGenFunction::emitMonomorphicApply(
    SILLocation loc, ManagedValue fn, ArrayRef<ManagedValue> args,
    CanType foreignResultType, CanType nativeResultType, ApplyOptions options,
    llvm::Optional<SILFunctionTypeRepresentation> overrideRep,
    const llvm::Optional<ForeignErrorConvention> &foreignError,
    SGFContext evalContext) {
  auto fnType = fn.getType().castTo<SILFunctionType>();
  assert(!fnType->isPolymorphic());
  ForeignInfo foreign{
      {}, foreignError, {}, // TODO: take a foreign async convention?
  };
  CalleeTypeInfo calleeTypeInfo(fnType, AbstractionPattern(foreignResultType),
                                nativeResultType,
                                foreign.error,
                                foreign.async,
                                foreign.self, overrideRep);
  ResultPlanPtr resultPlan = ResultPlanBuilder::computeResultPlan(
      *this, calleeTypeInfo, loc, evalContext);
  ArgumentScope argScope(*this, loc);
  return emitApply(std::move(resultPlan), std::move(argScope), loc, fn, {},
                   args, calleeTypeInfo, options, evalContext, llvm::None);
}

/// Emit either an 'apply' or a 'try_apply', with the error branch of
/// the 'try_apply' simply branching out of all cleanups and throwing.
SILValue SILGenFunction::emitApplyWithRethrow(SILLocation loc, SILValue fn,
                                              SILType substFnType,
                                              SubstitutionMap subs,
                                              ArrayRef<SILValue> args) {
  CanSILFunctionType silFnType = substFnType.castTo<SILFunctionType>();
  SILFunctionConventions fnConv(silFnType, SGM.M);
  SILType resultType = fnConv.getSILResultType(getTypeExpansionContext());

  if (!silFnType->hasErrorResult()) {
    return B.createApply(loc, fn, subs, args);
  }

  SILBasicBlock *errorBB = createBasicBlock();
  SILBasicBlock *normalBB = createBasicBlock();
  B.createTryApply(loc, fn, subs, args, normalBB, errorBB);

  // Emit the rethrow logic.
  {
    B.emitBlock(errorBB);
    SILValue error = errorBB->createPhiArgument(
        fnConv.getSILErrorType(getTypeExpansionContext()),
        OwnershipKind::Owned);

    Cleanups.emitCleanupsForReturn(CleanupLocation(loc), IsForUnwind);
    B.createThrow(loc, error);
  }

  // Enter the normal path.
  B.emitBlock(normalBB);
  return normalBB->createPhiArgument(resultType, OwnershipKind::Owned);
}

std::pair<MultipleValueInstructionResult *, CleanupHandle>
SILGenFunction::emitBeginApplyWithRethrow(SILLocation loc, SILValue fn,
                                          SILType substFnType,
                                          SubstitutionMap subs,
                                          ArrayRef<SILValue> args,
                                          SmallVectorImpl<SILValue> &yields) {
  // TODO: adjust this to create try_begin_apply when appropriate.
  assert(!substFnType.castTo<SILFunctionType>()->hasErrorResult());

  auto beginApply = B.createBeginApply(loc, fn, subs, args);

  auto yieldResults = beginApply->getYieldedValues();
  yields.append(yieldResults.begin(), yieldResults.end());

  auto *token = beginApply->getTokenResult();

  Cleanups.pushCleanup<EndCoroutineApply>(token);
  auto abortCleanup = Cleanups.getTopCleanup();

  return { token, abortCleanup };
}

void SILGenFunction::emitEndApplyWithRethrow(SILLocation loc,
                                        MultipleValueInstructionResult *token) {
  // TODO: adjust this to handle results of TryBeginApplyInst.
  assert(token->isBeginApplyToken());

  B.createEndApply(loc, token);
}

void SILGenFunction::emitYield(SILLocation loc,
                               MutableArrayRef<ArgumentSource> valueSources,
                               ArrayRef<AbstractionPattern> origTypes,
                               JumpDest unwindDest) {
  assert(valueSources.size() == origTypes.size());

  ArgumentScope evalScope(*this, loc);

  SmallVector<ManagedValue, 4> yieldArgs;
  SmallVector<DelayedArgument, 2> delayedArgs;

  auto fnType = F.getLoweredFunctionTypeInContext(getTypeExpansionContext())
    ->getUnsubstitutedType(SGM.M);
  SmallVector<SILParameterInfo, 4> substYieldTys;
  for (auto origYield : fnType->getYields()) {
    substYieldTys.push_back(
        {F.mapTypeIntoContext(origYield.getArgumentType(
                                  SGM.M, fnType, getTypeExpansionContext()))
             ->getCanonicalType(),
         origYield.getConvention()});
  }

  ArgEmitter emitter(*this, loc, fnType->getRepresentation(), /*yield*/ true,
                     /*isForCoroutine*/ false, ClaimedParamsRef(substYieldTys),
                     yieldArgs, delayedArgs, ForeignInfo{});

  for (auto i : indices(valueSources)) {
    emitter.emitSingleArg(std::move(valueSources[i]), origTypes[i]);
  }

  if (!delayedArgs.empty())
    emitDelayedArguments(*this, delayedArgs, yieldArgs);

  emitRawYield(loc, yieldArgs, unwindDest, /*unique*/ false);

  evalScope.pop();
}

void SILGenFunction::emitRawYield(SILLocation loc,
                                  ArrayRef<ManagedValue> yieldArgs,
                                  JumpDest unwindDest,
                                  bool isUniqueYield) {
  SmallVector<SILValue, 4> yieldValues;
  for (auto arg : yieldArgs)
    yieldValues.push_back(arg.getValue());

  // The normal continuation block.
  auto resumeBB = createBasicBlock();

  // The unwind block.  We can use the dest block we were passed
  // directly if there are no active cleanups between here and it.
  bool requiresSeparateUnwindBB =
    !isUniqueYield ||
    Cleanups.hasAnyActiveCleanups(unwindDest.getDepth());
  auto unwindBB = requiresSeparateUnwindBB
                    ? createBasicBlock(FunctionSection::Postmatter)
                    : unwindDest.getBlock();

  // Perform the yield.
  B.createYield(loc, yieldValues, resumeBB, unwindBB);

  // Emit the unwind branch if necessary.
  if (requiresSeparateUnwindBB) {
    SILGenSavedInsertionPoint savedIP(*this, unwindBB,
                                      FunctionSection::Postmatter);
    Cleanups.emitBranchAndCleanups(unwindDest, loc);
  }

  // Emit the resumption path.
  B.emitBlock(resumeBB);
}

/// Emits SIL instructions to create an enum value. Attempts to avoid
/// unnecessary copies by emitting the payload directly into the enum
/// payload, or into the box in the case of an indirect payload.
ManagedValue SILGenFunction::emitInjectEnum(SILLocation loc,
                                            ArgumentSource &&payload,
                                            SILType enumTy,
                                            EnumElementDecl *element,
                                            SGFContext C) {
  // Easy case -- no payload
  if (!payload) {
    if (enumTy.isLoadable(F) || !silConv.useLoweredAddresses()) {
      return emitManagedRValueWithCleanup(
          B.createEnum(loc, SILValue(), element, enumTy.getObjectType()));
    }

    // Emit the enum directly into the context if possible
    return B.bufferForExpr(loc, enumTy, getTypeLowering(enumTy), C,
                           [&](SILValue newAddr) {
                             B.createInjectEnumAddr(loc, newAddr, element);
                           });
  }

  ManagedValue payloadMV;
  AbstractionPattern origFormalType =
      (element == getASTContext().getOptionalSomeDecl()
           ? AbstractionPattern(payload.getSubstRValueType())
           : SGM.M.Types.getAbstractionPattern(element));
  auto &payloadTL = getTypeLowering(origFormalType,
                                    payload.getSubstRValueType());

  SILType loweredPayloadType = payloadTL.getLoweredType();

  // If the payload is indirect, emit it into a heap allocated box.
  //
  // To avoid copies, evaluate it directly into the box, being
  // careful to stage the cleanups so that if the expression
  // throws, we know to deallocate the uninitialized box.
  if (element->isIndirect() || element->getParentEnum()->isIndirect()) {
    auto boxTy = SGM.M.Types.getBoxTypeForEnumElement(getTypeExpansionContext(),
                                                      enumTy, element);
    auto *box = B.createAllocBox(loc, boxTy);
    auto *addr = B.createProjectBox(loc, box, 0);

    CleanupHandle initCleanup = enterDestroyCleanup(box);
    Cleanups.setCleanupState(initCleanup, CleanupState::Dormant);
    CleanupHandle uninitCleanup = enterDeallocBoxCleanup(box);

    BoxInitialization dest(box, addr, uninitCleanup, initCleanup);
    std::move(payload).forwardInto(*this, origFormalType, &dest,
                                   payloadTL);

    payloadMV = dest.getManagedBox();
    loweredPayloadType = payloadMV.getType();
  }

  // Loadable with payload
  if (enumTy.isLoadable(F) || !silConv.useLoweredAddresses()) {
    if (!payloadMV) {
      // If the payload was indirect, we already evaluated it and
      // have a single value. Otherwise, evaluate the payload.
      payloadMV = std::move(payload).getAsSingleValue(*this, origFormalType,
                                                      loweredPayloadType);
    }

    SILValue argValue = payloadMV.forward(*this);

    return emitManagedRValueWithCleanup(
        B.createEnum(loc, argValue, element, enumTy.getObjectType()));
  }

  // Address-only with payload
  return B.bufferForExpr(
      loc, enumTy, getTypeLowering(enumTy), C, [&](SILValue bufferAddr) {
        SILValue resultData = B.createInitEnumDataAddr(
            loc, bufferAddr, element, loweredPayloadType.getAddressType());

        if (payloadMV) {
          // If the payload was indirect, we already evaluated it and
          // have a single value. Store it into the result.
          B.emitStoreValueOperation(loc, payloadMV.forward(*this), resultData,
                                    StoreOwnershipQualifier::Init);
        } else if (payloadTL.isLoadable()) {
          // The payload of this specific enum case might be loadable
          // even if the overall enum is address-only.
          payloadMV =
            std::move(payload).getAsSingleValue(*this, origFormalType,
                                                loweredPayloadType);
          B.emitStoreValueOperation(loc, payloadMV.forward(*this), resultData,
                                    StoreOwnershipQualifier::Init);
        } else {
          // The payload is address-only. Evaluate it directly into
          // the enum.

          TemporaryInitialization dest(resultData, CleanupHandle::invalid());
          std::move(payload).forwardInto(*this, origFormalType, &dest,
                                         payloadTL);
        }

        // The payload is initialized, now apply the tag.
        B.createInjectEnumAddr(loc, bufferAddr, element);
      });
}

RValue SILGenFunction::emitApplyExpr(ApplyExpr *e, SGFContext c) {
  CallEmission emission = CallEmission::forApplyExpr(*this, e);
  return emission.apply(c);
}

RValue
SILGenFunction::emitApplyOfLibraryIntrinsic(SILLocation loc,
                                            FuncDecl *fn,
                                            SubstitutionMap subMap,
                                            ArrayRef<ManagedValue> args,
                                            SGFContext ctx) {
  return emitApplyOfLibraryIntrinsic(loc, SILDeclRef(fn), subMap, args, ctx);
}

RValue SILGenFunction::emitApplyOfLibraryIntrinsic(SILLocation loc,
                                                   SILDeclRef declRef,
                                                   SubstitutionMap subMap,
                                                   ArrayRef<ManagedValue> args,
                                                   SGFContext ctx) {
  auto callee = Callee::forDirect(*this, declRef, subMap, loc);

  auto origFormalType = callee.getOrigFormalType();
  auto substFormalType = callee.getSubstFormalType();

  auto calleeTypeInfo = callee.getTypeInfo(*this);

  llvm::Optional<ManagedValue> borrowedSelf;
  if (callee.requiresSelfValueForDispatch())
    borrowedSelf = args.back();
  auto mv = callee.getFnValue(*this, borrowedSelf);

  assert(!calleeTypeInfo.foreign.error);
  assert(!calleeTypeInfo.foreign.async);
  assert(!calleeTypeInfo.foreign.self.isImportAsMember());
  assert(calleeTypeInfo.substFnType->getExtInfo().getLanguage() ==
         SILFunctionLanguage::Swift);

  calleeTypeInfo.origResultType = origFormalType.getFunctionResultType();
  calleeTypeInfo.substResultType = substFormalType.getResult();

  SILFunctionConventions silConv(calleeTypeInfo.substFnType, getModule());
  llvm::SmallVector<ManagedValue, 8> finalArgs;
  convertOwnershipConventionsGivenParamInfos(
      *this, silConv.getParameters(), args, loc,
      /*isForCoroutine*/ calleeTypeInfo.substFnType->isCoroutine(), finalArgs);

  ResultPlanPtr resultPlan =
  ResultPlanBuilder::computeResultPlan(*this, calleeTypeInfo, loc, ctx);
  ArgumentScope argScope(*this, loc);
  return emitApply(std::move(resultPlan), std::move(argScope), loc, mv, subMap,
                   finalArgs, calleeTypeInfo, ApplyOptions(), ctx, llvm::None);
}

void SILGenFunction::emitApplyOfUnavailableCodeReached() {
  auto loc = RegularLocation::getAutoGeneratedLocation(F.getLocation());
  FuncDecl *fd = getASTContext().getDiagnoseUnavailableCodeReached();

  if (!fd) {
    // Broken stdlib?
    B.createUnconditionalFail(loc, "unavailable code reached");
    return;
  }

  auto declRef = SILDeclRef(fd);
  if (fd->isBackDeployed(getASTContext())) {
    // The standard library entry point for the diagnostic function was
    // introduced in Swift 5.9 so we call the back deployment thunk in case this
    // code will execute on an older runtime.
    declRef =
        declRef.asBackDeploymentKind(SILDeclRef::BackDeploymentKind::Thunk);
  }

  emitApplyOfLibraryIntrinsic(loc, declRef, SubstitutionMap(), {},
                              SGFContext());
}

StringRef SILGenFunction::getMagicFunctionString() {
  assert(MagicFunctionName
         && "asking for #function but we don't have a function name?!");
  if (MagicFunctionString.empty()) {
    llvm::raw_string_ostream os(MagicFunctionString);
    MagicFunctionName.print(os);
  }
  return MagicFunctionString;
}

StringRef SILGenFunction::getMagicFilePathString(SourceLoc loc) {
  assert(loc.isValid());
  return getSourceManager().getDisplayNameForLoc(loc);
}

std::string SILGenFunction::getMagicFileIDString(SourceLoc loc) {
  auto path = getMagicFilePathString(loc);

  auto result = SGM.FileIDsByFilePath.find(path);
  if (result != SGM.FileIDsByFilePath.end())
    return std::get<0>(result->second);

  return path.str();
}

/// Emit an application of the given allocating initializer.
RValue SILGenFunction::emitApplyAllocatingInitializer(SILLocation loc,
                                                      ConcreteDeclRef init,
                                                      PreparedArguments &&args,
                                                      Type overriddenSelfType,
                                                      SGFContext C) {
  ConstructorDecl *ctor = cast<ConstructorDecl>(init.getDecl());

  // Form the reference to the allocating initializer.
  auto initRef = SILDeclRef(ctor, SILDeclRef::Kind::Allocator)
    .asForeign(requiresForeignEntryPoint(ctor));
  auto initConstant = getConstantInfo(getTypeExpansionContext(), initRef);
  auto subs = init.getSubstitutions();

  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  // Form the metatype argument.
  ManagedValue selfMetaVal;
  SILType selfMetaTy;
  {
    // Determine the self metatype type.
    CanSILFunctionType substFnType = initConstant.SILFnType->substGenericArgs(
        SGM.M, subs, getTypeExpansionContext());
    SILType selfParamMetaTy =
        getSILType(substFnType->getSelfParameter(), substFnType);

    if (overriddenSelfType) {
      // If the 'self' type has been overridden, form a metatype to the
      // overriding 'Self' type.
      Type overriddenSelfMetaType =
        MetatypeType::get(overriddenSelfType,
                          selfParamMetaTy.castTo<MetatypeType>()
                              ->getRepresentation());
      selfMetaTy =
        getLoweredType(overriddenSelfMetaType->getCanonicalType());
    } else {
      selfMetaTy = selfParamMetaTy;
    }

    // Form the metatype value.
    SILValue selfMeta = B.createMetatype(loc, selfMetaTy);

    // If the types differ, we need an upcast.
    if (selfMetaTy != selfParamMetaTy)
      selfMeta = B.createUpcast(loc, selfMeta, selfParamMetaTy);

    selfMetaVal = ManagedValue::forUnmanaged(selfMeta);
  }

  // Form the callee.
  llvm::Optional<Callee> callee;
  if (isa<ProtocolDecl>(ctor->getDeclContext())) {
    callee.emplace(Callee::forWitnessMethod(
        *this, selfMetaVal.getType().getASTType(),
        initRef, subs, loc));
  } else if (getMethodDispatch(ctor) == MethodDispatch::Class) {
    callee.emplace(Callee::forClassMethod(*this, initRef, subs, loc));
  } else {
    callee.emplace(Callee::forDirect(*this, initRef, subs, loc));
  }

  auto substFormalType = callee->getSubstFormalType();
  auto resultType = cast<FunctionType>(substFormalType.getResult()).getResult();

  // Form the call emission.
  CallEmission emission(*this, std::move(*callee), std::move(writebackScope));

  // Self metatype.
  emission.addSelfParam(loc,
                        ArgumentSource(loc,
                                       RValue(*this, loc,
                                              selfMetaVal.getType()
                                                .getASTType(),
                                              std::move(selfMetaVal))),
                        substFormalType.getParams()[0]);

  // Arguments.
  emission.addCallSite(loc, std::move(args));

  // For an inheritable initializer, determine whether we'll need to adjust the
  // result type.
  bool requiresDowncast = false;
  if (ctor->isRequired() && overriddenSelfType) {
    if (!resultType->isEqual(overriddenSelfType))
      requiresDowncast = true;
  }

  // Perform the call.
  // TODO: in general this produces RValues...
  RValue result = emission.apply(requiresDowncast ? SGFContext() : C);

  // If we need a downcast, do it down.
  if (requiresDowncast) {
    ManagedValue v = std::move(result).getAsSingleValue(*this, loc);
    CanType canOverriddenSelfType = overriddenSelfType->getCanonicalType();
    SILType loweredResultTy = getLoweredType(canOverriddenSelfType);
    v = B.createUncheckedRefCast(loc, v, loweredResultTy);
    result = RValue(*this, loc, canOverriddenSelfType, v);
  }

  return result;
}

RValue SILGenFunction::emitApplyOfPropertyWrapperBackingInitializer(
    SILLocation loc,
    VarDecl *var,
    SubstitutionMap subs,
    RValue &&originalValue,
    SILDeclRef::Kind initKind,
    SGFContext C) {
  assert(initKind == SILDeclRef::Kind::PropertyWrapperBackingInitializer ||
         initKind == SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue);

  SILDeclRef constant(var, initKind);

  FormalEvaluationScope writebackScope(*this);

  auto callee = Callee::forDirect(*this, constant, subs, loc);
  auto substFnType = callee.getSubstFormalType();

  CallEmission emission(*this, std::move(callee), std::move(writebackScope));

  PreparedArguments args(substFnType->getAs<AnyFunctionType>()->getParams());
  args.add(loc, std::move(originalValue));
  emission.addCallSite(loc, std::move(args));
  
  return emission.apply(C);
}

/// Emit a literal that applies the various initializers.
RValue SILGenFunction::emitLiteral(LiteralExpr *literal, SGFContext C) {
  ConcreteDeclRef builtinInit;
  ConcreteDeclRef init;
  if (auto builtinLiteral = dyn_cast<BuiltinLiteralExpr>(literal)) {
    builtinInit = builtinLiteral->getBuiltinInitializer();
    init = builtinLiteral->getInitializer();
  } else {
    builtinInit = literal->getInitializer();
  }

  // Emit the raw, builtin literal arguments.
  PreparedArguments builtinLiteralArgs =
      buildBuiltinLiteralArgs(*this, C, literal);

  // Call the builtin initializer.
  RValue builtinResult = emitApplyAllocatingInitializer(
      literal, builtinInit, std::move(builtinLiteralArgs), Type(),
      init ? SGFContext() : C);

  // If we were able to directly initialize the literal we wanted, we're done.
  if (!init)
    return builtinResult;

  // Otherwise, perform the second initialization step.
  auto ty = builtinResult.getType();
  PreparedArguments args((AnyFunctionType::Param(ty)));
  args.add(literal, std::move(builtinResult));

  RValue result = emitApplyAllocatingInitializer(literal, init,
                                                 std::move(args),
                                                 literal->getType(), C);
  return result;
}

/// Allocate an uninitialized array of a given size, returning the array
/// and a pointer to its uninitialized contents, which must be initialized
/// before the array is valid.
std::pair<ManagedValue, SILValue>
SILGenFunction::emitUninitializedArrayAllocation(Type ArrayTy,
                                                 SILValue Length,
                                                 SILLocation Loc) {
  auto &Ctx = getASTContext();
  auto allocate = Ctx.getAllocateUninitializedArray();

  // Invoke the intrinsic, which returns a tuple.
  auto subMap = ArrayTy->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                                   Ctx.getArrayDecl());
  auto result = emitApplyOfLibraryIntrinsic(Loc, allocate,
                                            subMap,
                                            ManagedValue::forUnmanaged(Length),
                                            SGFContext());

  // Explode the tuple.
  SmallVector<ManagedValue, 2> resultElts;
  std::move(result).getAll(resultElts);

  return {resultElts[0], resultElts[1].getUnmanagedValue()};
}

/// Deallocate an uninitialized array.
void SILGenFunction::emitUninitializedArrayDeallocation(SILLocation loc,
                                                        SILValue array) {
  auto &Ctx = getASTContext();
  auto deallocate = Ctx.getDeallocateUninitializedArray();

  CanType arrayTy = array->getType().getASTType();

  // Invoke the intrinsic.
  auto subMap = arrayTy->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                                   Ctx.getArrayDecl());
  emitApplyOfLibraryIntrinsic(loc, deallocate, subMap,
                              ManagedValue::forUnmanaged(array),
                              SGFContext());
}

ManagedValue SILGenFunction::emitUninitializedArrayFinalization(SILLocation loc,
                                                      ManagedValue array) {
  auto &Ctx = getASTContext();
  FuncDecl *finalize = Ctx.getFinalizeUninitializedArray();
  
  // The _finalizeUninitializedArray function only needs to be called if the
  // library contains it.
  // The Array implementation in the stdlib <= 5.3 does not use SIL COW
  // support yet and therefore does not provide the _finalizeUninitializedArray
  // intrinsic function.
  if (!finalize)
    return array;

  SILValue arrayVal = array.forward(*this);
  CanType arrayTy = arrayVal->getType().getASTType();

  // Invoke the intrinsic.
  auto subMap = arrayTy->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                                   Ctx.getArrayDecl());
  RValue result = emitApplyOfLibraryIntrinsic(loc, finalize, subMap,
                              ManagedValue::forUnmanaged(arrayVal),
                              SGFContext());
  return std::move(result).getScalarValue();
}

namespace {
  /// A cleanup that deallocates an uninitialized array.
  class DeallocateUninitializedArray: public Cleanup {
    SILValue Array;
  public:
    DeallocateUninitializedArray(SILValue array)
      : Array(array) {}

    void emit(SILGenFunction &SGF, CleanupLocation l, ForUnwind_t forUnwind) override {
      SGF.emitUninitializedArrayDeallocation(l, Array);
    }

    void dump(SILGenFunction &SGF) const override {
#ifndef NDEBUG
      llvm::errs() << "DeallocateUninitializedArray "
                   << "State:" << getState() << " "
                   << "Array:" << Array << "\n";
#endif
    }
  };
} // end anonymous namespace

CleanupHandle
SILGenFunction::enterDeallocateUninitializedArrayCleanup(SILValue array) {
  Cleanups.pushCleanup<DeallocateUninitializedArray>(array);
  return Cleanups.getTopCleanup();
}

static Callee getBaseAccessorFunctionRef(SILGenFunction &SGF,
                                         SILLocation loc,
                                         SILDeclRef constant,
                                         ArgumentSource &selfValue,
                                         bool isSuper,
                                         bool isDirectUse,
                                         SubstitutionMap subs,
                                         bool isOnSelfParameter) {
  auto *decl = cast<AbstractFunctionDecl>(constant.getDecl());

  bool isObjCReplacementSelfCall = false;
  if (isOnSelfParameter &&
      SGF.getOptions()
          .EnableDynamicReplacementCanCallPreviousImplementation &&
      isCallToReplacedInDynamicReplacement(SGF, decl,
                                           isObjCReplacementSelfCall)) {
    return Callee::forDirect(
        SGF,
        SILDeclRef(cast<AbstractFunctionDecl>(SGF.FunctionDC->getAsDecl()),
                   constant.kind),
        subs, loc, true);
  }

  // The accessor might be a local function that does not capture any
  // generic parameters, in which case we don't want to pass in any
  // substitutions.
  auto captureInfo = SGF.SGM.Types.getLoweredLocalCaptures(constant);
  if (decl->getDeclContext()->isLocalContext() &&
      !captureInfo.hasGenericParamCaptures()) {
    subs = SubstitutionMap();
  }

  // If this is a method in a protocol, generate it as a protocol call.
  if (isa<ProtocolDecl>(decl->getDeclContext())) {
    assert(!isDirectUse && "direct use of protocol accessor?");
    assert(!isSuper && "super call to protocol method?");

    return Callee::forWitnessMethod(
        SGF, selfValue.getSubstRValueType(),
        constant, subs, loc);
  }

  bool isClassDispatch = false;
  if (!isDirectUse) {
    switch (getMethodDispatch(decl)) {
    case MethodDispatch::Class:
      isClassDispatch = true;
      break;
    case MethodDispatch::Static:
      isClassDispatch = false;
      break;
    }
  }

  bool isObjCDirect = false;
  if (auto objcDecl = dyn_cast_or_null<clang::ObjCMethodDecl>(
          decl->getClangDecl())) {
    isObjCDirect = objcDecl->isDirectMethod();
  }

  // Dispatch in a struct/enum or to a final method is always direct.
  if (!isClassDispatch || isObjCDirect)
    return Callee::forDirect(SGF, constant, subs, loc);

  // Otherwise, if we have a non-final class dispatch to a normal method,
  // perform a dynamic dispatch.
  if (!isSuper)
    return Callee::forClassMethod(SGF, constant, subs, loc);

  // If this is a "super." dispatch, we do a dynamic dispatch for objc methods
  // or non-final native Swift methods.
  if (!canUseStaticDispatch(SGF, constant))
    return Callee::forSuperMethod(SGF, constant, subs, loc);

  return Callee::forDirect(SGF, constant, subs, loc);
}

static Callee
emitSpecializedAccessorFunctionRef(SILGenFunction &SGF,
                                   SILLocation loc,
                                   SILDeclRef constant,
                                   SubstitutionMap substitutions,
                                   ArgumentSource &selfValue,
                                   bool isSuper,
                                   bool isDirectUse,
                                   bool isOnSelfParameter)
{
  // Get the accessor function. The type will be a polymorphic function if
  // the Self type is generic.
  Callee callee = getBaseAccessorFunctionRef(SGF, loc, constant, selfValue,
                                             isSuper, isDirectUse,
                                             substitutions, isOnSelfParameter);
  
  // Collect captures if the accessor has them.
  if (SGF.SGM.M.Types.hasLoweredLocalCaptures(constant)) {
    assert(!selfValue && "local property has self param?!");
    SmallVector<ManagedValue, 4> captures;
    SGF.emitCaptures(loc, constant, CaptureEmission::ImmediateApplication,
                     captures);
    callee.setCaptures(std::move(captures));
  }

  return callee;
}

namespace {

/// A builder class that creates the base argument for accessors.
///
/// *NOTE* All cleanups created inside of this builder on base arguments must be
/// formal access to ensure that we do not extend the lifetime of a guaranteed
/// base after the accessor is evaluated.
struct AccessorBaseArgPreparer final {
  SILGenFunction &SGF;
  SILLocation loc;
  ManagedValue base;
  CanType baseFormalType;
  SILDeclRef accessor;
  SILParameterInfo selfParam;
  SILType baseLoweredType;

  AccessorBaseArgPreparer(SILGenFunction &SGF, SILLocation loc,
                          ManagedValue base, CanType baseFormalType,
                          SILDeclRef accessor);
  ArgumentSource prepare();

private:
  /// Prepare our base if we have an address base.
  ArgumentSource prepareAccessorAddressBaseArg();
  /// Prepare our base if we have an object base.
  ArgumentSource prepareAccessorObjectBaseArg();

  /// Returns true if given an address base, we need to load the underlying
  /// address. Asserts if baseLoweredType is not an address.
  bool shouldLoadBaseAddress() const;
};

} // end anonymous namespace

bool AccessorBaseArgPreparer::shouldLoadBaseAddress() const {
  assert(baseLoweredType.isAddress() &&
         "Should only call this helper method if the base is an address");
  switch (selfParam.getConvention()) {
  // If the accessor wants the value 'inout', always pass the
  // address we were given.  This is semantically required.
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    return false;

  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
    llvm_unreachable("self parameter was a pack?");

  // If the accessor wants the value 'in', we have to copy if the
  // base isn't a temporary.  We aren't allowed to pass aliased
  // memory to 'in', and we have pass at +1.
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Guaranteed:
    // TODO: We shouldn't be able to get an lvalue here, but the AST
    // sometimes produces an inout base for non-mutating accessors.
    // rdar://problem/19782170
    // assert(!base.isLValue());
    return base.isLValue() || base.isPlusZeroRValueOrTrivial();

  // If the accessor wants the value directly, we definitely have to
  // load.
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
    return true;
  }
  llvm_unreachable("bad convention");
}

ArgumentSource AccessorBaseArgPreparer::prepareAccessorAddressBaseArg() {
  // If the base is currently an address, we may have to copy it.
  if (shouldLoadBaseAddress()) {
    if (selfParam.isConsumed() ||
        (base.getType().isAddressOnly(SGF.F)
         // If a move-only base is borrowed, then we have to try our best to
         // borrow it in-place without copying.
         // TODO: Can we avoid copying a non-move-only value too in this
         // circumstance?
         && !base.getType().isMoveOnly())) {
      // The load can only be a take if the base is a +1 rvalue.
      auto shouldTake = IsTake_t(base.hasCleanup());

      base = SGF.emitFormalAccessLoad(loc, base.forward(SGF),
                                      SGF.getTypeLowering(baseLoweredType),
                                      SGFContext(), shouldTake);
      return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
    }
    
    // If the type is address-only, we can borrow the memory location as is.
    if (base.getType().isAddressOnly(SGF.F)) {
      return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
    }

    // If we do not have a consumed base and need to perform a load, perform a
    // formal access load borrow.
    base = SGF.B.createFormalAccessLoadBorrow(loc, base);
    return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
  }

  // Handle inout bases specially here.
  if (selfParam.isIndirectInOut()) {
    // It sometimes happens that we get r-value bases here, e.g. when calling a
    // mutating setter on a materialized temporary.  Just don't claim the value.
    if (!base.isLValue()) {
      base = ManagedValue::forLValue(base.getValue());
    }

    // FIXME: this assumes that there's never meaningful reabstraction of self
    // arguments.
    return ArgumentSource(
        loc,
        LValue::forAddress(SGFAccessKind::ReadWrite, base, llvm::None,
                           AbstractionPattern(baseFormalType), baseFormalType));
  }

  // Otherwise, we have a value that we can forward without any additional
  // handling.
  return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
}

ArgumentSource AccessorBaseArgPreparer::prepareAccessorObjectBaseArg() {
  // If the base is currently scalar, we may have to drop it in
  // memory or copy it.
  assert(!base.isLValue());

  // We need to produce the value at +1 if it's going to be consumed.
  if (selfParam.isConsumed() && !base.hasCleanup()) {
    base = base.copyUnmanaged(SGF, loc);
  }

  // If the parameter is indirect, we need to drop the value into
  // temporary memory.
  if (SGF.silConv.isSILIndirect(selfParam)) {
    // It's a really bad idea to materialize when we're about to
    // pass a value to an inout argument, because it's a really easy
    // way to silently drop modifications (e.g. from a mutating
    // getter in a writeback pair).  Our caller should always take
    // responsibility for that decision (by doing the materialization
    // itself).
    assert(!selfParam.isIndirectMutating() &&
           "passing unmaterialized r-value as inout argument");

    base = base.formallyMaterialize(SGF, loc);
    auto shouldTake = IsTake_t(base.hasCleanup());
    base = SGF.emitFormalAccessLoad(loc, base.forward(SGF),
                                    SGF.getTypeLowering(baseLoweredType),
                                    SGFContext(), shouldTake);
  }

  return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
}

AccessorBaseArgPreparer::AccessorBaseArgPreparer(SILGenFunction &SGF,
                                                 SILLocation loc,
                                                 ManagedValue base,
                                                 CanType baseFormalType,
                                                 SILDeclRef accessor)
    : SGF(SGF), loc(loc), base(base), baseFormalType(baseFormalType),
      accessor(accessor), selfParam(SGF.SGM.Types.getConstantSelfParameter(
                              SGF.getTypeExpansionContext(), accessor)),
      baseLoweredType(base.getType()) {
  assert(!base.isInContext());
  assert(!base.isLValue() || !base.hasCleanup());
}

ArgumentSource AccessorBaseArgPreparer::prepare() {
  // If the base is a boxed existential, we will open it later.
  if (baseLoweredType.getPreferredExistentialRepresentation() ==
      ExistentialRepresentation::Boxed) {
    assert(!baseLoweredType.isAddress() &&
           "boxed existential should not be an address");
    return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
  }

  if (baseLoweredType.isAddress())
    return prepareAccessorAddressBaseArg();

  // At this point, we know we have an object.
  assert(baseLoweredType.isObject());
  return prepareAccessorObjectBaseArg();
}

ArgumentSource SILGenFunction::prepareAccessorBaseArg(SILLocation loc,
                                                      ManagedValue base,
                                                      CanType baseFormalType,
                                                      SILDeclRef accessor) {
  if (!base)
    return ArgumentSource();

  AccessorBaseArgPreparer Preparer(*this, loc, base, baseFormalType, accessor);
  return Preparer.prepare();
}

static void collectFakeIndexParameters(SILGenFunction &SGF,
                                       CanType substType,
                                    SmallVectorImpl<SILParameterInfo> &params) {
  if (auto tuple = dyn_cast<TupleType>(substType)) {
    for (auto substEltType : tuple.getElementTypes())
      collectFakeIndexParameters(SGF, substEltType, params);
    return;
  }

  // Use conventions that will produce a +1 value.
  auto &tl = SGF.getTypeLowering(substType);
  ParameterConvention convention;
  if (tl.isAddressOnly()) {
    convention = ParameterConvention::Indirect_In;
  } else if (tl.isTrivial()) {
    convention = ParameterConvention::Direct_Unowned;
  } else {
    convention = ParameterConvention::Direct_Owned;
  }

  params.push_back(SILParameterInfo{tl.getLoweredType().getASTType(),
                                    convention});
}

static void emitPseudoFunctionArguments(SILGenFunction &SGF,
                                        SILLocation applyLoc,
                                        AbstractionPattern origFnType,
                                        CanFunctionType substFnType,
                                        SmallVectorImpl<ManagedValue> &outVals,
                                        PreparedArguments &&args) {
  auto substParams = substFnType->getParams();

  SmallVector<SILParameterInfo, 4> substParamTys;
  for (auto substParam : substParams) {
    auto substParamType = substParam.getParameterType()->getCanonicalType();
    collectFakeIndexParameters(SGF, substParamType, substParamTys);
  }

  SmallVector<ManagedValue, 4> argValues;
  SmallVector<DelayedArgument, 2> delayedArgs;

  ArgEmitter emitter(SGF, applyLoc, SILFunctionTypeRepresentation::Thin,
                     /*yield*/ false,
                     /*isForCoroutine*/ false, ClaimedParamsRef(substParamTys),
                     argValues, delayedArgs, ForeignInfo{});

  emitter.emitPreparedArgs(std::move(args), origFnType);

  // TODO: do something to preserve LValues in the delayed arguments?
  if (!delayedArgs.empty())
    emitDelayedArguments(SGF, delayedArgs, argValues);

  outVals.swap(argValues);
}

PreparedArguments
SILGenFunction::prepareSubscriptIndices(SILLocation loc,
                                        SubscriptDecl *subscript,
                                        SubstitutionMap subs,
                                        AccessStrategy strategy,
                                        ArgumentList *argList) {
  // TODO: use the real abstraction pattern from the accessor(s) in the
  // strategy.
  // Currently we use the substituted type so that we can reconstitute these
  // as RValues.
  Type interfaceType = subscript->getInterfaceType();

  CanFunctionType substFnType;
  if (subs)
    substFnType = cast<FunctionType>(interfaceType
                                       ->castTo<GenericFunctionType>()
                                       ->substGenericArgs(subs)
                                       ->getCanonicalType());
  else
    substFnType = cast<FunctionType>(interfaceType
                                       ->getCanonicalType());


  AbstractionPattern origFnType(substFnType);

  // Prepare the unevaluated index expression.
  auto substParams = substFnType->getParams();
  PreparedArguments args(substParams, argList);

  // Now, force it to be evaluated.
  SmallVector<ManagedValue, 4> argValues;
  emitPseudoFunctionArguments(*this, loc, origFnType, substFnType,
                              argValues, std::move(args));

  // Finally, prepare the evaluated index expression. We might be calling
  // the getter and setter, and it is important to only evaluate the
  // index expression once.
  PreparedArguments result(substParams);

  ArrayRef<ManagedValue> remainingArgs = argValues;
  for (auto i : indices(substParams)) {
    auto substParamType = substParams[i].getParameterType()->getCanonicalType();
    auto count = RValue::getRValueSize(substParamType);
    RValue elt(*this, remainingArgs.slice(0, count), substParamType);
    result.add(argList->getExpr(i), std::move(elt));
    remainingArgs = remainingArgs.slice(count);
  }
  assert(remainingArgs.empty());

  assert(result.isValid());
  return result;
}

SILDeclRef SILGenModule::getAccessorDeclRef(AccessorDecl *accessor,
                                            ResilienceExpansion expansion) {
  auto declRef = SILDeclRef(accessor, SILDeclRef::Kind::Func);

  if (shouldApplyBackDeploymentThunk(accessor, getASTContext(), expansion))
    return declRef.asBackDeploymentKind(SILDeclRef::BackDeploymentKind::Thunk);

  return declRef.asForeign(requiresForeignEntryPoint(accessor));
}

/// Emit a call to a getter.
RValue SILGenFunction::emitGetAccessor(
    SILLocation loc, SILDeclRef get, SubstitutionMap substitutions,
    ArgumentSource &&selfValue, bool isSuper, bool isDirectUse,
    PreparedArguments &&subscriptIndices, SGFContext c, bool isOnSelfParameter,
    llvm::Optional<ActorIsolation> implicitActorHopTarget) {
  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  // Calls to getters are implicit because the compiler inserts them on a
  // property access, but the location is useful in backtraces so it should be
  // preserved.
  loc.markExplicit();

  Callee getter = emitSpecializedAccessorFunctionRef(
      *this, loc, get, substitutions, selfValue, isSuper, isDirectUse,
      isOnSelfParameter);
  bool hasSelf = (bool)selfValue;
  CanAnyFunctionType accessType = getter.getSubstFormalType();

  CallEmission emission(*this, std::move(getter), std::move(writebackScope));
  if (implicitActorHopTarget)
    emission.setImplicitlyAsync(implicitActorHopTarget);

  // Self ->
  if (hasSelf) {
    emission.addSelfParam(loc, std::move(selfValue),
                         accessType.getParams()[0]);
    accessType = cast<AnyFunctionType>(accessType.getResult());
  }
  // Index or () if none.
  if (subscriptIndices.isNull())
    subscriptIndices.emplace({});

  emission.addCallSite(loc, std::move(subscriptIndices));

  // T
  return emission.apply(c);
}

void SILGenFunction::emitSetAccessor(SILLocation loc, SILDeclRef set,
                                     SubstitutionMap substitutions,
                                     ArgumentSource &&selfValue,
                                     bool isSuper, bool isDirectUse,
                                     PreparedArguments &&subscriptIndices,
                                     ArgumentSource &&setValue,
                                     bool isOnSelfParameter) {
  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  Callee setter = emitSpecializedAccessorFunctionRef(
      *this, loc, set, substitutions, selfValue, isSuper, isDirectUse,
      isOnSelfParameter);
  bool hasSelf = (bool)selfValue;
  CanAnyFunctionType accessType = setter.getSubstFormalType();

  CallEmission emission(*this, std::move(setter), std::move(writebackScope));
  // Self ->
  if (hasSelf) {
    emission.addSelfParam(loc, std::move(selfValue),
                          accessType.getParams()[0]);
    accessType = cast<AnyFunctionType>(accessType.getResult());
  }

  // (value)  or (value, indices...)
  PreparedArguments values(accessType->getParams());
  values.addArbitrary(std::move(setValue));

  if (!subscriptIndices.isNull()) {
    for (auto &component : std::move(subscriptIndices).getSources()) {
      auto argLoc = component.getKnownRValueLocation();
      RValue &&arg = std::move(component).asKnownRValue(*this);
      values.add(argLoc, std::move(arg));
    }
  }
  assert(values.isValid());
  emission.addCallSite(loc, std::move(values));
  // ()
  emission.apply();
}

/// Emit a call to an addressor.
///
/// Returns an l-value managed value.
ManagedValue SILGenFunction::emitAddressorAccessor(
    SILLocation loc, SILDeclRef addressor, SubstitutionMap substitutions,
    ArgumentSource &&selfValue, bool isSuper, bool isDirectUse,
    PreparedArguments &&subscriptIndices,
    SILType addressType, bool isOnSelfParameter) {
  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  Callee callee = emitSpecializedAccessorFunctionRef(
      *this, loc, addressor, substitutions, selfValue, isSuper, isDirectUse,
      isOnSelfParameter);
  bool hasSelf = (bool)selfValue;
  CanAnyFunctionType accessType = callee.getSubstFormalType();

  CallEmission emission(*this, std::move(callee), std::move(writebackScope));
  // Self ->
  if (hasSelf) {
    emission.addSelfParam(loc, std::move(selfValue),
                          accessType.getParams()[0]);
    accessType = cast<AnyFunctionType>(accessType.getResult());
  }
  // Index or () if none.
  if (subscriptIndices.isNull())
    subscriptIndices.emplace({});

  emission.addCallSite(loc, std::move(subscriptIndices));

  // Unsafe{Mutable}Pointer<T> or
  // (Unsafe{Mutable}Pointer<T>, Builtin.UnknownPointer) or
  // (Unsafe{Mutable}Pointer<T>, Builtin.NativePointer) or
  // (Unsafe{Mutable}Pointer<T>, Builtin.NativePointer?) or
  SmallVector<ManagedValue, 2> results;
  emission.apply().getAll(results);

  assert(results.size() == 1);
  auto pointer = results[0].getUnmanagedValue();

  // Drill down to the raw pointer using intrinsic knowledge of those types.
  auto pointerType =
    pointer->getType().castTo<BoundGenericStructType>()->getDecl();
  auto props = pointerType->getStoredProperties();
  assert(props.size() == 1);
  VarDecl *rawPointerField = props[0];
  pointer = B.createStructExtract(loc, pointer, rawPointerField,
                                  SILType::getRawPointerType(getASTContext()));

  // Convert to the appropriate address type and return.
  SILValue address = B.createPointerToAddress(loc, pointer, addressType,
                                              /*isStrict*/ true,
                                              /*isInvariant*/ false);

  return ManagedValue::forLValue(address);
}

CleanupHandle
SILGenFunction::emitCoroutineAccessor(SILLocation loc, SILDeclRef accessor,
                                      SubstitutionMap substitutions,
                                      ArgumentSource &&selfValue,
                                      bool isSuper, bool isDirectUse,
                                      PreparedArguments &&subscriptIndices,
                                      SmallVectorImpl<ManagedValue> &yields,
                                      bool isOnSelfParameter) {
  Callee callee =
    emitSpecializedAccessorFunctionRef(*this, loc, accessor,
                                       substitutions, selfValue,
                                       isSuper, isDirectUse,
                                       isOnSelfParameter);

  // We're already in a full formal-evaluation scope.
  // Make a dead writeback scope; applyCoroutine won't try to pop this.
  FormalEvaluationScope writebackScope(*this);
  writebackScope.pop();

  bool hasSelf = (bool)selfValue;
  CanAnyFunctionType accessType = callee.getSubstFormalType();

  CallEmission emission(*this, std::move(callee), std::move(writebackScope));
  // Self ->
  if (hasSelf) {
    emission.addSelfParam(loc, std::move(selfValue),
                          accessType.getParams()[0]);
    accessType = cast<AnyFunctionType>(accessType.getResult());
  }
  // Index or () if none.
  if (subscriptIndices.isNull())
    subscriptIndices.emplace({});

  emission.addCallSite(loc, std::move(subscriptIndices));

  auto endApplyHandle = emission.applyCoroutine(yields);

  return endApplyHandle;
}

ManagedValue SILGenFunction::emitAsyncLetStart(
    SILLocation loc,
    SILValue taskOptions,
    AbstractClosureExpr *asyncLetEntryPoint,
    SILValue resultBuf) {
  ASTContext &ctx = getASTContext();
  Type resultType = asyncLetEntryPoint->getType()
    ->castTo<FunctionType>()->getResult();
  Type replacementTypes[] = {resultType};
  auto startBuiltin = cast<FuncDecl>(
      getBuiltinValueDecl(ctx, ctx.getIdentifier("startAsyncLet")));
  auto subs = SubstitutionMap::get(startBuiltin->getGenericSignature(),
                                   replacementTypes,
                                   ArrayRef<ProtocolConformanceRef>{});
  CanType origParamType = startBuiltin->getParameters()->get(2)
      ->getInterfaceType()->getCanonicalType();
  CanType substParamType = origParamType.subst(subs)->getCanonicalType();

  // Ensure that the closure has the appropriate type.
  AbstractionPattern origParam(
      startBuiltin->getGenericSignature().getCanonicalSignature(),
      origParamType);
  
  auto conversion = Conversion::getSubstToOrig(origParam, substParamType,
                                     getLoweredType(origParam, substParamType));
  ConvertingInitialization convertingInit(conversion, SGFContext());
  auto taskFunction = emitRValue(asyncLetEntryPoint,
                                 SGFContext(&convertingInit))
    .getAsSingleValue(*this, loc);
  taskFunction = emitSubstToOrigValue(loc, taskFunction,
                                      origParam, substParamType);

  auto apply = B.createBuiltin(
      loc,
      ctx.getIdentifier(getBuiltinName(BuiltinValueKind::StartAsyncLetWithLocalBuffer)),
      getLoweredType(ctx.TheRawPointerType), subs,
      {taskOptions, taskFunction.getValue(), resultBuf});

  return ManagedValue::forUnmanaged(apply);
}

ManagedValue SILGenFunction::emitCancelAsyncTask(
    SILLocation loc, SILValue task) {
  ASTContext &ctx = getASTContext();
  auto apply = B.createBuiltin(
      loc,
      ctx.getIdentifier(getBuiltinName(BuiltinValueKind::CancelAsyncTask)),
      getLoweredType(ctx.TheEmptyTupleType), SubstitutionMap(),
      { task });
  return ManagedValue::forUnmanaged(apply);
}

ManagedValue SILGenFunction::emitReadAsyncLetBinding(SILLocation loc,
                                                     VarDecl *var) {
  auto patternBinding = var->getParentPatternBinding();
  auto index = patternBinding->getPatternEntryIndexForVarDecl(var);
  auto childTask = AsyncLetChildTasks[{patternBinding, index}];

  auto pattern = patternBinding->getPattern(index);
  Type formalPatternType = pattern->getType();
  // async let context stores the maximally-abstracted representation.
  SILType loweredOpaquePatternType = getLoweredType(AbstractionPattern::getOpaque(),
                                                    formalPatternType);

  auto asyncLetGet = childTask.isThrowing
      ? SGM.getAsyncLetGetThrowing()
      : SGM.getAsyncLetGet();

  // The intrinsic returns a pointer to the address of the result value inside
  // the async let task context.
  emitApplyOfLibraryIntrinsic(loc, asyncLetGet, {},
                   {ManagedValue::forTrivialObjectRValue(childTask.asyncLet),
                    ManagedValue::forTrivialObjectRValue(childTask.resultBuf)},
                   SGFContext());
  
  auto resultAddr = B.createPointerToAddress(loc, childTask.resultBuf,
                loweredOpaquePatternType.getAddressType(),
                /*strict*/ true,
                /*invariant*/ true);
  
  // Project the address of the variable within the pattern binding result.
  struct ProjectResultVisitor: public PatternVisitor<ProjectResultVisitor>
  {
    SILGenFunction &SGF;
    SILLocation loc;
    VarDecl *var;
    SILValue resultAddr, varAddr;
    SmallVector<unsigned, 4> path;
    
    ProjectResultVisitor(SILGenFunction &SGF,
                         SILLocation loc,
                         VarDecl *var,
                         SILValue resultAddr)
      : SGF(SGF), loc(loc), var(var), resultAddr(resultAddr), varAddr() {}

    // Walk through non-binding patterns.
    void visitParenPattern(ParenPattern *P) {
      return visit(P->getSubPattern());
    }
    void visitTypedPattern(TypedPattern *P) {
      return visit(P->getSubPattern());
    }
    void visitBindingPattern(BindingPattern *P) {
      return visit(P->getSubPattern());
    }
    void visitTuplePattern(TuplePattern *P) {
      path.push_back(0);
      for (unsigned i : indices(P->getElements())) {
        path.back() = i;
        visit(P->getElement(i).getPattern());
        // If we found the variable of interest, we're done.
        if (varAddr)
          return;
      }
      path.pop_back();
    }
    void visitAnyPattern(AnyPattern *P) {}

    // When we see the variable binding, project it out of the aggregate.
    void visitNamedPattern(NamedPattern *P) {
      if (P->getDecl() != var)
        return;
      
      assert(!varAddr && "var appears in pattern more than once?");
      varAddr = resultAddr;
      for (unsigned component : path) {
        varAddr = SGF.B.createTupleElementAddr(loc, varAddr, component);
      }
    }

  #define INVALID_PATTERN(Id, Parent) \
    void visit##Id##Pattern(Id##Pattern *) { \
      llvm_unreachable("pattern not valid in var binding"); \
    }
  #define PATTERN(Id, Parent)
  #define REFUTABLE_PATTERN(Id, Parent) INVALID_PATTERN(Id, Parent)
  #include "swift/AST/PatternNodes.def"
  #undef INVALID_PATTERN
  };

  ProjectResultVisitor visitor(*this, loc, var, resultAddr);
  visitor.visit(pattern);
  assert(visitor.varAddr && "didn't find var in pattern?");
  
  // Load and reabstract the value if needed.
  auto genericSig = F.getLoweredFunctionType()->getInvocationGenericSignature();
  auto substVarTy = var->getType()->getReducedType(genericSig);
  auto substAbstraction = AbstractionPattern(genericSig, substVarTy);
  return emitLoad(loc, visitor.varAddr, substAbstraction, substVarTy,
                  getTypeLowering(substAbstraction, substVarTy),
                  SGFContext(), IsNotTake);
}

void SILGenFunction::emitFinishAsyncLet(
    SILLocation loc, SILValue asyncLet, SILValue resultPtr) {
  // This runtime function cancels the task, awaits its completion, and
  // destroys the value in the result buffer if necessary.
  emitApplyOfLibraryIntrinsic(loc, SGM.getFinishAsyncLet(), {},
                             {ManagedValue::forTrivialObjectRValue(asyncLet),
                              ManagedValue::forTrivialObjectRValue(resultPtr)},
                             SGFContext());
  // This builtin ends the lifetime of the allocation for the async let.
  auto &ctx = getASTContext();
  B.createBuiltin(loc,
    ctx.getIdentifier(getBuiltinName(BuiltinValueKind::EndAsyncLetLifetime)),
    getLoweredType(ctx.TheEmptyTupleType), {},
    {asyncLet});
}

// Create a partial application of a dynamic method, applying bridging thunks
// if necessary.
static ManagedValue emitDynamicPartialApply(SILGenFunction &SGF,
                                            SILLocation loc,
                                            SILValue method,
                                            SILValue self,
                                         CanAnyFunctionType foreignFormalType,
                                         CanAnyFunctionType nativeFormalType) {
  auto calleeConvention = ParameterConvention::Direct_Guaranteed;

  // Retain 'self' because the partial apply will take ownership.
  // We can't simply forward 'self' because the partial apply is conditional.
  if (!self->getType().isAddress())
    self = SGF.B.emitCopyValueOperation(loc, self);

  SILValue resultValue =
    SGF.B.createPartialApply(loc, method, {}, self, calleeConvention);
  ManagedValue result = SGF.emitManagedRValueWithCleanup(resultValue);

  // If necessary, thunk to the native ownership conventions and bridged types.
  auto nativeTy =
    SGF.getLoweredLoadableType(nativeFormalType).castTo<SILFunctionType>();

  if (nativeTy != resultValue->getType().getASTType()) {
    result = SGF.emitBlockToFunc(loc, result, foreignFormalType,
                                 nativeFormalType, nativeTy);
  }

  return result;
}

RValue SILGenFunction::emitDynamicMemberRef(SILLocation loc, SILValue operand,
                                            ConcreteDeclRef memberRef,
                                            CanType refTy, SGFContext C) {
  assert(refTy->isOptional());

  if (!memberRef.getDecl()->isInstanceMember()) {
    auto metatype = operand->getType().castTo<MetatypeType>();
    assert(metatype->getRepresentation() == MetatypeRepresentation::Thick);
    metatype = CanMetatypeType::get(metatype.getInstanceType(),
                                    MetatypeRepresentation::ObjC);
    operand = B.createThickToObjCMetatype(
        loc, operand, SILType::getPrimitiveObjectType(metatype));
  }

  // Create the continuation block.
  SILBasicBlock *contBB = createBasicBlock();

  // Create the no-member block.
  SILBasicBlock *noMemberBB = createBasicBlock();

  // Create the has-member block.
  SILBasicBlock *hasMemberBB = createBasicBlock();

  const TypeLowering &optTL = getTypeLowering(refTy);
  auto loweredOptTy = optTL.getLoweredType();

  SILValue optTemp = emitTemporaryAllocation(loc, loweredOptTy);

  // Create the branch.
  FuncDecl *memberFunc;
  if (auto *VD = dyn_cast<VarDecl>(memberRef.getDecl())) {
    memberFunc = VD->getOpaqueAccessor(AccessorKind::Get);
  } else {
    memberFunc = cast<FuncDecl>(memberRef.getDecl());
  }
  auto member = SILDeclRef(memberFunc, SILDeclRef::Kind::Func)
    .asForeign();
  B.createDynamicMethodBranch(loc, operand, member, hasMemberBB, noMemberBB);

  // Create the has-member branch.
  {
    B.emitBlock(hasMemberBB);

    FullExpr hasMemberScope(Cleanups, CleanupLocation(loc));

    // The argument to the has-member block is the uncurried method.
    const CanType valueTy = refTy.getOptionalObjectType();
    CanFunctionType methodTy;

    // For a computed variable, we want the getter.
    if (isa<VarDecl>(memberRef.getDecl())) {
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      CanFunctionType::ExtInfo info;
      methodTy = CanFunctionType::get({}, valueTy, info);
    } else {
      methodTy = cast<FunctionType>(valueTy);
    }

    // Build a partially-applied foreign formal type.
    // TODO: instead of building this and then potentially converting, we
    // should just build a single thunk.
    auto foreignMethodTy =
        getPartialApplyOfDynamicMethodFormalType(SGM, member, memberRef);

    // FIXME: Verify ExtInfo state is correct, not working by accident.
    CanFunctionType::ExtInfo info;
    FunctionType::Param arg(operand->getType().getASTType());
    auto memberFnTy = CanFunctionType::get({arg}, methodTy, info);

    auto loweredMethodTy = getDynamicMethodLoweredType(SGM.M, member,
                                                       memberFnTy);
    SILValue memberArg =
        hasMemberBB->createPhiArgument(loweredMethodTy, OwnershipKind::Owned);

    // Create the result value.
    Scope applyScope(Cleanups, CleanupLocation(loc));
    ManagedValue result = emitDynamicPartialApply(
        *this, loc, memberArg, operand, foreignMethodTy, methodTy);

    RValue resultRV;
    if (isa<VarDecl>(memberRef.getDecl())) {
      resultRV =
          emitMonomorphicApply(loc, result, {}, foreignMethodTy.getResult(),
                               valueTy, ApplyOptions(), llvm::None, llvm::None);
    } else {
      resultRV = RValue(*this, loc, valueTy, result);
    }

    // Package up the result in an optional.
    emitInjectOptionalValueInto(loc, {loc, std::move(resultRV)}, optTemp,
                                optTL);

    applyScope.pop();
    // Branch to the continuation block.
    B.createBranch(loc, contBB);
  }

  // Create the no-member branch.
  {
    B.emitBlock(noMemberBB);

    emitInjectOptionalNothingInto(loc, optTemp, optTL);

    // Branch to the continuation block.
    B.createBranch(loc, contBB);
  }

  // Emit the continuation block.
  B.emitBlock(contBB);

  // Package up the result.
  auto optResult = optTemp;
  if (optTL.isLoadable())
    optResult = optTL.emitLoad(B, loc, optResult, LoadOwnershipQualifier::Take);
  return RValue(*this, loc, refTy,
                emitManagedRValueWithCleanup(optResult, optTL));
}

RValue
SILGenFunction::emitDynamicSubscriptGetterApply(SILLocation loc,
                                                SILValue operand,
                                                ConcreteDeclRef subscriptRef,
                                                PreparedArguments &&indexArgs,
                                                CanType resultTy,
                                                SGFContext c) {
  assert(resultTy->isOptional());

  // Create the continuation block.
  SILBasicBlock *contBB = createBasicBlock();

  // Create the no-member block.
  SILBasicBlock *noMemberBB = createBasicBlock();

  // Create the has-member block.
  SILBasicBlock *hasMemberBB = createBasicBlock();

  const TypeLowering &optTL = getTypeLowering(resultTy);
  const SILValue optTemp = emitTemporaryAllocation(loc, optTL.getLoweredType());

  // Create the branch.
  auto *subscriptDecl = cast<SubscriptDecl>(subscriptRef.getDecl());
  auto member = SILDeclRef(subscriptDecl->getOpaqueAccessor(AccessorKind::Get),
                           SILDeclRef::Kind::Func)
    .asForeign();
  B.createDynamicMethodBranch(loc, operand, member, hasMemberBB, noMemberBB);

  // Create the has-member branch.
  {
    B.emitBlock(hasMemberBB);

    FullExpr hasMemberScope(Cleanups, CleanupLocation(loc));

    // The argument to the has-member block is the uncurried method.
    // Build the substituted getter type from the AST nodes.
    const CanType valueTy = resultTy.getOptionalObjectType();

    // FIXME: Verify ExtInfo state is correct, not working by accident.
    CanFunctionType::ExtInfo methodInfo;
    const auto methodTy =
        CanFunctionType::get(indexArgs.getParams(), valueTy, methodInfo);
    auto foreignMethodTy =
        getPartialApplyOfDynamicMethodFormalType(SGM, member, subscriptRef);

    // FIXME: Verify ExtInfo state is correct, not working by accident.
    CanFunctionType::ExtInfo functionInfo;
    FunctionType::Param baseArg(operand->getType().getASTType());
    auto functionTy = CanFunctionType::get({baseArg}, methodTy, functionInfo);
    auto loweredMethodTy = getDynamicMethodLoweredType(SGM.M, member,
                                                       functionTy);
    SILValue memberArg =
        hasMemberBB->createPhiArgument(loweredMethodTy, OwnershipKind::Owned);
    // Emit the application of 'self'.
    Scope applyScope(Cleanups, CleanupLocation(loc));
    ManagedValue result = emitDynamicPartialApply(
        *this, loc, memberArg, operand, foreignMethodTy, methodTy);

    // Collect the index values for application.
    llvm::SmallVector<ManagedValue, 2> indexValues;
    for (auto &source : std::move(indexArgs).getSources()) {
      // @objc subscripts cannot have 'inout' indices.
      RValue rVal = std::move(source).asKnownRValue(*this);

      // @objc subscripts cannot have tuple indices.
      indexValues.push_back(std::move(rVal).getScalarValue());
    }

    auto resultRV = emitMonomorphicApply(
        loc, result, indexValues, foreignMethodTy.getResult(), valueTy,
        ApplyOptions(), llvm::None, llvm::None);

    // Package up the result in an optional.
    emitInjectOptionalValueInto(loc, {loc, std::move(resultRV)}, optTemp,
                                optTL);

    applyScope.pop();
    // Branch to the continuation block.
    B.createBranch(loc, contBB);
  }

  // Create the no-member branch.
  {
    B.emitBlock(noMemberBB);

    emitInjectOptionalNothingInto(loc, optTemp, optTL);

    // Branch to the continuation block.
    B.createBranch(loc, contBB);
  }

  // Emit the continuation block.
  B.emitBlock(contBB);

  // Package up the result.
  auto optResult = optTemp;
  if (optTL.isLoadable())
    optResult = optTL.emitLoad(B, loc, optResult, LoadOwnershipQualifier::Take);
  return RValue(*this, loc, resultTy,
                emitManagedRValueWithCleanup(optResult, optTL));
}

SmallVector<ManagedValue, 4> SILGenFunction::emitKeyPathSubscriptOperands(
    SILLocation loc, SubscriptDecl *subscript,
    SubstitutionMap subs, ArgumentList *argList) {
  Type interfaceType = subscript->getInterfaceType();
  CanFunctionType substFnType =
      subs ? cast<FunctionType>(interfaceType->castTo<GenericFunctionType>()
                                    ->substGenericArgs(subs)
                                    ->getCanonicalType())
           : cast<FunctionType>(interfaceType->getCanonicalType());
  AbstractionPattern origFnType(substFnType);
  auto fnType =
      getLoweredType(origFnType, substFnType).castTo<SILFunctionType>()
        ->getUnsubstitutedType(SGM.M);

  SmallVector<ManagedValue, 4> argValues;
  SmallVector<DelayedArgument, 2> delayedArgs;
  ArgEmitter emitter(*this, loc, fnType->getRepresentation(),
                     /*yield*/ false,
                     /*isForCoroutine*/ false,
                     ClaimedParamsRef(fnType->getParameters()), argValues,
                     delayedArgs, ForeignInfo{});

  auto prepared =
      prepareSubscriptIndices(loc, subscript, subs,
                              // Strategy doesn't matter
                              AccessStrategy::getStorage(), argList);
  emitter.emitPreparedArgs(std::move(prepared), origFnType);

  if (!delayedArgs.empty())
    emitDelayedArguments(*this, delayedArgs, argValues);

  return argValues;
}

ManagedValue ArgumentScope::popPreservingValue(ManagedValue mv) {
  formalEvalScope.pop();
  return normalScope.popPreservingValue(mv);
}

RValue ArgumentScope::popPreservingValue(RValue &&rv) {
  formalEvalScope.pop();
  return normalScope.popPreservingValue(std::move(rv));
}
