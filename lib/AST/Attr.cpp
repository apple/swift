//===--- Attr.cpp - Swift Language Attr ASTs ------------------------------===//
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
//  This file implements routines relating to declaration attributes.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Attr.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IndexSubset.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/QuotedString.h"
#include "swift/Strings.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

#define DECL_ATTR(_, Id, ...) \
  static_assert(IsTriviallyDestructible<Id##Attr>::value, \
                "Attrs are BumpPtrAllocated; the destructor is never called");
#include "swift/AST/Attr.def"
static_assert(IsTriviallyDestructible<DeclAttributes>::value,
              "DeclAttributes are BumpPtrAllocated; the d'tor is never called");
static_assert(IsTriviallyDestructible<TypeAttributes>::value,
              "TypeAttributes are BumpPtrAllocated; the d'tor is never called");

#define DECL_ATTR(Name, Id, ...)                                                                     \
static_assert(DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::ABIBreakingToAdd) != \
              DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::ABIStableToAdd),     \
              #Name " needs to specify either ABIBreakingToAdd or ABIStableToAdd");
#include "swift/AST/Attr.def"

#define DECL_ATTR(Name, Id, ...)                                                                        \
static_assert(DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::ABIBreakingToRemove) != \
              DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::ABIStableToRemove),     \
              #Name " needs to specify either ABIBreakingToRemove or ABIStableToRemove");
#include "swift/AST/Attr.def"

#define DECL_ATTR(Name, Id, ...)                                                                     \
static_assert(DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::APIBreakingToAdd) != \
              DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::APIStableToAdd),     \
              #Name " needs to specify either APIBreakingToAdd or APIStableToAdd");
#include "swift/AST/Attr.def"

#define DECL_ATTR(Name, Id, ...)                                                                        \
static_assert(DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::APIBreakingToRemove) != \
              DeclAttribute::isOptionSetFor##Id(DeclAttribute::DeclAttrOptions::APIStableToRemove),     \
              #Name " needs to specify either APIBreakingToRemove or APIStableToRemove");
#include "swift/AST/Attr.def"

StringRef swift::getAccessLevelSpelling(AccessLevel value) {
  switch (value) {
  case AccessLevel::Private: return "private";
  case AccessLevel::FilePrivate: return "fileprivate";
  case AccessLevel::Internal: return "internal";
  case AccessLevel::Package: return "package";
  case AccessLevel::Public: return "public";
  case AccessLevel::Open: return "open";
  }

  llvm_unreachable("Unhandled AccessLevel in switch.");
}

void TypeAttributes::getConventionArguments(SmallVectorImpl<char> &buf) const {
  llvm::raw_svector_ostream stream(buf);
  auto &convention = ConventionArguments.value();
  stream << convention.Name;
  if (convention.WitnessMethodProtocol) {
    stream << ": " << convention.WitnessMethodProtocol;
    return;
  }
  if (!convention.ClangType.Item.empty())
    stream << ", cType: " << QuotedString(convention.ClangType.Item);
}

/// Given a name like "autoclosure", return the type attribute ID that
/// corresponds to it.  This returns TAK_Count on failure.
///
TypeAttrKind TypeAttributes::getAttrKindFromString(StringRef Str) {
  return llvm::StringSwitch<TypeAttrKind>(Str)
#define TYPE_ATTR(X) .Case(#X, TAK_##X)
#include "swift/AST/Attr.def"
  .Default(TAK_Count);
}

/// Return the name (like "autoclosure") for an attribute ID.
const char *TypeAttributes::getAttrName(TypeAttrKind kind) {
  switch (kind) {
  default: llvm_unreachable("Invalid attribute ID");
#define TYPE_ATTR(X) case TAK_##X: return #X;
#include "swift/AST/Attr.def"
  }
}



/// Given a name like "inline", return the decl attribute ID that corresponds
/// to it.  Note that this is a many-to-one mapping, and that the identifier
/// passed in may only be the first portion of the attribute (e.g. in the case
/// of the 'unowned(unsafe)' attribute, the string passed in is 'unowned'.
///
/// Also note that this recognizes both attributes like '@inline' (with no @)
/// and decl modifiers like 'final'.  This returns DAK_Count on failure.
///
DeclAttrKind DeclAttribute::getAttrKindFromString(StringRef Str) {
  return llvm::StringSwitch<DeclAttrKind>(Str)
#define DECL_ATTR(X, CLASS, ...) .Case(#X, DAK_##CLASS)
#define DECL_ATTR_ALIAS(X, CLASS) .Case(#X, DAK_##CLASS)
#include "swift/AST/Attr.def"
  .Case(SPI_AVAILABLE_ATTRNAME, DAK_Available)
  .Default(DAK_Count);
}

/// Returns true if this attribute can appear on the specified decl.
bool DeclAttribute::canAttributeAppearOnDecl(DeclAttrKind DK, const Decl *D) {
  if ((getOptions(DK) & OnAnyClangDecl) && D->hasClangNode())
    return true;
  return canAttributeAppearOnDeclKind(DK, D->getKind());
}

bool DeclAttribute::canAttributeAppearOnDeclKind(DeclAttrKind DAK, DeclKind DK) {
  auto Options = getOptions(DAK);
  switch (DK) {
#define DECL(Id, Parent) case DeclKind::Id: return (Options & On##Id) != 0;
#include "swift/AST/DeclNodes.def"
  }
  llvm_unreachable("bad DeclKind");
}

bool
DeclAttributes::isUnavailableInSwiftVersion(
  const version::Version &effectiveVersion) const {
  llvm::VersionTuple vers = effectiveVersion;
  for (auto attr : *this) {
    if (auto available = dyn_cast<AvailableAttr>(attr)) {
      if (available->isInvalid())
        continue;

      if (available->getPlatformAgnosticAvailability() ==
          PlatformAgnosticAvailabilityKind::SwiftVersionSpecific) {
        if (available->Introduced.has_value() &&
            available->Introduced.value() > vers)
          return true;
        if (available->Obsoleted.has_value() &&
            available->Obsoleted.value() <= vers)
          return true;
      }
    }
  }

  return false;
}

const AvailableAttr *
DeclAttributes::findMostSpecificActivePlatform(const ASTContext &ctx) const{
  const AvailableAttr *bestAttr = nullptr;

  for (auto attr : *this) {
    auto *avAttr = dyn_cast<AvailableAttr>(attr);
    if (!avAttr)
      continue;

    if (avAttr->isInvalid())
      continue;

    if (!avAttr->hasPlatform())
      continue;

    if (!avAttr->isActivePlatform(ctx))
      continue;

    // We have an attribute that is active for the platform, but
    // is it more specific than our current best?
    if (!bestAttr || inheritsAvailabilityFromPlatform(avAttr->Platform,
                                                      bestAttr->Platform)) {
      bestAttr = avAttr;
    }
  }

  return bestAttr;
}

const AvailableAttr *
DeclAttributes::getPotentiallyUnavailable(const ASTContext &ctx) const {
  const AvailableAttr *potential = nullptr;
  const AvailableAttr *conditional = nullptr;

  for (auto Attr : *this)
    if (auto AvAttr = dyn_cast<AvailableAttr>(Attr)) {
      if (AvAttr->isInvalid())
        continue;

      if (!AvAttr->isActivePlatform(ctx) &&
          !AvAttr->isLanguageVersionSpecific() &&
          !AvAttr->isPackageDescriptionVersionSpecific())
        continue;

      // Definitely not available.
      if (AvAttr->isUnconditionallyUnavailable())
        return AvAttr;

      switch (AvAttr->getVersionAvailability(ctx)) {
      case AvailableVersionComparison::Available:
        // Doesn't limit the introduced version.
        break;

      case AvailableVersionComparison::PotentiallyUnavailable:
        // We'll return this if we don't see something that proves it's
        // not available in this version.
        potential = AvAttr;
        break;

      case AvailableVersionComparison::Unavailable:
      case AvailableVersionComparison::Obsoleted:
        conditional = AvAttr;
        break;
      }
    }

  if (conditional)
    return conditional;
  return potential;
}

const AvailableAttr *DeclAttributes::getUnavailable(
                          const ASTContext &ctx) const {
  const AvailableAttr *conditional = nullptr;
  const AvailableAttr *bestActive = findMostSpecificActivePlatform(ctx);

  for (auto Attr : *this)
    if (auto AvAttr = dyn_cast<AvailableAttr>(Attr)) {
      if (AvAttr->isInvalid())
        continue;

      // If this is a platform-specific attribute and it isn't the most
      // specific attribute for the current platform, we're done.
      if (AvAttr->hasPlatform() &&
          (!bestActive || AvAttr != bestActive))
        continue;

      // If this attribute doesn't apply to the active platform, we're done.
      if (!AvAttr->isActivePlatform(ctx) &&
          !AvAttr->isLanguageVersionSpecific() &&
          !AvAttr->isPackageDescriptionVersionSpecific())
        continue;

      // Unconditional unavailable.
      if (AvAttr->isUnconditionallyUnavailable())
        return AvAttr;

      switch (AvAttr->getVersionAvailability(ctx)) {
      case AvailableVersionComparison::Available:
      case AvailableVersionComparison::PotentiallyUnavailable:
        break;

      case AvailableVersionComparison::Obsoleted:
      case AvailableVersionComparison::Unavailable:
        conditional = AvAttr;
        break;
      }
    }
  return conditional;
}

const AvailableAttr *
DeclAttributes::getDeprecated(const ASTContext &ctx) const {
  const AvailableAttr *conditional = nullptr;
  const AvailableAttr *bestActive = findMostSpecificActivePlatform(ctx);
  for (auto Attr : *this) {
    if (auto AvAttr = dyn_cast<AvailableAttr>(Attr)) {
      if (AvAttr->isInvalid())
        continue;

      if (AvAttr->hasPlatform() &&
          (!bestActive || AvAttr != bestActive))
        continue;

      if (!AvAttr->isActivePlatform(ctx) &&
          !AvAttr->isLanguageVersionSpecific() &&
          !AvAttr->isPackageDescriptionVersionSpecific())
        continue;

      // Unconditional deprecated.
      if (AvAttr->isUnconditionallyDeprecated())
        return AvAttr;

      Optional<llvm::VersionTuple> DeprecatedVersion = AvAttr->Deprecated;
      if (!DeprecatedVersion.has_value())
        continue;

      llvm::VersionTuple MinVersion = AvAttr->getActiveVersion(ctx);

      // We treat the declaration as deprecated if it is deprecated on
      // all deployment targets.
      // Once availability checking is enabled by default, we should
      // query the type refinement context hierarchy to determine
      // whether a declaration is deprecated on all versions
      // allowed by the context containing the reference.
      if (DeprecatedVersion.value() <= MinVersion) {
        conditional = AvAttr;
      }
    }
  }
  return conditional;
}

const AvailableAttr *
DeclAttributes::getSoftDeprecated(const ASTContext &ctx) const {
  const AvailableAttr *conditional = nullptr;
  const AvailableAttr *bestActive = findMostSpecificActivePlatform(ctx);
  for (auto Attr : *this) {
    if (auto AvAttr = dyn_cast<AvailableAttr>(Attr)) {
      if (AvAttr->isInvalid())
        continue;

      if (AvAttr->hasPlatform() &&
          (!bestActive || AvAttr != bestActive))
        continue;

      if (!AvAttr->isActivePlatform(ctx) &&
          !AvAttr->isLanguageVersionSpecific() &&
          !AvAttr->isPackageDescriptionVersionSpecific())
        continue;

      Optional<llvm::VersionTuple> DeprecatedVersion = AvAttr->Deprecated;
      if (!DeprecatedVersion.has_value())
        continue;

      llvm::VersionTuple ActiveVersion = AvAttr->getActiveVersion(ctx);

      if (DeprecatedVersion.value() > ActiveVersion) {
        conditional = AvAttr;
      }
    }
  }
  return conditional;
}

