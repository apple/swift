//===--- TypeCheckNameLookup.cpp - Type Checker Name Lookup ---------------===//
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
// This file implements name lookup within the type checker, which can
// involve additional type-checking operations and the implicit
// declaration of members (such as constructors).
//
//===----------------------------------------------------------------------===//
#include "TypeChecker.h"
#include "TypoCorrection.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/TopCollection.h"
#include <algorithm>

using namespace swift;

namespace {
  /// Builder that helps construct a lookup result from the raw lookup
  /// data.
  class LookupResultBuilder {
    LookupResult &Result;
    DeclContext *DC;
    NameLookupOptions Options;

    /// The vector of found declarations.
    SmallVector<ValueDecl *, 4> FoundDecls;
    /// The vector of found declarations.
    SmallVector<ValueDecl *, 4> FoundOuterDecls;

    /// The set of known declarations.
    llvm::SmallDenseMap<std::pair<ValueDecl *, DeclContext *>, bool, 4> Known;

  public:
    LookupResultBuilder(LookupResult &result, DeclContext *dc,
                        NameLookupOptions options)
      : Result(result), DC(dc), Options(options) {
      if (dc->getASTContext().isAccessControlDisabled())
        Options |= NameLookupFlags::IgnoreAccessControl;
    }

    ~LookupResultBuilder() {
      // Remove any overridden declarations from the found-declarations set.
      removeOverriddenDecls(FoundDecls);
      removeOverriddenDecls(FoundOuterDecls);

      // Remove any shadowed declarations from the found-declarations set.
      removeShadowedDecls(FoundDecls, DC);
      removeShadowedDecls(FoundOuterDecls, DC);

      // Filter out those results that have been removed from the
      // found-declarations set.
      unsigned foundIdx = 0, foundSize = FoundDecls.size(),
               foundOuterSize = FoundOuterDecls.size();
      Result.filter([&](LookupResultEntry result, bool isOuter) -> bool {
        unsigned idx = foundIdx;
        unsigned limit = foundSize;
        ArrayRef<ValueDecl *> decls = FoundDecls;
        if (isOuter) {
          idx = foundIdx - foundSize;
          limit = foundOuterSize;
          decls = FoundOuterDecls;
        }
        // If the current result matches the remaining found declaration,
        // keep it and move to the next found declaration.
        if (idx < limit && result.getValueDecl() == decls[idx]) {
          ++foundIdx;
          return true;
        }

        // Otherwise, this result should be filtered out.
        return false;
      });
    }

