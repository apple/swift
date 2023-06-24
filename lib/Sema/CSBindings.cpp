//===--- CSBindings.cpp - Constraint Solver -------------------------------===//
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
// This file implements selection of bindings for type variables.
//
//===----------------------------------------------------------------------===//
#include "swift/Sema/CSBindings.h"
#include "TypeChecker.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/raw_ostream.h"
#include <tuple>

using namespace swift;
using namespace constraints;
using namespace inference;

bool BindingSet::forClosureResult() const {
  return Info.TypeVar->getImpl().isClosureResultType();
}

bool BindingSet::forGenericParameter() const {
  return bool(Info.TypeVar->getImpl().getGenericParameter());
}

bool BindingSet::canBeNil() const {
  auto &ctx = CS.getASTContext();
  return Literals.count(
      ctx.getProtocol(KnownProtocolKind::ExpressibleByNilLiteral));
}

bool BindingSet::isDirectHole() const {
  // Direct holes are only allowed in "diagnostic mode".
  if (!CS.shouldAttemptFixes())
    return false;

  return Bindings.empty() && getNumViableLiteralBindings() == 0 &&
         Defaults.empty() && Info.TypeVar->getImpl().canBindToHole();
}

bool PotentialBindings::isGenericParameter() const {
  auto *locator = TypeVar->getImpl().getLocator();
  return locator && locator->isLastElement<LocatorPathElt::GenericParameter>();
}

bool PotentialBinding::isViableForJoin() const {
  return Kind == AllowedBindingKind::Supertypes &&
         !BindingType->hasLValueType() &&
         !BindingType->hasUnresolvedType() &&
         !BindingType->hasTypeVariable() &&
         !BindingType->hasPlaceholder() &&
         !BindingType->hasUnboundGenericType() &&
         !hasDefaultedLiteralProtocol() &&
         !isDefaultableBinding();
}

bool BindingSet::isDelayed() const {
  if (auto *locator = TypeVar->getImpl().getLocator()) {
    if (locator->isLastElement<LocatorPathElt::MemberRefBase>()) {
      // If first binding is a "fallback" to a protocol type,
      // it means that this type variable should be delayed
      // until it either gains more contextual information, or
      // there are no other type variables to attempt to make
      // forward progress.
      if (Bindings.empty())
        return true;


      if (Bindings[0].BindingType->is<ProtocolType>())
        return true;
    }

    // Since force unwrap preserves l-valueness, resulting
    // type variable has to be delayed until either l-value
    // binding becomes available or there are no other
    // variables to attempt.
    if (locator->directlyAt<ForceValueExpr>() &&
        TypeVar->getImpl().canBindToLValue()) {
      return llvm::none_of(Bindings, [](const PotentialBinding &binding) {
        return binding.BindingType->is<LValueType>();
      });
    }
  }

  if (isHole()) {
    auto *locator = TypeVar->getImpl().getLocator();
    assert(locator && "a hole without locator?");

    // Delay resolution of the code completion expression until
    // the very end to give it a chance to be bound to some
    // contextual type even if it's a hole.
    if (locator->directlyAt<CodeCompletionExpr>())
      return true;

    // Delay resolution of the `nil` literal to a hole until
    // the very end to give it a change to be bound to some
    // other type, just like code completion expression which
    // relies solely on contextual information.
    if (locator->directlyAt<NilLiteralExpr>())
      return true;

    // When inferring the type of a variable in a pattern, delay its resolution
    // so that we resolve type variables inside the expression as placeholders
    // instead of marking the type of the variable itself as a placeholder. This
    // allows us to produce more specific errors because the type variable in
    // the expression that introduced the placeholder might be diagnosable using
    // fixForHole.
    if (locator->isLastElement<LocatorPathElt::PatternDecl>()) {
      return true;
    }

    // It's possible that type of member couldn't be determined,
    // and if so it would be beneficial to bind member to a hole
    // early to propagate that information down to arguments,
    // result type of a call that references such a member.
    //
    // Note: This is done here instead of during binding inference,
    // because it's possible that variable is marked as a "hole"
    // (or that status is propagated to it) after constraints
    // mentioned below are recorded.
    return llvm::any_of(Info.DelayedBy, [&](Constraint *constraint) {
      switch (constraint->getKind()) {
      case ConstraintKind::ApplicableFunction:
      case ConstraintKind::DynamicCallableApplicableFunction:
      case ConstraintKind::BindOverload: {
        return !ConstraintSystem::typeVarOccursInType(
            TypeVar, CS.simplifyType(constraint->getSecondType()));
      }

      default:
        return true;
      }
    });
  }

  return !Info.DelayedBy.empty();
}

bool BindingSet::involvesTypeVariables() const {
  // This type variable always depends on a pack expansion variable
  // which should be inferred first if possible.
  if (TypeVar->getImpl().getGenericParameter() &&
      TypeVar->getImpl().canBindToPack())
    return true;

  // This is effectively O(1) right now since bindings are re-computed
  // on each step of the solver, but once bindings are computed
  // incrementally it becomes more important to double-check that
  // any adjacent type variables found previously are still unresolved.
  return llvm::any_of(AdjacentVars, [](TypeVariableType *typeVar) {
    return !typeVar->getImpl().getFixedType(/*record=*/nullptr);
  });
}

bool BindingSet::isPotentiallyIncomplete() const {
  // Generic parameters are always potentially incomplete.
  if (Info.isGenericParameter())
    return true;

  // If current type variable is associated with a code completion token
  // it's possible that it doesn't have enough contextual information
  // to be resolved to anything so let's delay considering it until everything
  // else is resolved.
  if (Info.AssociatedCodeCompletionToken)
    return true;

  auto *locator = TypeVar->getImpl().getLocator();
  if (!locator)
    return false;

  if (locator->isLastElement<LocatorPathElt::MemberRefBase>() &&
      !Bindings.empty()) {
    // If the base of the unresolved member reference like `.foo`
    // couldn't be resolved we'd want to bind it to a hole at the
    // very last moment possible, just like generic parameters.
    if (isHole())
      return true;

    auto &binding = Bindings.front();
    // If base type of a member chain is inferred to be a protocol type,
    // let's consider this binding set to be potentially incomplete since
    // that's done as a last resort effort at resolving first member.
    if (auto *constraint = binding.getSource()) {
      if (binding.BindingType->is<ProtocolType>() &&
          constraint->getKind() == ConstraintKind::ConformsTo)
        return true;
    }
  }

  if (locator->isLastElement<LocatorPathElt::UnresolvedMemberChainResult>()) {
    // If subtyping is allowed and this is a result of an implicit member chain,
    // let's delay binding it to an optional until its object type resolved too or
    // it has been determined that there is no possibility to resolve it. Otherwise
    // we might end up missing solutions since it's allowed to implicitly unwrap
    // base type of the chain but it can't be done early - type variable
    // representing chain's result type has a different l-valueness comparing
    // to generic parameter of the optional.
    if (llvm::any_of(Bindings, [&](const PotentialBinding &binding) {
          if (binding.Kind != AllowedBindingKind::Subtypes)
            return false;

          auto objectType = binding.BindingType->getOptionalObjectType();
          return objectType && objectType->isTypeVariableOrMember();
        }))
      return true;
  }

  if (isHole()) {
    // Delay resolution of the code completion expression until
    // the very end to give it a chance to be bound to some
    // contextual type even if it's a hole.
    if (locator->directlyAt<CodeCompletionExpr>())
      return true;

    // Delay resolution of the `nil` literal to a hole until
    // the very end to give it a change to be bound to some
    // other type, just like code completion expression which
    // relies solely on contextual information.
    if (locator->directlyAt<NilLiteralExpr>())
      return true;
  }

  // If there is a `bind param` constraint associated with
  // current type variable, result should be aware of that
  // fact. Binding set might be incomplete until
  // this constraint is resolved, because we currently don't
  // look-through constraints expect to `subtype` to try and
  // find related bindings.
  // This only affects type variable that appears one the
  // right-hand side of the `bind param` constraint and
  // represents result type of the closure body, because
  // left-hand side gets types from overload choices.
  if (llvm::any_of(
          Info.EquivalentTo,
          [&](const std::pair<TypeVariableType *, Constraint *> &equivalence) {
            auto *constraint = equivalence.second;
            return constraint->getKind() == ConstraintKind::BindParam &&
                   constraint->getSecondType()->isEqual(TypeVar);
          }))
    return true;

  return false;
}