const AvailableAttr *DeclAttributes::getNoAsync(const ASTContext &ctx) const {
  const AvailableAttr *bestAttr = nullptr;
  for (const DeclAttribute *attr : *this) {
    if (const AvailableAttr *avAttr = dyn_cast<AvailableAttr>(attr)) {
      if (avAttr->isInvalid())
        continue;

      if (avAttr->getPlatformAgnosticAvailability() ==
          PlatformAgnosticAvailabilityKind::NoAsync) {
        // An API may only be unavailable on specific platforms.
        // If it doesn't have a platform associated with it, then it's
        // unavailable for all platforms, so we should include it. If it does
        // have a platform and we are not that platform, then it doesn't apply
        // to us.
        const bool isGoodForPlatform =
            (avAttr->hasPlatform() && avAttr->isActivePlatform(ctx)) ||
            !avAttr->hasPlatform();

        if (!isGoodForPlatform)
          continue;

        if (!bestAttr) {
          // If there is no best attr selected
          // and the attr either has an active platform, or doesn't have one at
          // all, select it.
          bestAttr = avAttr;
        } else if (bestAttr && avAttr->hasPlatform() &&
                   bestAttr->hasPlatform() &&
                   inheritsAvailabilityFromPlatform(avAttr->Platform,
                                                    bestAttr->Platform)) {
          // if they both have a viable platform, use the better one
          bestAttr = avAttr;
        } else if (avAttr->hasPlatform() && !bestAttr->hasPlatform()) {
          // Use the one more specific
          bestAttr = avAttr;
        }
      }
    }
  }
  return bestAttr;
}

const BackDeployedAttr *
DeclAttributes::getBackDeployed(const ASTContext &ctx) const {
  const BackDeployedAttr *bestAttr = nullptr;

  for (auto attr : *this) {
    auto *backDeployedAttr = dyn_cast<BackDeployedAttr>(attr);
    if (!backDeployedAttr)
      continue;

    if (backDeployedAttr->isInvalid() ||
        !backDeployedAttr->isActivePlatform(ctx))
      continue;

    // We have an attribute that is active for the platform, but
    // is it more specific than our current best?
    if (!bestAttr || inheritsAvailabilityFromPlatform(
                         backDeployedAttr->Platform, bestAttr->Platform)) {
      bestAttr = backDeployedAttr;
    }
  }

  return bestAttr;
}

void DeclAttributes::dump(const Decl *D) const {
  StreamPrinter P(llvm::errs());
  PrintOptions PO = PrintOptions::printDeclarations();
  print(P, PO, D);
}

/// Returns true if the attribute can be presented as a short form available
/// attribute (e.g., as @available(iOS 8.0, *). The presentation requires an
/// introduction version and does not support deprecation, obsoletion, or
/// messages.
LLVM_READONLY
static bool isShortAvailable(const DeclAttribute *DA) {
  auto *AvailAttr = dyn_cast<AvailableAttr>(DA);
  if (!AvailAttr)
    return false;

  if (AvailAttr->IsSPI)
    return false;

  if (!AvailAttr->Introduced.has_value())
    return false;

  if (AvailAttr->Deprecated.has_value())
    return false;

  if (AvailAttr->Obsoleted.has_value())
    return false;

  if (!AvailAttr->Message.empty())
    return false;

  if (!AvailAttr->Rename.empty())
    return false;

  switch (AvailAttr->PlatformAgnostic) {
  case PlatformAgnosticAvailabilityKind::Deprecated:
  case PlatformAgnosticAvailabilityKind::Unavailable:
  case PlatformAgnosticAvailabilityKind::UnavailableInSwift:
  case PlatformAgnosticAvailabilityKind::NoAsync:
    return false;
  case PlatformAgnosticAvailabilityKind::None:
  case PlatformAgnosticAvailabilityKind::SwiftVersionSpecific:
  case PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
    return true;
  }

  return true;
}

/// Return true when another availability attribute implies the same availability as this
/// attribute and so printing the attribute can be skipped to de-clutter the declaration
/// when printing the short form.
/// For example, iOS availability implies macCatalyst availability so if attributes for
/// both are present and they have the same 'introduced' version, we can skip printing an
/// explicit availability for macCatalyst.
static bool isShortFormAvailabilityImpliedByOther(const AvailableAttr *Attr,
    ArrayRef<const DeclAttribute *> Others) {
  assert(isShortAvailable(Attr));

  for (auto *DA : Others) {
    auto *Other = cast<AvailableAttr>(DA);
    if (Attr->Platform == Other->Platform)
      continue;

    if (!inheritsAvailabilityFromPlatform(Attr->Platform, Other->Platform))
      continue;

    if (Attr->Introduced == Other->Introduced)
      return true;
  }
  return false;
}

/// Print the short-form @available() attribute for an array of long-form
/// AvailableAttrs that can be represented in the short form.
/// For example, for:
///   @available(OSX, introduced: 10.10)
///   @available(iOS, introduced: 8.0)
/// this will print:
///   @available(OSX 10.10, iOS 8.0, *)
static void printShortFormAvailable(ArrayRef<const DeclAttribute *> Attrs,
                                    ASTPrinter &Printer,
                                    const PrintOptions &Options,
                                    bool forAtSpecialize = false) {
  assert(!Attrs.empty());
  if (!forAtSpecialize)
    Printer << "@available(";
  auto FirstAvail = cast<AvailableAttr>(Attrs.front());
  if (Attrs.size() == 1 &&
      FirstAvail->getPlatformAgnosticAvailability() !=
      PlatformAgnosticAvailabilityKind::None) {
    assert(FirstAvail->Introduced.has_value());
    if (FirstAvail->isLanguageVersionSpecific()) {
      Printer << "swift ";
    } else {
      assert(FirstAvail->isPackageDescriptionVersionSpecific());
      Printer << "_PackageDescription ";
    }
    Printer << FirstAvail->Introduced.value().getAsString();
    if (!forAtSpecialize)
      Printer << ")";
  } else {
    for (auto *DA : Attrs) {
      auto *AvailAttr = cast<AvailableAttr>(DA);
      assert(AvailAttr->Introduced.has_value());
      // Avoid omitting available attribute when we are printing module interface.
      if (!Options.IsForSwiftInterface &&
          isShortFormAvailabilityImpliedByOther(AvailAttr, Attrs))
        continue;
      Printer << platformString(AvailAttr->Platform) << " "
              << AvailAttr->Introduced.value().getAsString() << ", ";
    }
    Printer << "*";
    if (!forAtSpecialize)
      Printer << ")";
  }
  if (!forAtSpecialize)
    Printer.printNewline();
}

static void printShortFormBackDeployed(ArrayRef<const DeclAttribute *> Attrs,
                                       ASTPrinter &Printer,
                                       const PrintOptions &Options) {
  assert(!Attrs.empty());
  Printer << "@backDeployed(before: ";
  bool isFirst = true;

  for (auto *DA : Attrs) {
    if (!isFirst)
      Printer << ", ";
    auto *attr = cast<BackDeployedAttr>(DA);
    Printer << platformString(attr->Platform) << " "
            << attr->Version.getAsString();
    isFirst = false;
  }
  Printer << ")";
  Printer.printNewline();
}

/// The kind of a parameter in a `wrt:` differentiation parameters clause:
/// either a differentiability parameter or a linearity parameter. Used for
/// printing `@differentiable`, `@derivative`, and `@transpose` attributes.
enum class DifferentiationParameterKind {
  /// A differentiability parameter, printed by name.
  /// Used for `@differentiable` and `@derivative` attribute.
  Differentiability,
  /// A linearity parameter, printed by index.
  /// Used for `@transpose` attribute.
  Linearity
};

/// Returns the differentiation parameters clause string for the given function,
/// parameter indices, parsed parameters, and differentiation parameter kind.
/// Use the parameter indices if specified; otherwise, use the parsed
/// parameters.
static std::string getDifferentiationParametersClauseString(
    const AbstractFunctionDecl *function, IndexSubset *parameterIndices,
    ArrayRef<ParsedAutoDiffParameter> parsedParams,
    DifferentiationParameterKind parameterKind) {
  assert(function);
  bool isInstanceMethod = function->isInstanceMember();
  bool isStaticMethod = function->isStatic();
  std::string result;
  llvm::raw_string_ostream printer(result);

  // Use the parameter indices, if specified.
  if (parameterIndices) {
    auto parameters = parameterIndices->getBitVector();
    auto parameterCount = parameters.count();
    printer << "wrt: ";
    if (parameterCount > 1)
      printer << '(';
    // Check if differentiating wrt `self`. If so, manually print it first.
    bool isWrtSelf =
        (isInstanceMethod ||
         (isStaticMethod &&
          parameterKind == DifferentiationParameterKind::Linearity)) &&
        parameters.test(parameters.size() - 1);
    if (isWrtSelf) {
      parameters.reset(parameters.size() - 1);
      printer << "self";
      if (parameters.any())
        printer << ", ";
    }
    // Print remaining differentiation parameters.
    interleave(parameters.set_bits(), [&](unsigned index) {
      switch (parameterKind) {
      // Print differentiability parameters by name.
      case DifferentiationParameterKind::Differentiability:
        printer << function->getParameters()->get(index)->getName().str();
        break;
      // Print linearity parameters by index.
      case DifferentiationParameterKind::Linearity:
        printer << index;
        break;
      }
    }, [&] { printer << ", "; });
    if (parameterCount > 1)
      printer << ')';
  }
  // Otherwise, use the parsed parameters.
  else if (!parsedParams.empty()) {
    printer << "wrt: ";
    if (parsedParams.size() > 1)
      printer << '(';
    interleave(parsedParams, [&](const ParsedAutoDiffParameter &param) {
      switch (param.getKind()) {
      case ParsedAutoDiffParameter::Kind::Named:
        printer << param.getName();
        break;
      case ParsedAutoDiffParameter::Kind::Self:
        printer << "self";
        break;
      case ParsedAutoDiffParameter::Kind::Ordered:
        auto *paramList = function->getParameters();
        assert(param.getIndex() <= paramList->size() &&
               "wrt parameter is out of range");
        auto *funcParam = paramList->get(param.getIndex());
        printer << funcParam->getNameStr();
        break;
      }
    }, [&] { printer << ", "; });
    if (parsedParams.size() > 1)
      printer << ')';
  }
  return printer.str();
}

