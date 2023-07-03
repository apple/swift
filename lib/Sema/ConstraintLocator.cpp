//===--- ConstraintLocator.cpp - Constraint Locator -----------------------===//
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
// This file implements the \c ConstraintLocator class and its related types,
// which is used by the constraint-based type checker to describe how
// a particular constraint was derived.
//
//===----------------------------------------------------------------------===//
#include "swift/Sema/ConstraintLocator.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Types.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Sema/Constraint.h"
#include "swift/Sema/ConstraintLocator.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace constraints;

void ConstraintLocator::Profile(llvm::FoldingSetNodeID &id, ASTNode anchor,
                                ArrayRef<PathElement> path) {
  id.AddPointer(anchor.getOpaqueValue());
  id.AddInteger(path.size());
  for (auto elt : path) {
    id.AddInteger(elt.getKind());
    id.AddInteger(elt.getRawStorage());
  }
}

unsigned LocatorPathElt::getNewSummaryFlags() const {
  switch (getKind()) {
  case ConstraintLocator::ApplyArgument:
  case ConstraintLocator::ApplyFunction:
  case ConstraintLocator::SequenceElementType:
  case ConstraintLocator::ClosureResult:
  case ConstraintLocator::ClosureBody:
  case ConstraintLocator::ConstructorMember:
  case ConstraintLocator::ConstructorMemberType:
  case ConstraintLocator::ResultBuilderBodyResult:
  case ConstraintLocator::InstanceType:
  case ConstraintLocator::AutoclosureResult:
  case ConstraintLocator::OptionalPayload:
  case ConstraintLocator::Member:
  case ConstraintLocator::MemberRefBase:
  case ConstraintLocator::UnresolvedMember:
  case ConstraintLocator::ParentType:
  case ConstraintLocator::ExistentialConstraintType:
  case ConstraintLocator::ProtocolCompositionSuperclassType:
  case ConstraintLocator::LValueConversion:
  case ConstraintLocator::DynamicType:
  case ConstraintLocator::SubscriptMember:
  case ConstraintLocator::OpenedGeneric:
  case ConstraintLocator::OpenedOpaqueArchetype:
  case ConstraintLocator::WrappedValue:
  case ConstraintLocator::GenericParameter:
  case ConstraintLocator::GenericArgument:
  case ConstraintLocator::TupleType:
  case ConstraintLocator::GenericType:
  case ConstraintLocator::NamedTupleElement:
  case ConstraintLocator::TupleElement:
  case ConstraintLocator::ProtocolRequirement:
  case ConstraintLocator::Witness:
  case ConstraintLocator::KeyPathComponent:
  case ConstraintLocator::ConditionalRequirement:
  case ConstraintLocator::TypeParameterRequirement:
  case ConstraintLocator::ConformanceRequirement:
  case ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice:
  case ConstraintLocator::DynamicLookupResult:
  case ConstraintLocator::ContextualType:
  case ConstraintLocator::SynthesizedArgument:
  case ConstraintLocator::KeyPathDynamicMember:
  case ConstraintLocator::KeyPathType:
  case ConstraintLocator::KeyPathRoot:
  case ConstraintLocator::KeyPathValue:
  case ConstraintLocator::KeyPathComponentResult:
  case ConstraintLocator::Condition:
  case ConstraintLocator::DynamicCallable:
  case ConstraintLocator::ImplicitCallAsFunction:
  case ConstraintLocator::TernaryBranch:
  case ConstraintLocator::PatternMatch:
  case ConstraintLocator::EnumPatternImplicitCastMatch:
  case ConstraintLocator::ArgumentAttribute:
  case ConstraintLocator::UnresolvedMemberChainResult:
  case ConstraintLocator::PlaceholderType:
  case ConstraintLocator::ImplicitConversion:
  case ConstraintLocator::ImplicitDynamicMemberSubscript:
  case ConstraintLocator::SyntacticElement:
  case ConstraintLocator::PackType:
  case ConstraintLocator::PackElement:
  case ConstraintLocator::PackShape:
  case ConstraintLocator::PackExpansionPattern:
  case ConstraintLocator::PatternBindingElement:
  case ConstraintLocator::NamedPatternDecl:
  case ConstraintLocator::SingleValueStmtBranch:
  case ConstraintLocator::AnyPatternDecl:
  case ConstraintLocator::GlobalActorType:
  case ConstraintLocator::CoercionOperand:
  case ConstraintLocator::PackExpansionType:
    return 0;

  case ConstraintLocator::FunctionArgument:
  case ConstraintLocator::FunctionResult:
    return IsFunctionConversion;

  case ConstraintLocator::ApplyArgToParam: {
    auto flags = castTo<LocatorPathElt::ApplyArgToParam>().getParameterFlags();
    return flags.isNonEphemeral() ? IsNonEphemeralParam : 0;
  }
  }

  llvm_unreachable("Unhandled PathElementKind in switch.");
}

