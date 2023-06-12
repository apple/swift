//===--- SILTypeSubstitution.cpp - Apply substitutions to SIL types -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the core operations that apply substitutions to
// the lowered types used for SIL values.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "libsil"

#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/AST/InFlightSubstitution.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/CanTypeVisitor.h"

using namespace swift;
using namespace Lowering;

namespace {

/// Given a lowered SIL type, apply a substitution to it to produce another
/// lowered SIL type which uses the same abstraction conventions.
class SILTypeSubstituter :
    public CanTypeVisitor<SILTypeSubstituter, CanType> {
  TypeConverter &TC;
  InFlightSubstitution &IFS;

  // The signature for the original type.
  //
  // Replacement types are lowered with respect to the current
  // context signature.
  CanGenericSignature Sig;

  TypeExpansionContext typeExpansionContext;

public:
  SILTypeSubstituter(TypeConverter &TC,
                     TypeExpansionContext context,
                     InFlightSubstitution &IFS,
                     CanGenericSignature Sig)
    : TC(TC),
      IFS(IFS),
      Sig(Sig),
      typeExpansionContext(context)
  {}

  // SIL type lowering only does special things to tuples and functions.

  // When a function appears inside of another type, we only perform
  // substitutions if it is not polymorphic.
  CanSILFunctionType visitSILFunctionType(CanSILFunctionType origType) {
    return substSILFunctionType(origType, false);
  }

  SubstitutionMap substOpaqueTypes(SubstitutionMap subs) {
    if (!typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes())
      return subs;

    return subs.subst([&](SubstitutableType *s) -> Type {
        return substOpaqueTypesWithUnderlyingTypes(s->getCanonicalType(),
                                                   typeExpansionContext);
      }, [&](CanType dependentType,
             Type conformingReplacementType,
             ProtocolDecl *conformedProtocol) -> ProtocolConformanceRef {
        return substOpaqueTypesWithUnderlyingTypes(
               ProtocolConformanceRef(conformedProtocol),
               conformingReplacementType->getCanonicalType(),
               typeExpansionContext);
      },
      SubstFlags::SubstituteOpaqueArchetypes |
      SubstFlags::PreservePackExpansionLevel);
  }

  // Substitute a function type.
  CanSILFunctionType substSILFunctionType(CanSILFunctionType origType,
                                          bool isGenericApplication) {
    assert((!isGenericApplication || origType->isPolymorphic()) &&
           "generic application without invocation signature or with "
           "existing arguments");
    assert((!isGenericApplication || !IFS.shouldSubstituteOpaqueArchetypes()) &&
           "generic application while substituting opaque archetypes");

    // The general substitution rule is that we should only substitute
    // into the free components of the type, i.e. the components that
    // aren't inside a generic signature.  That rule would say:
    //
    // - If there are invocation substitutions, just substitute those;
    //   the other components are necessarily inside the invocation
    //   generic signature.
    //
    // - Otherwise, if there's an invocation generic signature,
    //   substitute nothing.  If we are applying generic arguments,
    //   add the appropriate invocation substitutions.
    //
    // - Otherwise, if there are pattern substitutions, just substitute
    //   those; the other components are inside the pattern generic
    //   signature.
    //
    // - Otherwise, substitute the basic components.
    //
    // There are two caveats here.  The first is that we haven't yet
    // written all the code that would be necessary in order to handle
    // invocation substitutions everywhere, and so we never build those.
    // Instead, we substitute into the pattern substitutions if present,
    // or the components if not, and build a type with no invocation
    // signature.  As a special case, when substituting a coroutine type,
    // we build pattern substitutions instead of substituting the
    // component types in order to preserve the original yield structure,
    // which factors into the continuation function ABI.
    //
    // The second is that this function is also used when substituting
    // opaque archetypes.  In this case, we may need to substitute
    // into component types even within generic signatures.  This is
    // safe because the substitutions used in this case don't change
    // generics, they just narrowly look through certain opaque archetypes.
    // If substitutions are present, we still don't substitute into
    // the basic components, in order to maintain the information about
    // what was abstracted there.

    auto patternSubs = origType->getPatternSubstitutions();

    // If we have an invocation signature, we generally shouldn't
    // substitute into the pattern substitutions and component types.
    if (auto sig = origType->getInvocationGenericSignature()) {
      // Substitute the invocation substitutions if present.
      if (auto invocationSubs = origType->getInvocationSubstitutions()) {
        assert(!isGenericApplication);
        invocationSubs = substSubstitutions(invocationSubs);
        auto substType =
          origType->withInvocationSubstitutions(invocationSubs);

        // Also do opaque-type substitutions on the pattern substitutions
        // if requested and applicable.
        if (patternSubs) {
          patternSubs = substOpaqueTypes(patternSubs);
          substType = substType->withPatternSubstitutions(patternSubs);
        }

        return substType;
      }

      // Otherwise, we shouldn't substitute any components except
      // when substituting opaque archetypes.

      // If we're doing a generic application, and there are pattern
      // substitutions, substitute into the pattern substitutions; or if
      // it's a coroutine, build pattern substitutions; or else, fall
      // through to substitute the component types as discussed above.
      if (isGenericApplication) {
        if (patternSubs || origType->isCoroutine()) {
          CanSILFunctionType substType = origType;
          if (typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes()) {
            substType =
              origType->substituteOpaqueArchetypes(TC, typeExpansionContext);
          }

          SubstitutionMap subs;
          if (patternSubs) {
            subs = substSubstitutions(patternSubs);
          } else {
            subs = SubstitutionMap::get(sig, IFS);
          }
          auto witnessConformance = substWitnessConformance(origType);
          substType = substType->withPatternSpecialization(nullptr, subs,
                                                           witnessConformance);
          if (typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes()) {
            substType =
              substType->substituteOpaqueArchetypes(TC, typeExpansionContext);
          }
          return substType;
        }
        // else fall down to component substitution

      // If we're substituting opaque archetypes, and there are pattern
      // substitutions present, just substitute those and preserve the
      // basic structure in the component types.  Otherwise, fall through
      // to substitute the component types.
      } else if (IFS.shouldSubstituteOpaqueArchetypes()) {
        if (patternSubs) {
          patternSubs = substOpaqueTypes(patternSubs);
          auto witnessConformance = substWitnessConformance(origType);
          return origType->withPatternSpecialization(sig, patternSubs,
                                                     witnessConformance);
        }
        // else fall down to component substitution

      // Otherwise, don't try to substitute bound components.
      } else {
        auto substType = origType;
        if (patternSubs) {
          patternSubs = substOpaqueTypes(patternSubs);
          auto witnessConformance = substWitnessConformance(origType);
          substType = substType->withPatternSpecialization(sig, patternSubs,
                                                           witnessConformance);
        }
        return substType;
      }

    // Otherwise, if there are pattern substitutions, just substitute
    // into those and don't touch the component types.
    } else if (patternSubs) {
      patternSubs = substSubstitutions(patternSubs);
      auto witnessConformance = substWitnessConformance(origType);
      return origType->withPatternSpecialization(nullptr, patternSubs,
                                                 witnessConformance);
    }

    // Otherwise, we need to substitute component types.

    SmallVector<SILResultInfo, 8> substResults;
    substResults.reserve(origType->getNumResults());
    for (auto origResult : origType->getResults()) {
      substResults.push_back(substInterface(origResult));
    }

    auto substErrorResult = origType->getOptionalErrorResult();
    assert(!substErrorResult ||
           (!substErrorResult->getInterfaceType()->hasTypeParameter() &&
            !substErrorResult->getInterfaceType()->hasArchetype()));

    SmallVector<SILParameterInfo, 8> substParams;
    substParams.reserve(origType->getParameters().size());
    for (auto &origParam : origType->getParameters()) {
      substParams.push_back(substInterface(origParam));
    }

    SmallVector<SILYieldInfo, 8> substYields;
    substYields.reserve(origType->getYields().size());
    for (auto &origYield : origType->getYields()) {
      substYields.push_back(substInterface(origYield));
    }

    auto witnessMethodConformance = substWitnessConformance(origType);

    // The substituted type is no longer generic, so it'd never be
    // pseudogeneric.
    auto extInfo = origType->getExtInfo();
    if (!IFS.shouldSubstituteOpaqueArchetypes())
      extInfo = extInfo.intoBuilder().withIsPseudogeneric(false).build();

    auto genericSig = IFS.shouldSubstituteOpaqueArchetypes()
                        ? origType->getInvocationGenericSignature()
                        : nullptr;

    return SILFunctionType::get(genericSig, extInfo,
                                origType->getCoroutineKind(),
                                origType->getCalleeConvention(), substParams,
                                substYields, substResults, substErrorResult,
                                SubstitutionMap(), SubstitutionMap(),
                                TC.Context, witnessMethodConformance);
  }

  ProtocolConformanceRef substWitnessConformance(CanSILFunctionType origType) {
    auto conformance = origType->getWitnessMethodConformanceOrInvalid();
    if (!conformance) return conformance;

    assert(origType->getExtInfo().hasSelfParam());
    auto selfType = origType->getSelfParameter().getInterfaceType();

    // The Self type can be nested in a few layers of metatypes (etc.).
    while (auto metatypeType = dyn_cast<MetatypeType>(selfType)) {
      auto next = metatypeType.getInstanceType();
      if (next == selfType)
        break;
      selfType = next;
    }

    auto substConformance = conformance.subst(selfType, IFS);

    // Substitute the underlying conformance of opaque type archetypes if we
    // should look through opaque archetypes.
    if (typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes()) {
      auto substType = IFS.withNewOptions(None, [&] {
        return selfType.subst(IFS)->getCanonicalType();
      });
      if (substType->hasOpaqueArchetype()) {
        substConformance = substOpaqueTypesWithUnderlyingTypes(
            substConformance, substType, typeExpansionContext);
      }
    }

    return substConformance;
  }

  SILType subst(SILType type) {
    return SILType::getPrimitiveType(visit(type.getRawASTType()),
                                     type.getCategory());
  }

  SILResultInfo substInterface(SILResultInfo orig) {
    return SILResultInfo(visit(orig.getInterfaceType()), orig.getConvention());
  }

  SILYieldInfo substInterface(SILYieldInfo orig) {
    return SILYieldInfo(visit(orig.getInterfaceType()), orig.getConvention());
  }

  SILParameterInfo substInterface(SILParameterInfo orig) {
    return SILParameterInfo(visit(orig.getInterfaceType()),
                            orig.getConvention(), orig.getDifferentiability());
  }

  CanType visitSILPackType(CanSILPackType origType) {
    // Fast-path the empty pack.
    if (origType->getNumElements() == 0) return origType;

    SmallVector<CanType, 8> substEltTypes;

    substEltTypes.reserve(origType->getNumElements());

    for (CanType origEltType : origType->getElementTypes()) {
      if (auto origExpansionType = dyn_cast<PackExpansionType>(origEltType)) {
        substPackExpansion(origExpansionType, [&](CanType substExpandedType) {
          substEltTypes.push_back(substExpandedType);
        });
      } else {
        auto substEltType = visit(origEltType);
        substEltTypes.push_back(substEltType);
      }
    }
    return SILPackType::get(TC.Context, origType->getExtInfo(), substEltTypes);
  }

  CanType visitPackType(CanPackType origType) {
    llvm_unreachable("CanPackType shouldn't show in lowered types");
  }

  /* FIXME: Uncomment this once SubstFlags::PreservePackExpansionLevel is gone */
#if 0
  CanType visitPackExpansionType(CanPackExpansionType origType) {
    llvm_unreachable("shouldn't substitute an independent lowered pack "
                     "expansion type");
  }
#endif

  void substPackExpansion(CanPackExpansionType origType,
                          llvm::function_ref<void(CanType)> addExpandedType) {
    IFS.expandPackExpansionShape(origType.getCountType(),
                                [&](Type substExpansionShape) {
      CanType substComponentType = visit(origType.getPatternType());
      if (substExpansionShape) {
        substComponentType = CanPackExpansionType::get(substComponentType,
                                    substExpansionShape->getCanonicalType());
      }
      addExpandedType(substComponentType);
    });
  }

  /// Tuples need to have their component types substituted by these
  /// same rules.
  CanType visitTupleType(CanTupleType origType) {
    // Fast-path the empty tuple.
    if (origType->getNumElements() == 0) return origType;

    SmallVector<TupleTypeElt, 8> substElts;
    substElts.reserve(origType->getNumElements());
    for (auto &origElt : origType->getElements()) {
      CanType origEltType = CanType(origElt.getType());

      if (auto origExpansion = dyn_cast<PackExpansionType>(origEltType)) {
        bool first = true;
        substPackExpansion(origExpansion, [&](CanType substEltType) {
          auto substElt = origElt.getWithType(substEltType);
          if (first) {
            first = false;
          } else {
            substElt = substElt.getWithoutName();
          }
          substElts.push_back(substElt);
        });
      } else {
        auto substEltType = visit(origEltType);
        substElts.push_back(origElt.getWithType(substEltType));
      }
    }

    // Turn unlabeled singleton scalar tuples into their underlying types.
    // The AST type substituter doesn't actually implement this rule yet,
    // but we need to implement it in SIL in order to support testing,
    // since the type parser can't parse a singleton tuple.
    //
    // For compatibility with previous behavior, don't do this if the
    // original tuple type was singleton.  AutoDiff apparently really
    // likes making singleton tuples.
    if (isParenType(substElts) && !isParenType(origType->getElements()))
      return CanType(substElts[0].getType());

    return CanType(TupleType::get(substElts, TC.Context));
  }

  static bool isParenType(ArrayRef<TupleTypeElt> elts) {
    return (elts.size() == 1 &&
            !elts[0].hasName() &&
            !isa<PackExpansionType>(CanType(elts[0].getType())));
  }

  // Block storage types need to substitute their capture type by these same
  // rules.
  CanType visitSILBlockStorageType(CanSILBlockStorageType origType) {
    auto substCaptureType = visit(origType->getCaptureType());
    return SILBlockStorageType::get(substCaptureType);
  }

  /// Optionals need to have their object types substituted by these rules.
  CanType visitBoundGenericEnumType(CanBoundGenericEnumType origType) {
    // Only use a special rule if it's Optional.
    if (!origType->getDecl()->isOptionalDecl()) {
      return visitType(origType);
    }

    CanType origObjectType = origType.getGenericArgs()[0];
    CanType substObjectType = visit(origObjectType);
    return CanType(BoundGenericType::get(origType->getDecl(), Type(),
                                         substObjectType));
  }

  /// Any other type would be a valid type in the AST. Just apply the
  /// substitution on the AST level and then lower that.
  CanType visitType(CanType origType) {
    assert(!isa<AnyFunctionType>(origType));
    assert(!isa<LValueType>(origType) && !isa<InOutType>(origType));

    CanType substType = substASTType(origType);

    // If the substitution didn't change anything, we know that the
    // original type was a lowered type, so we're good.
    if (origType == substType) {
      return origType;
    }

    // We've looked through all the top-level structure in the orig
    // type that's affected by type lowering.  If substitution has
    // given us a type with top-level structure that's affected by
    // type lowering, it must be because the orig type was a type
    // variable of some sort, and we should lower using an opaque
    // abstraction pattern.  If substitution hasn't given us such a
    // type, it doesn't matter what abstraction pattern we use,
    // lowering will just come back with substType.  So we can just
    // use an opaque abstraction pattern here and not put any effort
    // into computing a more "honest" abstraction pattern.
    AbstractionPattern abstraction = AbstractionPattern::getOpaque();
    return TC.getLoweredRValueType(typeExpansionContext, abstraction,
                                   substType);
  }

  CanType substASTType(CanType origType) {
    return origType.subst(IFS)->getCanonicalType();
  }

  SubstitutionMap substSubstitutions(SubstitutionMap subs) {
    SubstitutionMap newSubs = subs.subst(IFS);

    // If we need to look through opaque types in this context, re-substitute
    // according to the expansion context.
    newSubs = substOpaqueTypes(newSubs);

    return newSubs;
  }
};

} // end anonymous namespace