    /// Add a new result.
    ///
    /// \param found The declaration we found.
    ///
    /// \param baseDC The declaration context through which we found the
    /// declaration.
    ///
    /// \param baseDecl The declaration that defines the base of the
    /// call to `found`
    ///
    /// \param foundInType The type through which we found the
    /// declaration.
    ///
    /// \param isOuter Whether this is an outer result (i.e. a result that isn't
    /// from the innermost scope with results)
    void add(ValueDecl *found, DeclContext *baseDC, ValueDecl *baseDecl,
             Type foundInType, bool isOuter) {
      DeclContext *foundDC = found->getDeclContext();

      auto addResult = [&](ValueDecl *result) {
        if (Known.insert({{result, baseDC}, false}).second) {
          // HERE, need to look up base decl
          Result.add(LookupResultEntry(baseDC, baseDecl, result), isOuter);
          if (isOuter)
            FoundOuterDecls.push_back(result);
          else
            FoundDecls.push_back(result);
        }
      };

      // If this isn't a protocol member to be given special
      // treatment, just add the result.
      if (!isa<ProtocolDecl>(foundDC) ||
          isa<GenericTypeParamDecl>(found) ||
          isa<TypeAliasDecl>(found) ||
          (isa<FuncDecl>(found) && cast<FuncDecl>(found)->isOperator())) {
        addResult(found);
        return;
      }

      assert(isa<ProtocolDecl>(foundDC));

      // If we found something within the protocol itself, and our
      // search began somewhere that is not in a protocol or extension
      // thereof, remap this declaration to the witness.
      auto conformingType = foundInType;

      // When performing a lookup on a subclass existential, we might
      // find a member of the class that witnesses a requirement on a
      // protocol that the class conforms to.
      //
      // Since subclass existentials don't normally conform to protocols,
      // pull out the superclass instead, and use that below.
      if (foundInType->isExistentialType()) {
        auto layout = foundInType->getExistentialLayout();
        if (auto superclass = layout.getSuperclass()) {
          conformingType = superclass;
        } else {
          // Non-subclass existential: don't need to look for further
          // conformance or witness.
          addResult(found);
          return;
        }
      }

      // Dig out the protocol conformance.
      auto *foundProto = cast<ProtocolDecl>(foundDC);
      auto conformance = DC->getParentModule()->lookupConformance(
          conformingType, foundProto);
      if (conformance.isInvalid()) {
        if (foundInType->isExistentialType()) {
          // If there's no conformance, we have an existential
          // and we found a member from one of the protocols, and
          // not a class constraint if any.
          addResult(found);
        }
        return;
      }

      if (conformance.isAbstract()) {
        assert(foundInType->is<ArchetypeType>() ||
               foundInType->isExistentialType());
        addResult(found);
        return;
      }

      // Dig out the witness.
      ValueDecl *witness = nullptr;
      auto concrete = conformance.getConcrete();
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(found)) {
        witness = concrete->getTypeWitnessAndDecl(assocType).getWitnessDecl();
      } else if (found->isProtocolRequirement()) {
        witness = concrete->getWitnessDecl(found);

        // It is possible that a requirement is visible to us, but
        // not the witness. In this case, just return the requirement;
        // we will perform virtual dispatch on the concrete type.
        if (witness &&
            !Options.contains(NameLookupFlags::IgnoreAccessControl) &&
            !witness->isAccessibleFrom(DC)) {
          addResult(found);
          return;
        }
      } else if (isa<NominalTypeDecl>(found)) {
        // Declaring nested types inside other types is currently
        // not supported by lookup would still return such members
        // so we have to account for that here as well.
        addResult(found);
        return;
      }

      // FIXME: the "isa<ProtocolDecl>()" check will be wrong for
      // default implementations in protocols.
      //
      // If we have an imported conformance or the witness could
      // not be deserialized, getWitnessDecl() will just return
      // the requirement, so just drop the lookup result here.
      if (witness && !isa<ProtocolDecl>(witness->getDeclContext()))
        addResult(witness);
    }
  };
} // end anonymous namespace

static UnqualifiedLookupOptions
convertToUnqualifiedLookupOptions(NameLookupOptions options) {
  UnqualifiedLookupOptions newOptions = UnqualifiedLookupFlags::AllowProtocolMembers;
  if (options.contains(NameLookupFlags::IgnoreAccessControl))
    newOptions |= UnqualifiedLookupFlags::IgnoreAccessControl;
  if (options.contains(NameLookupFlags::IncludeOuterResults))
    newOptions |= UnqualifiedLookupFlags::IncludeOuterResults;
  if (options.contains(NameLookupFlags::IncludeUsableFromInline))
    newOptions |= UnqualifiedLookupFlags::IncludeUsableFromInline;
  if (options.contains(NameLookupFlags::ExcludeMacroExpansions))
    newOptions |= UnqualifiedLookupFlags::ExcludeMacroExpansions;

  return newOptions;
}