void LocatorPathElt::dump(raw_ostream &out) const {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;

  auto dumpReqKind = [&out](RequirementKind kind) {
    out << " (";
    switch (kind) {
    case RequirementKind::SameShape:
      out << "same_shape";
      break;
    case RequirementKind::Conformance:
      out << "conformance";
      break;
    case RequirementKind::Superclass:
      out << "superclass";
      break;
    case RequirementKind::SameType:
      out << "same-type";
      break;
    case RequirementKind::Layout:
      out << "layout";
      break;
    }
    out << ")";
  };


  const LocatorPathElt &elt = *this;
  switch (getKind()) {
  case ConstraintLocator::GenericParameter: {
    auto gpElt = elt.castTo<LocatorPathElt::GenericParameter>();
    out << "generic parameter '" << gpElt.getType()->getString(PO) << "'";
    break;
  }
  case ConstraintLocator::WrappedValue: {
    auto wrappedValueElt = elt.castTo<LocatorPathElt::WrappedValue>();
    out << "composed property wrapper type '"
        << wrappedValueElt.getType()->getString(PO) << "'";
    break;
  }
  case ConstraintLocator::ApplyArgument:
    out << "apply argument";
    break;

  case ConstraintLocator::ApplyFunction:
    out << "apply function";
    break;

  case ConstraintLocator::OptionalPayload:
    out << "optional payload";
    break;

  case ConstraintLocator::ApplyArgToParam: {
    auto argElt = elt.castTo<LocatorPathElt::ApplyArgToParam>();
    out << "comparing call argument #" << llvm::utostr(argElt.getArgIdx())
        << " to parameter #" << llvm::utostr(argElt.getParamIdx());
    if (argElt.getParameterFlags().isNonEphemeral())
      out << " (non-ephemeral)";
    break;
  }
  case ConstraintLocator::ClosureResult:
    out << "closure result";
    break;

  case ConstraintLocator::ClosureBody:
    out << "type of a closure body";
    break;

  case ConstraintLocator::ConstructorMember:
    out << "constructor member";
    break;

  case ConstraintLocator::ConstructorMemberType: {
    auto memberTypeElt = elt.castTo<LocatorPathElt::ConstructorMemberType>();
    out << "constructor member type";
    if (memberTypeElt.isShortFormOrSelfDelegatingConstructor())
      out << " (for short-form or self.init call)";
    break;
  }

  case ConstraintLocator::FunctionArgument:
    out << "function argument";
    break;

  case ConstraintLocator::FunctionResult:
    out << "function result";
    break;

  case ConstraintLocator::ResultBuilderBodyResult:
    out << "result builder body result";
    break;

  case ConstraintLocator::SequenceElementType:
    out << "sequence element type";
    break;

  case ConstraintLocator::GenericArgument: {
    auto genericElt = elt.castTo<LocatorPathElt::GenericArgument>();
    out << "generic argument #" << llvm::utostr(genericElt.getIndex());
    break;
  }
  case ConstraintLocator::InstanceType:
    out << "instance type";
    break;

  case ConstraintLocator::AutoclosureResult:
    out << "@autoclosure result";
    break;

  case ConstraintLocator::Member:
    out << "member";
    break;

  case ConstraintLocator::MemberRefBase:
    out << "member reference base";
    break;

  case ConstraintLocator::TupleType: {
    auto tupleElt = elt.castTo<LocatorPathElt::TupleType>();
    out << "tuple type '" << tupleElt.getType()->getString(PO) << "'";
    break;
  }

  case ConstraintLocator::GenericType: {
    auto genericTyElt = elt.castTo<LocatorPathElt::GenericType>();
    out << "generic type '" << genericTyElt.getType()->getString(PO) << "'";
    break;
  }

  case ConstraintLocator::NamedTupleElement: {
    auto tupleElt = elt.castTo<LocatorPathElt::NamedTupleElement>();
    out << "named tuple element #" << llvm::utostr(tupleElt.getIndex());
    break;
  }
  case ConstraintLocator::UnresolvedMember:
    out << "unresolved member";
    break;

  case ConstraintLocator::ParentType:
    out << "parent type";
    break;

  case ConstraintLocator::ExistentialConstraintType:
    out << "existential constraint type";
    break;

  case ConstraintLocator::ProtocolCompositionSuperclassType:
    out << "protocol composition superclass type";
    break;

  case ConstraintLocator::LValueConversion:
    out << "@lvalue-to-inout conversion";
    break;

  case ConstraintLocator::DynamicType:
    out << "`.dynamicType` reference";
    break;

  case ConstraintLocator::SubscriptMember:
    out << "subscript member";
    break;

  case ConstraintLocator::TupleElement: {
    auto tupleElt = elt.castTo<LocatorPathElt::TupleElement>();
    out << "tuple element #" << llvm::utostr(tupleElt.getIndex());
    break;
  }
  case ConstraintLocator::KeyPathComponent: {
    auto kpElt = elt.castTo<LocatorPathElt::KeyPathComponent>();
    out << "key path component #" << llvm::utostr(kpElt.getIndex());
    break;
  }
  case ConstraintLocator::ProtocolRequirement: {
    auto reqElt = elt.castTo<LocatorPathElt::ProtocolRequirement>();
    out << "protocol requirement ";
    reqElt.getDecl()->dumpRef(out);
    break;
  }
  case ConstraintLocator::Witness: {
    auto witnessElt = elt.castTo<LocatorPathElt::Witness>();
    out << "witness ";
    witnessElt.getDecl()->dumpRef(out);
    break;
  }
  case ConstraintLocator::OpenedGeneric:
    out << "opened generic";
    break;

  case ConstraintLocator::OpenedOpaqueArchetype:
    out << "opened opaque archetype";
    break;

  case ConstraintLocator::ConditionalRequirement: {
    auto reqElt = elt.castTo<LocatorPathElt::ConditionalRequirement>();
    out << "conditional requirement #" << llvm::utostr(reqElt.getIndex());
    dumpReqKind(reqElt.getRequirementKind());
    break;
  }
  case ConstraintLocator::TypeParameterRequirement: {
    auto reqElt = elt.castTo<LocatorPathElt::TypeParameterRequirement>();
    out << "type parameter requirement #" << llvm::utostr(reqElt.getIndex());
    dumpReqKind(reqElt.getRequirementKind());
    break;
  }

  case ConstraintLocator::ConformanceRequirement: {
    auto *conformance =
        elt.castTo<LocatorPathElt::ConformanceRequirement>().getConformance();
    out << "conformance requirement (";
    conformance->getProtocol()->dumpRef(out);
    out << ")";
    break;
  }

  case ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice:
    out << "implicitly unwrapped disjunction choice";
    break;

  case ConstraintLocator::DynamicLookupResult:
    out << "dynamic lookup result";
    break;

  case ConstraintLocator::ContextualType:
    out << "contextual type";
    break;

  case ConstraintLocator::SynthesizedArgument: {
    auto argElt = elt.castTo<LocatorPathElt::SynthesizedArgument>();
    out << "synthesized argument #" << llvm::utostr(argElt.getIndex());
    break;
  }
  case ConstraintLocator::KeyPathDynamicMember:
    out << "key path dynamic member lookup";
    break;

  case ConstraintLocator::KeyPathType:
    out << "key path type";
    break;

  case ConstraintLocator::KeyPathRoot:
    out << "key path root";
    break;

  case ConstraintLocator::KeyPathValue:
    out << "key path value";
    break;

  case ConstraintLocator::KeyPathComponentResult:
    out << "key path component result";
    break;

  case ConstraintLocator::Condition:
    out << "condition expression";
    break;

  case ConstraintLocator::DynamicCallable:
    out << "implicit call to @dynamicCallable method";
    break;

  case ConstraintLocator::ImplicitCallAsFunction:
    out << "implicit reference to callAsFunction";
    break;

  case ConstraintLocator::TernaryBranch: {
    auto branchElt = elt.castTo<LocatorPathElt::TernaryBranch>();
    out << (branchElt.forThen() ? "'then'" : "'else'")
        << " branch of a ternary operator";
    break;
  }

  case ConstraintLocator::SingleValueStmtBranch: {
    auto branch = elt.castTo<LocatorPathElt::SingleValueStmtBranch>();
    out << "expr branch [" << branch.getExprBranchIndex() << "] of "
           "single value stmt";
    break;
  }

  case ConstraintLocator::PatternMatch:
    out << "pattern match";
    break;

  case ConstraintLocator::EnumPatternImplicitCastMatch:
    out << "enum pattern implicit cast match";
    break;

  case ConstraintLocator::ArgumentAttribute: {
    using AttrLoc = LocatorPathElt::ArgumentAttribute;

    auto attrElt = elt.castTo<AttrLoc>();
    out << "argument attribute: ";

    switch (attrElt.getAttr()) {
    case AttrLoc::Attribute::InOut:
      out << "inout";
      break;

    case AttrLoc::Attribute::Escaping:
      out << "@escaping";
      break;

    case AttrLoc::Attribute::Concurrent:
      out << "@Sendable";
      break;

    case AttrLoc::Attribute::GlobalActor:
      out << "@<global actor>";
      break;
    }

    break;
  }

  case ConstraintLocator::UnresolvedMemberChainResult:
    out << "unresolved chain result";
    break;

  case ConstraintLocator::PlaceholderType:
    out << "placeholder type";
    break;

  case ConstraintLocator::ConstraintLocator::SyntacticElement:
    // TODO: Would be great to print a kind of element this is e.g.
    //       "if", "for each", "switch" etc.
    out << "syntactic element";
    break;

  case ConstraintLocator::ConstraintLocator::ImplicitDynamicMemberSubscript:
    out << "implicit dynamic member subscript";
    break;

  case ConstraintLocator::ConstraintLocator::ImplicitConversion: {
    auto convElt = elt.castTo<LocatorPathElt::ImplicitConversion>();
    out << "implicit conversion " << getName(convElt.getConversionKind());
    break;
  }

  case ConstraintLocator::ConstraintLocator::PackType: {
    auto packElt = elt.castTo<LocatorPathElt::PackType>();
    out << "pack type '" << packElt.getType()->getString(PO) << "'";
    break;
  }

  case ConstraintLocator::PackElement: {
    auto packElt = elt.castTo<LocatorPathElt::PackElement>();
    out << "pack element #" << llvm::utostr(packElt.getIndex());
    break;
  }

  case ConstraintLocator::PackShape: {
    out << "pack shape";
    break;
  }

  case ConstraintLocator::PackExpansionPattern: {
    out << "pack expansion pattern";
    break;
  }

  case ConstraintLocator::PatternBindingElement: {
    auto patternBindingElt =
        elt.castTo<LocatorPathElt::PatternBindingElement>();
    out << "pattern binding element #"
        << llvm::utostr(patternBindingElt.getIndex());
    break;
  }

  case ConstraintLocator::NamedPatternDecl: {
    out << "named pattern decl";
    break;
  }

  case ConstraintLocator::AnyPatternDecl: {
    out << "'_' pattern decl";
    break;
  }

  case ConstraintLocator::GlobalActorType: {
    out << "global actor type";
    break;
  }

  case ConstraintLocator::CoercionOperand: {
    out << "coercion operand";
    break;

  case ConstraintLocator::PackExpansionType:
    auto expansionElt = elt.castTo<LocatorPathElt::PackExpansionType>();
    out << "pack expansion type ("
        << expansionElt.getOpenedType()->getString(PO) << ")";
    break;
  }
  }
}

