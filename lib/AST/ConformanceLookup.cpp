//===--- ConformanceLookup.cpp - Global Conformance Lookup ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements global conformance lookup.
//
//  - ModuleDecl::lookupConformance(type, proto) takes a nominal type or an
//    archetype and returns the appropriate normal, specialized or abstract
//    conformance. It does not check conditional requirements.
//
//  - ModuleDecl::checkConformance(type, proto) is like the above, but checks
//    conditional requirements. The type must not contain type parameters;
//    they must either be substituted with concrete types by applying a
//    substitution map, or mapped to archetypes in a generic environment first.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Module.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

ArrayRef<ProtocolConformanceRef>
ModuleDecl::collectExistentialConformances(CanType fromType,
                                           CanType existential,
                                           bool skipConditionalRequirements,
                                           bool allowMissing) {
  CollectExistentialConformancesRequest request{this,
                                                fromType,
                                                existential,
                                                skipConditionalRequirements,
                                                allowMissing};
  return evaluateOrDefault(getASTContext().evaluator, request, /*default=*/{});
}

ProtocolConformanceRef
ModuleDecl::lookupExistentialConformance(Type type, ProtocolDecl *protocol) {
  ASTContext &ctx = getASTContext();

  assert(type->isExistentialType());

  // If the existential type cannot be represented or the protocol does not
  // conform to itself, there's no point in looking further.
  if (!protocol->existentialConformsToSelf())
    return ProtocolConformanceRef::forInvalid();

  if (protocol->isSpecificProtocol(KnownProtocolKind::Copyable)
      && !ctx.LangOpts.AssumesNoncopyableGenerics) {
    // Prior to noncopyable generics, all existentials conform to Copyable.
        return ProtocolConformanceRef(
            ctx.getBuiltinConformance(type, protocol,
                                      BuiltinConformanceKind::Synthesized));
  }

  auto layout = type->getExistentialLayout();

  // Due to an IRGen limitation, witness tables cannot be passed from an
  // existential to an archetype parameter, so for now we restrict this to
  // @objc protocols and marker protocols.
  if (!layout.isObjC() && !protocol->isMarkerProtocol()) {
    auto constraint = type;
    if (auto existential = constraint->getAs<ExistentialType>())
      constraint = existential->getConstraintType();

    // There's a specific exception for protocols with self-conforming
    // witness tables, but the existential has to be *exactly* that type.
    // TODO: synthesize witness tables on-demand for protocol compositions
    // that can satisfy the requirement.
    if (protocol->requiresSelfConformanceWitnessTable() &&
        constraint->is<ProtocolType>() &&
        constraint->castTo<ProtocolType>()->getDecl() == protocol)
      return ProtocolConformanceRef(ctx.getSelfConformance(protocol));

    return ProtocolConformanceRef::forInvalid();
  }

  // If the existential is class-constrained, the class might conform
  // concretely.
  if (auto superclass = layout.explicitSuperclass) {
    if (auto result = lookupConformance(
            superclass, protocol, /*allowMissing=*/false)) {
      if (protocol->isSpecificProtocol(KnownProtocolKind::Sendable) &&
          result.hasUnavailableConformance())
        result = ProtocolConformanceRef::forInvalid();

      return result;
    }
  }

  // Otherwise, the existential might conform abstractly.
  for (auto protoDecl : layout.getProtocols()) {

    // If we found the protocol we're looking for, return an abstract
    // conformance to it.
    if (protoDecl == protocol)
      return ProtocolConformanceRef(ctx.getSelfConformance(protocol));

    // If the protocol has a superclass constraint, we might conform
    // concretely.
    if (auto superclass = protoDecl->getSuperclass()) {
      if (auto result = lookupConformance(superclass, protocol))
        return result;
    }

    // Now check refined protocols.
    if (protoDecl->inheritsFrom(protocol))
      return ProtocolConformanceRef(ctx.getSelfConformance(protocol));
  }

  // We didn't find our protocol in the existential's list; it doesn't
  // conform.
  return ProtocolConformanceRef::forInvalid();
}

/// Whether we should create missing conformances to the given protocol.
static bool shouldCreateMissingConformances(Type type, ProtocolDecl *proto) {
  // Sendable may be able to be synthesized.
  if (proto->isSpecificProtocol(KnownProtocolKind::Sendable)) {
    return true;
  }

  return false;
}