void BindingSet::inferTransitiveProtocolRequirements(
    llvm::SmallDenseMap<TypeVariableType *, BindingSet> &inferredBindings) {
  if (TransitiveProtocols)
    return;

  llvm::SmallVector<std::pair<TypeVariableType *, TypeVariableType *>, 4>
      workList;
  llvm::SmallPtrSet<TypeVariableType *, 4> visitedRelations;

  llvm::SmallDenseMap<TypeVariableType *, SmallPtrSet<Constraint *, 4>, 4>
      protocols;

  auto addToWorkList = [&](TypeVariableType *parent,
                           TypeVariableType *typeVar) {
    if (visitedRelations.insert(typeVar).second)
      workList.push_back({parent, typeVar});
  };

  auto propagateProtocolsTo =
      [&protocols](TypeVariableType *dstVar,
                   const ArrayRef<Constraint *> &direct,
                   const SmallPtrSetImpl<Constraint *> &transitive) {
        auto &destination = protocols[dstVar];

        if (direct.size() > 0)
          destination.insert(direct.begin(), direct.end());

        if (transitive.size() > 0)
          destination.insert(transitive.begin(), transitive.end());
      };

  addToWorkList(nullptr, TypeVar);

  do {
    auto *currentVar = workList.back().second;

    auto cachedBindings = inferredBindings.find(currentVar);
    if (cachedBindings == inferredBindings.end()) {
      workList.pop_back();
      continue;
    }

    auto &bindings = cachedBindings->getSecond();

    // If current variable already has transitive protocol
    // conformances inferred, there is no need to look deeper
    // into subtype/equivalence chain.
    if (bindings.TransitiveProtocols) {
      TypeVariableType *parent = nullptr;
      std::tie(parent, currentVar) = workList.pop_back_val();
      assert(parent);
      propagateProtocolsTo(parent, bindings.getConformanceRequirements(),
                           *bindings.TransitiveProtocols);
      continue;
    }

    for (const auto &entry : bindings.Info.SubtypeOf)
      addToWorkList(currentVar, entry.first);

    // If current type variable is part of an equivalence
    // class, make it a "representative" and let it infer
    // supertypes and direct protocol requirements from
    // other members and their equivalence classes.
    SmallSetVector<TypeVariableType *, 4> equivalenceClass;
    {
      SmallVector<TypeVariableType *, 4> workList;
      workList.push_back(currentVar);

      do {
        auto *typeVar = workList.pop_back_val();

        if (!equivalenceClass.insert(typeVar))
          continue;

        auto bindingSet = inferredBindings.find(typeVar);
        if (bindingSet == inferredBindings.end())
          continue;

        auto &equivalences = bindingSet->getSecond().Info.EquivalentTo;
        for (const auto &eqVar : equivalences) {
          workList.push_back(eqVar.first);
        }
      } while (!workList.empty());
    }

    for (const auto &memberVar : equivalenceClass) {
      if (memberVar == currentVar)
        continue;

      auto eqBindings = inferredBindings.find(memberVar);
      if (eqBindings == inferredBindings.end())
        continue;

      const auto &bindings = eqBindings->getSecond();

      llvm::SmallPtrSet<Constraint *, 2> placeholder;
      // Add any direct protocols from members of the
      // equivalence class, so they could be propagated
      // to all of the members.
      propagateProtocolsTo(currentVar, bindings.getConformanceRequirements(),
                           placeholder);

      // Since type variables are equal, current type variable
      // becomes a subtype to any supertype found in the current
      // equivalence  class.
      for (const auto &eqEntry : bindings.Info.SubtypeOf)
        addToWorkList(currentVar, eqEntry.first);
    }

    // More subtype/equivalences relations have been added.
    if (workList.back().second != currentVar)
      continue;

    TypeVariableType *parent = nullptr;
    std::tie(parent, currentVar) = workList.pop_back_val();

    // At all of the protocols associated with current type variable
    // are transitive to its parent, propagate them down the subtype/equivalence
    // chain.
    if (parent) {
      propagateProtocolsTo(parent, bindings.getConformanceRequirements(),
                           protocols[currentVar]);
    }

    auto &inferredProtocols = protocols[currentVar];

    llvm::SmallPtrSet<Constraint *, 4> protocolsForEquivalence;

    // Equivalence class should contain both:
    // - direct protocol requirements of the current type
    //   variable;
    // - all of the transitive protocols inferred through
    //   the members of the equivalence class.
    {
      auto directRequirements = bindings.getConformanceRequirements();
      protocolsForEquivalence.insert(directRequirements.begin(),
                                     directRequirements.end());

      protocolsForEquivalence.insert(inferredProtocols.begin(),
                                     inferredProtocols.end());
    }

    // Propagate inferred protocols to all of the members of the
    // equivalence class.
    for (const auto &equivalence : bindings.Info.EquivalentTo) {
      auto eqBindings = inferredBindings.find(equivalence.first);
      if (eqBindings != inferredBindings.end()) {
        auto &bindings = eqBindings->getSecond();
        bindings.TransitiveProtocols.emplace(protocolsForEquivalence.begin(),
                                             protocolsForEquivalence.end());
      }
    }

    // Update the bindings associated with current type variable,
    // to avoid repeating this inference process.
    bindings.TransitiveProtocols.emplace(inferredProtocols.begin(),
                                         inferredProtocols.end());
  } while (!workList.empty());
}

void BindingSet::inferTransitiveBindings(
    const llvm::SmallDenseMap<TypeVariableType *, BindingSet>
        &inferredBindings) {
  using BindingKind = AllowedBindingKind;

  for (const auto &entry : Info.SupertypeOf) {
    auto relatedBindings = inferredBindings.find(entry.first);
    if (relatedBindings == inferredBindings.end())
      continue;

    auto &bindings = relatedBindings->getSecond();

    // FIXME: This is a workaround necessary because solver doesn't filter
    // bindings based on protocol requirements placed on a type variable.
    //
    // Forward propagate (subtype -> supertype) only literal conformance
    // requirements since that helps solver to infer more types at
    // parameter positions.
    //
    // \code
    // func foo<T: ExpressibleByStringLiteral>(_: String, _: T) -> T {
    //   fatalError()
    // }
    //
    // func bar(_: Any?) {}
    //
    // func test() {
    //   bar(foo("", ""))
    // }
    // \endcode
    //
    // If one of the literal arguments doesn't propagate its
    // `ExpressibleByStringLiteral` conformance, we'd end up picking
    // `T` with only one type `Any?` which is incorrect.
    for (const auto &literal : bindings.Literals)
      addLiteralRequirement(literal.second.getSource());

    // Infer transitive defaults.
    for (const auto &def : bindings.Defaults) {
      if (def.getSecond()->getKind() == ConstraintKind::FallbackType)
        continue;

      addDefault(def.second);
    }

    // TODO: We shouldn't need this in the future.
    if (entry.second->getKind() != ConstraintKind::Subtype)
      continue;

    for (auto &binding : bindings.Bindings) {
      // We need the binding kind for the potential binding to
      // either be Exact or Supertypes in order for it to make sense
      // to add Supertype bindings based on the relationship between
      // our type variables.
      if (binding.Kind != BindingKind::Exact &&
          binding.Kind != BindingKind::Supertypes)
        continue;

      auto type = binding.BindingType;

      if (type->isPlaceholder())
        continue;

      if (ConstraintSystem::typeVarOccursInType(TypeVar, type))
        continue;

      addBinding(binding.withSameSource(type, BindingKind::Supertypes));
    }
  }
}

void BindingSet::finalize(
    llvm::SmallDenseMap<TypeVariableType *, BindingSet> &inferredBindings) {
  inferTransitiveBindings(inferredBindings);

  determineLiteralCoverage();

  if (auto *locator = TypeVar->getImpl().getLocator()) {
    if (locator->isLastElement<LocatorPathElt::MemberRefBase>()) {
      // If this is a base of an unresolved member chain, as a last
      // resort effort let's infer base to be a protocol type based
      // on contextual conformance requirements.
      //
      // This allows us to find solutions in cases like this:
      //
      // \code
      // func foo<T: P>(_: T) {}
      // foo(.bar) <- `.bar` should be a static member of `P`.
      // \endcode
      if (!hasViableBindings()) {
        inferTransitiveProtocolRequirements(inferredBindings);

        if (TransitiveProtocols.has_value()) {
          for (auto *constraint : *TransitiveProtocols) {
            Type protocolTy = constraint->getSecondType();

            // The Copyable protocol can't have members, yet will be a
            // constraint of basically all type variables, so don't suggest it.
            //
            // NOTE: worth considering for all marker protocols, but keep in
            // mind that you're allowed to extend them with members!
            if (auto p = protocolTy->getAs<ProtocolType>()) {
              if (ProtocolDecl *decl = p->getDecl())
                if (decl->isSpecificProtocol(KnownProtocolKind::Copyable))
                  continue;
            }

            addBinding({protocolTy, AllowedBindingKind::Exact, constraint});
          }
        }
      }
    }

    if (CS.shouldAttemptFixes() &&
        locator->isLastElement<LocatorPathElt::UnresolvedMemberChainResult>()) {
      // Let's see whether this chain is valid, if it isn't then to avoid
      // diagnosing the same issue multiple different ways, let's infer
      // result of the chain to be a hole.
      auto *resultExpr =
          castToExpr<UnresolvedMemberChainResultExpr>(locator->getAnchor());
      auto *baseLocator = CS.getConstraintLocator(
          resultExpr->getChainBase(), ConstraintLocator::UnresolvedMember);

      if (CS.hasFixFor(
              baseLocator,
              FixKind::AllowInvalidStaticMemberRefOnProtocolMetatype)) {
        CS.recordPotentialHole(TypeVar);
        // Clear all of the previously collected bindings which are inferred
        // from inside of a member chain.
        Bindings.remove_if([](const PotentialBinding &binding) {
          return binding.Kind == AllowedBindingKind::Supertypes;
        });
      }
    }
  }
}