/// Determine whether given locator points to the subscript reference
/// e.g. `foo[0]` or `\Foo.[0]`
bool ConstraintLocator::isSubscriptMemberRef() const {
  auto anchor = getAnchor();
  auto path = getPath();

  if (!anchor || path.empty())
    return false;

  return path.back().getKind() == ConstraintLocator::SubscriptMember;
}

bool ConstraintLocator::isKeyPathType() const {
  auto anchor = getAnchor();
  auto path = getPath();
  // The format of locator should be `<keypath expr> -> key path type`
  if (!anchor || !isExpr<KeyPathExpr>(anchor) || path.size() != 1)
    return false;
  return path.back().is<LocatorPathElt::KeyPathType>();
}

bool ConstraintLocator::isKeyPathRoot() const {
  auto anchor = getAnchor();
  auto path = getPath();

  if (!anchor || path.empty())
    return false;

  return path.back().getKind() == ConstraintLocator::KeyPathRoot;
}

bool ConstraintLocator::isKeyPathValue() const {
  auto anchor = getAnchor();
  auto path = getPath();

  if (!anchor || path.empty())
    return false;

  return path.back().getKind() == ConstraintLocator::KeyPathValue;
}

bool ConstraintLocator::isResultOfKeyPathDynamicMemberLookup() const {
  return llvm::any_of(getPath(), [](const LocatorPathElt &elt) {
    return elt.isKeyPathDynamicMember();
  });
}

