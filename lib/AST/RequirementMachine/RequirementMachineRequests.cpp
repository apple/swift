//===--- RequirementMachineRequests.cpp - Request evaluator requests ------===//
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
//
// This file implements the main entry points for computing minimized generic
// signatures using the requirement machine via the request evaluator.
//
// There are three requests:
//
// - RequirementSignatureRequest computes protocol requirement signatures from
//   user-written requirements.
// - AbstractGenericSignatureRequest computes minimal generic signatures from a
//   set of abstract Requirements.
// - InferredGenericSignatureRequest computes minimal generic signatures from a
//   set of user-written requirements on a parsed generic declaration.
//
// Each request begins by constructing some desugared requirements using the
// entry points in RequirementLowering.cpp.
//
// The desugared requirements are fed into a new requirement machine instance,
// which is then asked to produce a minimal set of rewrite rules. These rules
// are converted into minimal canonical Requirements using the entry points in
// RuleBuilder.cpp.
//
// The actual logic for finding a minimal set of rewrite rules is implemented in
// HomotopyReduction.cpp and MinimalConformances.cpp.
//
// Routines for constructing Requirements from Rules are implemented in
// RequirementBuilder.cpp.
//
// This process is actually iterated to implement "concrete equivalence class
// splitting", a compatibility behavior to produce the same results as the
// GenericSignatureBuilder in certain esoteric edge cases:
//
//           ------------------------
//          / Desugared Requirement /
//          ------------------------
//                     |
//                     |  +---------------------+
//                     |  |                     |
//                     v  v                     |
//              +-------------+                 |
//              | RuleBuilder |                 |
//              +-------------+                 |
//                     |                        |
//                     v                        |
//             +--------------+                 |
//             | Minimization |                 |
//             +--------------+                 |
//                     |                        |
//                     v                        |
//          +--------------------+              |
//          | RequirementBuilder |              |
//          +--------------------+              |
//                     |                        |
//                     v                        |
//               --------------                 |
//              / Requirement /                 |
//              --------------                  |
//                     |                        |
//                     v                        |
//  +------------------------------------+      |
//  | Split concrete equivalence classes |  ----+
//  +------------------------------------+
//                     |
//                     v
//               --------------
//              / Requirement /
//              --------------
//
// This transformation is described in splitConcreteEquivalenceClasses() below.
//
//===----------------------------------------------------------------------===//

#include "RequirementMachine.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include <memory>
#include <vector>
#include "RequirementLowering.h"

using namespace swift;
using namespace rewriting;

/// Hack for GenericSignatureBuilder compatibility. We might end up with a
/// same-type requirement between type parameters where one of them has an
/// implied concrete type requirement. In this case, split it up into two
/// concrete type requirements.
static bool shouldSplitConcreteEquivalenceClass(
    Requirement req,
    const ProtocolDecl *proto,
    const RequirementMachine *machine) {
  return (req.getKind() == RequirementKind::SameType &&
          req.getSecondType()->isTypeParameter() &&
          machine->isConcreteType(req.getSecondType(), proto));
}

/// Returns true if this generic signature contains abstract same-type
/// requirements between concrete type parameters. In this case, we split
/// the abstract same-type requirements into pairs of concrete type
/// requirements, and minimize the signature again.
static bool shouldSplitConcreteEquivalenceClasses(
    ArrayRef<Requirement> requirements,
    const ProtocolDecl *proto,
    const RequirementMachine *machine) {
  for (auto req : requirements) {
    if (shouldSplitConcreteEquivalenceClass(req, proto, machine))
      return true;
  }

  return false;
}

/// Same as the above, but with the requirements of a protocol connected
/// component.
static bool shouldSplitConcreteEquivalenceClasses(
    const llvm::DenseMap<const ProtocolDecl *, RequirementSignature> &protos,
    const RequirementMachine *machine) {
  for (const auto &pair : protos) {
    if (shouldSplitConcreteEquivalenceClasses(pair.second.getRequirements(),
                                              pair.first, machine))
      return true;
  }

  return false;
}