ProtocolConformanceRef ProtocolConformanceRef::forMissingOrInvalid(
    Type type, ProtocolDecl *proto) {
  // Introduce "missing" conformances when appropriate, so that type checking
  // (and even code generation) can continue.
  ASTContext &ctx = proto->getASTContext();
  if (shouldCreateMissingConformances(type, proto)) {
    return ProtocolConformanceRef(
        ctx.getBuiltinConformance(
          type, proto, BuiltinConformanceKind::Missing));
  }

  return ProtocolConformanceRef::forInvalid();
}

ProtocolConformanceRef ModuleDecl::lookupConformance(Type type,
                                                     ProtocolDecl *protocol,
                                                     bool allowMissing) {
  // If we are recursively checking for implicit conformance of a nominal
  // type to a KnownProtocol, fail without evaluating this request. This
  // squashes cycles.
  LookupConformanceInModuleRequest request{{this, type, protocol}};
  if (auto kp = protocol->getKnownProtocolKind()) {
    if (auto nominal = type->getAnyNominal()) {
      ImplicitKnownProtocolConformanceRequest icvRequest{nominal, *kp};
      if (getASTContext().evaluator.hasActiveRequest(icvRequest) ||
          getASTContext().evaluator.hasActiveRequest(request)) {
        assert(!getInvertibleProtocolKind(*kp));
        return ProtocolConformanceRef::forInvalid();
      }
    }
  }

  auto result = evaluateOrDefault(
      getASTContext().evaluator, request, ProtocolConformanceRef::forInvalid());

  // If we aren't supposed to allow missing conformances but we have one,
  // replace the result with an "invalid" result.
  if (!allowMissing &&
      shouldCreateMissingConformances(type, protocol) &&
      result.hasMissingConformance())
    return ProtocolConformanceRef::forInvalid();

  return result;
}

/// Synthesize a builtin tuple type conformance to the given protocol, if
/// appropriate.
static ProtocolConformanceRef getBuiltinTupleTypeConformance(
    Type type, const TupleType *tupleType, ProtocolDecl *protocol,
    ModuleDecl *module) {
  ASTContext &ctx = protocol->getASTContext();

  auto *tupleDecl = ctx.getBuiltinTupleDecl();

  // Find the (unspecialized) conformance.
  SmallVector<ProtocolConformance *, 2> conformances;
  if (tupleDecl->lookupConformance(protocol, conformances)) {
    // If we have multiple conformances, first try to filter out any that are
    // unavailable on the current deployment target.
    //
    // FIXME: Conformance lookup should really depend on source location for
    // this to be 100% correct.
    if (conformances.size() > 1) {
      SmallVector<ProtocolConformance *, 2> availableConformances;

      for (auto *conformance : conformances) {
        if (conformance->getDeclContext()->isAlwaysAvailableConformanceContext())
          availableConformances.push_back(conformance);
      }

      // Don't filter anything out if all conformances are unavailable.
      if (!availableConformances.empty())
        std::swap(availableConformances, conformances);
    }

    auto *conformance = cast<NormalProtocolConformance>(conformances.front());
    auto subMap = type->getContextSubstitutionMap(module,
                                                  conformance->getDeclContext());

    // TODO: labels
    auto *specialized = ctx.getSpecializedConformance(type, conformance, subMap);
    return ProtocolConformanceRef(specialized);
  }

  return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
}

using EitherFunctionType =
    llvm::PointerUnion<const SILFunctionType *, const FunctionType *>;

/// Whether the given function type conforms to Sendable.
static bool isSendableFunctionType(EitherFunctionType eitherFnTy) {
  FunctionTypeRepresentation representation;

  if (auto silFnTy = eitherFnTy.dyn_cast<const SILFunctionType *>()) {
    if (silFnTy->isSendable())
      return true;

    // convert SILFunctionTypeRepresentation -> FunctionTypeRepresentation
    auto converted = convertRepresentation(silFnTy->getRepresentation());
    if (!converted)
      return false;

    representation = *converted;

  } else {
    auto functionType = eitherFnTy.get<const FunctionType *>();

    if (functionType->isSendable())
      return true;

    representation = functionType->getExtInfo().getRepresentation();
  }

  // C and thin function types have no captures, so they are Sendable.
  switch (representation) {
  case FunctionTypeRepresentation::Block:
  case FunctionTypeRepresentation::Swift:
    return false;

  case FunctionTypeRepresentation::CFunctionPointer:
  case FunctionTypeRepresentation::Thin:
    return true;
  }
}

