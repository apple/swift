//===--- LifetimeDependence.cpp -----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/LifetimeDependence.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"

namespace swift {
std::string LifetimeDependenceInfo::getString() const {
  std::string lifetimeDependenceString;
  auto getOnIndices = [](IndexSubset *bitvector) {
    std::string result;
    bool isFirstSetBit = true;
    for (unsigned i = 0; i < bitvector->getCapacity(); i++) {
      if (bitvector->contains(i)) {
        if (!isFirstSetBit) {
          result += ", ";
        }
        result += std::to_string(i);
        isFirstSetBit = false;
      }
    }
    return result;
  };
  if (inheritLifetimeParamIndices && !inheritLifetimeParamIndices->isEmpty()) {
    lifetimeDependenceString =
        "_inherit(" + getOnIndices(inheritLifetimeParamIndices) + ") ";
  }
  if (scopeLifetimeParamIndices && !scopeLifetimeParamIndices->isEmpty()) {
    lifetimeDependenceString +=
        "_scope(" + getOnIndices(scopeLifetimeParamIndices) + ") ";
  }
  return lifetimeDependenceString;
}

void LifetimeDependenceInfo::Profile(llvm::FoldingSetNodeID &ID) const {
  if (inheritLifetimeParamIndices) {
    ID.AddInteger((uint8_t)LifetimeDependenceKind::Inherit);
    inheritLifetimeParamIndices->Profile(ID);
  }
  if (scopeLifetimeParamIndices) {
    ID.AddInteger((uint8_t)LifetimeDependenceKind::Scope);
    scopeLifetimeParamIndices->Profile(ID);
  }
}

static LifetimeDependenceKind
getLifetimeDependenceKindFromType(Type sourceType) {
  if (sourceType->isEscapable()) {
    return LifetimeDependenceKind::Scope;
  }
  return LifetimeDependenceKind::Inherit;
}

static ValueOwnership getLoweredOwnership(AbstractFunctionDecl *afd) {
  if (isa<ConstructorDecl>(afd)) {
    return ValueOwnership::Owned;
  }
  if (auto *ad = dyn_cast<AccessorDecl>(afd)) {
    if (ad->getAccessorKind() == AccessorKind::Set ||
        ad->getAccessorKind() == AccessorKind::Modify) {
      return ValueOwnership::InOut;
    }
  }
  return ValueOwnership::Shared;
}

static bool
isLifetimeDependenceCompatibleWithOwnership(LifetimeDependenceKind kind,
                                            ValueOwnership ownership,
                                            AbstractFunctionDecl *afd) {
  if (kind == LifetimeDependenceKind::Inherit) {
    return true;
  }
  assert(kind == LifetimeDependenceKind::Scope);
  auto loweredOwnership = ownership != ValueOwnership::Default
                              ? ownership
                              : getLoweredOwnership(afd);

  if (loweredOwnership == ValueOwnership::InOut ||
      loweredOwnership == ValueOwnership::Shared) {
    return true;
  }
  assert(loweredOwnership == ValueOwnership::Owned);
  return false;
}

LifetimeDependenceInfo LifetimeDependenceInfo::getForParamIndex(
    AbstractFunctionDecl *afd, unsigned index, LifetimeDependenceKind kind) {
  auto *dc = afd->getDeclContext();
  auto &ctx = dc->getASTContext();
  unsigned capacity = afd->hasImplicitSelfDecl()
                          ? (afd->getParameters()->size() + 1)
                          : afd->getParameters()->size();
  auto indexSubset = IndexSubset::get(ctx, capacity, {index});
  if (kind == LifetimeDependenceKind::Scope) {
    return LifetimeDependenceInfo{/*inheritLifetimeParamIndices*/ nullptr,
                                  /*scopeLifetimeParamIndices*/ indexSubset,
                                  /*isImmortal*/ false};
  }
  return LifetimeDependenceInfo{/*inheritLifetimeParamIndices*/ indexSubset,
                                /*scopeLifetimeParamIndices*/ nullptr,
                                /*isImmortal*/ false};
}

void LifetimeDependenceInfo::getConcatenatedData(
    SmallVectorImpl<bool> &concatenatedData) const {
  auto pushData = [&](IndexSubset *paramIndices) {
    if (paramIndices == nullptr) {
      return;
    }
    assert(!paramIndices->isEmpty());

    for (unsigned i = 0; i < paramIndices->getCapacity(); i++) {
      if (paramIndices->contains(i)) {
        concatenatedData.push_back(true);
        continue;
      }
      concatenatedData.push_back(false);
    }
  };
  if (hasInheritLifetimeParamIndices()) {
    pushData(inheritLifetimeParamIndices);
  }
  if (hasScopeLifetimeParamIndices()) {
    pushData(scopeLifetimeParamIndices);
  }
}

static bool hasEscapableResultOrYield(AbstractFunctionDecl *afd) {
  if (auto *accessor = dyn_cast<AccessorDecl>(afd)) {
    if (accessor->isCoroutine()) {
      auto yieldTyInContext = accessor->mapTypeIntoContext(
          accessor->getStorage()->getValueInterfaceType());
      return yieldTyInContext->isEscapable();
    }
  }

  Type resultType;
  if (auto fn = dyn_cast<FuncDecl>(afd)) {
    resultType = fn->getResultInterfaceType();
  } else {
    auto ctor = cast<ConstructorDecl>(afd);
    resultType = ctor->getResultInterfaceType();
  }
  return afd->mapTypeIntoContext(resultType)->isEscapable();
}

static LifetimeDependenceKind getLifetimeDependenceKindFromDecl(
    ParsedLifetimeDependenceKind parsedLifetimeDependenceKind, Type paramType) {
  if (parsedLifetimeDependenceKind == ParsedLifetimeDependenceKind::Scope) {
    return LifetimeDependenceKind::Scope;
  }
  if (parsedLifetimeDependenceKind == ParsedLifetimeDependenceKind::Inherit) {
    // TODO: assert that this can happen in SIL tests
    return LifetimeDependenceKind::Inherit;
  }
  return paramType->isEscapable() ? LifetimeDependenceKind::Scope
                                  : LifetimeDependenceKind::Inherit;
}

static bool isBitwiseCopyable(Type type, ModuleDecl *mod, ASTContext &ctx) {
  auto *bitwiseCopyableProtocol =
      ctx.getProtocol(KnownProtocolKind::BitwiseCopyable);
  if (!bitwiseCopyableProtocol) {
    return false;
  }
  return (bool)(mod->checkConformance(type, bitwiseCopyableProtocol));
}

std::optional<LifetimeDependenceInfo>
LifetimeDependenceInfo::fromTypeRepr(AbstractFunctionDecl *afd) {
  auto *dc = afd->getDeclContext();
  auto &ctx = dc->getASTContext();
  auto *mod = afd->getModuleContext();
  auto &diags = ctx.Diags;
  auto capacity = afd->hasImplicitSelfDecl()
                      ? (afd->getParameters()->size() + 1)
                      : afd->getParameters()->size();
  auto lifetimeDependentRepr =
      cast<LifetimeDependentReturnTypeRepr>(afd->getResultTypeRepr());

  if (hasEscapableResultOrYield(afd)) {
    diags.diagnose(lifetimeDependentRepr->getLoc(),
                   diag::lifetime_dependence_invalid_return_type);
    return std::nullopt;
  }

  SmallBitVector inheritLifetimeParamIndices(capacity);
  SmallBitVector scopeLifetimeParamIndices(capacity);

  auto updateLifetimeDependenceInfo = [&](LifetimeDependenceSpecifier specifier,
                                          unsigned paramIndexToSet,
                                          Type paramType,
                                          ValueOwnership ownership) {
    auto loc = specifier.getLoc();
    auto parsedLifetimeKind = specifier.getParsedLifetimeDependenceKind();
    auto lifetimeKind =
        getLifetimeDependenceKindFromDecl(parsedLifetimeKind, paramType);
    bool isCompatible = true;
    // Lifetime dependence always propagates through temporary BitwiseCopyable
    // values, even if the dependence is scoped.
    if (!isBitwiseCopyable(paramType, mod, ctx)) {
      isCompatible = isLifetimeDependenceCompatibleWithOwnership(
        lifetimeKind, ownership, afd);
    }
    if (parsedLifetimeKind == ParsedLifetimeDependenceKind::Scope &&
        !isCompatible) {
      diags.diagnose(
          loc, diag::lifetime_dependence_cannot_use_parsed_scoped_consuming);
      return true;
    }
    if (!isCompatible) {
      assert(lifetimeKind == LifetimeDependenceKind::Scope);
      diags.diagnose(
          loc, diag::lifetime_dependence_cannot_use_inferred_scoped_consuming);
      return true;
    }

    if (inheritLifetimeParamIndices.test(paramIndexToSet) ||
        scopeLifetimeParamIndices.test(paramIndexToSet)) {
      diags.diagnose(loc, diag::lifetime_dependence_duplicate_param_id);
      return true;
    }
    if (lifetimeKind == LifetimeDependenceKind::Inherit) {
      inheritLifetimeParamIndices.set(paramIndexToSet);
    } else {
      assert(lifetimeKind == LifetimeDependenceKind::Scope);
      scopeLifetimeParamIndices.set(paramIndexToSet);
    }
    return false;
  };

  for (auto specifier : lifetimeDependentRepr->getLifetimeDependencies()) {
    switch (specifier.getSpecifierKind()) {
    case LifetimeDependenceSpecifier::SpecifierKind::Immortal: {
      auto immortalParam =
          std::find_if(afd->getParameters()->begin(), afd->getParameters()->end(), [](ParamDecl *param) {
            return strcmp(param->getName().get(), "immortal") == 0;
          });
      if (immortalParam != afd->getParameters()->end()) {
        diags.diagnose(*immortalParam,
                       diag::lifetime_dependence_immortal_conflict_name);
      }

      return LifetimeDependenceInfo(nullptr, nullptr, /*isImmortal*/ true);
    }
    case LifetimeDependenceSpecifier::SpecifierKind::Named: {
      bool foundParamName = false;
      unsigned paramIndex = 0;
      for (auto *param : *afd->getParameters()) {
        if (param->getParameterName() == specifier.getName()) {
          if (updateLifetimeDependenceInfo(
                  specifier, paramIndex,
                  afd->mapTypeIntoContext(
                      param->toFunctionParam().getParameterType()),
                  param->getValueOwnership())) {
            return std::nullopt;
          }
          foundParamName = true;
          break;
        }
        paramIndex++;
      }
      if (!foundParamName) {
        diags.diagnose(specifier.getLoc(),
                       diag::lifetime_dependence_invalid_param_name,
                       specifier.getName());
        return std::nullopt;
      }
      break;
    }
    case LifetimeDependenceSpecifier::SpecifierKind::Ordered: {
      auto index = specifier.getIndex();
      if (index >= afd->getParameters()->size()) {
        diags.diagnose(specifier.getLoc(),
                       diag::lifetime_dependence_invalid_param_index, index);
        return std::nullopt;
      }
      auto param = afd->getParameters()->get(index);
      auto ownership = param->getValueOwnership();
      auto type =
          afd->mapTypeIntoContext(param->toFunctionParam().getParameterType());
      if (updateLifetimeDependenceInfo(specifier, index, type, ownership)) {
        return std::nullopt;
      }
      break;
    }
    case LifetimeDependenceSpecifier::SpecifierKind::Self: {
      if (!afd->hasImplicitSelfDecl()) {
        diags.diagnose(specifier.getLoc(),
                       diag::lifetime_dependence_invalid_self_in_static);
        return std::nullopt;
      }
      if (isa<ConstructorDecl>(afd)) {
        diags.diagnose(specifier.getLoc(),
                       diag::lifetime_dependence_invalid_self_in_init);
        return std::nullopt;
      }
      if (updateLifetimeDependenceInfo(
              specifier, /* selfIndex */ afd->getParameters()->size(),
              afd->getImplicitSelfDecl()->getTypeInContext(),
              afd->getImplicitSelfDecl()->getValueOwnership())) {
        return std::nullopt;
      }
      break;
    }
    }
  }

  return LifetimeDependenceInfo(
      inheritLifetimeParamIndices.any()
          ? IndexSubset::get(ctx, inheritLifetimeParamIndices)
          : nullptr,
      scopeLifetimeParamIndices.any()
          ? IndexSubset::get(ctx, scopeLifetimeParamIndices)
          : nullptr,
      /*isImmortal*/ false);
}

// This utility is similar to its overloaded version that builds the
// LifetimeDependenceInfo from the swift decl. Reason for duplicated code is the
// apis on type and ownership is different in SIL compared to Sema.
std::optional<LifetimeDependenceInfo> LifetimeDependenceInfo::fromTypeRepr(
    LifetimeDependentReturnTypeRepr *lifetimeDependentRepr,
    SmallVectorImpl<SILParameterInfo> &params, DeclContext *dc) {
  auto &ctx = dc->getASTContext();
  auto &diags = ctx.Diags;
  auto capacity = params.size(); // SIL parameters include self

  SmallBitVector inheritLifetimeParamIndices(capacity);
  SmallBitVector scopeLifetimeParamIndices(capacity);

  auto updateLifetimeDependenceInfo = [&](LifetimeDependenceSpecifier specifier,
                                          unsigned paramIndexToSet,
                                          ParameterConvention paramConvention) {
    auto loc = specifier.getLoc();
    auto kind = specifier.getParsedLifetimeDependenceKind();

    if (kind == ParsedLifetimeDependenceKind::Scope &&
        (!isGuaranteedParameterInCallee(paramConvention) &&
         !isMutatingParameter(paramConvention))) {
      diags.diagnose(loc, diag::lifetime_dependence_cannot_use_kind, "_scope",
                     getStringForParameterConvention(paramConvention));
      return true;
    }

    if (inheritLifetimeParamIndices.test(paramIndexToSet) ||
        scopeLifetimeParamIndices.test(paramIndexToSet)) {
      diags.diagnose(loc, diag::lifetime_dependence_duplicate_param_id);
      return true;
    }
    if (kind == ParsedLifetimeDependenceKind::Inherit) {
      inheritLifetimeParamIndices.set(paramIndexToSet);
    } else {
      assert(kind == ParsedLifetimeDependenceKind::Scope);
      scopeLifetimeParamIndices.set(paramIndexToSet);
    }
    return false;
  };

  for (auto specifier : lifetimeDependentRepr->getLifetimeDependencies()) {
    switch (specifier.getSpecifierKind()) {
    case LifetimeDependenceSpecifier::SpecifierKind::Ordered: {
      auto index = specifier.getIndex();
      if (index > capacity) {
        diags.diagnose(specifier.getLoc(),
                       diag::lifetime_dependence_invalid_param_index, index);
        return std::nullopt;
      }
      auto param = params[index];
      auto paramConvention = param.getConvention();
      if (updateLifetimeDependenceInfo(specifier, index, paramConvention)) {
        return std::nullopt;
      }
      break;
    }
    case LifetimeDependenceSpecifier::SpecifierKind::Immortal: {
      return LifetimeDependenceInfo(/*inheritLifetimeParamIndices*/ nullptr,
                                    /*scopeLifetimeParamIndices*/ nullptr,
                                    /*isImmortal*/ true);
    }
    default:
      llvm_unreachable("SIL can only have ordered or immortal lifetime "
                       "dependence specifier kind");
    }
  }

  return LifetimeDependenceInfo(
      inheritLifetimeParamIndices.any()
          ? IndexSubset::get(ctx, inheritLifetimeParamIndices)
          : nullptr,
      scopeLifetimeParamIndices.any()
          ? IndexSubset::get(ctx, scopeLifetimeParamIndices)
          : nullptr,
      /*isImmortal*/ false);
}

std::optional<LifetimeDependenceInfo>
LifetimeDependenceInfo::infer(AbstractFunctionDecl *afd) {
  auto *dc = afd->getDeclContext();
  auto &ctx = dc->getASTContext();
  auto *mod = afd->getModuleContext();

  if (!ctx.LangOpts.hasFeature(Feature::NonescapableTypes)) {
    return std::nullopt;
  }

  // Disable inference if requested.
  if (!ctx.LangOpts.EnableExperimentalLifetimeDependenceInference) {
    return std::nullopt;
  }

  if (hasEscapableResultOrYield(afd)) {
    return std::nullopt;
  }

  if (afd->getAttrs().hasAttribute<UnsafeNonEscapableResultAttr>()) {
    return std::nullopt;
  }

  auto &diags = ctx.Diags;
  auto returnTypeRepr = afd->getResultTypeRepr();
  auto returnLoc = returnTypeRepr ? returnTypeRepr->getLoc()
                                  : afd->getLoc(/* SerializedOK */ false);

  auto *cd = dyn_cast<ConstructorDecl>(afd);
  if (cd && cd->isImplicit()) {
    if (cd->getParameters()->size() == 0) {
      return std::nullopt;
    }
  }

  if (!cd && afd->hasImplicitSelfDecl()) {
    Type selfTypeInContext = dc->getSelfTypeInContext();
    if (selfTypeInContext->isEscapable()) {
      if (isBitwiseCopyable(selfTypeInContext, mod, ctx)) {
          diags.diagnose(
              returnLoc,
              diag::lifetime_dependence_method_escapable_bitwisecopyable_self);
          return std::nullopt;
      }
    }
    auto kind = getLifetimeDependenceKindFromType(selfTypeInContext);
    auto selfOwnership = afd->getImplicitSelfDecl()->getValueOwnership();
    if (!isLifetimeDependenceCompatibleWithOwnership(kind, selfOwnership,
                                                     afd)) {
      diags.diagnose(returnLoc,
                     diag::lifetime_dependence_invalid_self_ownership);
      return std::nullopt;
    }
    return LifetimeDependenceInfo::getForParamIndex(
        afd, /*selfIndex*/ afd->getParameters()->size(), kind);
  }

  LifetimeDependenceInfo lifetimeDependenceInfo;
  ParamDecl *candidateParam = nullptr;
  unsigned paramIndex = 0;
  bool hasParamError = false;
  for (auto *param : *afd->getParameters()) {
    SWIFT_DEFER { paramIndex++; };
    Type paramTypeInContext =
        afd->mapTypeIntoContext(param->getInterfaceType());
    if (paramTypeInContext->hasError()) {
      hasParamError = true;
      continue;
    }
    auto paramOwnership = param->getValueOwnership();
    if (paramTypeInContext->isEscapable()) {
      if (isBitwiseCopyable(paramTypeInContext, mod, ctx)) {
        continue;
      }
      if (paramOwnership == ValueOwnership::Default) {
        continue;
      }
    }

    auto lifetimeKind = getLifetimeDependenceKindFromType(paramTypeInContext);
    if (!isLifetimeDependenceCompatibleWithOwnership(lifetimeKind,
                                                     paramOwnership, afd)) {
      continue;
    }
    if (candidateParam) {
      if (cd && afd->isImplicit()) {
        diags.diagnose(
            returnLoc,
            diag::lifetime_dependence_cannot_infer_ambiguous_candidate,
            "on implicit initializer");
        return std::nullopt;
      }
      diags.diagnose(returnLoc,
                     diag::lifetime_dependence_cannot_infer_ambiguous_candidate,
                     "");
      return std::nullopt;
    }
    candidateParam = param;
    lifetimeDependenceInfo =
        LifetimeDependenceInfo::getForParamIndex(afd, paramIndex, lifetimeKind);
  }

  if (!candidateParam && !hasParamError) {
    if (cd && afd->isImplicit()) {
      diags.diagnose(returnLoc,
                     diag::lifetime_dependence_cannot_infer_no_candidates,
                     " on implicit initializer");
      return std::nullopt;
    }
    diags.diagnose(returnLoc,
                   diag::lifetime_dependence_cannot_infer_no_candidates, "");
    return std::nullopt;
  }
  return lifetimeDependenceInfo;
}

std::optional<LifetimeDependenceInfo>
LifetimeDependenceInfo::get(AbstractFunctionDecl *afd) {
  assert(isa<FuncDecl>(afd) || isa<ConstructorDecl>(afd));
  auto *returnTypeRepr = afd->getResultTypeRepr();
  if (isa_and_nonnull<LifetimeDependentReturnTypeRepr>(returnTypeRepr)) {
    return LifetimeDependenceInfo::fromTypeRepr(afd);
  }
  return LifetimeDependenceInfo::infer(afd);
}

std::optional<LifetimeDependenceKind>
LifetimeDependenceInfo::getLifetimeDependenceOnParam(unsigned paramIndex) {
  if (inheritLifetimeParamIndices) {
    if (inheritLifetimeParamIndices->contains(paramIndex)) {
      return LifetimeDependenceKind::Inherit;
    }
  }
  if (scopeLifetimeParamIndices) {
    if (scopeLifetimeParamIndices->contains(paramIndex)) {
      return LifetimeDependenceKind::Scope;
    }
  }
  return {};
}

} // namespace swift