static bool isSubstitutionInvariant(SILType ty, SubstOptions options) {
  return (!ty.hasArchetype() &&
          !ty.hasTypeParameter() &&
          (!options.contains(SubstFlags::SubstituteOpaqueArchetypes) ||
           !ty.getRawASTType()->hasOpaqueArchetype()));
}

SILType SILType::subst(TypeConverter &tc, TypeSubstitutionFn subs,
                       LookupConformanceFn conformances,
                       CanGenericSignature genericSig,
                       SubstOptions options) const {
  if (isSubstitutionInvariant(*this, options))
    return *this;

  InFlightSubstitution IFS(subs, conformances, options);
  SILTypeSubstituter STST(tc, TypeExpansionContext::minimal(), IFS,
                          genericSig);
  return STST.subst(*this);
}

SILType SILType::subst(TypeConverter &tc, InFlightSubstitution &IFS,
                       CanGenericSignature genericSig) const {
  if (isSubstitutionInvariant(*this, IFS.getOptions()))
    return *this;

  SILTypeSubstituter STST(tc, TypeExpansionContext::minimal(), IFS,
                          genericSig);
  return STST.subst(*this);
}

SILType SILType::subst(SILModule &M, TypeSubstitutionFn subs,
                       LookupConformanceFn conformances,
                       CanGenericSignature genericSig,
                       SubstOptions options) const {
  return subst(M.Types, subs, conformances, genericSig, options);
}