/// Whether the given function type conforms to Escapable.
static bool isEscapableFunctionType(EitherFunctionType eitherFnTy) {
  if (auto silFnTy = eitherFnTy.dyn_cast<const SILFunctionType *>()) {
    return !silFnTy->isNoEscape();
  }

  auto functionType = eitherFnTy.get<const FunctionType *>();

  // TODO: what about autoclosures?
  return !functionType->isNoEscape();
}

static bool isBitwiseCopyableFunctionType(EitherFunctionType eitherFnTy) {
  SILFunctionTypeRepresentation representation;
  if (auto silFnTy = eitherFnTy.dyn_cast<const SILFunctionType *>()) {
    representation = silFnTy->getRepresentation();
  } else {
    auto fnTy = eitherFnTy.get<const FunctionType *>();
    representation = convertRepresentation(fnTy->getRepresentation());
  }

  switch (representation) {
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Block:
    return false;
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::CXXMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::KeyPathAccessorGetter:
  case SILFunctionTypeRepresentation::KeyPathAccessorSetter:
  case SILFunctionTypeRepresentation::KeyPathAccessorEquals:
  case SILFunctionTypeRepresentation::KeyPathAccessorHash:
    return true;
  }
}

/// Synthesize a builtin function type conformance to the given protocol, if
/// appropriate.
static ProtocolConformanceRef getBuiltinFunctionTypeConformance(
    Type type, EitherFunctionType functionType, ProtocolDecl *protocol) {
  ASTContext &ctx = protocol->getASTContext();

  auto synthesizeConformance = [&]() -> ProtocolConformanceRef {
    return ProtocolConformanceRef(
        ctx.getBuiltinConformance(type, protocol,
                                  BuiltinConformanceKind::Synthesized));
  };

  if (auto kp = protocol->getKnownProtocolKind()) {
    switch (*kp) {
    case KnownProtocolKind::Escapable:
      if (isEscapableFunctionType(functionType))
        return synthesizeConformance();
      break;
    case KnownProtocolKind::Sendable:
      // @Sendable function types are Sendable.
      if (isSendableFunctionType(functionType))
        return synthesizeConformance();
      break;
    case KnownProtocolKind::Copyable:
      // Functions cannot permanently destroy a move-only var/let
      // that they capture, so it's safe to copy functions, like classes.
      return synthesizeConformance();
    case KnownProtocolKind::BitwiseCopyable:
      if (isBitwiseCopyableFunctionType(functionType))
        return synthesizeConformance();
      break;
    default:
      break;
    }
  }

  return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
}

/// Synthesize a builtin metatype type conformance to the given protocol, if
/// appropriate.
static ProtocolConformanceRef getBuiltinMetaTypeTypeConformance(
    Type type, const AnyMetatypeType *metatypeType, ProtocolDecl *protocol) {
  ASTContext &ctx = protocol->getASTContext();

  if (!ctx.LangOpts.AssumesNoncopyableGenerics &&
      protocol->isSpecificProtocol(KnownProtocolKind::Copyable)) {
    // Only metatypes of Copyable types are Copyable.
    if (metatypeType->getInstanceType()->isNoncopyable()) {
      return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
    } else {
      return ProtocolConformanceRef(
          ctx.getBuiltinConformance(type, protocol,
                                    BuiltinConformanceKind::Synthesized));
    }
  }

  // All metatypes are Sendable, Copyable, Escapable, and BitwiseCopyable.
  if (auto kp = protocol->getKnownProtocolKind()) {
    switch (*kp) {
    case KnownProtocolKind::Sendable:
    case KnownProtocolKind::Copyable:
    case KnownProtocolKind::Escapable:
    case KnownProtocolKind::BitwiseCopyable:
      return ProtocolConformanceRef(
          ctx.getBuiltinConformance(type, protocol,
                                    BuiltinConformanceKind::Synthesized));
    default:
      break;
    }
  }

  return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
}