/// Print the arguments of the given `@differentiable` attribute.
/// - If `omitWrtClause` is true, omit printing the `wrt:` differentiation
///   parameters clause.
static void printDifferentiableAttrArguments(
    const DifferentiableAttr *attr, ASTPrinter &printer,
    const PrintOptions &Options, const Decl *D, bool omitWrtClause = false) {
  // Create a temporary string for the attribute argument text.
  std::string attrArgText;
  llvm::raw_string_ostream stream(attrArgText);

  // Print comma if not leading clause.
  bool isLeadingClause = false;
  auto printCommaIfNecessary = [&] {
    if (isLeadingClause) {
      isLeadingClause = false;
      return;
    }
    stream << ", ";
  };

  // Print if the function is marked as linear.
  switch (attr->getDifferentiabilityKind()) {
  case DifferentiabilityKind::Normal:
    isLeadingClause = true;
    break;
  case DifferentiabilityKind::Forward:
    stream << "_forward";
    break;
  case DifferentiabilityKind::Reverse:
    stream << "reverse";
    break;
  case DifferentiabilityKind::Linear:
    stream << "_linear";
    break;
  case DifferentiabilityKind::NonDifferentiable:
    llvm_unreachable("Impossible case `NonDifferentiable`");
  }

  // If the declaration is not available, there is not enough context to print
  // the differentiability parameters or the 'where' clause, so just print the
  // differentiability kind if applicable (when not `Normal`).
  if (!D) {
    if (attr->getDifferentiabilityKind() != DifferentiabilityKind::Normal) {
      printer << '(' << stream.str() << ')';
    }
    return;
  }

  // Get original function.
  auto *original = dyn_cast<AbstractFunctionDecl>(D);
  // Handle stored/computed properties and subscript methods.
  if (auto *asd = dyn_cast<AbstractStorageDecl>(D))
    original = asd->getAccessor(AccessorKind::Get);
  assert(original && "Must resolve original declaration");

  // Print differentiation parameters clause, unless it is to be omitted.
  if (!omitWrtClause) {
    auto diffParamsString = getDifferentiationParametersClauseString(
        original, attr->getParameterIndices(), attr->getParsedParameters(),
        DifferentiationParameterKind::Differentiability);
    // Check whether differentiation parameter clause is empty.
    // Handles edge case where resolved parameter indices are unset and
    // parsed parameters are empty. This case should never trigger for
    // user-visible printing.
    if (!diffParamsString.empty()) {
      printCommaIfNecessary();
      stream << diffParamsString;
    }
  }
  // Print 'where' clause, if any.
  // First, filter out requirements satisfied by the original function's
  // generic signature. They should not be printed.
  ArrayRef<Requirement> derivativeRequirements;
  if (auto derivativeGenSig = attr->getDerivativeGenericSignature())
    derivativeRequirements = derivativeGenSig.getRequirements();
  auto requirementsToPrint =
    llvm::make_filter_range(derivativeRequirements, [&](Requirement req) {
        if (const auto &originalGenSig = original->getGenericSignature())
          if (originalGenSig->isRequirementSatisfied(req))
            return false;
        return true;
      });
  if (!requirementsToPrint.empty()) {
    if (!isLeadingClause)
      stream << ' ';
    stream << "where ";
    interleave(requirementsToPrint, [&](Requirement req) {
      if (const auto &originalGenSig = original->getGenericSignature())
        if (originalGenSig->isRequirementSatisfied(req))
          return;
      req.print(stream, Options);
    }, [&] {
      stream << ", ";
    });
  }

  // If the attribute argument text is empty, return. Do not print parentheses.
  if (stream.str().empty())
    return;

  // Otherwise, print the attribute argument text enclosed in parentheses.
  printer << '(' << stream.str() << ')';
}

void DeclAttributes::print(ASTPrinter &Printer, const PrintOptions &Options,
                           const Decl *D) const {
  if (!DeclAttrs)
    return;

  SmallVector<const DeclAttribute *, 8> orderedAttributes(begin(), end());
  print(Printer, Options, orderedAttributes, D);
}

void DeclAttributes::print(ASTPrinter &Printer, const PrintOptions &Options,
                           ArrayRef<const DeclAttribute *> FlattenedAttrs,
                           const Decl *D) {
  using AttributeVector = SmallVector<const DeclAttribute *, 8>;

  // Process attributes in passes.
  AttributeVector shortAvailableAttributes;
  const DeclAttribute *swiftVersionAvailableAttribute = nullptr;
  const DeclAttribute *packageDescriptionVersionAvailableAttribute = nullptr;
  AttributeVector backDeployedAttributes;
  AttributeVector longAttributes;
  AttributeVector attributes;
  AttributeVector modifiers;

  for (auto DA : llvm::reverse(FlattenedAttrs)) {
    // Always print result builder attribute.
    if (!Options.PrintImplicitAttrs && DA->isImplicit())
      continue;
    if (!Options.PrintUserInaccessibleAttrs &&
        DeclAttribute::isUserInaccessible(DA->getKind()))
      continue;
    if (Options.excludeAttrKind(DA->getKind()))
      continue;

    // If this attribute is only allowed because this is a Clang decl, don't
    // print it.
    if (D && D->hasClangNode()
        && !DeclAttribute::canAttributeAppearOnDeclKind(
                               DA->getKind(), D->getKind()))
      continue;

    // Be careful not to coalesce `@available(swift 5)` with other short
    // `available' attributes.
    if (auto *availableAttr = dyn_cast<AvailableAttr>(DA)) {
      if (availableAttr->isLanguageVersionSpecific() &&
          isShortAvailable(availableAttr)) {
        swiftVersionAvailableAttribute = availableAttr;
        continue;
      }
      if (availableAttr->isPackageDescriptionVersionSpecific() &&
          isShortAvailable(availableAttr)) {
        packageDescriptionVersionAvailableAttribute = availableAttr;
        continue;
      }
    }

    AttributeVector &which = DA->isDeclModifier() ? modifiers :
                             isa<BackDeployedAttr>(DA) ? backDeployedAttributes :
                             isShortAvailable(DA) ? shortAvailableAttributes :
                             DA->isLongAttribute() ? longAttributes :
                             attributes;
    which.push_back(DA);
  }

  if (swiftVersionAvailableAttribute)
    printShortFormAvailable(swiftVersionAvailableAttribute, Printer, Options);
  if (packageDescriptionVersionAvailableAttribute)
    printShortFormAvailable(packageDescriptionVersionAvailableAttribute, Printer, Options);
  if (!shortAvailableAttributes.empty())
    printShortFormAvailable(shortAvailableAttributes, Printer, Options);
  if (!backDeployedAttributes.empty())
    printShortFormBackDeployed(backDeployedAttributes, Printer, Options);

  for (auto DA : longAttributes)
    DA->print(Printer, Options, D);
  for (auto DA : attributes)
    DA->print(Printer, Options, D);
  for (auto DA : modifiers)
    DA->print(Printer, Options, D);
}

SourceLoc DeclAttributes::getStartLoc(bool forModifiers) const {
  if (isEmpty())
    return SourceLoc();

  const DeclAttribute *lastAttr = nullptr;
  for (auto attr : *this) {
    if (attr->getRangeWithAt().Start.isValid() &&
        (!forModifiers || attr->isDeclModifier()))
      lastAttr = attr;
  }

  return lastAttr ? lastAttr->getRangeWithAt().Start : SourceLoc();
}

Optional<const DeclAttribute *>
OrigDeclAttrFilter::operator()(const DeclAttribute *Attr) const {
  auto declLoc = decl->getStartLoc();
  auto *mod = decl->getModuleContext();
  auto *declFile = mod->getSourceFileContainingLocation(declLoc);
  auto *attrFile = mod->getSourceFileContainingLocation(Attr->getLocation());
  if (!declFile || !attrFile)
    return Attr;

  // Only attributes in the same buffer as the declaration they're attached to
  // are part of the original attribute list.
  if (declFile->getBufferID() != attrFile->getBufferID())
    return None;

  return Attr;
}

static void printAvailableAttr(const AvailableAttr *Attr, ASTPrinter &Printer,
                               const PrintOptions &Options) {
  if (Attr->isLanguageVersionSpecific())
    Printer << "swift";
  else if (Attr->isPackageDescriptionVersionSpecific())
    Printer << "_PackageDescription";
  else
    Printer << Attr->platformString();

  if (Attr->isUnconditionallyUnavailable())
    Printer << ", unavailable";
  else if (Attr->isUnconditionallyDeprecated())
    Printer << ", deprecated";
  else if (Attr->isNoAsync())
    Printer << ", noasync";

  if (Attr->Introduced)
    Printer << ", introduced: " << Attr->Introduced.value().getAsString();
  if (Attr->Deprecated)
    Printer << ", deprecated: " << Attr->Deprecated.value().getAsString();
  if (Attr->Obsoleted)
    Printer << ", obsoleted: " << Attr->Obsoleted.value().getAsString();

  if (!Attr->Rename.empty()) {
    Printer << ", renamed: \"" << Attr->Rename << "\"";
  } else if (Attr->RenameDecl) {
    Printer << ", renamed: \"";
    if (auto *Accessor = dyn_cast<AccessorDecl>(Attr->RenameDecl)) {
      SmallString<32> Name;
      llvm::raw_svector_ostream OS(Name);
      Accessor->printUserFacingName(OS);
      Printer << Name.str();
    } else {
      Printer << Attr->RenameDecl->getName();
    }
    Printer << "\"";
  }

  // If there's no message, but this is specifically an imported
  // "unavailable in Swift" attribute, synthesize a message to look good in
  // the generated interface.
  if (!Attr->Message.empty()) {
    Printer << ", message: ";
    Printer.printEscapedStringLiteral(Attr->Message);
  } else if (Attr->getPlatformAgnosticAvailability() ==
             PlatformAgnosticAvailabilityKind::UnavailableInSwift)
    Printer << ", message: \"Not available in Swift\"";
}