SILType SILType::subst(TypeConverter &tc, SubstitutionMap subs) const {
  auto sig = subs.getGenericSignature();

  InFlightSubstitutionViaSubMap IFS(subs, None);
  return subst(tc, IFS, sig.getCanonicalSignature());
}
SILType SILType::subst(SILModule &M, SubstitutionMap subs) const{
  return subst(M.Types, subs);
}

SILType SILType::subst(SILModule &M, SubstitutionMap subs,
                       TypeExpansionContext context) const {
  if (isSubstitutionInvariant(*this, None))
    return *this;

  InFlightSubstitutionViaSubMap IFS(subs, None);

  SILTypeSubstituter STST(M.Types, context, IFS,
                          subs.getGenericSignature().getCanonicalSignature());
  return STST.subst(*this);
}

/// Apply a substitution to this polymorphic SILFunctionType so that
/// it has the form of the normal SILFunctionType for the substituted
/// type, except using the original conventions.
CanSILFunctionType
SILFunctionType::substGenericArgs(SILModule &silModule, SubstitutionMap subs,
                                  TypeExpansionContext context) {
  if (!isPolymorphic()) {
    return CanSILFunctionType(this);
  }

  if (subs.empty()) {
    return CanSILFunctionType(this);
  }

  InFlightSubstitutionViaSubMap IFS(subs, None);

  return substGenericArgs(silModule, IFS, context);
}