LookupResult TypeChecker::lookupUnqualified(DeclContext *dc, DeclNameRef name,
                                            SourceLoc loc,
                                            NameLookupOptions options) {
  auto &ctx = dc->getASTContext();
  // HACK: Qualified lookup cannot be allowed to synthesize CodingKeys because
  // it would lead to a number of egregious cycles through
  // QualifiedLookupRequest when we resolve the protocol conformance. Codable's
  // magic has pushed its way so deeply into the compiler, we have to
  // pessimistically force every nominal context above this one to synthesize
  // it in the event the user needs it from e.g. a non-primary input.
  // We can undo this if Codable's semantic content is divorced from its
  // syntactic content - so we synthesize just enough to allow lookups to
  // succeed, but don't force protocol conformances while we're doing it.
  if (name.getBaseIdentifier() == ctx.Id_CodingKeys) {
    for (auto typeCtx = dc->getInnermostTypeContext(); typeCtx != nullptr;
         typeCtx = typeCtx->getParent()->getInnermostTypeContext()) {
      if (auto *nominal = typeCtx->getSelfNominalTypeDecl()) {
        nominal->synthesizeSemanticMembersIfNeeded(name.getFullName());
      }
    }
  }

  auto ulOptions = convertToUnqualifiedLookupOptions(options);
  auto descriptor = UnqualifiedLookupDescriptor(name, dc, loc, ulOptions);
  auto lookup = evaluateOrDefault(ctx.evaluator,
                                  UnqualifiedLookupRequest{descriptor}, {});

  LookupResult result;
  LookupResultBuilder builder(result, dc, options);
  for (auto idx : indices(lookup.allResults())) {
    const auto &found = lookup[idx];
    // Determine which type we looked through to find this result.
    Type foundInType;

    if (auto *typeDC = found.getDeclContext()) {
      if (!typeDC->isTypeContext()) {
        // If we don't have a type context this is an implicit 'self' reference.
        if (auto *CE = dyn_cast<ClosureExpr>(typeDC)) {
          typeDC = typeDC->getInnermostTypeContext();
        } else {
          // Otherwise, we must have the method context.
          typeDC = typeDC->getParent();
        }
        assert(typeDC->isTypeContext());
      }
      foundInType = dc->mapTypeIntoContext(
        typeDC->getDeclaredInterfaceType());
      assert(foundInType && "bogus base declaration?");
    }

    builder.add(found.getValueDecl(), found.getDeclContext(),
                found.getBaseDecl(), foundInType,
                /*isOuter=*/idx >= lookup.getIndexOfFirstOuterResult());
  }
  return result;
}

LookupResult
TypeChecker::lookupUnqualifiedType(DeclContext *dc, DeclNameRef name,
                                   SourceLoc loc,
                                   NameLookupOptions options) {
  auto &ctx = dc->getASTContext();
  auto ulOptions = convertToUnqualifiedLookupOptions(options) |
                   UnqualifiedLookupFlags::TypeLookup;
  {
    // Try lookup without ProtocolMembers first.
    auto desc = UnqualifiedLookupDescriptor(
        name, dc, loc,
        ulOptions - UnqualifiedLookupFlags::AllowProtocolMembers);

    auto lookup =
        evaluateOrDefault(ctx.evaluator, UnqualifiedLookupRequest{desc}, {});
    if (!lookup.allResults().empty())
      return lookup;
  }

  {
    // Try again, this time with protocol members.
    //
    // FIXME: Fix the problem where if NominalTypeDecl::getAllProtocols()
    // is called too early, we start resolving extensions -- even those
    // which do provide not conformances.
    auto desc = UnqualifiedLookupDescriptor(
        name, dc, loc,
        ulOptions | UnqualifiedLookupFlags::AllowProtocolMembers);
    return evaluateOrDefault(ctx.evaluator, UnqualifiedLookupRequest{desc}, {});
  }
}