bool DeclAttribute::printImpl(ASTPrinter &Printer, const PrintOptions &Options,
                              const Decl *D) const {

  // Handle any attributes that are not printed at all before we make printer
  // callbacks.
  switch (getKind()) {
  case DAK_ObjC:
    if (Options.PrintForSIL && isImplicit())
      return false;
    break;
  case DAK_RawDocComment:
  case DAK_ObjCBridged:
  case DAK_SynthesizedProtocol:
  case DAK_Rethrows:
  case DAK_Reasync:
  case DAK_Infix:
    return false;
  case DAK_Override: {
    if (!Options.IsForSwiftInterface)
      break;
    // When we are printing Swift interface, we have to skip the override keyword
    // if the overridden decl is invisible from the interface. Otherwise, an error
    // will occur while building the Swift module because the overriding decl
    // doesn't override anything.
    // We couldn't skip every `override` keywords because they change the
    // ABI if the overridden decl is also publicly visible.
    // For public-override-internal case, having `override` doesn't have ABI
    // implication. Thus we can skip them.
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (auto *BD = VD->getOverriddenDecl()) {
        // If the overridden decl won't be printed, printing override will fail
        // the build of the interface file.
        if (!Options.shouldPrint(BD))
          return false;
        if (!BD->hasClangNode() &&
            !BD->getFormalAccessScope(VD->getDeclContext(),
                                      /*treatUsableFromInlineAsPublic*/ true)
                 .isPublic()) {
          return false;
        }
      }
    }
    break;
  }
  case DAK_Custom: {

    auto attr = cast<CustomAttr>(this);
    if (auto type = attr->getType()) {
      // Print custom attributes only if the attribute decl is accessible.
      // FIXME: rdar://85477478 They should be rejected.
      if (auto attrDecl = type->getNominalOrBoundGenericNominal()) {
        if (attrDecl->getFormalAccess() < Options.AccessFilter) {
          return false;
        }
      }
    }

    if (!Options.IsForSwiftInterface)
      break;
    // For Swift interface, we should print result builder attributes
    // on parameter decls and on protocol requirements.
    // Printing the attribute elsewhere isn't ABI relevant.
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (VD->getAttachedResultBuilder() == this) {
        if (!isa<ParamDecl>(D) &&
            !((isa<VarDecl>(D) || isa<FuncDecl>(D)) &&
               isa<ProtocolDecl>(D->getDeclContext())))
          return false;
      }
    }
    break;
  }
  default:
    break;
  }

  // Handle any decl-modifiers.
  // FIXME: Ideally we would handle decl modifiers as a special kind of
  // attribute, but for now it's simpler to treat them as a keyword in the
  // printer.
  switch (getKind()) {
    // Handle all of the SIMPLE_DECL_ATTRs.
#define SIMPLE_DECL_ATTR(X, CLASS, ...) case DAK_##CLASS:
#include "swift/AST/Attr.def"
  case DAK_Inline:
  case DAK_AccessControl:
  case DAK_ReferenceOwnership:
  case DAK_Effects:
  case DAK_Optimize:
  case DAK_Exclusivity:
  case DAK_NonSendable:
  case DAK_ObjCImplementation:
    if (getKind() == DAK_Effects &&
        cast<EffectsAttr>(this)->getKind() == EffectsKind::Custom) {
      Printer.printAttrName("@_effects");
      Printer << "(" << cast<EffectsAttr>(this)->getCustomString() << ")";
    } else if (DeclAttribute::isDeclModifier(getKind())) {
      Printer.printKeyword(getAttrName(), Options);
    } else if (Options.IsForSwiftInterface && getKind() == DAK_ResultBuilder) {
      // Use @_functionBuilder in Swift interfaces to maintain backward
      // compatibility.
      Printer.printSimpleAttr("_functionBuilder", /*needAt=*/true);
    } else {
      Printer.printSimpleAttr(getAttrName(), /*needAt=*/true);
    }
    return true;

  case DAK_MainType: {
    // Don't print into SIL. Necessary bits have already been generated.
    if (Options.PrintForSIL)
      return false;
    Printer.printSimpleAttr(getAttrName(), /*needAt=*/true);
    return true;
  }

  case DAK_SetterAccess:
    Printer.printKeyword(getAttrName(), Options, "(set)");
    return true;

  case DAK_SPIAccessControl: {
    if (!Options.PrintSPIs) return false;

    auto spiAttr = static_cast<const SPIAccessControlAttr*>(this);
    interleave(spiAttr->getSPIGroups(),
               [&](Identifier spiName) {
                 Printer.printAttrName(getAttrName(), true);
                 Printer << "(" << spiName << ")";
               },
               [&] { Printer << " "; });
    return true;
  }

  default:
    break;
  }

  Printer.callPrintStructurePre(PrintStructureKind::BuiltinAttribute);
  SWIFT_DEFER {
    Printer.printStructurePost(PrintStructureKind::BuiltinAttribute);
  };

  switch (getKind()) {
  case DAK_Semantics:
    Printer.printAttrName("@_semantics");
    Printer << "(\"" << cast<SemanticsAttr>(this)->Value << "\")";
    break;

  case DAK_Alignment:
    Printer.printAttrName("@_alignment");
    Printer << "(" << cast<AlignmentAttr>(this)->getValue() << ")";
    break;

  case DAK_SILGenName:
    Printer.printAttrName("@_silgen_name");
    Printer << "(\"" << cast<SILGenNameAttr>(this)->Name << "\")";
    break;

  case DAK_OriginallyDefinedIn: {
    Printer.printAttrName("@_originallyDefinedIn");
    Printer << "(module: ";
    auto Attr = cast<OriginallyDefinedInAttr>(this);
    Printer << "\"" << Attr->OriginalModuleName << "\", ";
    Printer << platformString(Attr->Platform) << " " <<
      Attr->MovedVersion.getAsString();
    Printer << ")";
    break;
  }

  case DAK_Available: {
    auto Attr = cast<AvailableAttr>(this);
    if (Options.SuppressNoAsyncAvailabilityAttr && Attr->isNoAsync())
      return false;
    if (!Options.PrintSPIs && Attr->IsSPI) {
      assert(Attr->hasPlatform());
      assert(Attr->Introduced.has_value());
      Printer.printAttrName("@available");
      Printer << "(";
      Printer << Attr->platformString();
      Printer << ", unavailable)";
      break;
    }
    if (Attr->IsSPI) {
      std::string atSPI = (llvm::Twine("@") + SPI_AVAILABLE_ATTRNAME).str();
      Printer.printAttrName(atSPI);
    } else {
      Printer.printAttrName("@available");
    }
    Printer << "(";
    printAvailableAttr(Attr, Printer, Options);
    Printer << ")";
    break;
  }

  case DAK_CDecl:
    Printer << "@_cdecl(\"" << cast<CDeclAttr>(this)->Name << "\")";
    break;

  case DAK_Expose:
    Printer.printAttrName("@_expose");
    Printer << "(Cxx";
    if (!cast<ExposeAttr>(this)->Name.empty())
      Printer << ", \"" << cast<ExposeAttr>(this)->Name << "\"";
    Printer << ")";
    break;

  case DAK_ObjC: {
    Printer.printAttrName("@objc");
    llvm::SmallString<32> scratch;
    if (auto Name = cast<ObjCAttr>(this)->getName()) {
      if (!cast<ObjCAttr>(this)->isNameImplicit())
        Printer << "(" << Name->getString(scratch) << ")";
    }
    break;
  }

  case DAK_PrivateImport: {
    Printer.printAttrName("@_private(sourceFile: \"");
    Printer << cast<PrivateImportAttr>(this)->getSourceFile() << "\")";
    break;
  }
    
  case DAK_SwiftNativeObjCRuntimeBase: {
    auto *attr = cast<SwiftNativeObjCRuntimeBaseAttr>(this);
    Printer.printAttrName("@_swift_native_objc_runtime_base");
    Printer << "(" << attr->BaseClassName.str() << ")";
    break;
  }

  case DAK_Specialize: {
    auto *attr = cast<SpecializeAttr>(this);
    // Don't print the _specialize attribute if it is marked spi and we are
    // asked to skip SPI.
    if (!Options.PrintSPIs && !attr->getSPIGroups().empty())
      return false;

    // Don't print the _specialize attribute if we are asked to skip the ones
    // with availability parameters.
    if (!Options.PrintSpecializeAttributeWithAvailability &&
        !attr->getAvailableAttrs().empty())
      return false;

    Printer << "@" << getAttrName() << "(";
    auto exported = attr->isExported() ? "true" : "false";
    auto kind = attr->isPartialSpecialization() ? "partial" : "full";
    auto target = attr->getTargetFunctionName();
    Printer << "exported: "<<  exported << ", ";
    for (auto id : attr->getSPIGroups()) {
      Printer << "spi: " << id << ", ";
    }
    Printer << "kind: " << kind << ", ";
    if (target)
      Printer << "target: " << target << ", ";
    auto availAttrs = attr->getAvailableAttrs();
    if (!availAttrs.empty()) {
      Printer << "availability: ";
      auto numAttrs = availAttrs.size();
      if (numAttrs == 1) {
        printAvailableAttr(availAttrs[0], Printer, Options);
        Printer << "; ";
      } else {
        SmallVector<const DeclAttribute *, 8> tmp(availAttrs.begin(),
                                                  availAttrs.end());
        printShortFormAvailable(tmp, Printer, Options,
                                true /*forAtSpecialize*/);
        Printer << "; ";
      }
    }
    SmallVector<Requirement, 4> requirementsScratch;
    auto requirements = attr->getSpecializedSignature().getRequirements();
    auto *FnDecl = dyn_cast_or_null<AbstractFunctionDecl>(D);
    if (FnDecl && FnDecl->getGenericSignature()) {
      auto genericSig = FnDecl->getGenericSignature();

      if (auto sig = attr->getSpecializedSignature()) {
        requirementsScratch = sig.requirementsNotSatisfiedBy(genericSig);
        requirements = requirementsScratch;
      }
    }

    if (!requirements.empty()) {
      Printer << "where ";
    }

    interleave(requirements,
                [&](Requirement req) {
                  bool typeErased = false;
                  if (req.getKind() == RequirementKind::Layout &&
                      !attr->getTypeErasedParams().empty()) {
                    const auto &erasedParams = attr->getTypeErasedParams();
                    typeErased = std::any_of(erasedParams.begin(),
                                           erasedParams.end(),
                                           [&](Type t) { return t->isEqual(req.getFirstType()); });
                    if (typeErased)
                      Printer << "@_noMetadata ";
                  }
                  auto OptionsCopy = Options;
                  OptionsCopy.PrintClassLayoutName = typeErased;
                  req.print(Printer, OptionsCopy);
                },
                [&] { Printer << ", "; });

    Printer << ")";
    break;
  }

  case DAK_Implements: {
    Printer.printAttrName("@_implements");
    Printer << "(";
    auto *attr = cast<ImplementsAttr>(this);
    attr->getProtocolType().print(Printer, Options);
    Printer << ", " << attr->getMemberName() << ")";
    break;
  }

  case DAK_ObjCRuntimeName: {
    Printer.printAttrName("@_objcRuntimeName");
    Printer << "(";
    auto *attr = cast<ObjCRuntimeNameAttr>(this);
    Printer << attr->Name;
    Printer << ")";
    break;
  }

  case DAK_ClangImporterSynthesizedType: {
    Printer.printAttrName("@_clangImporterSynthesizedType");
    auto *attr = cast<ClangImporterSynthesizedTypeAttr>(this);
    Printer << "(originalTypeName: \"" << attr->originalTypeName
            << "\", manglingForKind: \"" << attr->getManglingName() << "\")";
    break;
  }

  case DAK_DynamicReplacement: {
    Printer.printAttrName("@_dynamicReplacement");
    Printer << "(for: \"";
    auto *attr = cast<DynamicReplacementAttr>(this);
    Printer << attr->getReplacedFunctionName() << "\")";
    break;
  }

  case DAK_TypeEraser: {
    Printer.printAttrName("@_typeEraser");
    Printer << "(";
    Printer.callPrintNamePre(PrintNameContext::Attribute);
    auto *attr = cast<TypeEraserAttr>(this);
    if (auto *repr = attr->getParsedTypeEraserTypeRepr())
      repr->print(Printer, Options);
    else if (auto proto = dyn_cast<ProtocolDecl>(D))
      attr->getResolvedType(proto)->print(Printer, Options);
    Printer.printNamePost(PrintNameContext::Attribute);
    Printer << ")";
    break;
  }

  case DAK_Custom: {
    Printer.callPrintNamePre(PrintNameContext::Attribute);
    Printer << "@";
    auto *attr = cast<CustomAttr>(this);
    if (auto type = attr->getType())
      type.print(Printer, Options);
    else
      attr->getTypeRepr()->print(Printer, Options);
    if (attr->isArgUnsafe() && Options.IsForSwiftInterface)
      Printer << "(unsafe)";
    Printer.printNamePost(PrintNameContext::Attribute);
    break;
  }

  case DAK_ProjectedValueProperty:
    Printer.printAttrName("@_projectedValueProperty");
    Printer << "(";
    Printer << cast<ProjectedValuePropertyAttr>(this)->ProjectionPropertyName;
    Printer << ")";
    break;

  case DAK_Differentiable: {
    Printer.printAttrName("@differentiable");
    auto *attr = cast<DifferentiableAttr>(this);
    printDifferentiableAttrArguments(attr, Printer, Options, D);
    break;
  }

  case DAK_Derivative: {
    Printer.printAttrName("@derivative");
    Printer << "(of: ";
    auto *attr = cast<DerivativeAttr>(this);
    if (auto *baseType = attr->getBaseTypeRepr())
      baseType->print(Printer, Options);
    attr->getOriginalFunctionName().print(Printer);
    auto *derivative = cast<AbstractFunctionDecl>(D);
    auto diffParamsString = getDifferentiationParametersClauseString(
        derivative, attr->getParameterIndices(), attr->getParsedParameters(),
        DifferentiationParameterKind::Differentiability);
    if (!diffParamsString.empty())
      Printer << ", " << diffParamsString;
    Printer << ')';
    break;
  }

  case DAK_Transpose: {
    Printer.printAttrName("@transpose");
    Printer << "(of: ";
    auto *attr = cast<TransposeAttr>(this);
    if (auto *baseType = attr->getBaseTypeRepr())
      baseType->print(Printer, Options);
    attr->getOriginalFunctionName().print(Printer);
    auto *transpose = cast<AbstractFunctionDecl>(D);
    auto transParamsString = getDifferentiationParametersClauseString(
        transpose, attr->getParameterIndices(), attr->getParsedParameters(),
        DifferentiationParameterKind::Linearity);
    if (!transParamsString.empty())
      Printer << ", " << transParamsString;
    Printer << ')';
    break;
  }

  case DAK_UnavailableFromAsync: {
    Printer.printAttrName("@_unavailableFromAsync");
    const UnavailableFromAsyncAttr *attr = cast<UnavailableFromAsyncAttr>(this);
    if (attr->hasMessage()) {
      Printer << "(message: \"";
      Printer << attr->Message;
      Printer << "\")";
    }
    break;
  }

  case DAK_BackDeployed: {
    Printer.printAttrName("@backDeployed");
    Printer << "(before: ";
    auto Attr = cast<BackDeployedAttr>(this);
    Printer << platformString(Attr->Platform) << " " <<
      Attr->Version.getAsString();
    Printer << ")";
    break;
  }

  case DAK_MacroRole: {
    auto Attr = cast<MacroRoleAttr>(this);
    switch (Attr->getMacroSyntax()) {
    case MacroSyntax::Freestanding:
      Printer.printAttrName("@freestanding");
      break;

    case MacroSyntax::Attached:
      Printer.printAttrName("@attached");
      break;
    }
    Printer << "(";
    Printer << getMacroRoleString(Attr->getMacroRole());
    if (!Attr->getNames().empty()) {
      Printer << ", names: ";
      interleave(
          Attr->getNames(),
          [&](MacroIntroducedDeclName name) {
            Printer << getMacroIntroducedDeclNameString(name.getKind());
            if (macroIntroducedNameRequiresArgument(name.getKind())) {
              SmallString<32> buffer;
              StringRef nameText = name.getName().getString(buffer);
              bool shouldEscape =
                  escapeKeywordInContext(nameText, PrintNameContext::Normal) ||
                  nameText == "$";
              Printer << "(";
              if (shouldEscape)
                Printer << "`";
              Printer << nameText;
              if (shouldEscape)
                Printer << "`";
              Printer << ")";
            }
          },
          [&] {
            Printer << ", ";
          }
      );
    }
    Printer << ")";
    break;
  }

  case DAK_Documentation: {
    auto *attr = cast<DocumentationAttr>(this);

    Printer.printAttrName("@_documentation");
    Printer << "(";

    bool needs_comma = !attr->Metadata.empty() && attr->Visibility;

    if (attr->Visibility) {
      Printer << "visibility: ";
      Printer << getAccessLevelSpelling(*attr->Visibility);
    }

    if (needs_comma) {
      Printer << ", ";
    }

    if (!attr->Metadata.empty()) {
      Printer << "metadata: ";
      Printer << attr->Metadata;
    }

    Printer << ")";
    break;
  }

  case DAK_Count:
    llvm_unreachable("exceed declaration attribute kinds");

#define SIMPLE_DECL_ATTR(X, CLASS, ...) case DAK_##CLASS:
#include "swift/AST/Attr.def"
    llvm_unreachable("handled above");

  default:
    assert(DeclAttribute::isDeclModifier(getKind()) &&
           "handled above");
  }

  return true;
}