void BindingSet::addBinding(PotentialBinding binding) {
  if (Bindings.count(binding))
    return;

  if (!isViable(binding))
    return;

  SmallPtrSet<TypeVariableType *, 4> referencedTypeVars;
  binding.BindingType->getTypeVariables(referencedTypeVars);

  // If type variable is not allowed to bind to `lvalue`,
  // let's check if type of potential binding has any
  // type variables, which are allowed to bind to `lvalue`,
  // and postpone such type from consideration.
  //
  // This check is done here and not in `checkTypeOfBinding`
  // because the l-valueness of the variable might change during
  // solving and that would not be reflected in the graph.
  if (!TypeVar->getImpl().canBindToLValue()) {
    for (auto *typeVar : referencedTypeVars) {
      if (typeVar->getImpl().canBindToLValue())
        return;
    }
  }

  // Since Double and CGFloat are effectively the same type due to an
  // implicit conversion between them, always prefer Double over CGFloat
  // when possible.
  //
  // Note: This optimization can't be performed for closure parameters
  //       because their type could be converted only at the point of
  //       use in the closure body.
  if (!TypeVar->getImpl().isClosureParameterType()) {
    auto type = binding.BindingType;

    if (type->isCGFloat() &&
        llvm::any_of(Bindings, [](const PotentialBinding &binding) {
          return binding.BindingType->isDouble();
        }))
      return;

    if (type->isDouble()) {
      auto inferredCGFloat =
          llvm::find_if(Bindings, [](const PotentialBinding &binding) {
            return binding.BindingType->isCGFloat();
          });

      if (inferredCGFloat != Bindings.end()) {
        Bindings.erase(inferredCGFloat);
        Bindings.insert(inferredCGFloat->withType(type));
        return;
      }
    }
  }

  // If this is a non-defaulted supertype binding,
  // check whether we can combine it with another
  // supertype binding by computing the 'join' of the types.
  if (binding.isViableForJoin()) {
    auto isAcceptableJoin = [](Type type) {
      return !type->isAny() && (!type->getOptionalObjectType() ||
                                !type->getOptionalObjectType()->isAny());
    };

    SmallVector<PotentialBinding, 4> joined;
    for (auto existingBinding = Bindings.begin();
         existingBinding != Bindings.end();) {
      if (existingBinding->isViableForJoin()) {
        auto join =
            Type::join(existingBinding->BindingType, binding.BindingType);

        if (join && isAcceptableJoin(*join)) {
          // Result of the join has to use new binding because it refers
          // to the constraint that triggered the join that replaced the
          // existing binding.
          joined.push_back(binding.withType(*join));
          // Remove existing binding from the set.
          // It has to be re-introduced later, since its type has been changed.
          existingBinding = Bindings.erase(existingBinding);
          continue;
        }
      }

      ++existingBinding;
    }

    for (const auto &binding : joined)
      (void)Bindings.insert(binding);

    // If new binding has been joined with at least one of existing
    // bindings, there is no reason to include it into the set.
    if (!joined.empty())
      return;
  }

  for (auto *adjacentVar : referencedTypeVars)
    AdjacentVars.insert(adjacentVar);

  (void)Bindings.insert(std::move(binding));
}

void BindingSet::determineLiteralCoverage() {
  if (Literals.empty())
    return;

  bool allowsNil = canBeNil();

  for (auto &entry : Literals) {
    auto &literal = entry.second;

    if (!literal.viableAsBinding())
      continue;

    for (auto binding = Bindings.begin(); binding != Bindings.end();
         ++binding) {
      bool isCovered = false;
      Type adjustedTy;

      std::tie(isCovered, adjustedTy) =
          literal.isCoveredBy(*binding, allowsNil, CS.DC);

      if (!isCovered)
        continue;

      literal.setCoveredBy(binding->getSource());

      if (adjustedTy) {
        Bindings.erase(binding);
        Bindings.insert(binding->withType(adjustedTy));
      }

      break;
    }
  }
}

void BindingSet::addLiteralRequirement(Constraint *constraint) {
  auto isDirectRequirement = [&](Constraint *constraint) -> bool {
    if (auto *typeVar = constraint->getFirstType()->getAs<TypeVariableType>()) {
      auto *repr = CS.getRepresentative(typeVar);
      return repr == TypeVar;
    }

    return false;
  };

  auto *protocol = constraint->getProtocol();

  // Let's try to coalesce integer and floating point literal protocols
  // if they appear together because the only possible default type that
  // could satisfy both requirements is `Double`.
  {
    if (protocol->isSpecificProtocol(
            KnownProtocolKind::ExpressibleByIntegerLiteral)) {
      auto *floatLiteral = CS.getASTContext().getProtocol(
          KnownProtocolKind::ExpressibleByFloatLiteral);
      if (Literals.count(floatLiteral))
        return;
    }

    if (protocol->isSpecificProtocol(
            KnownProtocolKind::ExpressibleByFloatLiteral)) {
      auto *intLiteral = CS.getASTContext().getProtocol(
          KnownProtocolKind::ExpressibleByIntegerLiteral);
      Literals.erase(intLiteral);
    }
  }

  if (Literals.count(protocol) > 0)
    return;

  bool isDirect = isDirectRequirement(constraint);

  // Coverage is not applicable to `ExpressibleByNilLiteral` since it
  // doesn't have a default type.
  if (protocol->isSpecificProtocol(
          KnownProtocolKind::ExpressibleByNilLiteral)) {
    Literals.insert(
        {protocol, LiteralRequirement(constraint,
                                      /*DefaultType=*/Type(), isDirect)});
    return;
  }

  // Check whether any of the existing bindings covers this literal
  // protocol.
  LiteralRequirement literal(
      constraint, TypeChecker::getDefaultType(protocol, CS.DC), isDirect);

  Literals.insert({protocol, std::move(literal)});
}

BindingSet::BindingScore BindingSet::formBindingScore(const BindingSet &b) {
  // If there are no bindings available but this type
  // variable represents a closure - let's consider it
  // as having a single non-default binding - that would
  // be a type inferred based on context.
  // It's considered to be non-default for purposes of
  // ranking because we'd like to prioritize resolving
  // closures to gain more information from their bodies.
  unsigned numBindings = b.Bindings.size() + b.getNumViableLiteralBindings();
  auto numNonDefaultableBindings = numBindings > 0 ? numBindings
                                   : b.TypeVar->getImpl().isClosureType() ? 1
                                                                          : 0;

  return std::make_tuple(b.isHole(), numNonDefaultableBindings == 0,
                         b.isDelayed(), b.isSubtypeOfExistentialType(),
                         b.involvesTypeVariables(),
                         static_cast<unsigned char>(b.getLiteralForScore()),
                         -numNonDefaultableBindings);
}

Optional<BindingSet> ConstraintSystem::determineBestBindings(
    llvm::function_ref<void(const BindingSet &)> onCandidate) {
  // Look for potential type variable bindings.
  Optional<BindingSet> bestBindings;
  llvm::SmallDenseMap<TypeVariableType *, BindingSet> cache;

  // First, let's collect all of the possible bindings.
  for (auto *typeVar : getTypeVariables()) {
    if (!typeVar->getImpl().hasRepresentativeOrFixed()) {
      cache.insert({typeVar, getBindingsFor(typeVar, /*finalize=*/false)});
    }
  }

  // Determine whether given type variable with its set of bindings is
  // viable to be attempted on the next step of the solver. If type variable
  // has no "direct" bindings of any kind e.g. direct bindings to concrete
  // types, default types from "defaultable" constraints or literal
  // conformances, such type variable is not viable to be evaluated to be
  // attempted next.
  auto isViableForRanking = [this](const BindingSet &bindings) -> bool {
    auto *typeVar = bindings.getTypeVariable();

    // Type variable representing a base of unresolved member chain should
    // always be considered viable for ranking since it's allow to infer
    // types from transitive protocol requirements.
    if (auto *locator = typeVar->getImpl().getLocator()) {
      if (locator->isLastElement<LocatorPathElt::MemberRefBase>())
        return true;
    }

    // If type variable is marked as a potential hole there is always going
    // to be at least one binding available for it.
    if (shouldAttemptFixes() && typeVar->getImpl().canBindToHole())
      return true;

    return bool(bindings);
  };

  // Now let's see if we could infer something for related type
  // variables based on other bindings.
  for (auto *typeVar : getTypeVariables()) {
    auto cachedBindings = cache.find(typeVar);
    if (cachedBindings == cache.end())
      continue;

    auto &bindings = cachedBindings->getSecond();
    // Before attempting to infer transitive bindings let's check
    // whether there are any viable "direct" bindings associated with
    // current type variable, if there are none - it means that this type
    // variable could only be used to transitively infer bindings for
    // other type variables and can't participate in ranking.
    //
    // Viable bindings include - any types inferred from constraints
    // associated with given type variable, any default constraints,
    // or any conformance requirements to literal protocols with can
    // produce a default type.
    bool isViable = isViableForRanking(bindings);

    bindings.finalize(cache);

    if (!bindings || !isViable)
      continue;

    onCandidate(bindings);

    // If these are the first bindings, or they are better than what
    // we saw before, use them instead.
    if (!bestBindings || bindings < *bestBindings)
      bestBindings.emplace(bindings);
  }

  return bestBindings;
}

/// Find the set of type variables that are inferable from the given type.
///
/// \param type The type to search.
/// \param typeVars Collects the type variables that are inferable from the
/// given type. This set is not cleared, so that multiple types can be explored
/// and introduce their results into the same set.
static void
findInferableTypeVars(Type type,
                      SmallPtrSetImpl<TypeVariableType *> &typeVars) {
  type = type->getCanonicalType();
  if (!type->hasTypeVariable())
    return;

  class Walker : public TypeWalker {
    SmallPtrSetImpl<TypeVariableType *> &typeVars;

  public:
    explicit Walker(SmallPtrSetImpl<TypeVariableType *> &typeVars)
        : typeVars(typeVars) {}

    Action walkToTypePre(Type ty) override {
      if (ty->is<DependentMemberType>())
        return Action::SkipChildren;

      if (auto typeVar = ty->getAs<TypeVariableType>())
        typeVars.insert(typeVar);
      return Action::Continue;
    }
  };

  type.walk(Walker(typeVars));
}

void PotentialBindings::addDefault(Constraint *constraint) {
  Defaults.insert(constraint);
}

bool LiteralRequirement::isCoveredBy(Type type, DeclContext *useDC) const {
  auto coversDefaultType = [](Type type, Type defaultType) -> bool {
    if (!defaultType->hasUnboundGenericType())
      return type->isEqual(defaultType);

    // For generic literal types, check whether we already have a
    // specialization of this generic within our list.
    // FIXME: This assumes that, e.g., the default literal
    // int/float/char/string types are never generic.
    auto nominal = defaultType->getAnyNominal();
    if (!nominal)
      return false;

    // FIXME: Check parents?
    return nominal == type->getAnyNominal();
  };

  if (hasDefaultType() && coversDefaultType(type, getDefaultType()))
    return true;

  return (bool)TypeChecker::conformsToProtocol(type, getProtocol(),
                                               useDC->getParentModule());
}