LookupResult TypeChecker::lookupMember(DeclContext *dc,
                                       Type type, DeclNameRef name,
                                       SourceLoc loc,
                                       NameLookupOptions options) {
  assert(type->mayHaveMembers());

  LookupResult result;
  NLOptions subOptions = (NL_QualifiedDefault | NL_ProtocolMembers);
  if (options.contains(NameLookupFlags::IgnoreAccessControl))
    subOptions |= NL_IgnoreAccessControl;

  // We handle our own overriding/shadowing filtering.
  subOptions &= ~NL_RemoveOverridden;
  subOptions &= ~NL_RemoveNonVisible;

  // Make sure we've resolved implicit members, if we need them.
  namelookup::installSemanticMembersIfNeeded(type, name);

  LookupResultBuilder builder(result, dc, options);
  SmallVector<ValueDecl *, 4> lookupResults;
  dc->lookupQualified(type, name, loc, subOptions, lookupResults);

  for (auto found : lookupResults)
    builder.add(found, nullptr, /*baseDecl=*/nullptr, type, /*isOuter=*/false);

  return result;
}

static bool doesTypeAliasFullyConstrainAllOuterGenericParams(
    TypeAliasDecl *aliasDecl) {
  auto parentSig = aliasDecl->getDeclContext()->getGenericSignatureOfContext();
  auto genericSig = aliasDecl->getGenericSignature();

  if (!parentSig || !genericSig)
    return false;

  for (auto *paramType : parentSig.getGenericParams()) {
    if (!genericSig->isConcreteType(paramType))
      return false;
  }

  return true;
}

TypeChecker::UnsupportedMemberTypeAccessKind
TypeChecker::isUnsupportedMemberTypeAccess(Type type, TypeDecl *typeDecl,
                                           bool hasUnboundOpener,
                                           bool isExtensionBinding) {
  // We don't allow lookups of a non-generic typealias of an unbound
  // generic type, because we have no way to model such a type in the
  // AST.
  //
  // For generic typealiases, the typealias itself has an unbound
  // generic form whose parent type can be another unbound generic
  // type.
  if (type->hasUnboundGenericType()) {
    // Generic typealiases can be accessed with an unbound generic
    // base, since we represent the member type as an unbound generic
    // type.
    //
    // Non-generic type aliases can only be accessed if the
    // underlying type is not dependent.
    if (auto *aliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      if (!aliasDecl->isGeneric() &&
          aliasDecl->getUnderlyingType()->hasTypeParameter() &&
          !doesTypeAliasFullyConstrainAllOuterGenericParams(aliasDecl)) {
        return UnsupportedMemberTypeAccessKind::TypeAliasOfUnboundGeneric;
      }
    }

    if (isa<AssociatedTypeDecl>(typeDecl))
      return UnsupportedMemberTypeAccessKind::AssociatedTypeOfUnboundGeneric;

    if (isa<NominalTypeDecl>(typeDecl))
      if (!hasUnboundOpener && !isExtensionBinding)
        return UnsupportedMemberTypeAccessKind::NominalTypeOfUnboundGeneric;
  }

  if (type->isExistentialType() &&
      typeDecl->getDeclContext()->getSelfProtocolDecl()) {
    // Allow typealias member access on existential types if the underlying
    // type does not have any type parameters.
    if (auto *aliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      if (aliasDecl->getUnderlyingType()->getCanonicalType()
          ->hasTypeParameter())
        return UnsupportedMemberTypeAccessKind::TypeAliasOfExistential;
    } else if (isa<AssociatedTypeDecl>(typeDecl)) {
      return UnsupportedMemberTypeAccessKind::AssociatedTypeOfExistential;
    }
  }

  return UnsupportedMemberTypeAccessKind::None;
}