void DeclAttribute::print(ASTPrinter &Printer, const PrintOptions &Options,
                          const Decl *D) const {

  if (!printImpl(Printer, Options, D))
    return; // Nothing printed.

  if (isLongAttribute() && Options.PrintLongAttrsOnSeparateLines)
    Printer.printNewline();
  else
    Printer << " ";
}

void DeclAttribute::print(llvm::raw_ostream &OS, const Decl *D) const {
  StreamPrinter P(OS);
  print(P, PrintOptions(), D);
}

uint64_t DeclAttribute::getOptions(DeclAttrKind DK) {
  switch (DK) {
  case DAK_Count:
    llvm_unreachable("getOptions needs a valid attribute");
#define DECL_ATTR(_, CLASS, OPTIONS, ...)\
  case DAK_##CLASS: return OPTIONS;
#include "swift/AST/Attr.def"
  }
  llvm_unreachable("bad DeclAttrKind");
}

StringRef DeclAttribute::getAttrName() const {
  switch (getKind()) {
  case DAK_Count:
    llvm_unreachable("getAttrName needs a valid attribute");
#define SIMPLE_DECL_ATTR(NAME, CLASS, ...) \
  case DAK_##CLASS: \
    return #NAME;
#include "swift/AST/Attr.def"
  case DAK_SILGenName:
    return "_silgen_name";
  case DAK_Alignment:
    return "_alignment";
  case DAK_CDecl:
    return "_cdecl";
  case DAK_SwiftNativeObjCRuntimeBase:
    return "_swift_native_objc_runtime_base";
  case DAK_Semantics:
    return "_semantics";
  case DAK_Available:
    return "available";
  case DAK_ObjC:
  case DAK_ObjCRuntimeName:
    return "objc";
  case DAK_ObjCImplementation:
    return "_objcImplementation";
  case DAK_MainType:
    return "main";
  case DAK_DynamicReplacement:
    return "_dynamicReplacement";
  case DAK_TypeEraser:
    return "_typeEraser";
  case DAK_PrivateImport:
    return "_private";
  case DAK_RestatedObjCConformance:
    return "_restatedObjCConformance";
  case DAK_Inline: {
    switch (cast<InlineAttr>(this)->getKind()) {
    case InlineKind::Never:
      return "inline(never)";
    case InlineKind::Always:
      return "inline(__always)";
    }
    llvm_unreachable("Invalid inline kind");
  }
  case DAK_NonSendable: {
    switch (cast<NonSendableAttr>(this)->Specificity) {
    case NonSendableKind::Specific:
      return "_nonSendable";
    case NonSendableKind::Assumed:
      return "_nonSendable(_assumed)";
    }
    llvm_unreachable("Invalid nonSendable kind");
  }
  case DAK_Optimize: {
    switch (cast<OptimizeAttr>(this)->getMode()) {
    case OptimizationMode::NoOptimization:
      return "_optimize(none)";
    case OptimizationMode::ForSpeed:
      return "_optimize(speed)";
    case OptimizationMode::ForSize:
      return "_optimize(size)";
    default:
      llvm_unreachable("Invalid optimization kind");
    }
  }
  case DAK_Exclusivity: {
    switch (cast<ExclusivityAttr>(this)->getMode()) {
    case ExclusivityAttr::Checked:
      return "exclusivity(checked)";
    case ExclusivityAttr::Unchecked:
      return "exclusivity(unchecked)";
    }
    llvm_unreachable("Invalid optimization kind");
  }
  case DAK_Effects:
    switch (cast<EffectsAttr>(this)->getKind()) {
      case EffectsKind::ReadNone:
        return "_effects(readnone)";
      case EffectsKind::ReadOnly:
        return "_effects(readonly)";
      case EffectsKind::ReleaseNone:
        return "_effects(releasenone)";
      case EffectsKind::ReadWrite:
        return "_effects(readwrite)";
      case EffectsKind::Unspecified:
        return "_effects(unspecified)";
      case EffectsKind::Custom:
        return "_effects";
    }
  case DAK_AccessControl:
  case DAK_SetterAccess: {
    AccessLevel access = cast<AbstractAccessControlAttr>(this)->getAccess();
    return getAccessLevelSpelling(access);
  }

  case DAK_SPIAccessControl:
    return "_spi";
  case DAK_ReferenceOwnership:
    return keywordOf(cast<ReferenceOwnershipAttr>(this)->get());
  case DAK_RawDocComment:
    return "<<raw doc comment>>";
  case DAK_ObjCBridged:
    return "<<ObjC bridged>>";
  case DAK_SynthesizedProtocol:
    return "<<synthesized protocol>>";
  case DAK_Specialize:
    return "_specialize";
  case DAK_Implements:
    return "_implements";
  case DAK_ClangImporterSynthesizedType:
    return "_clangImporterSynthesizedType";
  case DAK_Custom:
    return "<<custom>>";
  case DAK_ProjectedValueProperty:
    return "_projectedValueProperty";
  case DAK_OriginallyDefinedIn:
    return "_originallyDefinedIn";
  case DAK_Differentiable:
    return "differentiable";
  case DAK_Derivative:
    return "derivative";
  case DAK_Transpose:
    return "transpose";
  case DAK_UnavailableFromAsync:
    return "_unavailableFromAsync";
  case DAK_BackDeployed:
    return "backDeployed";
  case DAK_Expose:
    return "_expose";
  case DAK_Documentation:
    return "_documentation";
  case DAK_MacroRole:
    switch (cast<MacroRoleAttr>(this)->getMacroSyntax()) {
    case MacroSyntax::Freestanding:
      return "freestanding";

    case MacroSyntax::Attached:
      return "attached";
    }
  }
  llvm_unreachable("bad DeclAttrKind");
}

ObjCAttr::ObjCAttr(SourceLoc atLoc, SourceRange baseRange,
                   Optional<ObjCSelector> name, SourceRange parenRange,
                   ArrayRef<SourceLoc> nameLocs)
  : DeclAttribute(DAK_ObjC, atLoc, baseRange, /*Implicit=*/false),
    NameData(nullptr)
{
  if (name) {
    // Store the name.
    assert(name->getNumSelectorPieces() == nameLocs.size());
    NameData = name->getOpaqueValue();

    // Store location information.
    Bits.ObjCAttr.HasTrailingLocationInfo = true;
    getTrailingLocations()[0] = parenRange.Start;
    getTrailingLocations()[1] = parenRange.End;
    std::memcpy(getTrailingLocations().slice(2).data(), nameLocs.data(),
                nameLocs.size() * sizeof(SourceLoc));
  } else {
    Bits.ObjCAttr.HasTrailingLocationInfo = false;
  }

  Bits.ObjCAttr.ImplicitName = false;
  Bits.ObjCAttr.Swift3Inferred = false;
}

ObjCAttr *ObjCAttr::create(ASTContext &Ctx, Optional<ObjCSelector> name,
                           bool isNameImplicit) {
  return new (Ctx) ObjCAttr(name, isNameImplicit);
}

ObjCAttr *ObjCAttr::createUnnamed(ASTContext &Ctx, SourceLoc AtLoc,
                                  SourceLoc ObjCLoc) {
  return new (Ctx) ObjCAttr(AtLoc, SourceRange(ObjCLoc), None,
                            SourceRange(), { });
}

ObjCAttr *ObjCAttr::createUnnamedImplicit(ASTContext &Ctx) {
  return new (Ctx) ObjCAttr(None, false);
}

ObjCAttr *ObjCAttr::createNullary(ASTContext &Ctx, SourceLoc AtLoc, 
                                  SourceLoc ObjCLoc, SourceLoc LParenLoc, 
                                  SourceLoc NameLoc, Identifier Name,
                                  SourceLoc RParenLoc) {
  void *mem = Ctx.Allocate(totalSizeToAlloc<SourceLoc>(3), alignof(ObjCAttr));
  return new (mem) ObjCAttr(AtLoc, SourceRange(ObjCLoc, RParenLoc),
                            ObjCSelector(Ctx, 0, Name),
                            SourceRange(LParenLoc, RParenLoc),
                            NameLoc);
}

ObjCAttr *ObjCAttr::createNullary(ASTContext &Ctx, Identifier Name,
                                  bool isNameImplicit) {
  return new (Ctx) ObjCAttr(ObjCSelector(Ctx, 0, Name), isNameImplicit);
}

ObjCAttr *ObjCAttr::createSelector(ASTContext &Ctx, SourceLoc AtLoc, 
                                   SourceLoc ObjCLoc, SourceLoc LParenLoc, 
                                   ArrayRef<SourceLoc> NameLocs,
                                   ArrayRef<Identifier> Names,
                                   SourceLoc RParenLoc) {
  assert(NameLocs.size() == Names.size());
  void *mem = Ctx.Allocate(totalSizeToAlloc<SourceLoc>(NameLocs.size() + 2),
                           alignof(ObjCAttr));
  return new (mem) ObjCAttr(AtLoc, SourceRange(ObjCLoc, RParenLoc),
                            ObjCSelector(Ctx, Names.size(), Names),
                            SourceRange(LParenLoc, RParenLoc),
                            NameLocs);
}