std::pair<bool, Type>
LiteralRequirement::isCoveredBy(const PotentialBinding &binding,
                                bool canBeNil,
                                DeclContext *useDC) const {
  auto type = binding.BindingType;
  switch (binding.Kind) {
  case AllowedBindingKind::Exact:
    type = binding.BindingType;
    break;

  case AllowedBindingKind::Subtypes:
  case AllowedBindingKind::Supertypes:
    type = binding.BindingType->getRValueType();
    break;
  }

  bool requiresUnwrap = false;
  do {
    // Conformance check on type variable would always return true,
    // but type variable can't cover anything until it's bound.
    if (type->isTypeVariableOrMember() || type->isPlaceholder())
      return std::make_pair(false, Type());

    if (isCoveredBy(type, useDC)) {
      return std::make_pair(true, requiresUnwrap ? type : binding.BindingType);
    }

    // Can't unwrap optionals if there is `ExpressibleByNilLiteral`
    // conformance requirement placed on the type variable.
    if (canBeNil)
      return std::make_pair(false, Type());

    // If this literal protocol is not a direct requirement it
    // would not be possible to change optionality while inferring
    // bindings for a supertype, so this hack doesn't apply.
    if (!isDirectRequirement())
      return std::make_pair(false, Type());

    // If we're allowed to bind to subtypes, look through optionals.
    // FIXME: This is really crappy special case of computing a reasonable
    // result based on the given constraints.
    if (binding.Kind == AllowedBindingKind::Subtypes) {
      if (auto objTy = type->getOptionalObjectType()) {
        requiresUnwrap = true;
        type = objTy;
        continue;
      }
    }

    return std::make_pair(false, Type());
  } while (true);
}

void PotentialBindings::addPotentialBinding(PotentialBinding binding) {
  assert(!binding.BindingType->is<ErrorType>());

  // If the type variable can't bind to an lvalue, make sure the
  // type we pick isn't an lvalue.
  if (!TypeVar->getImpl().canBindToLValue() &&
      binding.BindingType->hasLValueType()) {
    binding = binding.withType(binding.BindingType->getRValueType());
  }

  Bindings.push_back(std::move(binding));
}

void PotentialBindings::addLiteral(Constraint *constraint) {
  Literals.insert(constraint);
}

bool BindingSet::isViable(PotentialBinding &binding) {
  // Prevent against checking against the same opened nominal type
  // over and over again. Doing so means redundant work in the best
  // case. In the worst case, we'll produce lots of duplicate solutions
  // for this constraint system, which is problematic for overload
  // resolution.
  auto type = binding.BindingType;

  auto *NTD = type->getAnyNominal();
  if (!NTD)
    return true;

  for (auto existing = Bindings.begin(); existing != Bindings.end();
       ++existing) {
    auto existingType = existing->BindingType;

    auto *existingNTD = existingType->getAnyNominal();
    if (!existingNTD || NTD != existingNTD)
      continue;

    // If new type has a type variable it shouldn't
    // be considered  viable.
    if (type->hasTypeVariable())
      return false;

    // If new type doesn't have any type variables,
    // but existing binding does, let's replace existing
    // binding with new one.
    if (existingType->hasTypeVariable()) {
      // First, let's remove all of the adjacent type
      // variables associated with this binding.
      {
        SmallPtrSet<TypeVariableType *, 4> referencedVars;
        existingType->getTypeVariables(referencedVars);
        for (auto *var : referencedVars)
          AdjacentVars.erase(var);
      }

      // And now let's remove the binding itself.
      Bindings.erase(existing);
      break;
    }
  }

  return true;
}

bool BindingSet::favoredOverDisjunction(Constraint *disjunction) const {
  if (isHole())
    return false;

  if (llvm::any_of(Bindings, [&](const PotentialBinding &binding) {
        if (binding.Kind == AllowedBindingKind::Supertypes)
          return false;

        auto type = binding.BindingType;

        if (CS.shouldAttemptFixes())
          return false;

        if (type->isAnyHashable() || type->isDouble() || type->isCGFloat())
          return false;

        {
          PointerTypeKind pointerKind;
          if (type->getAnyPointerElementType(pointerKind)) {
            switch (pointerKind) {
            case PTK_UnsafeRawPointer:
            case PTK_UnsafeMutableRawPointer:
              return false;

            default:
              break;
            }
          }
        }

        return type->is<StructType>() || type->is<EnumType>() ||
               type->is<BuiltinType>();
      })) {
    // Result type of subscript could be l-value so we can't bind it early.
    if (!TypeVar->getImpl().isSubscriptResultType() &&
        llvm::none_of(Info.DelayedBy, [](const Constraint *constraint) {
          return constraint->getKind() == ConstraintKind::Disjunction ||
                 constraint->getKind() == ConstraintKind::ValueMember;
        }))
      return true;
  }

  if (isDelayed())
    return false;

  // If this bindings are for a closure and there are no holes,
  // it shouldn't matter whether it there are any type variables
  // or not because e.g. parameter type can have type variables,
  // but we still want to resolve closure body early (instead of
  // attempting any disjunction) to gain additional contextual
  // information.
  if (TypeVar->getImpl().isClosureType()) {
    auto boundType = disjunction->getNestedConstraints()[0]->getFirstType();
    // If disjunction is attempting to bind a type variable, let's
    // favor closure because it would add additional context, otherwise
    // if it's something like a collection (where it has to pick
    // between a conversion and bridging conversion) or concrete
    // type let's prefer the disjunction.
    //
    // We are looking through optionals here because it could be
    // a situation where disjunction is formed to match optionals
    // either as deep equality or optional-to-optional conversion.
    // Such type variables might be connected to closure as well
    // e.g. when result type is optional, so it makes sense to
    // open closure before attempting such disjunction.
    return boundType->lookThroughAllOptionalTypes()->is<TypeVariableType>();
  }

  return !involvesTypeVariables();
}

bool BindingSet::favoredOverConjunction(Constraint *conjunction) const {
  if (CS.shouldAttemptFixes() && isHole()) {
    if (forClosureResult() || forGenericParameter())
      return false;
  }

  auto *locator = conjunction->getLocator();
  if (locator->directlyAt<ClosureExpr>()) {
    auto *closure = castToExpr<ClosureExpr>(locator->getAnchor());

    if (auto transform = CS.getAppliedResultBuilderTransform(closure)) {
      // Conjunctions that represent closures with result builder transformed
      // bodies could be attempted right after their resolution if they meet
      // all of the following criteria:
      //
      // - Builder type doesn't have any unresolved generic parameters;
      // - Closure doesn't have any parameters;
      // - The contextual result type is either concrete or opaque type.
      auto contextualType = transform->contextualType;
      if (!(contextualType && contextualType->is<FunctionType>()))
        return true;

      auto *contextualFnType =
          CS.simplifyType(contextualType)->castTo<FunctionType>();
      {
        auto resultType = contextualFnType->getResult();
        if (resultType->hasTypeVariable()) {
          auto *typeVar = resultType->getAs<TypeVariableType>();
          // If contextual result type is represented by an opaque type,
          // it's a strong indication that body is self-contained, otherwise
          // closure might rely on external types flowing into the body for
          // disambiguation of `build{Partial}Block` or `buildFinalResult`
          // calls.
          if (!(typeVar && typeVar->getImpl().isOpaqueType()))
            return true;
        }
      }

      // If some of the closure parameters are unresolved, the conjunction
      // has to be delayed to give them a chance to be inferred.
      if (llvm::any_of(contextualFnType->getParams(), [](const auto &param) {
            return param.getPlainType()->hasTypeVariable();
          }))
        return true;

      // Check whether conjunction has any unresolved type variables
      // besides the variable that represents the closure.
      //
      // Conjunction could refer to declarations from outer context
      // (i.e. a variable declared in the outer closure) or generic
      // parameters of the builder type), if any of such references
      // are not yet inferred the conjunction has to be delayed.
      auto *closureType = CS.getType(closure)->castTo<TypeVariableType>();
      return llvm::any_of(
          conjunction->getTypeVariables(), [&](TypeVariableType *typeVar) {
            return !(typeVar == closureType || CS.getFixedType(typeVar));
          });
    }
  }

  return true;
}

BindingSet ConstraintSystem::getBindingsFor(TypeVariableType *typeVar,
                                            bool finalize) {
  assert(typeVar->getImpl().getRepresentative(nullptr) == typeVar &&
         "not a representative");
  assert(!typeVar->getImpl().getFixedType(nullptr) && "has a fixed type");

  BindingSet bindings{CG[typeVar].getCurrentBindings()};

  if (finalize) {
    llvm::SmallDenseMap<TypeVariableType *, BindingSet> cache;
    bindings.finalize(cache);
  }

  return bindings;
}

/// Check whether the given type can be used as a binding for the given
/// type variable.
///
/// \returns the type to bind to, if the binding is okay.
static Optional<Type> checkTypeOfBinding(TypeVariableType *typeVar, Type type) {
  // If the type references the type variable, don't permit the binding.
  if (type->hasTypeVariable()) {
    SmallPtrSet<TypeVariableType *, 4> referencedTypeVars;
    type->getTypeVariables(referencedTypeVars);
    if (referencedTypeVars.count(typeVar))
      return None;
  }

  {
    auto objType = type->getWithoutSpecifierType();

    // If the type is a type variable itself, don't permit the binding.
    if (objType->is<TypeVariableType>())
      return None;

    // Don't bind to a dependent member type, even if it's currently
    // wrapped in any number of optionals, because binding producer
    // might unwrap and try to attempt it directly later.
    if (objType->lookThroughAllOptionalTypes()->is<DependentMemberType>())
      return None;
  }

  // Okay, allow the binding (with the simplified type).
  return type;
}