LookupTypeResult TypeChecker::lookupMemberType(DeclContext *dc,
                                               Type type, DeclNameRef name,
                                               SourceLoc loc,
                                               NameLookupOptions options) {
  LookupTypeResult result;

  // Look for members with the given name.
  SmallVector<ValueDecl *, 4> decls;
  NLOptions subOptions = (NL_QualifiedDefault | NL_OnlyTypes | NL_ProtocolMembers);

  if (options.contains(NameLookupFlags::IgnoreAccessControl))
    subOptions |= NL_IgnoreAccessControl;
  if (options.contains(NameLookupFlags::IncludeUsableFromInline))
    subOptions |= NL_IncludeUsableFromInline;

  // Make sure we've resolved implicit members, if we need them.
  namelookup::installSemanticMembersIfNeeded(type, name);

  if (!dc->lookupQualified(type, name, loc, subOptions, decls))
    return result;

  // Look through the declarations, keeping only the unique type declarations.
  llvm::SmallPtrSet<CanType, 4> types;
  SmallVector<AssociatedTypeDecl *, 4> inferredAssociatedTypes;
  for (auto decl : decls) {
    auto *typeDecl = cast<TypeDecl>(decl);

    // HACK: Lookups rooted at a typealias are trying to look for its underlying
    // type so they shouldn't also find that same typealias.
    if (decl == dyn_cast<TypeAliasDecl>(dc)) {
      continue;
    }

    if (isUnsupportedMemberTypeAccess(type, typeDecl, true)
          != TypeChecker::UnsupportedMemberTypeAccessKind::None) {
      auto memberType = typeDecl->getDeclaredInterfaceType();

      // Add the type to the result set, so that we can diagnose the
      // reference instead of just saying the member does not exist.
      if (types.insert(memberType->getCanonicalType()).second)
        result.addResult({typeDecl, memberType, nullptr});

      continue;
    }

    // If we're looking up an associated type of a concrete type,
    // record it later for conformance checking; we might find a more
    // direct typealias with the same name later.
    if (typeDecl->getDeclContext()->getSelfProtocolDecl()) {
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(typeDecl)) {
        if (!type->is<ArchetypeType>() &&
            !type->isTypeParameter()) {
          inferredAssociatedTypes.push_back(assocType);
          continue;
        }
      }

      // Nominal type members of protocols cannot be accessed with an
      // archetype base, because we have no way to recover the correct
      // substitutions.
      if (type->is<ArchetypeType>() &&
          isa<NominalTypeDecl>(typeDecl)) {
        continue;
      }
    }

    // Substitute the base into the member's type.
    auto memberType = substMemberTypeWithBase(dc->getParentModule(),
                                              typeDecl, type);

    // If we haven't seen this type result yet, add it to the result set.
    if (types.insert(memberType->getCanonicalType()).second)
      result.addResult({typeDecl, memberType, nullptr});
  }

  if (!result) {
    // We couldn't find any normal declarations. Let's try inferring
    // associated types.
    for (AssociatedTypeDecl *assocType : inferredAssociatedTypes) {
      // If the type does not actually conform to the protocol, skip this
      // member entirely.
      auto *protocol = cast<ProtocolDecl>(assocType->getDeclContext());

      auto conformance = dc->getParentModule()->lookupConformance(type, protocol);
      if (!conformance) {
        // FIXME: This is an error path. Should we try to recover?
        continue;
      }

      // Use the type witness.
      auto concrete = conformance.getConcrete();

      // This is the only case where NormalProtocolConformance::
      // getTypeWitnessAndDecl() returns a null type.
      if (concrete->getState() ==
          ProtocolConformanceState::CheckingTypeWitnesses)
        continue;

      auto *typeDecl =
        concrete->getTypeWitnessAndDecl(assocType).getWitnessDecl();

      // Circularity.
      if (!typeDecl)
        continue;

      auto memberType =
          substMemberTypeWithBase(dc->getParentModule(), typeDecl, type);
      if (types.insert(memberType->getCanonicalType()).second)
        result.addResult({typeDecl, memberType, assocType});
    }
  }

  return result;
}