ObjCAttr *ObjCAttr::createSelector(ASTContext &Ctx, 
                                   ArrayRef<Identifier> Names,
                                   bool isNameImplicit) {
  return new (Ctx) ObjCAttr(ObjCSelector(Ctx, Names.size(), Names), 
                            isNameImplicit);
}

ArrayRef<SourceLoc> ObjCAttr::getNameLocs() const {
  if (!hasTrailingLocationInfo())
    return { };

  return getTrailingLocations().slice(2);
}

SourceLoc ObjCAttr::getLParenLoc() const {
  if (!hasTrailingLocationInfo())
    return SourceLoc();

  return getTrailingLocations()[0];
}

SourceLoc ObjCAttr::getRParenLoc() const {
  if (!hasTrailingLocationInfo())
    return SourceLoc();

  return getTrailingLocations()[1];
}

ObjCAttr *ObjCAttr::clone(ASTContext &context) const {
  auto attr = new (context) ObjCAttr(getName(), isNameImplicit());
  attr->setSwift3Inferred(isSwift3Inferred());
  attr->setAddedByAccessNote(getAddedByAccessNote());
  return attr;
}

PrivateImportAttr::PrivateImportAttr(SourceLoc atLoc, SourceRange baseRange,
                                     StringRef sourceFile,
                                     SourceRange parenRange)
    : DeclAttribute(DAK_PrivateImport, atLoc, baseRange, /*Implicit=*/false),
      SourceFile(sourceFile) {}

PrivateImportAttr *PrivateImportAttr::create(ASTContext &Ctxt, SourceLoc AtLoc,
                                             SourceLoc PrivateLoc,
                                             SourceLoc LParenLoc,
                                             StringRef sourceFile,
                                             SourceLoc RParenLoc) {
  return new (Ctxt)
      PrivateImportAttr(AtLoc, SourceRange(PrivateLoc, RParenLoc), sourceFile,
                        SourceRange(LParenLoc, RParenLoc));
}

DynamicReplacementAttr::DynamicReplacementAttr(SourceLoc atLoc,
                                               SourceRange baseRange,
                                               DeclNameRef name,
                                               SourceRange parenRange)
    : DeclAttribute(DAK_DynamicReplacement, atLoc, baseRange,
                    /*Implicit=*/false),
      ReplacedFunctionName(name) {
  Bits.DynamicReplacementAttr.HasTrailingLocationInfo = true;
  getTrailingLocations()[0] = parenRange.Start;
  getTrailingLocations()[1] = parenRange.End;
}

DynamicReplacementAttr *
DynamicReplacementAttr::create(ASTContext &Ctx, SourceLoc AtLoc,
                               SourceLoc DynReplLoc, SourceLoc LParenLoc,
                               DeclNameRef ReplacedFunction, SourceLoc RParenLoc) {
  void *mem = Ctx.Allocate(totalSizeToAlloc<SourceLoc>(2),
                           alignof(DynamicReplacementAttr));
  return new (mem) DynamicReplacementAttr(
      AtLoc, SourceRange(DynReplLoc, RParenLoc), ReplacedFunction,
      SourceRange(LParenLoc, RParenLoc));
}

DynamicReplacementAttr *
DynamicReplacementAttr::create(ASTContext &Ctx, DeclNameRef name,
                               AbstractFunctionDecl *f) {
  return new (Ctx) DynamicReplacementAttr(name, f);
}

DynamicReplacementAttr *
DynamicReplacementAttr::create(ASTContext &Ctx, DeclNameRef name,
                               LazyMemberLoader *Resolver, uint64_t Data) {
  return new (Ctx) DynamicReplacementAttr(name, Resolver, Data);
}

SourceLoc DynamicReplacementAttr::getLParenLoc() const {
  return getTrailingLocations()[0];
}

SourceLoc DynamicReplacementAttr::getRParenLoc() const {
  return getTrailingLocations()[1];
}

TypeEraserAttr *TypeEraserAttr::create(ASTContext &ctx,
                                       SourceLoc atLoc, SourceRange range,
                                       TypeExpr *typeEraserExpr) {
  return new (ctx) TypeEraserAttr(atLoc, range, typeEraserExpr, nullptr, 0);
}

TypeEraserAttr *TypeEraserAttr::create(ASTContext &ctx,
                                       LazyMemberLoader *Resolver,
                                       uint64_t Data) {
  return new (ctx) TypeEraserAttr(SourceLoc(), SourceRange(),
                                  nullptr, Resolver, Data);
}

bool TypeEraserAttr::hasViableTypeEraserInit(ProtocolDecl *protocol) const {
  return evaluateOrDefault(protocol->getASTContext().evaluator,
                           TypeEraserHasViableInitRequest{
                               const_cast<TypeEraserAttr *>(this), protocol},
                           false);
}

TypeRepr *TypeEraserAttr::getParsedTypeEraserTypeRepr() const {
  return TypeEraserExpr ? TypeEraserExpr->getTypeRepr() : nullptr;
}

SourceLoc TypeEraserAttr::getLoc() const {
  return TypeEraserExpr ? TypeEraserExpr->getLoc() : SourceLoc();
}

Type TypeEraserAttr::getTypeWithoutResolving() const {
  return TypeEraserExpr ? TypeEraserExpr->getInstanceType() : Type();
}

Type TypeEraserAttr::getResolvedType(const ProtocolDecl *PD) const {
  auto &ctx = PD->getASTContext();
  return evaluateOrDefault(ctx.evaluator,
                           ResolveTypeEraserTypeRequest{
                               const_cast<ProtocolDecl *>(PD),
                               const_cast<TypeEraserAttr *>(this)},
                           ErrorType::get(ctx));
}

AvailableAttr *
AvailableAttr::createPlatformAgnostic(ASTContext &C,
                                   StringRef Message,
                                   StringRef Rename,
                                   PlatformAgnosticAvailabilityKind Kind,
                                   llvm::VersionTuple Obsoleted) {
  assert(Kind != PlatformAgnosticAvailabilityKind::None);
  llvm::VersionTuple NoVersion;
  if (Kind == PlatformAgnosticAvailabilityKind::SwiftVersionSpecific) {
    assert(!Obsoleted.empty());
  }
  return new (C) AvailableAttr(
    SourceLoc(), SourceRange(), PlatformKind::none, Message, Rename, nullptr,
    NoVersion, SourceRange(),
    NoVersion, SourceRange(),
    Obsoleted, SourceRange(),
    Kind, /* isImplicit */ false, /*SPI*/false);
}

AvailableAttr *AvailableAttr::createForAlternative(
    ASTContext &C, AbstractFunctionDecl *AsyncFunc) {
  llvm::VersionTuple NoVersion;
  return new (C) AvailableAttr(
    SourceLoc(), SourceRange(), PlatformKind::none, "", "", AsyncFunc,
    NoVersion, SourceRange(),
    NoVersion, SourceRange(),
    NoVersion, SourceRange(),
    PlatformAgnosticAvailabilityKind::None, /*Implicit=*/true, /*SPI*/false);
}

bool AvailableAttr::isActivePlatform(const ASTContext &ctx) const {
  return isPlatformActive(Platform, ctx.LangOpts);
}

bool BackDeployedAttr::isActivePlatform(const ASTContext &ctx) const {
  return isPlatformActive(Platform, ctx.LangOpts);
}

AvailableAttr *AvailableAttr::clone(ASTContext &C, bool implicit) const {
  return new (C) AvailableAttr(implicit ? SourceLoc() : AtLoc,
                               implicit ? SourceRange() : getRange(),
                               Platform, Message, Rename, RenameDecl,
                               Introduced ? *Introduced : llvm::VersionTuple(),
                               implicit ? SourceRange() : IntroducedRange,
                               Deprecated ? *Deprecated : llvm::VersionTuple(),
                               implicit ? SourceRange() : DeprecatedRange,
                               Obsoleted ? *Obsoleted : llvm::VersionTuple(),
                               implicit ? SourceRange() : ObsoletedRange,
                               PlatformAgnostic,
                               implicit,
                               IsSPI);
}

Optional<OriginallyDefinedInAttr::ActiveVersion>
OriginallyDefinedInAttr::isActivePlatform(const ASTContext &ctx) const {
  OriginallyDefinedInAttr::ActiveVersion Result;
  Result.Platform = Platform;
  Result.Version = MovedVersion;
  Result.ModuleName = OriginalModuleName;
  if (isPlatformActive(Platform, ctx.LangOpts, /*TargetVariant*/false)) {
    Result.IsSimulator = ctx.LangOpts.Target.isSimulatorEnvironment();
    return Result;
  }

  // Also check if the platform is active by using target variant. This ensures
  // we emit linker directives for multiple platforms when building zippered
  // libraries.
  if (ctx.LangOpts.TargetVariant.has_value() &&
      isPlatformActive(Platform, ctx.LangOpts, /*TargetVariant*/true)) {
    Result.IsSimulator = ctx.LangOpts.TargetVariant->isSimulatorEnvironment();
    return Result;
  }
  return None;
}

OriginallyDefinedInAttr *OriginallyDefinedInAttr::clone(ASTContext &C,
                                                        bool implicit) const {
  return new (C) OriginallyDefinedInAttr(
      implicit ? SourceLoc() : AtLoc, implicit ? SourceRange() : getRange(),
      OriginalModuleName, Platform, MovedVersion, implicit);
}

bool AvailableAttr::isLanguageVersionSpecific() const {
  if (PlatformAgnostic ==
      PlatformAgnosticAvailabilityKind::SwiftVersionSpecific)
    {
      assert(Platform == PlatformKind::none &&
             (Introduced.has_value() ||
              Deprecated.has_value() ||
              Obsoleted.has_value()));
      return true;
    }
  return false;
}

bool AvailableAttr::isPackageDescriptionVersionSpecific() const {
  if (PlatformAgnostic ==
      PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific)
    {
      assert(Platform == PlatformKind::none &&
             (Introduced.has_value() ||
              Deprecated.has_value() ||
              Obsoleted.has_value()));
      return true;
    }
  return false;
}

bool AvailableAttr::isUnconditionallyUnavailable() const {
  switch (PlatformAgnostic) {
  case PlatformAgnosticAvailabilityKind::None:
  case PlatformAgnosticAvailabilityKind::Deprecated:
  case PlatformAgnosticAvailabilityKind::SwiftVersionSpecific:
  case PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
  case PlatformAgnosticAvailabilityKind::NoAsync:
    return false;

  case PlatformAgnosticAvailabilityKind::Unavailable:
  case PlatformAgnosticAvailabilityKind::UnavailableInSwift:
    return true;
  }

  llvm_unreachable("Unhandled PlatformAgnosticAvailabilityKind in switch.");
}

bool AvailableAttr::isUnconditionallyDeprecated() const {
  switch (PlatformAgnostic) {
  case PlatformAgnosticAvailabilityKind::None:
  case PlatformAgnosticAvailabilityKind::Unavailable:
  case PlatformAgnosticAvailabilityKind::UnavailableInSwift:
  case PlatformAgnosticAvailabilityKind::SwiftVersionSpecific:
  case PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
  case PlatformAgnosticAvailabilityKind::NoAsync:
    return false;

  case PlatformAgnosticAvailabilityKind::Deprecated:
    return true;
  }

  llvm_unreachable("Unhandled PlatformAgnosticAvailabilityKind in switch.");
}

bool AvailableAttr::isNoAsync() const {
  return PlatformAgnostic == PlatformAgnosticAvailabilityKind::NoAsync;
}

llvm::VersionTuple AvailableAttr::getActiveVersion(const ASTContext &ctx) const {
  if (isLanguageVersionSpecific()) {
    return ctx.LangOpts.EffectiveLanguageVersion;
  } else if (isPackageDescriptionVersionSpecific()) {
    return ctx.LangOpts.PackageDescriptionVersion;
  } else {
    return ctx.LangOpts.getMinPlatformVersion();
  }
}