/// Replace each same-type requirement 'T == U' where 'T' (and therefore 'U')
/// is known to equal a concrete type 'C' with a pair of requirements
/// 'T == C' and 'U == C'. We build the signature again in this case, since
/// one of the two requirements will be redundant, but we don't know which
/// ahead of time.
static void splitConcreteEquivalenceClasses(
    ASTContext &ctx,
    ArrayRef<Requirement> requirements,
    const ProtocolDecl *proto,
    const RequirementMachine *machine,
    TypeArrayView<GenericTypeParamType> genericParams,
    SmallVectorImpl<StructuralRequirement> &splitRequirements,
    unsigned &attempt) {
  bool debug = machine->getDebugOptions().contains(
      DebugFlags::SplitConcreteEquivalenceClass);

  unsigned maxAttempts =
      ctx.LangOpts.RequirementMachineMaxSplitConcreteEquivClassAttempts;

  if (attempt >= maxAttempts) {
    llvm::errs() << "Splitting concrete equivalence classes did not "
                 << "reach fixed point after " << attempt << " attempts.\n";
    llvm::errs() << "Last attempt produced these requirements:\n";
    for (auto req : requirements) {
      req.dump(llvm::errs());
      llvm::errs() << "\n";
    }
    machine->dump(llvm::errs());
    abort();
  }

  splitRequirements.clear();

  if (debug) {
    llvm::dbgs() << "\n# Splitting concrete equivalence classes:\n";
  }

  for (auto req : requirements) {
    if (shouldSplitConcreteEquivalenceClass(req, proto, machine)) {
      auto concreteType = machine->getConcreteType(
          req.getSecondType(), genericParams, proto);

      Requirement firstReq(RequirementKind::SameType,
                           req.getFirstType(), concreteType);
      Requirement secondReq(RequirementKind::SameType,
                            req.getSecondType(), concreteType);
      splitRequirements.push_back({firstReq, SourceLoc(), /*inferred=*/false});
      splitRequirements.push_back({secondReq, SourceLoc(), /*inferred=*/false});

      if (debug) {
        llvm::dbgs() << "- First split: ";
        firstReq.dump(llvm::dbgs());
        llvm::dbgs() << "\n- Second split: ";
        secondReq.dump(llvm::dbgs());
        llvm::dbgs() << "\n";
      }
      continue;
    }

    splitRequirements.push_back({req, SourceLoc(), /*inferred=*/false});

    if (debug) {
      llvm::dbgs() << "- Not split: ";
      req.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }
  }
}

/// Same as the above, but with the requirements of a protocol connected
/// component.
static void splitConcreteEquivalenceClasses(
    ASTContext &ctx,
    const llvm::DenseMap<const ProtocolDecl *, RequirementSignature> &protos,
    const RequirementMachine *machine,
    llvm::DenseMap<const ProtocolDecl *,
                   SmallVector<StructuralRequirement, 4>> &splitProtos,
    unsigned &attempt) {
  for (const auto &pair : protos) {
    const auto *proto = pair.first;
    auto genericParams = proto->getGenericSignature().getGenericParams();
    splitConcreteEquivalenceClasses(ctx, pair.second.getRequirements(),
                                    proto, machine, genericParams,
                                    splitProtos[proto],
                                    attempt);
  }
}

/// Builds the requirement signatures for each protocol in this strongly
/// connected component.
llvm::DenseMap<const ProtocolDecl *, RequirementSignature>
RequirementMachine::computeMinimalProtocolRequirements() {
  auto protos = System.getProtocols();

  assert(protos.size() > 0 &&
         "Not a protocol connected component rewrite system");

  System.minimizeRewriteSystem(Map);

  if (Dump) {
    llvm::dbgs() << "Minimized rewrite system:\n";
    dump(llvm::dbgs());
  }

  auto rules = System.getMinimizedProtocolRules();

  auto &ctx = Context.getASTContext();

  // Note that we build 'result' by iterating over 'protos' rather than
  // 'rules'; this is intentional, so that even if a protocol has no
  // rules, we still end up creating an entry for it in 'result'.
  llvm::DenseMap<const ProtocolDecl *, RequirementSignature> result;
  for (const auto *proto : protos) {
    auto genericParams = proto->getGenericSignature().getGenericParams();

    const auto &entry = rules[proto];

    std::vector<Requirement> reqs;
    std::vector<ProtocolTypeAlias> aliases;
    buildRequirementsFromRules(entry.Requirements,
                               entry.TypeAliases,
                               genericParams,
                               /*reconstituteSugar=*/true,
                               reqs, aliases);

    result[proto] = RequirementSignature(ctx.AllocateCopy(reqs),
                                         ctx.AllocateCopy(aliases),
                                         getErrors());
  }

  return result;
}