unsigned TypeChecker::getCallEditDistance(DeclNameRef writtenName,
                                          DeclName correctedName,
                                          unsigned maxEditDistance) {
  // TODO: consider arguments.
  // TODO: maybe ignore certain kinds of missing / present labels for the
  //   first argument label?
  // TODO: word-based rather than character-based?
  if (writtenName.getBaseName().getKind() !=
        correctedName.getBaseName().getKind()) {
    return UnreasonableCallEditDistance;
  }

  if (writtenName.getBaseName().getKind() != DeclBaseName::Kind::Normal) {
    return 0;
  }

  StringRef writtenBase = writtenName.getBaseName().userFacingName();
  StringRef correctedBase = correctedName.getBaseName().userFacingName();

  // Don't typo-correct to a name with a leading underscore unless the typed
  // name also begins with an underscore.
  if (correctedBase.startswith("_") && !writtenBase.startswith("_")) {
    return UnreasonableCallEditDistance;
  }

  unsigned distance = writtenBase.edit_distance(correctedBase, maxEditDistance);

  // Bound the distance to UnreasonableCallEditDistance.
  if (distance >= maxEditDistance ||
      distance > (correctedBase.size() + 2) / 3) {
    return UnreasonableCallEditDistance;
  }

  return distance;
}

static bool isPlausibleTypo(DeclRefKind refKind, DeclNameRef typedName,
                            ValueDecl *candidate) {
  // Ignore anonymous declarations.
  if (!candidate->hasName())
    return false;

  // An operator / identifier mismatch is never a plausible typo.
  auto fn = dyn_cast<FuncDecl>(candidate);
  if (typedName.isOperator() != (fn && fn->isOperator()))
    return false;
  if (!typedName.isOperator())
    return true;

  // TODO: honor ref kind?  This is trickier than it sounds because we
  // may not have processed attributes and types on the candidate yet.
  return true;
}

void TypeChecker::performTypoCorrection(DeclContext *DC, DeclRefKind refKind,
                                        Type baseTypeOrNull,
                                        NameLookupOptions lookupOptions,
                                        TypoCorrectionResults &corrections,
                                        GenericSignature genericSig,
                                        unsigned maxResults) {
  // Disable typo-correction if we won't show the diagnostic anyway or if
  // we've hit our typo correction limit.
  auto &Ctx = DC->getASTContext();
  if (!Ctx.shouldPerformTypoCorrection() ||
      (Ctx.Diags.hasFatalErrorOccurred() &&
       !Ctx.Diags.getShowDiagnosticsAfterFatalError()))
    return;

  // Fill in a collection of the most reasonable entries.
  TopCollection<unsigned, ValueDecl *> entries(maxResults);
  auto consumer = makeDeclConsumer([&](ValueDecl *decl,
                                       DeclVisibilityKind reason) {
    // Never match an operator with an identifier or vice-versa; this is
    // not a plausible typo.
    if (!isPlausibleTypo(refKind, corrections.WrittenName, decl))
      return;

    const auto candidateName = decl->getName();

    // Don't waste time computing edit distances that are more than
    // the worst in our collection.
    unsigned maxDistance =
      entries.getMinUninterestingScore(UnreasonableCallEditDistance);

    unsigned distance =
      getCallEditDistance(corrections.WrittenName, candidateName,
                          maxDistance);

    // Ignore values that are further than a reasonable distance.
    if (distance >= UnreasonableCallEditDistance)
      return;

    entries.insert(distance, std::move(decl));
  });

  if (baseTypeOrNull) {
    lookupVisibleMemberDecls(consumer, baseTypeOrNull, SourceLoc(), DC,
                             /*includeInstanceMembers*/true,
                             /*includeDerivedRequirements*/false,
                             /*includeProtocolExtensionMembers*/true,
                             genericSig);
  } else {
    lookupVisibleDecls(consumer, DC, /*top level*/ true,
                       corrections.Loc.getBaseNameLoc());
  }

  // Impose a maximum distance from the best score.
  entries.filterMaxScoreRange(MaxCallEditDistanceFromBestCandidate);

  for (auto &entry : entries)
    corrections.Candidates.push_back(entry.Value);
}

void
TypoCorrectionResults::addAllCandidatesToLookup(LookupResult &lookup) const {
  for (auto candidate : Candidates)
    lookup.add(LookupResultEntry(candidate), /*isOuter=*/false);
}