AvailableVersionComparison AvailableAttr::getVersionAvailability(
  const ASTContext &ctx) const {

  // Unconditional unavailability.
  if (isUnconditionallyUnavailable())
    return AvailableVersionComparison::Unavailable;

  llvm::VersionTuple queryVersion = getActiveVersion(ctx);

  // If this entity was obsoleted before or at the query platform version,
  // consider it obsolete.
  if (Obsoleted && *Obsoleted <= queryVersion)
    return AvailableVersionComparison::Obsoleted;

  // If this entity was introduced after the query version and we're doing a
  // platform comparison, true availability can only be determined dynamically;
  // if we're doing a _language_ version check, the query version is a
  // static requirement, so we treat "introduced later" as just plain
  // unavailable.
  if (Introduced && *Introduced > queryVersion) {
    if (isLanguageVersionSpecific() || isPackageDescriptionVersionSpecific())
      return AvailableVersionComparison::Unavailable;
    else
      return AvailableVersionComparison::PotentiallyUnavailable;
  }

  // The entity is available.
  return AvailableVersionComparison::Available;
}

const AvailableAttr *AvailableAttr::isUnavailable(const Decl *D) {
  ASTContext &ctx = D->getASTContext();
  if (auto attr = D->getAttrs().getUnavailable(ctx))
    return attr;

  // If D is an extension member, check if the extension is unavailable.
  //
  // Skip decls imported from Clang, they could be associated to the wrong
  // extension and inherit undesired unavailability. The ClangImporter
  // associates Objective-C protocol members to the first category where the
  // protocol is directly or indirectly adopted, no matter its availability
  // and the availability of other categories. rdar://problem/53956555
  if (!D->getClangNode())
    if (auto ext = dyn_cast<ExtensionDecl>(D->getDeclContext()))
        return AvailableAttr::isUnavailable(ext);

  return nullptr;
}

SpecializeAttr::SpecializeAttr(SourceLoc atLoc, SourceRange range,
                               TrailingWhereClause *clause, bool exported,
                               SpecializationKind kind,
                               GenericSignature specializedSignature,
                               DeclNameRef targetFunctionName,
                               ArrayRef<Identifier> spiGroups,
                               ArrayRef<AvailableAttr *> availableAttrs,
                               size_t typeErasedParamsCount)
    : DeclAttribute(DAK_Specialize, atLoc, range,
                    /*Implicit=*/clause == nullptr),
      trailingWhereClause(clause), specializedSignature(specializedSignature),
      targetFunctionName(targetFunctionName), numSPIGroups(spiGroups.size()),
      numAvailableAttrs(availableAttrs.size()),
      numTypeErasedParams(typeErasedParamsCount),
      typeErasedParamsInitialized(false) {
  std::uninitialized_copy(spiGroups.begin(), spiGroups.end(),
                          getTrailingObjects<Identifier>());
  std::uninitialized_copy(availableAttrs.begin(), availableAttrs.end(),
                          getTrailingObjects<AvailableAttr *>());

  Bits.SpecializeAttr.exported = exported;
  Bits.SpecializeAttr.kind = unsigned(kind);
}

TrailingWhereClause *SpecializeAttr::getTrailingWhereClause() const {
  return trailingWhereClause;
}

SpecializeAttr *SpecializeAttr::create(ASTContext &Ctx, SourceLoc atLoc,
                                       SourceRange range,
                                       TrailingWhereClause *clause,
                                       bool exported, SpecializationKind kind,
                                       DeclNameRef targetFunctionName,
                                       ArrayRef<Identifier> spiGroups,
                                       ArrayRef<AvailableAttr *> availableAttrs,
                                       size_t typeErasedParamsCount,
                                       GenericSignature specializedSignature) {
  unsigned size = totalSizeToAlloc<Identifier, AvailableAttr *, Type>(
      spiGroups.size(), availableAttrs.size(), typeErasedParamsCount);
  void *mem = Ctx.Allocate(size, alignof(SpecializeAttr));
  return new (mem)
      SpecializeAttr(atLoc, range, clause, exported, kind, specializedSignature,
                     targetFunctionName, spiGroups, availableAttrs, typeErasedParamsCount);
}

SpecializeAttr *SpecializeAttr::create(ASTContext &ctx, bool exported,
                                       SpecializationKind kind,
                                       ArrayRef<Identifier> spiGroups,
                                       ArrayRef<AvailableAttr *> availableAttrs,
                                       GenericSignature specializedSignature,
                                       DeclNameRef targetFunctionName) {
  unsigned size = totalSizeToAlloc<Identifier, AvailableAttr *, Type>(
      spiGroups.size(), availableAttrs.size(), 0);
  void *mem = ctx.Allocate(size, alignof(SpecializeAttr));
  return new (mem) SpecializeAttr(
      SourceLoc(), SourceRange(), nullptr, exported, kind, specializedSignature,
      targetFunctionName, spiGroups, availableAttrs, 0);
}

SpecializeAttr *SpecializeAttr::create(
    ASTContext &ctx, bool exported, SpecializationKind kind,
    ArrayRef<Identifier> spiGroups, ArrayRef<AvailableAttr *> availableAttrs,
    ArrayRef<Type> typeErasedParams, GenericSignature specializedSignature,
    DeclNameRef targetFunctionName, LazyMemberLoader *resolver,
    uint64_t data) {
  unsigned size = totalSizeToAlloc<Identifier, AvailableAttr *, Type>(
      spiGroups.size(), availableAttrs.size(), typeErasedParams.size());
  void *mem = ctx.Allocate(size, alignof(SpecializeAttr));
  auto *attr = new (mem) SpecializeAttr(
      SourceLoc(), SourceRange(), nullptr, exported, kind, specializedSignature,
      targetFunctionName, spiGroups, availableAttrs, typeErasedParams.size());
  attr->setTypeErasedParams(typeErasedParams);
  attr->resolver = resolver;
  attr->resolverContextData = data;
  return attr;
}

ValueDecl * SpecializeAttr::getTargetFunctionDecl(const ValueDecl *onDecl) const {
  return evaluateOrDefault(onDecl->getASTContext().evaluator,
                           SpecializeAttrTargetDeclRequest{
                               onDecl, const_cast<SpecializeAttr *>(this)},
                           nullptr);
}

SPIAccessControlAttr::SPIAccessControlAttr(SourceLoc atLoc, SourceRange range,
                                           ArrayRef<Identifier> spiGroups)
      : DeclAttribute(DAK_SPIAccessControl, atLoc, range,
                      /*Implicit=*/false),
        numSPIGroups(spiGroups.size()) {
  std::uninitialized_copy(spiGroups.begin(), spiGroups.end(),
                          getTrailingObjects<Identifier>());
}

SPIAccessControlAttr *
SPIAccessControlAttr::create(ASTContext &context,
                             SourceLoc atLoc,
                             SourceRange range,
                             ArrayRef<Identifier> spiGroups) {
  unsigned size = totalSizeToAlloc<Identifier>(spiGroups.size());
  void *mem = context.Allocate(size, alignof(SPIAccessControlAttr));
  return new (mem) SPIAccessControlAttr(atLoc, range, spiGroups);
}

SPIAccessControlAttr *SPIAccessControlAttr::clone(ASTContext &C,
                                                  bool implicit) const {
  auto *attr = SPIAccessControlAttr::create(
      C, implicit ? SourceLoc() : AtLoc, implicit ? SourceRange() : getRange(),
      getSPIGroups());
  attr->setImplicit(implicit);
  return attr;
}

DifferentiableAttr::DifferentiableAttr(bool implicit, SourceLoc atLoc,
                                       SourceRange baseRange,
                                       enum DifferentiabilityKind diffKind,
                                       ArrayRef<ParsedAutoDiffParameter> params,
                                       TrailingWhereClause *clause)
  : DeclAttribute(DAK_Differentiable, atLoc, baseRange, implicit),
    DifferentiabilityKind(diffKind), NumParsedParameters(params.size()),
    WhereClause(clause) {
  assert((diffKind != DifferentiabilityKind::Normal &&
          diffKind != DifferentiabilityKind::Forward) &&
         "'Normal' and 'Forward' are not supported");
  std::copy(params.begin(), params.end(),
            getTrailingObjects<ParsedAutoDiffParameter>());
}

DifferentiableAttr::DifferentiableAttr(Decl *original, bool implicit,
                                       SourceLoc atLoc, SourceRange baseRange,
                                       enum DifferentiabilityKind diffKind,
                                       IndexSubset *parameterIndices,
                                       GenericSignature derivativeGenSig)
    : DeclAttribute(DAK_Differentiable, atLoc, baseRange, implicit),
      OriginalDeclaration(original), DifferentiabilityKind(diffKind) {
  assert((diffKind != DifferentiabilityKind::Normal &&
          diffKind != DifferentiabilityKind::Forward) &&
         "'Normal' and 'Forward' are not supported");
  setParameterIndices(parameterIndices);
  setDerivativeGenericSignature(derivativeGenSig);
}

DifferentiableAttr *
DifferentiableAttr::create(ASTContext &context, bool implicit,
                           SourceLoc atLoc, SourceRange baseRange,
                           enum DifferentiabilityKind diffKind,
                           ArrayRef<ParsedAutoDiffParameter> parameters,
                           TrailingWhereClause *clause) {
  unsigned size = totalSizeToAlloc<ParsedAutoDiffParameter>(parameters.size());
  void *mem = context.Allocate(size, alignof(DifferentiableAttr));
  return new (mem) DifferentiableAttr(implicit, atLoc, baseRange, diffKind,
                                      parameters, clause);
}

DifferentiableAttr *
DifferentiableAttr::create(AbstractFunctionDecl *original, bool implicit,
                           SourceLoc atLoc, SourceRange baseRange,
                           enum DifferentiabilityKind diffKind,
                           IndexSubset *parameterIndices,
                           GenericSignature derivativeGenSig) {
  auto &ctx = original->getASTContext();
  
  size_t size = totalSizeToAlloc<ParsedAutoDiffParameter>(0); 
  void *mem = ctx.Allocate(size, alignof(DifferentiableAttr));
  return new (mem) DifferentiableAttr(original, implicit, atLoc, baseRange,
                                      diffKind, parameterIndices,
                                      derivativeGenSig);
}

void DifferentiableAttr::setOriginalDeclaration(Decl *originalDeclaration) {
  assert(originalDeclaration && "Original declaration must be non-null");
  assert(!OriginalDeclaration &&
         "Original declaration cannot have already been set");
  OriginalDeclaration = originalDeclaration;
}

bool DifferentiableAttr::hasBeenTypeChecked() const {
  return ParameterIndicesAndBit.getInt();
}

IndexSubset *DifferentiableAttr::getParameterIndices() const {
  assert(getOriginalDeclaration() &&
         "Original declaration must have been resolved");
  auto &ctx = getOriginalDeclaration()->getASTContext();
  return evaluateOrDefault(ctx.evaluator,
                           DifferentiableAttributeTypeCheckRequest{
                               const_cast<DifferentiableAttr *>(this)},
                           nullptr);
}

void DifferentiableAttr::setParameterIndices(IndexSubset *paramIndices) {
  assert(getOriginalDeclaration() &&
         "Original declaration must have been resolved");
  auto &ctx = getOriginalDeclaration()->getASTContext();
  ctx.evaluator.cacheOutput(
      DifferentiableAttributeTypeCheckRequest{
          const_cast<DifferentiableAttr *>(this)},
      std::move(paramIndices));
}

GenericEnvironment *DifferentiableAttr::getDerivativeGenericEnvironment(
    AbstractFunctionDecl *original) const {
  if (auto derivativeGenSig = getDerivativeGenericSignature())
    return derivativeGenSig.getGenericEnvironment();
  return original->getGenericEnvironment();
}

void DeclNameRefWithLoc::print(ASTPrinter &Printer) const {
  Printer << Name;
  if (AccessorKind)
    Printer << '.' << getAccessorLabel(*AccessorKind);
}