/// Synthesize a builtin type conformance to the given protocol, if
/// appropriate.
static ProtocolConformanceRef
getBuiltinBuiltinTypeConformance(Type type, const BuiltinType *builtinType,
                                 ProtocolDecl *protocol) {
  if (auto kp = protocol->getKnownProtocolKind()) {
    switch (*kp) {
    // All builtin types are Sendable, Copyable, and Escapable.
    case KnownProtocolKind::Sendable:
    case KnownProtocolKind::Copyable:
    case KnownProtocolKind::Escapable: {
      ASTContext &ctx = protocol->getASTContext();
      return ProtocolConformanceRef(
          ctx.getBuiltinConformance(type, protocol,
                                  BuiltinConformanceKind::Synthesized));
    }
    // Some builtin types are BitwiseCopyable.
    case KnownProtocolKind::BitwiseCopyable: {
      if (builtinType->isBitwiseCopyable()) {
        ASTContext &ctx = protocol->getASTContext();
        return ProtocolConformanceRef(ctx.getBuiltinConformance(
            type, protocol, BuiltinConformanceKind::Synthesized));
      }
      break;
    }
    default:
      break;
    }
  }

  return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
}

static ProtocolConformanceRef getPackTypeConformance(
    PackType *type, ProtocolDecl *protocol, ModuleDecl *mod) {
  SmallVector<ProtocolConformanceRef, 2> patternConformances;

  for (auto packElement : type->getElementTypes()) {
    if (auto *packExpansion = packElement->getAs<PackExpansionType>()) {
      auto patternType = packExpansion->getPatternType();

      auto patternConformance =
          (patternType->isTypeParameter()
           ? ProtocolConformanceRef(protocol)
           : mod->lookupConformance(patternType, protocol,
                                    /*allowMissing=*/true));
      patternConformances.push_back(patternConformance);
      continue;
    }

    auto patternConformance =
        (packElement->isTypeParameter()
         ? ProtocolConformanceRef(protocol)
         : mod->lookupConformance(packElement, protocol,
                                  /*allowMissing=*/true));
    patternConformances.push_back(patternConformance);
  }

  return ProtocolConformanceRef(
      PackConformance::get(type, protocol, patternConformances));
}