RequirementSignature
RequirementSignatureRequest::evaluate(Evaluator &evaluator,
                                      ProtocolDecl *proto) const {
  ASTContext &ctx = proto->getASTContext();

  // First check if we have a deserializable requirement signature.
  if (proto->hasLazyRequirementSignature()) {
    // FIXME: (transitional) increment the redundant "always-on" counter.
    if (ctx.Stats)
      ++ctx.Stats->getFrontendCounters().NumLazyRequirementSignaturesLoaded;

    auto contextData = static_cast<LazyProtocolData *>(
        ctx.getOrCreateLazyContextData(proto, nullptr));

    SmallVector<Requirement, 2> requirements;
    SmallVector<ProtocolTypeAlias, 2> typeAliases;
    contextData->loader->loadRequirementSignature(
        proto, contextData->requirementSignatureData,
        requirements, typeAliases);
    return RequirementSignature(ctx.AllocateCopy(requirements),
                                ctx.AllocateCopy(typeAliases));
  }

  auto &rewriteCtx = ctx.getRewriteContext();

  // We build requirement signatures for all protocols in a strongly connected
  // component at the same time.
  auto component = rewriteCtx.startComputingRequirementSignatures(proto);

  SWIFT_DEFER {
    rewriteCtx.finishComputingRequirementSignatures(proto);
  };

  SmallVector<RequirementError, 4> errors;

  // Collect user-written requirements from the protocols in this connected
  // component.
  llvm::DenseMap<const ProtocolDecl *,
                 SmallVector<StructuralRequirement, 4>> protos;
  for (const auto *proto : component) {
    auto &requirements = protos[proto];
    for (auto req : proto->getStructuralRequirements())
      requirements.push_back(req);
    for (auto req : proto->getTypeAliasRequirements())
      requirements.push_back({req, SourceLoc(), /*inferred=*/false});
  }

  if (rewriteCtx.getDebugOptions().contains(DebugFlags::Timers)) {
    rewriteCtx.beginTimer("RequirementSignatureRequest");
    llvm::dbgs() << "[";
    for (auto *proto : component)
      llvm::dbgs() << " " << proto->getName();
    llvm::dbgs() << " ]\n";
  }

  SWIFT_DEFER {
    if (rewriteCtx.getDebugOptions().contains(DebugFlags::Timers)) {
      rewriteCtx.endTimer("RequirementSignatureRequest");
      llvm::dbgs() << "[";
      for (auto *proto : component)
        llvm::dbgs() << " " << proto->getName();
      llvm::dbgs() << " ]\n";
    }
  };

  unsigned attempt = 0;
  for (;;) {
    for (const auto *proto : component) {
      auto &requirements = protos[proto];

      // Preprocess requirements to eliminate conformances on type parameters
      // which are made concrete.
      if (ctx.LangOpts.EnableRequirementMachineConcreteContraction) {
        SmallVector<StructuralRequirement, 4> contractedRequirements;

        bool debug = rewriteCtx.getDebugOptions()
                               .contains(DebugFlags::ConcreteContraction);

        if (performConcreteContraction(requirements, contractedRequirements,
                                       errors, debug)) {
          std::swap(contractedRequirements, requirements);
        }
      }
    }

    // Heap-allocate the requirement machine to save stack space.
    std::unique_ptr<RequirementMachine> machine(new RequirementMachine(
        rewriteCtx));

    auto status = machine->initWithProtocolWrittenRequirements(component, protos);

    // If completion failed, diagnose an error and return a dummy signature.
    if (status.first != CompletionResult::Success) {
      // All we can do at this point is diagnose and give each protocol an empty
      // requirement signature.
      for (const auto *otherProto : component) {
        ctx.Diags.diagnose(otherProto->getLoc(),
                           diag::requirement_machine_completion_failed,
                           /*protocol=*/1,
                           unsigned(status.first));

        auto rule = machine->getRuleAsStringForDiagnostics(status.second);
        ctx.Diags.diagnose(otherProto->getLoc(),
                           diag::requirement_machine_completion_rule,
                           rule);

        if (otherProto != proto) {
          ctx.evaluator.cacheOutput(
            RequirementSignatureRequest{const_cast<ProtocolDecl *>(otherProto)},
            RequirementSignature(GenericSignatureErrorFlags::CompletionFailed));
        }
      }

      return RequirementSignature(GenericSignatureErrorFlags::CompletionFailed);
    }

    auto minimalRequirements = machine->computeMinimalProtocolRequirements();

    // Don't bother splitting concrete equivalence classes if there were invalid
    // requirements, because the signature is not going to be ABI anyway.
    if (!machine->getErrors().contains(
          GenericSignatureErrorFlags::HasInvalidRequirements)) {
      if (shouldSplitConcreteEquivalenceClasses(minimalRequirements, machine.get())) {
        ++attempt;
        splitConcreteEquivalenceClasses(ctx, minimalRequirements,
                                        machine.get(), protos, attempt);
        continue;
      }
    }

    bool debug = machine->getDebugOptions().contains(DebugFlags::Minimization);

    // The requirement signature for the actual protocol that the result
    // was kicked off with.
    Optional<RequirementSignature> result;

    if (debug) {
      llvm::dbgs() << "\nRequirement signatures:\n";
    }

    // Cache the requirement signatures for all other protocols in this
    // connected component.
    for (const auto &pair : minimalRequirements) {
      auto *otherProto = pair.first;
      const auto &reqs = pair.second;

      // Dump the result if requested.
      if (debug) {
        llvm::dbgs() << "- Protocol " << otherProto->getName() << ": ";

        auto sig = GenericSignature::get(
            otherProto->getGenericSignature().getGenericParams(),
            reqs.getRequirements());

        PrintOptions opts;
        opts.ProtocolQualifiedDependentMemberTypes = true;
        sig.print(llvm::dbgs(), opts);
        llvm::dbgs() << "\n";
      }

      // Don't call setRequirementSignature() on the original proto; the
      // request evaluator will do it for us.
      if (otherProto == proto)
        result = reqs;
      else {
        auto temp = reqs;
        ctx.evaluator.cacheOutput(
          RequirementSignatureRequest{const_cast<ProtocolDecl *>(otherProto)},
          std::move(temp));
      }
    }

    // Diagnose redundant requirements and conflicting requirements.
    machine->computeRequirementDiagnostics(errors, proto->getLoc());
    diagnoseRequirementErrors(ctx, errors,
                              AllowConcreteTypePolicy::NestedAssocTypes);

    for (auto *protocol : machine->System.getProtocols()) {
      auto selfType = protocol->getSelfInterfaceType();
      auto concrete = machine->getConcreteType(selfType,
                                               machine->getGenericParams(),
                                               protocol);
      if (!concrete || concrete->hasError())
        continue;

      protocol->diagnose(diag::requires_generic_param_made_equal_to_concrete,
                         selfType);
    }

    if (!machine->getErrors()) {
      // If this signature was minimized without errors or non-redundant
      // concrete conformances, we can re-use the requirement machine for
      // subsequent queries, instead of building a new requirement machine
      // from the minimized signature.
      rewriteCtx.installRequirementMachine(proto, std::move(machine));
    }

    // Return the result for the specific protocol this request was kicked off on.
    return *result;
  }
}