Optional<PotentialBinding>
PotentialBindings::inferFromRelational(Constraint *constraint) {
  assert(constraint->getClassification() ==
             ConstraintClassification::Relational &&
         "only relational constraints handled here");

  auto first = CS.simplifyType(constraint->getFirstType());
  auto second = CS.simplifyType(constraint->getSecondType());

  if (first->is<TypeVariableType>() && first->isEqual(second))
    return None;

  Type type;
  AllowedBindingKind kind;
  if (first->getAs<TypeVariableType>() == TypeVar) {
    // Upper bound for this type variable.
    type = second;
    kind = AllowedBindingKind::Subtypes;
  } else if (second->getAs<TypeVariableType>() == TypeVar) {
    // Lower bound for this type variable.
    type = first;
    kind = AllowedBindingKind::Supertypes;
  } else {
    // If the left-hand side of a relational constraint is a
    // type variable representing a closure type, let's delay
    // attempting any bindings related to any type variables
    // on the other side since it could only be either a closure
    // parameter or a result type, and we can't get a full set
    // of bindings for them until closure's body is opened.
    if (auto *typeVar = first->getAs<TypeVariableType>()) {
      if (typeVar->getImpl().isClosureType()) {
        DelayedBy.push_back(constraint);
        return None;
      }
    }

    // Check whether both this type and another type variable are
    // inferable.
    SmallPtrSet<TypeVariableType *, 4> typeVars;
    findInferableTypeVars(first, typeVars);
    findInferableTypeVars(second, typeVars);

    if (typeVars.erase(TypeVar)) {
      for (auto *typeVar : typeVars)
        AdjacentVars.insert({typeVar, constraint});
    }

    return None;
  }

  // Do not attempt to bind to ErrorType.
  if (type->hasError())
    return None;

  if (TypeVar->getImpl().isKeyPathType()) {
    auto *BGT = type->lookThroughAllOptionalTypes()->getAs<BoundGenericType>();
    if (!BGT || !isKnownKeyPathType(BGT))
      return None;

    // `PartialKeyPath<T>` represents a type-erased version of `KeyPath<T, V>`.
    //
    // In situations where partial key path cannot be used directly i.e.
    // passing an argument to a parameter represented by a partial key path,
    // let's attempt a `KeyPath` binding which would then be converted to a
    // partial key path since there is a subtype relationship between them.
    if (BGT->isPartialKeyPath() && kind == AllowedBindingKind::Subtypes) {
      auto &ctx = CS.getASTContext();
      auto *keyPathLoc = TypeVar->getImpl().getLocator();

      auto rootTy = BGT->getGenericArgs()[0];
      // Since partial key path is an erased version of `KeyPath`, the value
      // type would never be used, which means that binding can use
      // type variable generated for a result of key path expression.
      auto valueTy =
          keyPathLoc->castLastElementTo<LocatorPathElt::KeyPathType>()
              .getValueType();

      type = BoundGenericType::get(ctx.getKeyPathDecl(), Type(),
                                   {rootTy, valueTy});
    }
  }

  if (auto *locator = TypeVar->getImpl().getLocator()) {
    // Don't allow a protocol type to get propagated from the base to the result
    // type of a chain, Result should always be a concrete type which conforms
    // to the protocol inferred for the base.
    if (constraint->getKind() == ConstraintKind::UnresolvedMemberChainBase &&
        kind == AllowedBindingKind::Subtypes && type->is<ProtocolType>())
      return None;
  }

  // If the source of the binding is 'OptionalObject' constraint
  // and type variable is on the left-hand side, that means
  // that it _has_ to be of optional type, since the right-hand
  // side of the constraint is object type of the optional.
  if (constraint->getKind() == ConstraintKind::OptionalObject &&
      kind == AllowedBindingKind::Subtypes) {
    type = OptionalType::get(type);
  }

  // If the type we'd be binding to is a dependent member, don't try to
  // resolve this type variable yet.
  if (type->getWithoutSpecifierType()
          ->lookThroughAllOptionalTypes()
          ->is<DependentMemberType>()) {
    llvm::SmallPtrSet<TypeVariableType *, 4> referencedVars;
    type->getTypeVariables(referencedVars);

    bool containsSelf = false;
    for (auto *var : referencedVars) {
      // Add all type variables encountered in the type except
      // to the current type variable.
      if (var != TypeVar) {
        AdjacentVars.insert({var, constraint});
        continue;
      }

      containsSelf = true;
    }

    // If inferred type doesn't contain the current type variable,
    // let's mark bindings as delayed until dependent member type
    // is resolved.
    if (!containsSelf)
      DelayedBy.push_back(constraint);

    return None;
  }

  // If our binding choice is a function type and we're attempting
  // to bind to a type variable that is the result of opening a
  // generic parameter, strip the noescape bit so that we only allow
  // bindings of escaping functions in this position. We do this
  // because within the generic function we have no indication of
  // whether the parameter is a function type and if so whether it
  // should be allowed to escape. As a result we allow anything
  // passed in to escape.
  if (auto *fnTy = type->getAs<AnyFunctionType>()) {
    // Since inference now happens during constraint generation,
    // this hack should be allowed in both `Solving`
    // (during non-diagnostic mode) and `ConstraintGeneration` phases.
    if (isGenericParameter() &&
        (!CS.shouldAttemptFixes() ||
         CS.getPhase() == ConstraintSystemPhase::ConstraintGeneration)) {
      type = fnTy->withExtInfo(fnTy->getExtInfo().withNoEscape(false));
    }
  }

  // Check whether we can perform this binding.
  if (auto boundType = checkTypeOfBinding(TypeVar, type)) {
    type = *boundType;
  } else {
    auto *bindingTypeVar = type->getRValueType()->getAs<TypeVariableType>();

    if (!bindingTypeVar)
      return None;

    // If current type variable is associated with a code completion token
    // it's possible that it doesn't have enough contextual information
    // to be resolved to anything, so let's note that fact in the potential
    // bindings and use it when forming a hole if there are no other bindings
    // available.
    if (auto *locator = bindingTypeVar->getImpl().getLocator()) {
      if (locator->directlyAt<CodeCompletionExpr>())
        AssociatedCodeCompletionToken = locator->getAnchor();
    }

    switch (constraint->getKind()) {
    case ConstraintKind::Subtype:
    case ConstraintKind::SubclassOf:
    case ConstraintKind::Conversion:
    case ConstraintKind::ArgumentConversion:
    case ConstraintKind::OperatorArgumentConversion: {
      if (kind == AllowedBindingKind::Subtypes) {
        SubtypeOf.insert({bindingTypeVar, constraint});
      } else {
        assert(kind == AllowedBindingKind::Supertypes);
        SupertypeOf.insert({bindingTypeVar, constraint});
      }

      AdjacentVars.insert({bindingTypeVar, constraint});
      break;
    }

    case ConstraintKind::Bind:
    case ConstraintKind::BindParam:
    case ConstraintKind::Equal: {
      EquivalentTo.insert({bindingTypeVar, constraint});
      AdjacentVars.insert({bindingTypeVar, constraint});
      break;
    }

    case ConstraintKind::UnresolvedMemberChainBase: {
      EquivalentTo.insert({bindingTypeVar, constraint});

      // Don't record adjacency between base and result types,
      // this is just an auxiliary constraint to enforce ordering.
      break;
    }

    default:
      break;
    }

    return None;
  }

  // Make sure we aren't trying to equate type variables with different
  // lvalue-binding rules.
  if (auto otherTypeVar = type->getAs<TypeVariableType>()) {
    if (TypeVar->getImpl().canBindToLValue() !=
        otherTypeVar->getImpl().canBindToLValue())
      return None;
  }

  if (type->is<InOutType>() && !TypeVar->getImpl().canBindToInOut())
    type = LValueType::get(type->getInOutObjectType());
  if (type->is<LValueType>() && !TypeVar->getImpl().canBindToLValue())
    type = type->getRValueType();

  // BindParam constraints are not reflexive and must be treated specially.
  if (constraint->getKind() == ConstraintKind::BindParam) {
    if (kind == AllowedBindingKind::Subtypes) {
      if (auto *lvt = type->getAs<LValueType>()) {
        type = InOutType::get(lvt->getObjectType());
      }
    } else if (kind == AllowedBindingKind::Supertypes) {
      if (auto *iot = type->getAs<InOutType>()) {
        type = LValueType::get(iot->getObjectType());
      }
    }
    kind = AllowedBindingKind::Exact;
  }

  return PotentialBinding{type, kind, constraint};
}