bool ConstraintLocator::isKeyPathSubscriptComponent() const {
  auto *anchor = getAsExpr(getAnchor());
  auto *KPE = dyn_cast_or_null<KeyPathExpr>(anchor);
  if (!KPE)
    return false;

  using ComponentKind = KeyPathExpr::Component::Kind;
  return llvm::any_of(getPath(), [&](const LocatorPathElt &elt) {
    auto keyPathElt = elt.getAs<LocatorPathElt::KeyPathComponent>();
    if (!keyPathElt)
      return false;

    auto index = keyPathElt->getIndex();
    auto &component = KPE->getComponents()[index];
    return component.getKind() == ComponentKind::Subscript ||
           component.getKind() == ComponentKind::UnresolvedSubscript;
  });
}

bool ConstraintLocator::isForKeyPathDynamicMemberLookup() const {
  auto path = getPath();
  return !path.empty() && path.back().isKeyPathDynamicMember();
}

bool ConstraintLocator::isInKeyPathComponent() const {
  return llvm::any_of(getPath(), [&](const LocatorPathElt &elt) {
    return elt.isKeyPathComponent();
  });
}

bool ConstraintLocator::isForKeyPathComponentResult() const {
  return isLastElement<LocatorPathElt::KeyPathComponentResult>();
}