/// Builds the top-level generic signature requirements for this rewrite system.
GenericSignature
RequirementMachine::computeMinimalGenericSignature(
    bool reconstituteSugar) {
  assert(!Sig &&
         "Already computed minimal generic signature");
  assert(System.getProtocols().empty() &&
         "Not a top-level generic signature rewrite system");
  assert(!Params.empty() &&
         "Not a from-source top-level generic signature rewrite system");

  System.minimizeRewriteSystem(Map);

  if (Dump) {
    llvm::dbgs() << "Minimized rewrite system:\n";
    dump(llvm::dbgs());
  }

  auto rules = System.getMinimizedGenericSignatureRules();

  std::vector<Requirement> reqs;
  std::vector<ProtocolTypeAlias> aliases;

  buildRequirementsFromRules(rules, ArrayRef<unsigned>(), getGenericParams(),
                             reconstituteSugar, reqs, aliases);
  assert(aliases.empty());

  auto sig = GenericSignature::get(getGenericParams(), reqs);

  // Remember the signature for generic signature queries. In particular,
  // getConformancePath() needs the current requirement machine's
  // generic signature.
  Sig = sig.getCanonicalSignature();

  return sig;
}

/// Check whether the inputs to the \c AbstractGenericSignatureRequest are
/// all canonical.
static bool isCanonicalRequest(GenericSignature baseSignature,
                               ArrayRef<GenericTypeParamType *> genericParams,
                               ArrayRef<Requirement> requirements) {
  if (baseSignature && !baseSignature->isCanonical())
    return false;

  for (auto gp : genericParams) {
    if (!gp->isCanonical())
      return false;
  }

  for (const auto &req : requirements) {
    if (!req.isCanonical())
      return false;
  }

  return true;
}