/// Retrieve the set of potential type bindings for the given
/// representative type variable, along with flags indicating whether
/// those types should be opened.
void PotentialBindings::infer(Constraint *constraint) {
  switch (constraint->getKind()) {
  case ConstraintKind::Bind:
  case ConstraintKind::Equal:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Subtype:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::UnresolvedMemberChainBase: {
    auto binding = inferFromRelational(constraint);
    if (!binding)
      break;

    addPotentialBinding(*binding);
    break;
  }
  case ConstraintKind::KeyPathApplication: {
    // If this variable is in the application projected result type, delay
    // binding until we've bound other type variables in the key-path
    // application constraint. This ensures we try to bind the key path type
    // first, which can allow us to discover additional bindings for the result
    // type.
    SmallPtrSet<TypeVariableType *, 4> typeVars;
    findInferableTypeVars(CS.simplifyType(constraint->getThirdType()),
                          typeVars);
    if (typeVars.count(TypeVar)) {
      DelayedBy.push_back(constraint);
    }

    break;
  }

  case ConstraintKind::BridgingConversion:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::KeyPath:
  case ConstraintKind::SyntacticElement:
  case ConstraintKind::Conjunction:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::SameShape:
    // Constraints from which we can't do anything.
    break;

  // For now let's avoid inferring protocol requirements from
  // this constraint, but in the future we could do that to
  // to filter bindings.
  case ConstraintKind::TransitivelyConformsTo:
    break;

  case ConstraintKind::DynamicTypeOf: {
    // Direct binding of the left-hand side could result
    // in `DynamicTypeOf` failure if right-hand side is
    // bound (because 'Bind' requires equal types to
    // succeed), or left is bound to Any which is not an
    // [existential] metatype.
    auto dynamicType = constraint->getFirstType();
    if (auto *tv = dynamicType->getAs<TypeVariableType>()) {
      if (tv->getImpl().getRepresentative(nullptr) == TypeVar) {
        DelayedBy.push_back(constraint);
        break;
      }
    }

    // This is right-hand side, let's continue.
    break;
  }

  case ConstraintKind::Defaultable:
  case ConstraintKind::FallbackType:
    // Do these in a separate pass.
    if (CS.getFixedTypeRecursive(constraint->getFirstType(), true)
            ->getAs<TypeVariableType>() == TypeVar) {
      addDefault(constraint);
    }
    break;

  case ConstraintKind::Disjunction:
    // If there is additional context available via disjunction
    // associated with closure literal (e.g. coercion to some other
    // type) let's delay resolving the closure until the disjunction
    // is attempted.
    DelayedBy.push_back(constraint);
    break;

  case ConstraintKind::ConformsTo:
  case ConstraintKind::SelfObjectOfProtocol: {
    auto protocolTy = constraint->getSecondType();
    if (protocolTy->is<ProtocolType>())
      Protocols.push_back(constraint);
    break;
  }

  case ConstraintKind::LiteralConformsTo: {
    // Record constraint where protocol requirement originated
    // this is useful to use for the binding later.
    addLiteral(constraint);
    break;
  }

  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::DynamicCallableApplicableFunction: {
    auto overloadTy = constraint->getSecondType();
    // If current type variable represents an overload set
    // being applied to the arguments, it can't be delayed
    // by application constraints, because it doesn't
    // depend on argument/result types being resolved first.
    if (overloadTy->isEqual(TypeVar))
      break;

    LLVM_FALLTHROUGH;
  }

  case ConstraintKind::BindOverload: {
    DelayedBy.push_back(constraint);
    break;
  }

  case ConstraintKind::ValueMember:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::ValueWitness:
  case ConstraintKind::PropertyWrapper: {
    // If current type variable represents a member type of some reference,
    // it would be bound once member is resolved either to a actual member
    // type or to a hole if member couldn't be found.
    auto memberTy = constraint->getSecondType()->castTo<TypeVariableType>();

    if (memberTy->getImpl().hasRepresentativeOrFixed()) {
      if (auto type = memberTy->getImpl().getFixedType(/*record=*/nullptr)) {
        // It's possible that member has been bound to some other type variable
        // instead of merged with it because it's wrapped in an l-value type.
        if (type->getWithoutSpecifierType()->isEqual(TypeVar)) {
          DelayedBy.push_back(constraint);
          break;
        }
      } else {
        memberTy = memberTy->getImpl().getRepresentative(/*record=*/nullptr);
      }
    }

    if (memberTy == TypeVar)
      DelayedBy.push_back(constraint);

    break;
  }

  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam: {
    // Don't produce any bindings if this type variable is on the left-hand
    // side of a one-way binding.
    auto firstType = constraint->getFirstType();
    if (auto *tv = firstType->getAs<TypeVariableType>()) {
      if (tv->getImpl().getRepresentative(nullptr) == TypeVar) {
        DelayedBy.push_back(constraint);
        break;
      }
    }

    break;
  }
  }
}

void PotentialBindings::retract(Constraint *constraint) {
  Bindings.erase(
      llvm::remove_if(Bindings,
                      [&constraint](const PotentialBinding &binding) {
                        return binding.getSource() == constraint;
                      }),
      Bindings.end());

  auto isMatchingConstraint = [&constraint](Constraint *existing) {
    return existing == constraint;
  };

  auto hasMatchingSource =
      [&constraint](
          const std::pair<TypeVariableType *, Constraint *> &adjacency) {
        return adjacency.second == constraint;
      };

  switch (constraint->getKind()) {
  case ConstraintKind::ConformsTo:
  case ConstraintKind::SelfObjectOfProtocol:
    Protocols.erase(llvm::remove_if(Protocols, isMatchingConstraint),
                    Protocols.end());
    break;

  case ConstraintKind::LiteralConformsTo:
    Literals.erase(constraint);
    break;

  case ConstraintKind::Defaultable:
  case ConstraintKind::FallbackType: {
    Defaults.erase(constraint);
    break;
  }

  default:
    break;
  }

  {
    llvm::SmallPtrSet<TypeVariableType *, 2> unviable;
    for (const auto &adjacent : AdjacentVars) {
      if (adjacent.second == constraint)
        unviable.insert(adjacent.first);
    }

    for (auto *adjacentVar : unviable)
      AdjacentVars.erase(std::make_pair(adjacentVar, constraint));
  }

  DelayedBy.erase(llvm::remove_if(DelayedBy, isMatchingConstraint),
                  DelayedBy.end());

  SubtypeOf.remove_if(hasMatchingSource);
  SupertypeOf.remove_if(hasMatchingSource);
  EquivalentTo.remove_if(hasMatchingSource);
}

void BindingSet::forEachLiteralRequirement(
    llvm::function_ref<void(KnownProtocolKind)> callback) const {
  for (const auto &literal : Literals) {
    auto *protocol = literal.first;
    const auto &info = literal.second;

    // Only uncovered defaultable literal protocols participate.
    if (!info.viableAsBinding())
      continue;
    
    if (auto protocolKind = protocol->getKnownProtocolKind())
      callback(*protocolKind);
  }
}

LiteralBindingKind BindingSet::getLiteralForScore() const {
  LiteralBindingKind kind = LiteralBindingKind::None;

  forEachLiteralRequirement([&](KnownProtocolKind protocolKind) {
    switch (protocolKind) {
    case KnownProtocolKind::ExpressibleByDictionaryLiteral:
    case KnownProtocolKind::ExpressibleByArrayLiteral:
    case KnownProtocolKind::ExpressibleByStringInterpolation:
      kind = LiteralBindingKind::Collection;
      break;

    case KnownProtocolKind::ExpressibleByFloatLiteral:
      kind = LiteralBindingKind::Float;
      break;

    default:
      if (kind != LiteralBindingKind::Collection)
        kind = LiteralBindingKind::Atom;
      break;
    }
  });
  return kind;
}

unsigned BindingSet::getNumViableLiteralBindings() const {
  return llvm::count_if(Literals, [&](const auto &literal) {
    return literal.second.viableAsBinding();
  });
}

/// Return string for atomic literal kinds (integer, string, & boolean) for
/// printing in debug output.
static std::string getAtomLiteralAsString(ExprKind EK) {
#define ENTRY(Kind, String)                                                    \
  case ExprKind::Kind:                                                         \
    return String
  switch (EK) {
    ENTRY(IntegerLiteral, "integer");
    ENTRY(StringLiteral, "string");
    ENTRY(BooleanLiteral, "boolean");
    ENTRY(NilLiteral, "nil");
  default:
    return "";
  }
#undef ENTRY
}

/// Return string for collection literal kinds (interpolated string, array,
/// dictionary) for printing in debug output.
static std::string getCollectionLiteralAsString(KnownProtocolKind KPK) {
#define ENTRY(Kind, String)                                                    \
  case KnownProtocolKind::Kind:                                                \
    return String
  switch (KPK) {
    ENTRY(ExpressibleByDictionaryLiteral, "dictionary");
    ENTRY(ExpressibleByArrayLiteral, "array");
    ENTRY(ExpressibleByStringInterpolation, "interpolated string");
  default:
    return "";
  }
#undef ENTRY
}