bool ConstraintLocator::isForGenericParameter() const {
  return isLastElement<LocatorPathElt::GenericParameter>();
}

bool ConstraintLocator::isForWrappedValue() const {
  return isLastElement<LocatorPathElt::WrappedValue>();
}

bool ConstraintLocator::isForSequenceElementType() const {
  return isLastElement<LocatorPathElt::SequenceElementType>();
}

bool ConstraintLocator::isForContextualType() const {
  return isLastElement<LocatorPathElt::ContextualType>();
}

bool ConstraintLocator::isForAssignment() const {
  return directlyAt<AssignExpr>();
}

bool ConstraintLocator::isForCoercion() const {
  return isLastElement<LocatorPathElt::CoercionOperand>();
}

bool ConstraintLocator::isForOptionalTry() const {
  return directlyAt<OptionalTryExpr>();
}

bool ConstraintLocator::isForResultBuilderBodyResult() const {
  return isFirstElement<LocatorPathElt::ResultBuilderBodyResult>();
}

bool ConstraintLocator::isForMacroExpansion() const {
  return directlyAt<MacroExpansionExpr>();
}

bool ConstraintLocator::isForSingleValueStmtConjunction() const {
  auto *SVE = getAsExpr<SingleValueStmtExpr>(getAnchor());
  if (!SVE)
    return false;

  // Ignore a trailing SyntacticElement path element for the statement.
  auto path = getPath();
  if (auto elt = getLastElementAs<LocatorPathElt::SyntacticElement>()) {
    if (elt->getElement() == ASTNode(SVE->getStmt()))
      path = path.drop_back();
  }

  // Other than the trailing SyntaticElement, we must be at the anchor.
  return path.empty();
}