GenericSignatureWithError
AbstractGenericSignatureRequest::evaluate(
         Evaluator &evaluator,
         const GenericSignatureImpl *baseSignatureImpl,
         SmallVector<GenericTypeParamType *, 2> addedParameters,
         SmallVector<Requirement, 2> addedRequirements) const {
  GenericSignature baseSignature = GenericSignature{baseSignatureImpl};
  // If nothing is added to the base signature, just return the base
  // signature.
  if (addedParameters.empty() && addedRequirements.empty())
    return GenericSignatureWithError(baseSignature, GenericSignatureErrors());

  ASTContext &ctx = addedParameters.empty()
      ? addedRequirements.front().getFirstType()->getASTContext()
      : addedParameters.front()->getASTContext();

  SmallVector<GenericTypeParamType *, 4> genericParams(
      baseSignature.getGenericParams().begin(),
      baseSignature.getGenericParams().end());
  genericParams.append(
      addedParameters.begin(),
      addedParameters.end());

  // If there are no added requirements, we can form the signature directly
  // with the added parameters.
  if (addedRequirements.empty()) {
    auto result = GenericSignature::get(genericParams,
                                        baseSignature.getRequirements());
    return GenericSignatureWithError(result, GenericSignatureErrors());
  }

  // If the request is non-canonical, we won't need to build our own
  // generic signature builder.
  if (!isCanonicalRequest(baseSignature, addedParameters, addedRequirements)) {
    // Canonicalize the inputs so we can form the canonical request.
    auto canBaseSignature = baseSignature.getCanonicalSignature();

    SmallVector<GenericTypeParamType *, 2> canAddedParameters;
    canAddedParameters.reserve(addedParameters.size());
    for (auto gp : addedParameters) {
      auto canGP = gp->getCanonicalType()->castTo<GenericTypeParamType>();
      canAddedParameters.push_back(canGP);
    }

    SmallVector<Requirement, 2> canAddedRequirements;
    canAddedRequirements.reserve(addedRequirements.size());
    for (const auto &req : addedRequirements) {
      canAddedRequirements.push_back(req.getCanonical());
    }

    // Build the canonical signature.
    auto canSignatureResult = evaluateOrDefault(
        ctx.evaluator,
        AbstractGenericSignatureRequest{
          canBaseSignature.getPointer(), std::move(canAddedParameters),
          std::move(canAddedRequirements)},
        GenericSignatureWithError());
    if (!canSignatureResult.getPointer())
      return GenericSignatureWithError();

    // Substitute in the original generic parameters to form the sugared
    // result the original request wanted.
    auto canSignature = canSignatureResult.getPointer();
    SmallVector<GenericTypeParamType *, 2> resugaredParameters;
    resugaredParameters.reserve(canSignature.getGenericParams().size());
    if (baseSignature) {
      resugaredParameters.append(baseSignature.getGenericParams().begin(),
                                 baseSignature.getGenericParams().end());
    }
    resugaredParameters.append(addedParameters.begin(), addedParameters.end());
    assert(resugaredParameters.size() ==
               canSignature.getGenericParams().size());

    SmallVector<Requirement, 2> resugaredRequirements;
    resugaredRequirements.reserve(canSignature.getRequirements().size());
    for (const auto &req : canSignature.getRequirements()) {
      auto resugaredReq = req.subst(
          [&](SubstitutableType *type) {
            if (auto gp = dyn_cast<GenericTypeParamType>(type)) {
              unsigned ordinal = canSignature->getGenericParamOrdinal(gp);
              return Type(resugaredParameters[ordinal]);
            }
            return Type(type);
          },
          MakeAbstractConformanceForGenericType(),
          SubstFlags::AllowLoweredTypes);
      resugaredRequirements.push_back(resugaredReq);
    }

    return GenericSignatureWithError(
        GenericSignature::get(resugaredParameters, resugaredRequirements),
        canSignatureResult.getInt());
  }

  // Convert the input Requirements into StructuralRequirements by adding
  // empty source locations.
  SmallVector<StructuralRequirement, 4> requirements;
  for (auto req : baseSignature.getRequirements())
    requirements.push_back({req, SourceLoc(), /*wasInferred=*/false});

  // We need to create this errors vector to pass to
  // desugarRequirement, but this request should never
  // diagnose errors.
  SmallVector<RequirementError, 4> errors;

  // The requirements passed to this request may have been substituted,
  // meaning the subject type might be a concrete type and not a type
  // parameter.
  //
  // Also, the right hand side of conformance requirements here might be
  // a protocol composition.
  //
  // Desugaring converts these kinds of requirements into "proper"
  // requirements where the subject type is always a type parameter,
  // which is what the RuleBuilder expects.
  for (auto req : addedRequirements) {
    SmallVector<Requirement, 2> reqs;
    desugarRequirement(req, SourceLoc(), reqs, errors);
    for (auto req : reqs)
      requirements.push_back({req, SourceLoc(), /*wasInferred=*/false});
  }

  auto &rewriteCtx = ctx.getRewriteContext();

  if (rewriteCtx.getDebugOptions().contains(DebugFlags::Timers)) {
    rewriteCtx.beginTimer("AbstractGenericSignatureRequest");
    llvm::dbgs() << "\n";
  }

  unsigned attempt = 0;
  for (;;) {
    // Preprocess requirements to eliminate conformances on generic parameters
    // which are made concrete.
    if (ctx.LangOpts.EnableRequirementMachineConcreteContraction) {
      SmallVector<StructuralRequirement, 4> contractedRequirements;
      bool debug = rewriteCtx.getDebugOptions()
                             .contains(DebugFlags::ConcreteContraction);
      if (performConcreteContraction(requirements, contractedRequirements,
                                     errors, debug)) {
        std::swap(contractedRequirements, requirements);
      }
    }

    // Heap-allocate the requirement machine to save stack space.
    std::unique_ptr<RequirementMachine> machine(new RequirementMachine(
        rewriteCtx));

    auto status =
        machine->initWithWrittenRequirements(genericParams, requirements);
    machine->checkCompletionResult(status.first);

    // We pass reconstituteSugar=false to ensure that if the original
    // requirements were canonical, the final signature remains canonical.
    auto result = machine->computeMinimalGenericSignature(
          /*reconstituteSugar=*/false);
    auto errorFlags = machine->getErrors();

    // Don't bother splitting concrete equivalence classes if there were invalid
    // requirements, because the signature is not going to be ABI anyway.
    if (!errorFlags.contains(GenericSignatureErrorFlags::HasInvalidRequirements)) {
      if (shouldSplitConcreteEquivalenceClasses(result.getRequirements(),
                                                /*proto=*/nullptr,
                                                machine.get())) {
        ++attempt;
        splitConcreteEquivalenceClasses(ctx, result.getRequirements(),
                                        /*proto=*/nullptr, machine.get(),
                                        result.getGenericParams(),
                                        requirements, attempt);
        continue;
      }
    }

    if (!errorFlags) {
      // If this signature was minimized without errors or non-redundant
      // concrete conformances, we can re-use the requirement machine for
      // subsequent queries, instead of building a new requirement machine
      // from the minimized signature. Do this before verify(), which
      // performs queries.
      rewriteCtx.installRequirementMachine(result.getCanonicalSignature(),
                                           std::move(machine));
    }

    if (!errorFlags.contains(GenericSignatureErrorFlags::HasInvalidRequirements)) {
      // Check invariants.
      result.verify();
    }

    if (rewriteCtx.getDebugOptions().contains(DebugFlags::Timers)) {
      rewriteCtx.endTimer("AbstractGenericSignatureRequest");
      llvm::dbgs() << result << "\n";
    }

    return GenericSignatureWithError(result, errorFlags);
  }
}