void BindingSet::dump(llvm::raw_ostream &out, unsigned indent) const {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;

  if (auto typeVar = getTypeVariable()) {
    typeVar->getImpl().print(out);
    out << " ";
  }

  std::vector<std::string> attributes;
  if (isDirectHole())
    attributes.push_back("hole");
  if (isPotentiallyIncomplete())
    attributes.push_back("potentially_incomplete");
  if (isDelayed())
    attributes.push_back("delayed");
  if (isSubtypeOfExistentialType())
    attributes.push_back("subtype_of_existential");
  if (!attributes.empty()) {
    out << "[attributes: ";
    interleave(attributes, out, ", ");
  }
  
  auto literalKind = getLiteralForScore();
  if (literalKind != LiteralBindingKind::None) {
    if (!attributes.empty()) {
      out << ", ";
    } else {
      out << "[attributes: ";
    }
    out << "[literal: ";
    switch (literalKind) {
    case LiteralBindingKind::Atom: {
      if (auto atomKind = TypeVar->getImpl().getAtomicLiteralKind()) {
        out << getAtomLiteralAsString(*atomKind);
      }
      break;
    }
    case LiteralBindingKind::Collection: {
      std::vector<std::string> collectionLiterals;
      forEachLiteralRequirement([&](KnownProtocolKind protocolKind) {
        collectionLiterals.push_back(
            getCollectionLiteralAsString(protocolKind));
      });
      interleave(collectionLiterals, out, ", ");
      break;
    }
    case LiteralBindingKind::Float:
    case LiteralBindingKind::None:
      out << getLiteralBindingKind(literalKind).str();
      break;
    }
    if (attributes.empty()) {
      out << "]] ";
    } else {
      out << "]";
    }
  }
  if (!attributes.empty())
    out << "] ";

  if (involvesTypeVariables()) {
    out << "[involves_type_vars: ";
    interleave(AdjacentVars,
               [&](const auto *typeVar) { out << typeVar->getString(PO); },
               [&out]() { out << ", "; });
    out << "] ";
  }

  auto numDefaultable = getNumViableDefaultableBindings();
  if (numDefaultable > 0)
    out << "[#defaultable_bindings: " << numDefaultable << "] ";

  struct PrintableBinding {
  private:
    enum class BindingKind { Exact, Subtypes, Supertypes, Literal };
    BindingKind Kind;
    Type BindingType;
    PrintableBinding(BindingKind kind, Type bindingType)
        : Kind(kind), BindingType(bindingType) {}

  public:
    static PrintableBinding supertypesOf(Type binding) {
      return PrintableBinding{BindingKind::Supertypes, binding};
    }
    
    static PrintableBinding subtypesOf(Type binding) {
      return PrintableBinding{BindingKind::Subtypes, binding};
    }
    
    static PrintableBinding exact(Type binding) {
      return PrintableBinding{BindingKind::Exact, binding};
    }
    
    static PrintableBinding literalDefaultType(Type binding) {
      return PrintableBinding{BindingKind::Literal, binding};
    }

    void print(llvm::raw_ostream &out, const PrintOptions &PO,
               unsigned indent = 0) const {
      switch (Kind) {
      case BindingKind::Exact:
        break;
      case BindingKind::Subtypes:
        out << "(subtypes of) ";
        break;
      case BindingKind::Supertypes:
        out << "(supertypes of) ";
        break;
      case BindingKind::Literal:
        out << "(default type of literal) ";
        break;
      }
      BindingType.print(out, PO);
    }
  };

  out << "[with possible bindings: ";
  SmallVector<PrintableBinding, 2> potentialBindings;
  for (const auto &binding : Bindings) {
    switch (binding.Kind) {
    case AllowedBindingKind::Exact:
      potentialBindings.push_back(PrintableBinding::exact(binding.BindingType));
      break;
    case AllowedBindingKind::Supertypes:
      potentialBindings.push_back(
          PrintableBinding::supertypesOf(binding.BindingType));
      break;
    case AllowedBindingKind::Subtypes:
      potentialBindings.push_back(
          PrintableBinding::subtypesOf(binding.BindingType));
      break;
    }
  }
  for (const auto &literal : Literals) {
    if (literal.second.viableAsBinding()) {
      potentialBindings.push_back(PrintableBinding::literalDefaultType(
          literal.second.getDefaultType()));
    }
  }
  if (potentialBindings.empty()) {
    out << "<empty>";
  } else {
    interleave(
        potentialBindings,
        [&](const PrintableBinding &binding) { binding.print(out, PO); },
        [&] { out << ", "; });
  }
  out << "]";

  if (!Defaults.empty()) {
    out << " [defaults: ";
    interleave(
        Defaults,
        [&](const auto &entry) {
          auto *constraint = entry.second;
          auto defaultBinding =
              PrintableBinding::exact(constraint->getSecondType());
          defaultBinding.print(out, PO);
        },
        [&] { out << ", "; });
    out << "]";
  }
  
}

// Given a possibly-Optional type, return the direct superclass of the
// (underlying) type wrapped in the same number of optional levels as
// type.
static Type getOptionalSuperclass(Type type) {
  int optionalLevels = 0;
  while (auto underlying = type->getOptionalObjectType()) {
    ++optionalLevels;
    type = underlying;
  }

  if (!type->mayHaveSuperclass())
    return Type();

  auto superclass = type->getSuperclass();
  if (!superclass)
    return Type();

  while (optionalLevels--)
    superclass = OptionalType::get(superclass);

  return superclass;
}

/// Enumerates all of the 'direct' supertypes of the given type.
///
/// The direct supertype S of a type T is a supertype of T (e.g., T < S)
/// such that there is no type U where T < U and U < S.
static SmallVector<Type, 4> enumerateDirectSupertypes(Type type) {
  SmallVector<Type, 4> result;

  if (type->is<InOutType>() || type->is<LValueType>()) {
    type = type->getWithoutSpecifierType();
    result.push_back(type);
  }

  if (auto superclass = getOptionalSuperclass(type)) {
    // FIXME: Can also weaken to the set of protocol constraints, but only
    // if there are any protocols that the type conforms to but the superclass
    // does not.

    result.push_back(superclass);
  }

  // FIXME: lots of other cases to consider!
  return result;
}

bool TypeVarBindingProducer::computeNext() {
  SmallVector<Binding, 4> newBindings;
  auto addNewBinding = [&](Binding binding) {
    auto type = binding.BindingType;

    // If we've already tried this binding, move on.
    if (!BoundTypes.insert(type.getPointer()).second)
      return;

    if (!ExploredTypes.insert(type->getCanonicalType()).second)
      return;

    newBindings.push_back(std::move(binding));
  };

  // Let's attempt only directly inferrable bindings for
  // a type variable representing a closure type because
  // such type variables are handled specially and only
  // bound to a type inferred from their expression, having
  // contextual bindings is just a trigger for that to
  // happen.
  if (TypeVar->getImpl().isClosureType())
    return false;

  for (auto &binding : Bindings) {
    const auto type = binding.BindingType;
    assert(!type->hasError());

    // If we have a protocol with a default type, look for alternative
    // types to the default.
    if (NumTries == 0 && binding.hasDefaultedLiteralProtocol()) {
      auto knownKind =
          *(binding.getDefaultedLiteralProtocol()->getKnownProtocolKind());
      SmallVector<Type, 2> scratch;
      for (auto altType : CS.getAlternativeLiteralTypes(knownKind, scratch)) {
        addNewBinding(binding.withSameSource(altType, BindingKind::Subtypes));
      }
    }

    if (getLocator()->directlyAt<ForceValueExpr>() &&
        TypeVar->getImpl().canBindToLValue() &&
        !binding.BindingType->is<LValueType>()) {
      // Result of force unwrap is always connected to its base
      // optional type via `OptionalObject` constraint which
      // preserves l-valueness, so in case where object type got
      // inferred before optional type (because it got the
      // type from context e.g. parameter type of a function call),
      // we need to test type with and without l-value after
      // delaying bindings for as long as possible.
      addNewBinding(binding.withType(LValueType::get(binding.BindingType)));
    }

    // There is a tailored fix for optional key path root references,
    // let's not create ambiguity by attempting unwrap when it's
    // not allowed.
    if (binding.Kind != BindingKind::Subtypes &&
        getLocator()->isKeyPathRoot() && type->getOptionalObjectType())
      continue;

    // Allow solving for T even for a binding kind where that's invalid
    // if fixes are allowed, because that gives us the opportunity to
    // match T? values to the T binding by adding an unwrap fix.
    if (binding.Kind == BindingKind::Subtypes || CS.shouldAttemptFixes()) {
      // If we were unsuccessful solving for T?, try solving for T.
      if (auto objTy = type->getOptionalObjectType()) {
        // If T is a type variable, only attempt this if both the
        // type variable we are trying bindings for, and the type
        // variable we will attempt to bind, both have the same
        // polarity with respect to being able to bind lvalues.
        if (auto otherTypeVar = objTy->getAs<TypeVariableType>()) {
          if (TypeVar->getImpl().canBindToLValue() ==
              otherTypeVar->getImpl().canBindToLValue()) {
            addNewBinding(binding.withSameSource(objTy, binding.Kind));
          }
        } else {
          addNewBinding(binding.withSameSource(objTy, binding.Kind));
        }
      }
    }

    auto srcLocator = binding.getLocator();
    if (srcLocator &&
        (srcLocator->isLastElement<LocatorPathElt::ApplyArgToParam>() ||
         srcLocator->isLastElement<LocatorPathElt::AutoclosureResult>()) &&
        !type->hasTypeVariable() && type->isKnownStdlibCollectionType()) {
      // If the type binding comes from the argument conversion, let's
      // instead of binding collection types directly, try to bind
      // using temporary type variables substituted for element
      // types, that's going to ensure that subtype relationship is
      // always preserved.
      auto *BGT = type->castTo<BoundGenericType>();
      auto dstLocator = TypeVar->getImpl().getLocator();
      auto newType =
          CS.openUnboundGenericType(BGT->getDecl(), BGT->getParent(),
                                    dstLocator, /*isTypeResolution=*/false)
              ->reconstituteSugar(/*recursive=*/false);
      addNewBinding(binding.withType(newType));
    }

    if (binding.Kind == BindingKind::Supertypes) {
      // If this is a type variable representing closure result,
      // which is on the right-side of some relational constraint
      // let's have it try `Void` as well because there is an
      // implicit conversion `() -> T` to `() -> Void` and this
      // helps to avoid creating a thunk to support it.
      if (getLocator()->isLastElement<LocatorPathElt::ClosureResult>() &&
          binding.Kind == AllowedBindingKind::Supertypes) {
        auto voidType = CS.getASTContext().TheEmptyTupleType;
        addNewBinding(binding.withSameSource(voidType, BindingKind::Exact));
      }

      for (auto supertype : enumerateDirectSupertypes(type)) {
        // If we're not allowed to try this binding, skip it.
        if (auto simplifiedSuper = checkTypeOfBinding(TypeVar, supertype))
          addNewBinding(binding.withType(*simplifiedSuper));
      }
    }
  }

  if (NumTries == 0) {
    // Add defaultable constraints (if any).
    for (auto *constraint : DelayedDefaults) {
      if (constraint->getKind() == ConstraintKind::FallbackType) {
        // If there are no other possible bindings for this variable
        // let's default it to the fallback type, otherwise we should
        // only attempt contextual types.
        if (!ExploredTypes.empty())
          continue;
      }

      addNewBinding(getDefaultBinding(constraint));
    }
  }

  if (newBindings.empty())
    return false;

  Index = 0;
  ++NumTries;
  Bindings = std::move(newBindings);
  return true;
}