static Decl *findExplicitParentForImplicitDecl(ValueDecl *decl) {
  if (!decl->getLoc().isValid() && decl->getDeclContext()->isTypeContext()) {
    Decl *parentDecl = dyn_cast<ExtensionDecl>(decl->getDeclContext());
    if (!parentDecl) parentDecl = cast<NominalTypeDecl>(decl->getDeclContext());
    if (parentDecl->getLoc().isValid())
      return parentDecl;
  }

  return nullptr;
}

static InFlightDiagnostic
noteTypoCorrection(DeclNameLoc loc, ValueDecl *decl,
                   bool wasClaimed) {
  if (auto var = dyn_cast<VarDecl>(decl)) {
    // Suggest 'self' at the use point instead of pointing at the start
    // of the function.
    if (var->isSelfParameter()) {
      if (wasClaimed) {
        // We don't need an extra note for this case because the programmer
        // knows what 'self' refers to.
        return InFlightDiagnostic();
      }

      auto &Diags = decl->getASTContext().Diags;
      return Diags.diagnose(loc.getBaseNameLoc(), diag::note_typo_candidate,
                            var->getName().str());
    }
  }

  if (Decl *parentDecl = findExplicitParentForImplicitDecl(decl)) {
    StringRef kind = (isa<VarDecl>(decl) ? "property" :
                      isa<ConstructorDecl>(decl) ? "initializer" :
                      isa<FuncDecl>(decl) ? "method" :
                      "member");

    return parentDecl->diagnose(
                       wasClaimed ? diag::implicit_member_declared_here
                                  : diag::note_typo_candidate_implicit_member,
                       decl->getBaseName().userFacingName(), kind);
  }

  if (wasClaimed) {
    return decl->diagnose(diag::decl_declared_here, decl->getBaseName());
  } else {
    return decl->diagnose(diag::note_typo_candidate,
                          decl->getBaseName().userFacingName());
  }
}

void TypoCorrectionResults::noteAllCandidates() const {
  for (auto candidate : Candidates) {
    auto &&diagnostic =
      noteTypoCorrection(Loc, candidate, ClaimedCorrection);

    // Don't add fix-its if we claimed the correction for the primary
    // diagnostic.
    if (!ClaimedCorrection) {
      SyntacticTypoCorrection correction(WrittenName, Loc,
                                         candidate->getName());
      correction.addFixits(diagnostic);
    }
  }
}

void SyntacticTypoCorrection::addFixits(InFlightDiagnostic &diagnostic) const {
  if (WrittenName.getBaseName() != CorrectedName.getBaseName())
    diagnostic.fixItReplace(Loc.getBaseNameLoc(),
                            CorrectedName.getBaseName().userFacingName());

  // TODO: add fix-its for typo'ed argument labels.  This is trickier
  // because of the reordering rules.
}

llvm::Optional<SyntacticTypoCorrection>
TypoCorrectionResults::claimUniqueCorrection() {
  // Look for a unique base name.  We ignore the rest of the name for now
  // because we don't actually typo-correct any of that.
  DeclBaseName uniqueCorrectedName;
  for (auto candidate : Candidates) {
    auto candidateName = candidate->getBaseName();

    // If this is the first name, record it.
    if (uniqueCorrectedName.empty())
      uniqueCorrectedName = candidateName;

    // If this is a different name from the last candidate, we don't have
    // a unique correction.
    else if (uniqueCorrectedName != candidateName)
      return llvm::None;
  }

  // If we didn't find any candidates, we're done.
  if (uniqueCorrectedName.empty())
    return llvm::None;

  // If the corrected name doesn't differ from the written name in its base
  // name, it's not simple enough for this (for now).
  if (WrittenName.getBaseName() == uniqueCorrectedName)
    return llvm::None;

  // Flag that we've claimed the correction.
  ClaimedCorrection = true;

  return SyntacticTypoCorrection(WrittenName, Loc, uniqueCorrectedName);
}