GenericSignatureWithError
InferredGenericSignatureRequest::evaluate(
        Evaluator &evaluator,
        const GenericSignatureImpl *parentSigImpl,
        GenericParamList *genericParamList,
        WhereClauseOwner whereClause,
        SmallVector<Requirement, 2> addedRequirements,
        SmallVector<TypeLoc, 2> inferenceSources,
        bool allowConcreteGenericParams) const {
  GenericSignature parentSig(parentSigImpl);

  SmallVector<GenericTypeParamType *, 4> genericParams(
      parentSig.getGenericParams().begin(),
      parentSig.getGenericParams().end());

  SmallVector<StructuralRequirement, 4> requirements;
  SmallVector<RequirementError, 4> errors;

  SourceLoc loc = [&]() {
    if (genericParamList) {
      auto loc = genericParamList->getLAngleLoc();
      if (loc.isValid())
        return loc;
    }
    if (whereClause) {
      auto loc = whereClause.getLoc();
      if (loc.isValid())
        return loc;
    }
    for (auto sourcePair : inferenceSources) {
      auto *typeRepr = sourcePair.getTypeRepr();
      auto loc = typeRepr ? typeRepr->getStartLoc() : SourceLoc();
      if (loc.isValid())
        return loc;
    }
    return SourceLoc();
  }();

  for (const auto &req : parentSig.getRequirements())
    requirements.push_back({req, loc, /*wasInferred=*/false});

  DeclContext *lookupDC = nullptr;

  const auto visitRequirement = [&](const Requirement &req,
                                    RequirementRepr *reqRepr) {
    realizeRequirement(lookupDC, req, reqRepr, /*inferRequirements=*/true,
                       requirements, errors);
    return false;
  };

  if (genericParamList) {
    // Extensions never have a parent signature.
    assert(genericParamList->getOuterParameters() == nullptr || !parentSig);

    // Collect all outer generic parameter lists.
    SmallVector<GenericParamList *, 2> gpLists;
    for (auto *outerParamList = genericParamList;
         outerParamList != nullptr;
         outerParamList = outerParamList->getOuterParameters()) {
      gpLists.push_back(outerParamList);
    }

    // The generic parameter lists must appear from innermost to outermost.
    // We walk them backwards to order outer parameters before inner
    // parameters.
    for (auto *gpList : llvm::reverse(gpLists)) {
      assert(gpList->size() > 0 &&
             "Parsed an empty generic parameter list?");

      for (auto *gpDecl : *gpList) {
        auto *gpType = gpDecl->getDeclaredInterfaceType()
                             ->castTo<GenericTypeParamType>();
        genericParams.push_back(gpType);

        realizeInheritedRequirements(gpDecl, gpType,
                                     /*inferRequirements=*/true,
                                     requirements, errors);
      }

      lookupDC = (*gpList->begin())->getDeclContext();

      // Add the generic parameter list's 'where' clause to the builder.
      //
      // The only time generic parameter lists have a 'where' clause is
      // in SIL mode; all other generic declarations have a free-standing
      // 'where' clause, which will be visited below.
      WhereClauseOwner(lookupDC, gpList)
        .visitRequirements(TypeResolutionStage::Structural,
                           visitRequirement);
    }
  }

  // Realize all requirements in the free-standing 'where' clause, if there
  // is one.
  if (whereClause) {
    lookupDC = whereClause.dc;

    std::move(whereClause).visitRequirements(
        TypeResolutionStage::Structural,
        visitRequirement);
  }

  auto *moduleForInference = lookupDC->getParentModule();

  // Perform requirement inference from function parameter and result
  // types and such.
  for (auto sourcePair : inferenceSources) {
    auto *typeRepr = sourcePair.getTypeRepr();
    auto typeLoc = typeRepr ? typeRepr->getStartLoc() : SourceLoc();

    inferRequirements(sourcePair.getType(), typeLoc, moduleForInference,
                      lookupDC, requirements);
  }

  // Finish by adding any remaining requirements. This is used to introduce
  // inferred same-type requirements when building the generic signature of
  // an extension whose extended type is a generic typealias.
  for (const auto &req : addedRequirements)
    requirements.push_back({req, SourceLoc(), /*wasInferred=*/true});

  // Re-order requirements so that inferred requirements appear last. This
  // ensures that if an inferred requirement is redundant with some other
  // requirement, it is the inferred requirement that becomes redundant,
  // which muffles the redundancy diagnostic.
  std::stable_partition(requirements.begin(), requirements.end(),
                        [](const StructuralRequirement &req) {
                          return !req.inferred;
                        });

  auto &ctx = moduleForInference->getASTContext();
  auto &rewriteCtx = ctx.getRewriteContext();

  if (rewriteCtx.getDebugOptions().contains(DebugFlags::Timers)) {
    rewriteCtx.beginTimer("InferredGenericSignatureRequest");

    llvm::dbgs() << "@ ";
    auto &sourceMgr = ctx.SourceMgr;
    loc.print(llvm::dbgs(), sourceMgr);
    llvm::dbgs() << "\n";
  }

  unsigned attempt = 0;
  for (;;) {
    // Preprocess requirements to eliminate conformances on generic parameters
    // which are made concrete.
    if (ctx.LangOpts.EnableRequirementMachineConcreteContraction) {
      SmallVector<StructuralRequirement, 4> contractedRequirements;
      bool debug = rewriteCtx.getDebugOptions()
                             .contains(DebugFlags::ConcreteContraction);
      if (performConcreteContraction(requirements, contractedRequirements,
                                     errors, debug)) {
        std::swap(contractedRequirements, requirements);
      }
    }

    // Heap-allocate the requirement machine to save stack space.
    std::unique_ptr<RequirementMachine> machine(new RequirementMachine(
        rewriteCtx));

    auto status =
        machine->initWithWrittenRequirements(genericParams, requirements);

    // If completion failed, diagnose an error and return a dummy signature.
    if (status.first != CompletionResult::Success) {
      ctx.Diags.diagnose(loc,
                         diag::requirement_machine_completion_failed,
                         /*protocol=*/0,
                         unsigned(status.first));

      auto rule = machine->getRuleAsStringForDiagnostics(status.second);
      ctx.Diags.diagnose(loc,
                         diag::requirement_machine_completion_rule,
                         rule);

      auto result = GenericSignature::get(genericParams,
                                          parentSig.getRequirements());

      if (rewriteCtx.getDebugOptions().contains(DebugFlags::Timers)) {
        rewriteCtx.endTimer("InferredGenericSignatureRequest");
        llvm::dbgs() << result << "\n";
      }

      return GenericSignatureWithError(
          result, GenericSignatureErrorFlags::CompletionFailed);
    }

    auto result = machine->computeMinimalGenericSignature(
          /*reconstituteSugar=*/true);
    auto errorFlags = machine->getErrors();

    // Diagnose redundant requirements and conflicting requirements.
    if (attempt == 0) {
      machine->computeRequirementDiagnostics(errors, loc);
      diagnoseRequirementErrors(ctx, errors,
                                allowConcreteGenericParams
                                ? AllowConcreteTypePolicy::All
                                : AllowConcreteTypePolicy::AssocTypes);
    }

    // Don't bother splitting concrete equivalence classes if there were invalid
    // requirements, because the signature is not going to be ABI anyway.
    if (!errorFlags.contains(GenericSignatureErrorFlags::HasInvalidRequirements)) {
      // Check if we need to rebuild the signature.
      if (shouldSplitConcreteEquivalenceClasses(result.getRequirements(),
                                                /*proto=*/nullptr,
                                                machine.get())) {
        ++attempt;
        splitConcreteEquivalenceClasses(ctx, result.getRequirements(),
                                        /*proto=*/nullptr, machine.get(),
                                        result.getGenericParams(),
                                        requirements, attempt);
        continue;
      }
    }

    if (!errorFlags) {
      // If this signature was minimized without errors or non-redundant
      // concrete conformances, we can re-use the requirement machine for
      // subsequent queries, instead of building a new requirement machine
      // from the minimized signature. Do this before verify(), which
      // performs queries.
      rewriteCtx.installRequirementMachine(result.getCanonicalSignature(),
                                           std::move(machine));
    }

    if (!allowConcreteGenericParams) {
      for (auto genericParam : result.getInnermostGenericParams()) {
        auto reduced = result.getReducedType(genericParam);

        if (reduced->hasError() || reduced->isEqual(genericParam))
          continue;

        if (reduced->isTypeParameter()) {
          // If one side is a parameter pack and the other is not, this is a
          // same-element requirement that cannot be expressed with only one
          // type parameter.
          if (genericParam->isParameterPack() != reduced->isParameterPack())
            continue;

          ctx.Diags.diagnose(loc, diag::requires_generic_params_made_equal,
                             genericParam, result->getSugaredType(reduced))
            .warnUntilSwiftVersion(6);
        } else {
          ctx.Diags.diagnose(loc,
                             diag::requires_generic_param_made_equal_to_concrete,
                             genericParam)
            .warnUntilSwiftVersion(6);
        }
      }
    }

    if (!errorFlags.contains(GenericSignatureErrorFlags::HasInvalidRequirements)) {
      // Check invariants.
      result.verify();
    }

    if (rewriteCtx.getDebugOptions().contains(DebugFlags::Timers)) {
      rewriteCtx.endTimer("InferredGenericSignatureRequest");
      llvm::dbgs() << result << "\n";
    }

    return GenericSignatureWithError(result, errorFlags);
  }
}