ProtocolConformanceRef
LookupConformanceInModuleRequest::evaluate(
    Evaluator &evaluator, LookupConformanceDescriptor desc) const {
  auto *mod = desc.Mod;
  auto type = desc.Ty;
  auto *protocol = desc.PD;
  ASTContext &ctx = mod->getASTContext();

  // A dynamic Self type conforms to whatever its underlying type
  // conforms to.
  if (auto selfType = type->getAs<DynamicSelfType>())
    type = selfType->getSelfType();

  // An archetype conforms to a protocol if the protocol is listed in the
  // archetype's list of conformances, or if the archetype has a superclass
  // constraint and the superclass conforms to the protocol.
  if (auto archetype = type->getAs<ArchetypeType>()) {

    // Without noncopyable generics, all archetypes are Copyable
    if (!ctx.LangOpts.AssumesNoncopyableGenerics)
      if (protocol->isSpecificProtocol(KnownProtocolKind::Copyable))
        return ProtocolConformanceRef(protocol);

    // The generic signature builder drops conformance requirements that are made
    // redundant by a superclass requirement, so check for a concrete
    // conformance first, since an abstract conformance might not be
    // able to be resolved by a substitution that makes the archetype
    // concrete.
    if (auto super = archetype->getSuperclass()) {
      auto inheritedConformance = mod->lookupConformance(
          super, protocol, /*allowMissing=*/false);
      if (protocol->isSpecificProtocol(KnownProtocolKind::Sendable) &&
          inheritedConformance.hasUnavailableConformance())
        inheritedConformance = ProtocolConformanceRef::forInvalid();
      if (inheritedConformance) {
        return ProtocolConformanceRef(ctx.getInheritedConformance(
            type, inheritedConformance.getConcrete()));
      }
    }

    for (auto ap : archetype->getConformsTo()) {
      if (ap == protocol || ap->inheritsFrom(protocol))
        return ProtocolConformanceRef(protocol);
    }

    return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
  }

  // An existential conforms to a protocol if the protocol is listed in the
  // existential's list of conformances and the existential conforms to
  // itself.
  if (type->isExistentialType()) {
    auto result = mod->lookupExistentialConformance(type, protocol);
    if (result.isInvalid())
      return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
    return result;
  }

  // Type variables have trivial conformances.
  if (type->isTypeVariableOrMember())
    return ProtocolConformanceRef(protocol);

  // UnresolvedType is a placeholder for an unknown type used when generating
  // diagnostics.  We consider it to conform to all protocols, since the
  // intended type might have. Same goes for PlaceholderType.
  if (type->is<UnresolvedType>() || type->is<PlaceholderType>())
    return ProtocolConformanceRef(protocol);

  // Pack types can conform to protocols.
  if (auto packType = type->getAs<PackType>()) {
    return getPackTypeConformance(packType, protocol, mod);
  }

  // Tuple types can conform to protocols.
  if (auto tupleType = type->getAs<TupleType>()) {
    return getBuiltinTupleTypeConformance(type, tupleType, protocol, mod);
  }

  // Function types can conform to protocols.
  if (auto functionType = type->getAs<FunctionType>()) {
    return getBuiltinFunctionTypeConformance(type, functionType, protocol);
  }

  // SIL function types in the AST can conform to protocols
  if (auto silFn = type->getAs<SILFunctionType>()) {
    return getBuiltinFunctionTypeConformance(type, silFn, protocol);
  }

  // Metatypes can conform to protocols.
  if (auto metatypeType = type->getAs<AnyMetatypeType>()) {
    return getBuiltinMetaTypeTypeConformance(type, metatypeType, protocol);
  }

  // Builtin types can conform to protocols.
  if (auto builtinType = type->getAs<BuiltinType>()) {
    return getBuiltinBuiltinTypeConformance(type, builtinType, protocol);
  }

#ifndef NDEBUG
  // Ensure we haven't missed queries for the specialty SIL types
  // in the AST in conformance to one of the invertible protocols.
  if (auto kp = protocol->getKnownProtocolKind())
    if (getInvertibleProtocolKind(*kp))
      assert(!(type->is<SILFunctionType,
                        SILBoxType,
                        SILMoveOnlyWrappedType,
                        SILPackType,
                        SILTokenType>()));
#endif

  auto nominal = type->getAnyNominal();

  // If we don't have a nominal type, there are no conformances.
  if (!nominal || isa<ProtocolDecl>(nominal))
    return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);

  // Expand conformances added by extension macros.
  //
  // FIXME: This expansion should only be done if the
  // extension macro can generate a conformance to the
  // given protocol, but conformance macros do not specify
  // that information upfront.
  (void)evaluateOrDefault(
      ctx.evaluator,
      ExpandExtensionMacros{nominal},
      { });

  // Find the (unspecialized) conformance.
  SmallVector<ProtocolConformance *, 2> conformances;
  if (!nominal->lookupConformance(protocol, conformances)) {
    if (protocol->isSpecificProtocol(KnownProtocolKind::Sendable)) {
      // Try to infer Sendable conformance.
      ImplicitKnownProtocolConformanceRequest
          cvRequest{nominal, KnownProtocolKind::Sendable};
      if (auto conformance = evaluateOrDefault(
              ctx.evaluator, cvRequest, nullptr)) {
        conformances.clear();
        conformances.push_back(conformance);
      } else {
        return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
      }
    } else if (protocol->isSpecificProtocol(KnownProtocolKind::Encodable) ||
               protocol->isSpecificProtocol(KnownProtocolKind::Decodable)) {
      if (nominal->isDistributedActor()) {
        auto protoKind =
            protocol->isSpecificProtocol(KnownProtocolKind::Encodable)
                ? KnownProtocolKind::Encodable
                : KnownProtocolKind::Decodable;
        auto request = GetDistributedActorImplicitCodableRequest{
          nominal, protoKind};

        if (auto conformance =
                evaluateOrDefault(ctx.evaluator, request, nullptr)) {
          conformances.clear();
          conformances.push_back(conformance);
        } else {
          return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
        }
      } else {
        return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
      }
    } else if (protocol->isSpecificProtocol(KnownProtocolKind::Copyable)
               || protocol->isSpecificProtocol(KnownProtocolKind::Escapable)) {
      const auto kp = protocol->getKnownProtocolKind().value();

      if (!ctx.LangOpts.AssumesNoncopyableGenerics
          && kp == KnownProtocolKind::Copyable) {
        // Return an abstract conformance to maintain legacy compatability.
        // We only need to do this until we are properly dealing with or
        // omitting Copyable conformances in modules/interfaces.

        if (nominal->canBeCopyable())
          return ProtocolConformanceRef(protocol);
        else
          return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
      }

      // Try to infer the conformance.
      ImplicitKnownProtocolConformanceRequest cvRequest{nominal, kp};
      if (auto conformance = evaluateOrDefault(
          ctx.evaluator, cvRequest, nullptr)) {
        conformances.clear();
        conformances.push_back(conformance);
      } else {
        return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
      }
    } else if (protocol->isSpecificProtocol(
                   KnownProtocolKind::BitwiseCopyable)) {
      // Try to infer BitwiseCopyable conformance.
      ImplicitKnownProtocolConformanceRequest request{
          nominal, KnownProtocolKind::BitwiseCopyable};
      if (auto conformance =
              evaluateOrDefault(ctx.evaluator, request, nullptr)) {
        conformances.clear();
        conformances.push_back(conformance);
      } else {
        return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
      }
    } else {
      // Was unable to infer the missing conformance.
      return ProtocolConformanceRef::forMissingOrInvalid(type, protocol);
    }
  }

  assert(!conformances.empty());

  // If we have multiple conformances, first try to filter out any that are
  // unavailable on the current deployment target.
  //
  // FIXME: Conformance lookup should really depend on source location for
  // this to be 100% correct.
  if (conformances.size() > 1) {
    SmallVector<ProtocolConformance *, 2> availableConformances;

    for (auto *conformance : conformances) {
      if (conformance->getDeclContext()->isAlwaysAvailableConformanceContext())
        availableConformances.push_back(conformance);
    }

    // Don't filter anything out if all conformances are unavailable.
    if (!availableConformances.empty())
      std::swap(availableConformances, conformances);
  }

  // If we still have multiple conformances, just pick the first one.
  auto conformance = conformances.front();

  // Rebuild inherited conformances based on the root normal conformance.
  // FIXME: This is a hack to work around our inability to handle multiple
  // levels of substitution through inherited conformances elsewhere in the
  // compiler.
  if (auto inherited = dyn_cast<InheritedProtocolConformance>(conformance)) {
    // Dig out the conforming nominal type.
    auto rootConformance = inherited->getRootConformance();
    auto conformingClass
      = rootConformance->getType()->getClassOrBoundGenericClass();

    // Map up to our superclass's type.
    auto superclassTy = type->getSuperclassForDecl(conformingClass);

    // Compute the conformance for the inherited type.
    auto inheritedConformance = mod->lookupConformance(
        superclassTy, protocol, /*allowMissing=*/true);
    assert(inheritedConformance &&
           "We already found the inherited conformance");

    // Create the inherited conformance entry.
    conformance =
        ctx.getInheritedConformance(type, inheritedConformance.getConcrete());
    return ProtocolConformanceRef(conformance);
  }

  // If the type is specialized, find the conformance for the generic type.
  if (type->isSpecialized()) {
    // Figure out the type that's explicitly conforming to this protocol.
    Type explicitConformanceType = conformance->getType();
    DeclContext *explicitConformanceDC = conformance->getDeclContext();

    // If the explicit conformance is associated with a type that is different
    // from the type we're checking, retrieve generic conformance.
    if (!explicitConformanceType->isEqual(type)) {
      // Gather the substitutions we need to map the generic conformance to
      // the specialized conformance.
      auto subMap = type->getContextSubstitutionMap(mod, explicitConformanceDC);

      // Create the specialized conformance entry.
      auto result = ctx.getSpecializedConformance(type,
        cast<RootProtocolConformance>(conformance), subMap);
      return ProtocolConformanceRef(result);
    }
  }

  // Record and return the simple conformance.
  return ProtocolConformanceRef(conformance);
}

ProtocolConformanceRef
ModuleDecl::checkConformance(Type type, ProtocolDecl *proto,
                             bool allowMissing) {
  assert(!type->hasTypeParameter());

  auto lookupResult = lookupConformance(type, proto, allowMissing);
  if (lookupResult.isInvalid()) {
    return ProtocolConformanceRef::forInvalid();
  }

  auto condReqs = lookupResult.getConditionalRequirements();

  // If we have a conditional requirements that we need to check, do so now.
  if (!condReqs.empty()) {
    switch (checkRequirements(condReqs)) {
    case CheckRequirementsResult::Success:
      break;

    case CheckRequirementsResult::RequirementFailure:
    case CheckRequirementsResult::SubstitutionFailure:
      return ProtocolConformanceRef::forInvalid();
    }
  }

  return lookupResult;
}