CanSILFunctionType
SILFunctionType::substGenericArgs(SILModule &silModule,
                                  TypeSubstitutionFn subs,
                                  LookupConformanceFn conformances,
                                  TypeExpansionContext context) {
  if (!isPolymorphic()) return CanSILFunctionType(this);

  InFlightSubstitution IFS(subs, conformances, None);
  return substGenericArgs(silModule, IFS, context);
}

CanSILFunctionType
SILFunctionType::substGenericArgs(SILModule &silModule,
                                  InFlightSubstitution &IFS,
                                  TypeExpansionContext context) {
  if (!isPolymorphic()) return CanSILFunctionType(this);

  SILTypeSubstituter substituter(silModule.Types, context, IFS,
                                 getSubstGenericSignature());
  return substituter.substSILFunctionType(CanSILFunctionType(this), true);
}

CanSILFunctionType
SILFunctionType::substituteOpaqueArchetypes(TypeConverter &TC,
                                            TypeExpansionContext context) {
  if (!hasOpaqueArchetype() ||
      !context.shouldLookThroughOpaqueTypeArchetypes())
    return CanSILFunctionType(this);

  ReplaceOpaqueTypesWithUnderlyingTypes replacer(
      context.getContext(), context.getResilienceExpansion(),
      context.isWholeModuleContext());

  InFlightSubstitution IFS(replacer, replacer,
                           SubstFlags::SubstituteOpaqueArchetypes |
                           SubstFlags::PreservePackExpansionLevel);

  SILTypeSubstituter substituter(TC, context, IFS, getSubstGenericSignature());
  auto resTy =
    substituter.substSILFunctionType(CanSILFunctionType(this), false);

  return resTy;
}