llvm::Optional<SingleValueStmtBranchKind>
ConstraintLocator::isForSingleValueStmtBranch() const {
  // Ignore a trailing ContextualType path element.
  auto path = getPath();
  if (auto elt = getLastElementAs<LocatorPathElt::ContextualType>())
    path = path.drop_back();

  if (path.empty())
    return llvm::None;

  if (!path.back().is<LocatorPathElt::SingleValueStmtBranch>())
    return llvm::None;

  auto *SVE = getAsExpr<SingleValueStmtExpr>(getAnchor());
  if (!SVE)
    return llvm::None;

  if (auto *CE = dyn_cast<ClosureExpr>(SVE->getDeclContext())) {
    if (CE->hasSingleExpressionBody() && !hasExplicitResult(CE))
      return SingleValueStmtBranchKind::InSingleExprClosure;
  }
  return SingleValueStmtBranchKind::Regular;
}

NullablePtr<Pattern> ConstraintLocator::getPatternMatch() const {
  auto matchElt = findLast<LocatorPathElt::PatternMatch>();
  if (!matchElt)
    return nullptr;

  return matchElt->getPattern();
}

bool ConstraintLocator::isForPatternMatch() const {
  return getPatternMatch() != nullptr;
}

bool ConstraintLocator::isMemberRef() const {
  if (isLastElement<LocatorPathElt::Member>()) {
    return true;
  } else if (isLastElement<LocatorPathElt::KeyPathDynamicMember>()) {
    auto path = getPath();
    if (path.size() >= 2 &&
        path[path.size() - 2].is<LocatorPathElt::Member>()) {
      return true;
    }
  }
  return false;
}

GenericTypeParamType *ConstraintLocator::getGenericParameter() const {
  // Check whether we have a path that terminates at a generic parameter.
  return isForGenericParameter() ?
      castLastElementTo<LocatorPathElt::GenericParameter>().getType() : nullptr;
}

Type ConstraintLocator::getWrappedValue() const {
  return isForWrappedValue()
             ? castLastElementTo<LocatorPathElt::WrappedValue>().getType()
             : Type();
}

void ConstraintLocator::dump(SourceManager *sm) const {
  dump(sm, llvm::errs());
  llvm::errs() << "\n";
}

void ConstraintLocator::dump(ConstraintSystem *CS) const {
  dump(&CS->getASTContext().SourceMgr, llvm::errs());
  llvm::errs() << "\n";
}


void ConstraintLocator::dump(SourceManager *sm, raw_ostream &out) const {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;
  
  out << "locator@" << (void*) this << " [";

  constraints::dumpAnchor(anchor, sm, out);

  for (auto elt : getPath()) {
    out << " → ";
    elt.dump(out);
  }
  out << ']';
}


void ConstraintLocatorBuilder::dump(SourceManager *sm) const {
  dump(sm, llvm::errs());
  llvm::errs() << "\n";
}

void ConstraintLocatorBuilder::dump(ConstraintSystem *CS) const {
  dump(&CS->getASTContext().SourceMgr, llvm::errs());
  llvm::errs() << "\n";
}

void ConstraintLocatorBuilder::dump(SourceManager *SM, llvm::raw_ostream &out) const {
  if (auto prev = previous.dyn_cast<ConstraintLocator *>()) {
    prev->dump(SM, out);
  } else if (auto prev = previous.dyn_cast<ConstraintLocatorBuilder *>()) {
    prev->dump(SM, out);
  }
  if (element) {
    out << " -> ";
    element->dump(out);
  }
}