void DifferentiableAttr::print(llvm::raw_ostream &OS, const Decl *D,
                               bool omitWrtClause) const {
  StreamPrinter P(OS);
  P << "@" << getAttrName();
  printDifferentiableAttrArguments(this, P, PrintOptions(), D, omitWrtClause);
}

DerivativeAttr::DerivativeAttr(bool implicit, SourceLoc atLoc,
                               SourceRange baseRange, TypeRepr *baseTypeRepr,
                               DeclNameRefWithLoc originalName,
                               ArrayRef<ParsedAutoDiffParameter> params)
    : DeclAttribute(DAK_Derivative, atLoc, baseRange, implicit),
      BaseTypeRepr(baseTypeRepr), OriginalFunctionName(std::move(originalName)),
      NumParsedParameters(params.size()) {
  std::copy(params.begin(), params.end(),
            getTrailingObjects<ParsedAutoDiffParameter>());
}

DerivativeAttr::DerivativeAttr(bool implicit, SourceLoc atLoc,
                               SourceRange baseRange, TypeRepr *baseTypeRepr,
                               DeclNameRefWithLoc originalName,
                               IndexSubset *parameterIndices)
    : DeclAttribute(DAK_Derivative, atLoc, baseRange, implicit),
      BaseTypeRepr(baseTypeRepr), OriginalFunctionName(std::move(originalName)),
      ParameterIndices(parameterIndices) {}

DerivativeAttr *
DerivativeAttr::create(ASTContext &context, bool implicit, SourceLoc atLoc,
                       SourceRange baseRange, TypeRepr *baseTypeRepr,
                       DeclNameRefWithLoc originalName,
                       ArrayRef<ParsedAutoDiffParameter> params) {
  unsigned size = totalSizeToAlloc<ParsedAutoDiffParameter>(params.size());
  void *mem = context.Allocate(size, alignof(DerivativeAttr));
  return new (mem) DerivativeAttr(implicit, atLoc, baseRange, baseTypeRepr,
                                  std::move(originalName), params);
}

DerivativeAttr *DerivativeAttr::create(ASTContext &context, bool implicit,
                                       SourceLoc atLoc, SourceRange baseRange,
                                       TypeRepr *baseTypeRepr,
                                       DeclNameRefWithLoc originalName,
                                       IndexSubset *parameterIndices) {
  void *mem = context.Allocate(sizeof(DerivativeAttr), alignof(DerivativeAttr));
  return new (mem) DerivativeAttr(implicit, atLoc, baseRange, baseTypeRepr,
                                  std::move(originalName), parameterIndices);
}

AbstractFunctionDecl *
DerivativeAttr::getOriginalFunction(ASTContext &context) const {
  return evaluateOrDefault(
      context.evaluator,
      DerivativeAttrOriginalDeclRequest{const_cast<DerivativeAttr *>(this)},
      nullptr);
}

void DerivativeAttr::setOriginalFunction(AbstractFunctionDecl *decl) {
  assert(!OriginalFunction && "cannot overwrite original function");
  OriginalFunction = decl;
}

void DerivativeAttr::setOriginalFunctionResolver(
    LazyMemberLoader *resolver, uint64_t resolverContextData) {
  assert(!OriginalFunction && "cannot overwrite original function");
  OriginalFunction = resolver;
  ResolverContextData = resolverContextData;
}

void DerivativeAttr::setOriginalDeclaration(Decl *originalDeclaration) {
  assert(originalDeclaration && "Original declaration must be non-null");
  assert(!OriginalDeclaration &&
         "Original declaration cannot have already been set");
  OriginalDeclaration = originalDeclaration;
}

TransposeAttr::TransposeAttr(bool implicit, SourceLoc atLoc,
                             SourceRange baseRange, TypeRepr *baseTypeRepr,
                             DeclNameRefWithLoc originalName,
                             ArrayRef<ParsedAutoDiffParameter> params)
    : DeclAttribute(DAK_Transpose, atLoc, baseRange, implicit),
      BaseTypeRepr(baseTypeRepr), OriginalFunctionName(std::move(originalName)),
      NumParsedParameters(params.size()) {
  std::uninitialized_copy(params.begin(), params.end(),
                          getTrailingObjects<ParsedAutoDiffParameter>());
}

TransposeAttr::TransposeAttr(bool implicit, SourceLoc atLoc,
                             SourceRange baseRange, TypeRepr *baseTypeRepr,
                             DeclNameRefWithLoc originalName,
                             IndexSubset *parameterIndices)
    : DeclAttribute(DAK_Transpose, atLoc, baseRange, implicit),
      BaseTypeRepr(baseTypeRepr), OriginalFunctionName(std::move(originalName)),
      ParameterIndices(parameterIndices) {}

TransposeAttr *TransposeAttr::create(ASTContext &context, bool implicit,
                                     SourceLoc atLoc, SourceRange baseRange,
                                     TypeRepr *baseType,
                                     DeclNameRefWithLoc originalName,
                                     ArrayRef<ParsedAutoDiffParameter> params) {
  unsigned size = totalSizeToAlloc<ParsedAutoDiffParameter>(params.size());
  void *mem = context.Allocate(size, alignof(TransposeAttr));
  return new (mem) TransposeAttr(implicit, atLoc, baseRange, baseType,
                                 std::move(originalName), params);
}

TransposeAttr *TransposeAttr::create(ASTContext &context, bool implicit,
                                     SourceLoc atLoc, SourceRange baseRange,
                                     TypeRepr *baseType,
                                     DeclNameRefWithLoc originalName,
                                     IndexSubset *parameterIndices) {
  void *mem = context.Allocate(sizeof(TransposeAttr), alignof(TransposeAttr));
  return new (mem) TransposeAttr(implicit, atLoc, baseRange, baseType,
                                 std::move(originalName), parameterIndices);
}

ImplementsAttr::ImplementsAttr(SourceLoc atLoc, SourceRange range,
                               TypeExpr *ProtocolType,
                               DeclName MemberName,
                               DeclNameLoc MemberNameLoc)
    : DeclAttribute(DAK_Implements, atLoc, range, /*Implicit=*/false),
      ProtocolType(ProtocolType),
      MemberName(MemberName),
      MemberNameLoc(MemberNameLoc) {
}


ImplementsAttr *ImplementsAttr::create(ASTContext &Ctx, SourceLoc atLoc,
                                       SourceRange range,
                                       TypeExpr *ProtocolType,
                                       DeclName MemberName,
                                       DeclNameLoc MemberNameLoc) {
  void *mem = Ctx.Allocate(sizeof(ImplementsAttr), alignof(ImplementsAttr));
  return new (mem) ImplementsAttr(atLoc, range, ProtocolType,
                                  MemberName, MemberNameLoc);
}

void ImplementsAttr::setProtocolType(Type ty) {
  assert(ty);
  ProtocolType->setType(MetatypeType::get(ty));
}

Type ImplementsAttr::getProtocolType() const {
  return ProtocolType->getInstanceType();
}

TypeRepr *ImplementsAttr::getProtocolTypeRepr() const {
  return ProtocolType->getTypeRepr();
}

CustomAttr::CustomAttr(SourceLoc atLoc, SourceRange range, TypeExpr *type,
                       PatternBindingInitializer *initContext,
                       ArgumentList *argList, bool implicit)
    : DeclAttribute(DAK_Custom, atLoc, range, implicit), typeExpr(type),
      argList(argList), initContext(initContext) {
  assert(type);
  isArgUnsafeBit = false;
}

CustomAttr *CustomAttr::create(ASTContext &ctx, SourceLoc atLoc, TypeExpr *type,
                               PatternBindingInitializer *initContext,
                               ArgumentList *argList, bool implicit) {
  assert(type);
  SourceRange range(atLoc, type->getSourceRange().End);
  if (argList)
    range.End = argList->getEndLoc();

  return new (ctx)
      CustomAttr(atLoc, range, type, initContext, argList, implicit);
}

TypeRepr *CustomAttr::getTypeRepr() const { return typeExpr->getTypeRepr(); }
Type CustomAttr::getType() const { return typeExpr->getInstanceType(); }

void CustomAttr::resetTypeInformation(TypeExpr *info) { typeExpr = info; }

void CustomAttr::setType(Type ty) {
  assert(ty);
  typeExpr->setType(MetatypeType::get(ty));
}

bool CustomAttr::isArgUnsafe() const {
  if (isArgUnsafeBit)
    return true;

  auto args = getArgs();
  if (!args)
    return false;

  auto *unary = args->getUnlabeledUnaryExpr();
  if (!unary)
    return false;

  if (auto declRef = dyn_cast<UnresolvedDeclRefExpr>(unary)) {
    if (declRef->getName().isSimpleName("unsafe"))
      isArgUnsafeBit = true;
  }

  return isArgUnsafeBit;
}

MacroRoleAttr::MacroRoleAttr(SourceLoc atLoc, SourceRange range,
                             MacroSyntax syntax, SourceLoc lParenLoc,
                             MacroRole role,
                             ArrayRef<MacroIntroducedDeclName> names,
                             SourceLoc rParenLoc, bool implicit)
    : DeclAttribute(DAK_MacroRole, atLoc, range, implicit),
      syntax(syntax), role(role), numNames(names.size()), lParenLoc(lParenLoc),
      rParenLoc(rParenLoc) {
  auto *trailingNamesBuffer = getTrailingObjects<MacroIntroducedDeclName>();
  std::uninitialized_copy(names.begin(), names.end(), trailingNamesBuffer);
}

MacroRoleAttr *
MacroRoleAttr::create(ASTContext &ctx, SourceLoc atLoc, SourceRange range,
                      MacroSyntax syntax, SourceLoc lParenLoc, MacroRole role,
                      ArrayRef<MacroIntroducedDeclName> names,
                      SourceLoc rParenLoc, bool implicit) {
  unsigned size = totalSizeToAlloc<MacroIntroducedDeclName>(names.size());
  auto *mem = ctx.Allocate(size, alignof(MacroRoleAttr));
  return new (mem) MacroRoleAttr(atLoc, range, syntax, lParenLoc, role, names,
                                 rParenLoc, implicit);
}

ArrayRef<MacroIntroducedDeclName> MacroRoleAttr::getNames() const {
  return {
    getTrailingObjects<MacroIntroducedDeclName>(),
    numNames
  };
}

bool MacroRoleAttr::hasNameKind(MacroIntroducedDeclNameKind kind) const {
  return llvm::find_if(getNames(), [kind](MacroIntroducedDeclName name) {
    return name.getKind() == kind;
  }) != getNames().end();
}

const DeclAttribute *
DeclAttributes::getEffectiveSendableAttr() const {
  const NonSendableAttr *assumedAttr = nullptr;

  for (auto attr : getAttributes<NonSendableAttr>()) {
    if (attr->Specificity == NonSendableKind::Specific)
      return attr;
    if (!assumedAttr)
      assumedAttr = attr;
  }

  if (auto sendableAttr = getAttribute<SendableAttr>())
    return sendableAttr;

  return assumedAttr;
}

void swift::simple_display(llvm::raw_ostream &out, const DeclAttribute *attr) {
  if (attr)
    attr->print(out);
}

bool swift::hasAttribute(
    const LangOptions &langOpts, llvm::StringRef attributeName) {
  DeclAttrKind kind = DeclAttribute::getAttrKindFromString(attributeName);
  if (kind == DAK_Count)
    return false;

  if (DeclAttribute::isUserInaccessible(kind))
    return false;
  if (DeclAttribute::isDeclModifier(kind))
    return false;
  if (DeclAttribute::shouldBeRejectedByParser(kind))
    return false;
  if (DeclAttribute::isSilOnly(kind))
    return false;
  if (DeclAttribute::isConcurrencyOnly(kind))
    return false;

  return true;
}