Optional<std::pair<ConstraintFix *, unsigned>>
TypeVariableBinding::fixForHole(ConstraintSystem &cs) const {
  auto *dstLocator = TypeVar->getImpl().getLocator();
  auto *srcLocator = Binding.getLocator();

  // FIXME: This check could be turned into an assert once
  // all code completion kinds are ported to use
  // `TypeChecker::typeCheckForCodeCompletion` API.
  if (cs.isForCodeCompletion()) {
    // If the hole is originated from code completion expression
    // let's not try to fix this, anything connected to a
    // code completion is allowed to be a hole because presence
    // of a code completion token makes constraint system
    // under-constrained due to e.g. lack of expressions on the
    // right-hand side of the token, which are required for a
    // regular type-check.
    if (dstLocator->directlyAt<CodeCompletionExpr>() ||
        srcLocator->directlyAt<CodeCompletionExpr>())
      return None;
  }

  unsigned defaultImpact = 1;

  if (auto *GP = TypeVar->getImpl().getGenericParameter()) {
    // If it is representative for a key path root, let's emit a more
    // specific diagnostic.
    auto *keyPathRoot =
        cs.isRepresentativeFor(TypeVar, ConstraintLocator::KeyPathRoot);
    if (keyPathRoot) {
      ConstraintFix *fix = SpecifyKeyPathRootType::create(
          cs, keyPathRoot->getImpl().getLocator());
      return std::make_pair(fix, defaultImpact);
    } else {
      auto path = dstLocator->getPath();
      // Drop `generic parameter` locator element so that all missing
      // generic parameters related to the same path can be coalesced later.
      ConstraintFix *fix = DefaultGenericArgument::create(
          cs, GP,
          cs.getConstraintLocator(dstLocator->getAnchor(), path.drop_back()));
      return std::make_pair(fix, defaultImpact);
    }
  }

  if (TypeVar->getImpl().isClosureParameterType()) {
    ConstraintFix *fix = SpecifyClosureParameterType::create(cs, dstLocator);
    return std::make_pair(fix, defaultImpact);
  }

  if (TypeVar->getImpl().isClosureResultType()) {
    auto *closure = castToExpr<ClosureExpr>(dstLocator->getAnchor());
    // If the whole body is being ignored due to a pre-check failure,
    // let's not record a fix about result type since there is
    // just not enough context to infer it without a body.
    auto *closureLoc = cs.getConstraintLocator(closure);
    if (cs.hasFixFor(closureLoc, FixKind::IgnoreInvalidResultBuilderBody) ||
        cs.hasFixFor(closureLoc, FixKind::IgnoreResultBuilderWithReturnStmts))
      return None;

    ConstraintFix *fix = SpecifyClosureReturnType::create(cs, dstLocator);
    return std::make_pair(fix, defaultImpact);
  }

  if (srcLocator->directlyAt<ObjectLiteralExpr>()) {
    ConstraintFix *fix = SpecifyObjectLiteralTypeImport::create(cs, dstLocator);
    return std::make_pair(fix, defaultImpact);
  }

  if (srcLocator->isKeyPathRoot()) {
    // If we recorded an invalid key path fix, let's skip this specify root
    // type fix because it wouldn't produce a useful diagnostic.
    auto *kpLocator = cs.getConstraintLocator(srcLocator->getAnchor());
    if (cs.hasFixFor(kpLocator, FixKind::AllowKeyPathWithoutComponents))
      return None;

    // If key path has any invalid component, let's just skip fix because the
    // invalid component would be already diagnosed.
    auto keyPath = castToExpr<KeyPathExpr>(srcLocator->getAnchor());
    if (llvm::any_of(keyPath->getComponents(),
                     [](KeyPathExpr::Component component) {
                       return !component.isValid();
                     }))
      return None;

    ConstraintFix *fix = SpecifyKeyPathRootType::create(cs, dstLocator);
    return std::make_pair(fix, defaultImpact);
  }

  if (srcLocator->isLastElement<LocatorPathElt::PlaceholderType>()) {
    // When a 'nil' has a placeholder as contextual type there is not enough
    // information to resolve it, so let's record a specify contextual type for
    // nil fix.
    if (isExpr<NilLiteralExpr>(srcLocator->getAnchor())) {
      ConstraintFix *fix = SpecifyContextualTypeForNil::create(cs, dstLocator);
      return std::make_pair(fix, /*impact=*/(unsigned)10);
    }

    ConstraintFix *fix = SpecifyTypeForPlaceholder::create(cs, srcLocator);
    return std::make_pair(fix, defaultImpact);
  }

  if (dstLocator->directlyAt<NilLiteralExpr>()) {
    // This is a dramatic event, it means that there is absolutely
    // no contextual information to resolve type of `nil`.
    ConstraintFix *fix = SpecifyContextualTypeForNil::create(cs, dstLocator);
    return std::make_pair(fix, /*impact=*/(unsigned)10);
  }

  if (auto pattern = dstLocator->getPatternMatch()) {
    if (dstLocator->isLastElement<LocatorPathElt::PatternDecl>()) {
      // If this is the pattern in a for loop, and we have a mismatch of the
      // element type, then we don't have any useful contextual information
      // for the pattern, and can just bind to a hole without needing to penalize
      // the solution further.
      auto *seqLoc = cs.getConstraintLocator(
          dstLocator->getAnchor(), ConstraintLocator::SequenceElementType);
      if (cs.hasFixFor(seqLoc,
                       FixKind::IgnoreCollectionElementContextualMismatch)) {
        return None;
      }
      // Not being able to infer the type of a variable in a pattern binding
      // decl is more dramatic than anything that could happen inside the
      // expression because we want to preferrably point the diagnostic to a
      // part of the expression that caused us to be unable to infer the
      // variable's type.
      ConstraintFix *fix =
          IgnoreUnresolvedPatternVar::create(cs, pattern.get(), dstLocator);
      return std::make_pair(fix, /*impact=*/(unsigned)100);
    }
  }

  if (srcLocator->isLastElement<LocatorPathElt::MemberRefBase>()) {
    auto *baseExpr = castToExpr<UnresolvedMemberExpr>(srcLocator->getAnchor());
    ConstraintFix *fix = SpecifyBaseTypeForContextualMember::create(
        cs, baseExpr->getName(), srcLocator);
    return std::make_pair(fix, defaultImpact);
  }

  return None;
}

bool TypeVariableBinding::attempt(ConstraintSystem &cs) const {
  auto type = Binding.BindingType;
  auto *srcLocator = Binding.getLocator();
  auto *dstLocator = TypeVar->getImpl().getLocator();

  if (Binding.hasDefaultedLiteralProtocol()) {
    type = cs.replaceInferableTypesWithTypeVars(type, dstLocator);
    type = type->reconstituteSugar(/*recursive=*/false);
  }

  // If type variable has been marked as a possible hole due to
  // e.g. reference to a missing member. Let's propagate that
  // information to the object type of the optional type it's
  // about to be bound to.
  //
  // In some situations like pattern bindings e.g. `if let x = base?.member`
  // - if `member` doesn't exist, `x` cannot be determined either, which
  // leaves `OptionalEvaluationExpr` representing outer type of `base?.member`
  // without any contextual information, so even though `x` would get
  // bound to result type of the chain, underlying type variable wouldn't
  // be resolved, so we need to propagate holes up the conversion chain.
  // Also propagate in code completion mode because in some cases code
  // completion relies on type variable being a potential hole.
  if (TypeVar->getImpl().canBindToHole()) {
    if (srcLocator->directlyAt<OptionalEvaluationExpr>() ||
        cs.isForCodeCompletion()) {
      if (auto objectTy = type->getOptionalObjectType()) {
        if (auto *typeVar = objectTy->getAs<TypeVariableType>()) {
          cs.recordPotentialHole(typeVar);
        }
      }
    }
  }

  ConstraintSystem::TypeMatchOptions options;

  options |= ConstraintSystem::TMF_GenerateConstraints;
  options |= ConstraintSystem::TMF_BindingTypeVariable;

  auto result =
      cs.matchTypes(TypeVar, type, ConstraintKind::Bind, options, srcLocator);

  if (result.isFailure()) {
    if (cs.isDebugMode()) {
      PrintOptions PO;
      PO.PrintTypesForDebugging = true;

      llvm::errs().indent(cs.solverState->getCurrentIndent())
          << "(failed to establish binding " << TypeVar->getString(PO)
          << " := " << type->getString(PO) << ")\n";
    }
    return false;
  }

  auto reportHole = [&]() {
    if (cs.isForCodeCompletion()) {
      // Don't penalize solutions with unresolved generics.
      if (TypeVar->getImpl().getGenericParameter())
        return false;

      // Don't penalize solutions with holes due to missing arguments after the
      // code completion position.
      auto argLoc = srcLocator->findLast<LocatorPathElt::SynthesizedArgument>();
      if (argLoc && argLoc->isAfterCodeCompletionLoc())
        return false;

      // Don't penalize solutions that have holes for ignored arguments.
      if (cs.hasArgumentsIgnoredForCodeCompletion()) {
        // Avoid simplifying the locator if the constraint system didn't ignore
        // any arguments.
        auto argExpr = simplifyLocatorToAnchor(TypeVar->getImpl().getLocator());
        if (cs.isArgumentIgnoredForCodeCompletion(argExpr.dyn_cast<Expr *>())) {
          return false;
        }
      }
    }
    // Reflect in the score that this type variable couldn't be
    // resolved and had to be bound to a placeholder "hole" type.
    cs.increaseScore(SK_Hole);

    if (auto fix = fixForHole(cs)) {
      if (cs.recordFix(/*fix=*/fix->first, /*impact=*/fix->second))
        return true;
    }
    return false;
  };

  // If this was from a defaultable binding note that.
  if (Binding.isDefaultableBinding()) {
    cs.DefaultedConstraints.insert(srcLocator);

    // Fail if hole reporting fails.
    if (type->isPlaceholder() && reportHole())
      return false;
  }

  if (cs.simplify())
    return false;

  // If all of the re-activated constraints where simplified,
  // let's notify binding inference about the fact that type
  // variable has been bound successfully.
  {
    auto &CG = cs.getConstraintGraph();
    CG[TypeVar].introduceToInference(type);
  }

  return true;
}
