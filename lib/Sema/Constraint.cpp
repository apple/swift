//===--- Constraint.cpp - Constraint in the Type Checker ------------------===//
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
// This file implements the \c Constraint class and its related types,
// which is used by the constraint-based type checker to describe a
// constraint that must be solved.
//
//===----------------------------------------------------------------------===//
#include "swift/AST/Types.h"
#include "swift/Basic/Compiler.h"
#include "swift/Sema/Constraint.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace swift;
using namespace constraints;

Constraint::Constraint(ConstraintKind kind, ArrayRef<Constraint *> constraints,
                       bool isIsolated, ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(kind), HasRestriction(false), IsActive(false), IsDisabled(false),
      IsDisabledForPerformance(false), RememberChoice(false), IsFavored(false),
      IsIsolated(isIsolated), NumTypeVariables(typeVars.size()),
      Nested(constraints), Locator(locator) {
  assert(kind == ConstraintKind::Disjunction ||
         kind == ConstraintKind::Conjunction);

#ifndef NDEBUG
  if (isIsolated)
    assert(kind == ConstraintKind::Conjunction &&
           "Isolation applies only to conjunctions");
#endif

  std::uninitialized_copy(typeVars.begin(), typeVars.end(),
                          getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind Kind, Type First, Type Second,
                       ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(Kind), HasRestriction(false), IsActive(false), IsDisabled(false),
      IsDisabledForPerformance(false), RememberChoice(false), IsFavored(false),
      IsIsolated(false),
      NumTypeVariables(typeVars.size()), Types{First, Second, Type()},
      Locator(locator) {
  switch (Kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::Equal:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Subtype:
  case ConstraintKind::Conversion:
  case ConstraintKind::BridgingConversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::TransitivelyConformsTo:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
  case ConstraintKind::UnresolvedMemberChainBase:
  case ConstraintKind::PropertyWrapper:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::SameShape:
  case ConstraintKind::MaterializePackExpansion:
    assert(!First.isNull());
    assert(!Second.isNull());
    break;
  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::DynamicCallableApplicableFunction:
    assert(First->is<FunctionType>()
           && "The left-hand side type should be a function type");
    trailingClosureMatching = 0;
    break;

  case ConstraintKind::ValueMember:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::ValueWitness:
    llvm_unreachable("Wrong constructor for member constraint");

  case ConstraintKind::Defaultable:
  case ConstraintKind::FallbackType:
    assert(!First.isNull());
    assert(!Second.isNull());
    break;

  case ConstraintKind::BindOverload:
    llvm_unreachable("Wrong constructor for overload binding constraint");

  case ConstraintKind::Disjunction:
    llvm_unreachable("Disjunction constraints should use create()");

  case ConstraintKind::Conjunction:
    llvm_unreachable("Conjunction constraints should use create()");

  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
    llvm_unreachable("Key path constraint takes three types");

  case ConstraintKind::SyntacticElement:
    llvm_unreachable("Syntactic element constraint should use create()");
  }

  std::uninitialized_copy(typeVars.begin(), typeVars.end(),
                          getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind Kind, Type First, Type Second, Type Third,
                       ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(Kind), HasRestriction(false), IsActive(false), IsDisabled(false),
      IsDisabledForPerformance(false), RememberChoice(false), IsFavored(false),
      IsIsolated(false),
      NumTypeVariables(typeVars.size()), Types{First, Second, Third},
      Locator(locator) {
  switch (Kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::Equal:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Subtype:
  case ConstraintKind::Conversion:
  case ConstraintKind::BridgingConversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::TransitivelyConformsTo:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::DynamicCallableApplicableFunction:
  case ConstraintKind::ValueMember:
  case ConstraintKind::ValueWitness:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::Defaultable:
  case ConstraintKind::BindOverload:
  case ConstraintKind::Disjunction:
  case ConstraintKind::Conjunction:
  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
  case ConstraintKind::FallbackType:
  case ConstraintKind::UnresolvedMemberChainBase:
  case ConstraintKind::PropertyWrapper:
  case ConstraintKind::SyntacticElement:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::SameShape:
  case ConstraintKind::MaterializePackExpansion:
    llvm_unreachable("Wrong constructor");

  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
    assert(!First.isNull());
    assert(!Second.isNull());
    assert(!Third.isNull());
    break;
  }

  std::uninitialized_copy(typeVars.begin(), typeVars.end(),
                          getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind kind, Type first, Type second,
                       DeclNameRef member, DeclContext *useDC,
                       FunctionRefKind functionRefKind,
                       ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(kind), HasRestriction(false), IsActive(false), IsDisabled(false),
      IsDisabledForPerformance(false), RememberChoice(false), IsFavored(false),
      IsIsolated(false),
      NumTypeVariables(typeVars.size()), Member{first, second, {member}, useDC},
      Locator(locator) {
  assert(kind == ConstraintKind::ValueMember ||
         kind == ConstraintKind::UnresolvedValueMember);
  TheFunctionRefKind = static_cast<unsigned>(functionRefKind);
  assert(getFunctionRefKind() == functionRefKind);
  assert(member && "Member constraint has no member");
  assert(useDC && "Member constraint has no use DC");

  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind kind, Type first, Type second,
                       ValueDecl *requirement, DeclContext *useDC,
                       FunctionRefKind functionRefKind,
                       ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(kind), HasRestriction(false), IsActive(false), IsDisabled(false),
      IsDisabledForPerformance(false), RememberChoice(false), IsFavored(false),
      IsIsolated(false), NumTypeVariables(typeVars.size()), Locator(locator) {
  Member.First = first;
  Member.Second = second;
  Member.Member.Ref = requirement;
  Member.UseDC = useDC;
  TheFunctionRefKind = static_cast<unsigned>(functionRefKind);

  assert(kind == ConstraintKind::ValueWitness);
  assert(getFunctionRefKind() == functionRefKind);
  assert(requirement && "Value witness constraint has no requirement");
  assert(useDC && "Member constraint has no use DC");

  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(Type type, OverloadChoice choice, DeclContext *useDC,
                       ConstraintFix *fix, ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(ConstraintKind::BindOverload), TheFix(fix), HasRestriction(false),
      IsActive(false), IsDisabled(bool(fix)), IsDisabledForPerformance(false),
      RememberChoice(false), IsFavored(false), IsIsolated(false),
      NumTypeVariables(typeVars.size()), Overload{type, choice, useDC},
      Locator(locator) {
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind kind,
                       ConversionRestrictionKind restriction, Type first,
                       Type second, ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(kind), Restriction(restriction), HasRestriction(true),
      IsActive(false), IsDisabled(false), IsDisabledForPerformance(false),
      RememberChoice(false), IsFavored(false), IsIsolated(false),
      NumTypeVariables(typeVars.size()), Types{first, second, Type()},
      Locator(locator) {
  assert(!first.isNull());
  assert(!second.isNull());
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind kind, ConstraintFix *fix, Type first,
                       Type second, ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(kind), TheFix(fix), HasRestriction(false), IsActive(false),
      IsDisabled(false), IsDisabledForPerformance(false), RememberChoice(false),
      IsFavored(false), IsIsolated(false),
      NumTypeVariables(typeVars.size()), Types{first, second, Type()},
      Locator(locator) {
  assert(!first.isNull());
  assert(!second.isNull());
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ASTNode node, ContextualTypeInfo context,
                       bool isDiscarded, ConstraintLocator *locator,
                       SmallPtrSetImpl<TypeVariableType *> &typeVars)
    : Kind(ConstraintKind::SyntacticElement), TheFix(nullptr),
      HasRestriction(false), IsActive(false), IsDisabled(false),
      IsDisabledForPerformance(false), RememberChoice(false), IsFavored(false),
      IsIsolated(false),
      NumTypeVariables(typeVars.size()), SyntacticElement{node, context,
                                                          isDiscarded},
      Locator(locator) {
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

ProtocolDecl *Constraint::getProtocol() const {
  assert((Kind == ConstraintKind::ConformsTo ||
          Kind == ConstraintKind::LiteralConformsTo ||
          Kind == ConstraintKind::SelfObjectOfProtocol ||
          Kind == ConstraintKind::TransitivelyConformsTo)
          && "Not a conformance constraint");
  return Types.Second->castTo<ProtocolType>()->getDecl();
}

Constraint *Constraint::clone(ConstraintSystem &cs) const {
  switch (getKind()) {
  case ConstraintKind::Bind:
  case ConstraintKind::Equal:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Subtype:
  case ConstraintKind::Conversion:
  case ConstraintKind::BridgingConversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::TransitivelyConformsTo:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::DynamicCallableApplicableFunction:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::Defaultable:
  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
  case ConstraintKind::FallbackType:
  case ConstraintKind::UnresolvedMemberChainBase:
  case ConstraintKind::PropertyWrapper:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::SameShape:
  case ConstraintKind::MaterializePackExpansion:
    return create(cs, getKind(), getFirstType(), getSecondType(), getLocator());

  case ConstraintKind::ApplicableFunction:
    return createApplicableFunction(
        cs, getFirstType(), getSecondType(), getTrailingClosureMatching(),
        getLocator());

  case ConstraintKind::BindOverload:
    return createBindOverload(cs, getFirstType(), getOverloadChoice(),
                              getOverloadUseDC(), getLocator());

  case ConstraintKind::ValueMember:
  case ConstraintKind::UnresolvedValueMember:
    return createMember(cs, getKind(), getFirstType(), getSecondType(),
                        getMember(), getMemberUseDC(), getFunctionRefKind(),
                        getLocator());

  case ConstraintKind::ValueWitness:
    return createValueWitness(
        cs, getKind(), getFirstType(), getSecondType(), getRequirement(),
        getMemberUseDC(), getFunctionRefKind(), getLocator());

  case ConstraintKind::Disjunction:
    return createDisjunction(
        cs, getNestedConstraints(), getLocator(),
        static_cast<RememberChoice_t>(shouldRememberChoice()));

  case ConstraintKind::Conjunction:
    return createConjunction(cs, getNestedConstraints(), IsIsolated,
                             getLocator(), getTypeVariables());

  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
    return create(cs, getKind(), getFirstType(), getSecondType(), getThirdType(),
                  getLocator());

  case ConstraintKind::SyntacticElement:
    return createSyntacticElement(cs, getSyntacticElement(), getLocator(),
                                  isDiscardedElement());
  }

  llvm_unreachable("Unhandled ConstraintKind in switch.");
}

void Constraint::print(llvm::raw_ostream &Out, SourceManager *sm,
                       unsigned indent, bool skipLocator) const {
  // Print all type variables as $T0 instead of _ here.
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;

  if (Kind == ConstraintKind::Disjunction ||
      Kind == ConstraintKind::Conjunction) {
    Out << (Kind == ConstraintKind::Disjunction ? "disjunction"
                                                : "conjunction");
    if (shouldRememberChoice())
      Out << " (remembered)";

    if (isIsolated())
      Out << " (isolated)";

    if (Locator) {
      Out << " @ ";
      Locator->dump(sm, Out);
    }
    Out << ":\n";
    
    // Sort constraints by favored, unmarked, disabled
    // for printing only.
    std::vector<Constraint *> sortedConstraints(getNestedConstraints().begin(),
                                                getNestedConstraints().end());
    llvm::sort(sortedConstraints,
               [](const Constraint *lhs, const Constraint *rhs) {
                 if (lhs->isFavored() != rhs->isFavored())
                   return lhs->isFavored();
                 if (lhs->isDisabled() != rhs->isDisabled())
                   return rhs->isDisabled();
                 return false;
               });

    interleave(
        sortedConstraints,
        [&](Constraint *constraint) {
          Out.indent(indent + 2);
          if (constraint->isDisabled())
            Out << ">  [disabled] ";
          else if (constraint->isFavored())
            Out << ">  [favored]  ";
          else
            Out << ">             ";
          constraint->print(Out, sm, indent,
                            /*skipLocator=*/constraint->getLocator() ==
                                Locator);
        },
        [&] { Out << "\n"; });
    return;
  }

  if (Kind == ConstraintKind::SyntacticElement) {
    auto *locator = getLocator();
    auto element = getSyntacticElement();

    if (auto patternBindingElt =
            locator
                ->getLastElementAs<LocatorPathElt::PatternBindingElement>()) {
      auto *patternBinding = cast<PatternBindingDecl>(element.get<Decl *>());
      Out << "pattern binding element @ ";
      Out << patternBindingElt->getIndex() << " : ";
      Out << '\n';
      patternBinding->getPattern(patternBindingElt->getIndex())->dump(Out, indent);
    } else {
      Out << "syntactic element ";
      Out << '\n';
      element.dump(Out, indent);
    }

    return;
  }

  Out << getFirstType()->getString(PO);

  bool skipSecond = false;

  switch (Kind) {
  case ConstraintKind::Bind: Out << " bind "; break;
  case ConstraintKind::Equal: Out << " equal "; break;
  case ConstraintKind::BindParam: Out << " bind param "; break;
  case ConstraintKind::BindToPointerType: Out << " bind to pointer "; break;
  case ConstraintKind::Subtype: Out << " subtype "; break;
  case ConstraintKind::Conversion: Out << " conv "; break;
  case ConstraintKind::BridgingConversion: Out << " bridging conv "; break;
  case ConstraintKind::ArgumentConversion: Out << " arg conv "; break;
  case ConstraintKind::OperatorArgumentConversion:
      Out << " operator arg conv "; break;
  case ConstraintKind::SubclassOf: Out << " subclass of "; break;
  case ConstraintKind::ConformsTo: Out << " conforms to "; break;
  case ConstraintKind::LiteralConformsTo: Out << " literal conforms to "; break;
  case ConstraintKind::TransitivelyConformsTo: Out << " transitive conformance to "; break;
  case ConstraintKind::CheckedCast: Out << " checked cast to "; break;
  case ConstraintKind::SelfObjectOfProtocol: Out << " Self type of "; break;
  case ConstraintKind::ApplicableFunction: Out << " applicable fn "; break;
  case ConstraintKind::DynamicCallableApplicableFunction:
      Out << " dynamic callable applicable fn "; break;
  case ConstraintKind::DynamicTypeOf: Out << " dynamicType type of "; break;
  case ConstraintKind::EscapableFunctionOf: Out << " @escaping type of "; break;
  case ConstraintKind::OpenedExistentialOf: Out << " opened archetype of "; break;
  case ConstraintKind::OneWayEqual: Out << " one-way bind to "; break;
  case ConstraintKind::OneWayBindParam: Out << " one-way bind param to "; break;
  case ConstraintKind::FallbackType:
    Out << " can fallback to ";
    break;
  case ConstraintKind::UnresolvedMemberChainBase:
    Out << " unresolved member chain base ";
    break;
  case ConstraintKind::PropertyWrapper:
    Out << " property wrapper with wrapped value of ";
    break;
  case ConstraintKind::KeyPath:
      Out << " key path from ";
      Out << getSecondType()->getString(PO);
      Out << " → ";
      Out << getThirdType()->getString(PO);
      skipSecond = true;
      break;

  case ConstraintKind::KeyPathApplication:
      Out << " key path projecting ";
      Out << getSecondType()->getString(PO);
      Out << " → ";
      Out << getThirdType()->getString(PO);
      skipSecond = true;
      break;
  case ConstraintKind::OptionalObject:
      Out << " optional with object type "; break;
  case ConstraintKind::BindOverload: {
    Out << " bound to ";
    auto overload = getOverloadChoice();
    auto printDecl = [&] {
      auto decl = overload.getDecl();
      decl->dumpRef(Out);
      Out << " : " << decl->getInterfaceType();
    };

    switch (overload.getKind()) {
    case OverloadChoiceKind::Decl:
      Out << "decl ";
      printDecl();
      break;
    case OverloadChoiceKind::DeclViaDynamic:
      Out << "decl-via-dynamic ";
      printDecl();
      break;
    case OverloadChoiceKind::DeclViaBridge:
      Out << "decl-via-bridge ";
      printDecl();
      break;
    case OverloadChoiceKind::DeclViaUnwrappedOptional:
      Out << "decl-via-unwrapped-optional ";
      printDecl();
      break;
    case OverloadChoiceKind::DynamicMemberLookup:
    case OverloadChoiceKind::KeyPathDynamicMemberLookup:
      Out << "dynamic member lookup '" << overload.getName() << "'";
      break;
    case OverloadChoiceKind::TupleIndex:
      Out << "tuple index " << overload.getTupleIndex();
      break;
    case OverloadChoiceKind::MaterializePack:
      Out << "materialize pack";
      break;
    case OverloadChoiceKind::KeyPathApplication:
      Out << "key path application";
      break;
    }

    skipSecond = true;
    break;
  }

  case ConstraintKind::ValueMember:
    Out << "[." << getMember() << ": value] == ";
    break;
  case ConstraintKind::UnresolvedValueMember:
    Out << "[(implicit) ." << getMember() << ": value] == ";
    break;

  case ConstraintKind::ValueWitness: {
    auto requirement = getRequirement();
    auto selfNominal = requirement->getDeclContext()->getSelfNominalTypeDecl();
    Out << "[." << selfNominal->getName() << "::" << requirement->getName()
        << ": witness] == ";
    break;
  }

  case ConstraintKind::Defaultable:
    Out << " can default to ";
    break;

  case ConstraintKind::BindTupleOfFunctionParams:
    Out << " bind tuple of function params to ";
    break;

  case ConstraintKind::PackElementOf:
    Out << " element of pack expansion pattern ";
    break;

  case ConstraintKind::ShapeOf:
    Out << " shape of ";
    break;

  case ConstraintKind::SameShape:
    Out << " same-shape ";
    break;

  case ConstraintKind::ExplicitGenericArguments:
    Out << " explicit generic argument binding ";
    break;

  case ConstraintKind::MaterializePackExpansion:
    Out << " materialize pack expansion ";
    break;

  case ConstraintKind::Disjunction:
    llvm_unreachable("disjunction handled above");
  case ConstraintKind::Conjunction:
    llvm_unreachable("conjunction handled above");
  case ConstraintKind::SyntacticElement:
    llvm_unreachable("syntactic element handled above");
  }

  if (!skipSecond)
    Out << getSecondType()->getString(PO);

  if (auto restriction = getRestriction()) {
    Out << ' ' << getName(*restriction);
  }

  if (getKind() == ConstraintKind::ApplicableFunction) {
    if (auto trailingClosureMatching = getTrailingClosureMatching()) {
      switch (*trailingClosureMatching) {
      case TrailingClosureMatching::Forward:
        Out << " [forward scan]";
        break;

      case TrailingClosureMatching::Backward:
        Out << " [backward scan]";
        break;
      }
    }
  }

  if (auto *fix = getFix()) {
    Out << ' ';
    fix->print(Out);
  }

  if (Locator && !skipLocator) {
    Out << " @ ";
    Locator->dump(sm, Out);
  }
}

void Constraint::dump(SourceManager *sm) const {
  print(llvm::errs(), sm);
  llvm::errs() << "\n";
}

void Constraint::dump(ConstraintSystem *CS) const {
  // Disable MSVC warning: only for use within the debugger.
#if SWIFT_COMPILER_IS_MSVC
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
  dump(&CS->getASTContext().SourceMgr);
#if SWIFT_COMPILER_IS_MSVC
#pragma warning(pop)
#endif
}


StringRef swift::constraints::getName(ConversionRestrictionKind kind) {
  switch (kind) {
  case ConversionRestrictionKind::DeepEquality:
    return "[deep equality]";
  case ConversionRestrictionKind::Superclass:
    return "[superclass]";
  case ConversionRestrictionKind::Existential:
    return "[existential]";
  case ConversionRestrictionKind::MetatypeToExistentialMetatype:
    return "[metatype-to-existential-metatype]";
  case ConversionRestrictionKind::ExistentialMetatypeToMetatype:
    return "[existential-metatype-to-metatype]";
  case ConversionRestrictionKind::ValueToOptional:
    return "[value-to-optional]";
  case ConversionRestrictionKind::OptionalToOptional:
    return "[optional-to-optional]";
  case ConversionRestrictionKind::ClassMetatypeToAnyObject:
    return "[class-metatype-to-object]";
  case ConversionRestrictionKind::ExistentialMetatypeToAnyObject:
    return "[existential-metatype-to-object]";
  case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass:
    return "[protocol-metatype-to-object]";
  case ConversionRestrictionKind::ArrayToPointer:
    return "[array-to-pointer]";
  case ConversionRestrictionKind::ArrayToCPointer:
    return "[array-to-c-pointer]";
  case ConversionRestrictionKind::StringToPointer:
    return "[string-to-pointer]";
  case ConversionRestrictionKind::InoutToPointer:
    return "[inout-to-pointer]";
  case ConversionRestrictionKind::InoutToCPointer:
    return "[inout-to-c-pointer]";
  case ConversionRestrictionKind::PointerToPointer:
    return "[pointer-to-pointer]";
  case ConversionRestrictionKind::PointerToCPointer:
    return "[pointer-to-c-pointer]";
  case ConversionRestrictionKind::ArrayUpcast:
    return "[array-upcast]";
  case ConversionRestrictionKind::DictionaryUpcast:
    return "[dictionary-upcast]";
  case ConversionRestrictionKind::SetUpcast:
    return "[set-upcast]";
  case ConversionRestrictionKind::HashableToAnyHashable:
    return "[hashable-to-anyhashable]";
  case ConversionRestrictionKind::CFTollFreeBridgeToObjC:
    return "[cf-toll-free-bridge-to-objc]";
  case ConversionRestrictionKind::ObjCTollFreeBridgeToCF:
    return "[objc-toll-free-bridge-to-cf]";
  case ConversionRestrictionKind::CGFloatToDouble:
    return "[CGFloat-to-Double]";
  case ConversionRestrictionKind::DoubleToCGFloat:
    return "[Double-to-CGFloat]";
  }
  llvm_unreachable("bad conversion restriction kind");
}

/// Recursively gather the set of type variables referenced by this constraint.
static void
gatherReferencedTypeVars(Constraint *constraint,
                         SmallPtrSetImpl<TypeVariableType *> &typeVars) {
  switch (constraint->getKind()) {
  case ConstraintKind::Disjunction:
    for (auto nested : constraint->getNestedConstraints())
      gatherReferencedTypeVars(nested, typeVars);
    return;

  case ConstraintKind::Conjunction:
    typeVars.insert(constraint->getTypeVariables().begin(),
                    constraint->getTypeVariables().end());
    return;

  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
    constraint->getThirdType()->getTypeVariables(typeVars);
    LLVM_FALLTHROUGH;

  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::DynamicCallableApplicableFunction:
  case ConstraintKind::Bind:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::Conversion:
  case ConstraintKind::BridgingConversion:
  case ConstraintKind::OperatorArgumentConversion:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::Equal:
  case ConstraintKind::Subtype:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::ValueMember:
  case ConstraintKind::ValueWitness:
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::Defaultable:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::TransitivelyConformsTo:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
  case ConstraintKind::FallbackType:
  case ConstraintKind::UnresolvedMemberChainBase:
  case ConstraintKind::PropertyWrapper:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::SameShape:
  case ConstraintKind::MaterializePackExpansion:
    constraint->getFirstType()->getTypeVariables(typeVars);
    constraint->getSecondType()->getTypeVariables(typeVars);
    break;

  case ConstraintKind::BindOverload:
    constraint->getFirstType()->getTypeVariables(typeVars);

    // Special case: the base type of an overloading binding.
    if (auto baseType = constraint->getOverloadChoice().getBaseType()) {
      baseType->getTypeVariables(typeVars);
    }

    break;

  case ConstraintKind::SyntacticElement:
    typeVars.insert(constraint->getTypeVariables().begin(),
                    constraint->getTypeVariables().end());
    break;
  }
}

unsigned Constraint::countResolvedArgumentTypes(ConstraintSystem &cs) const {
  auto *argumentFuncType = cs.getAppliedDisjunctionArgumentFunction(this);
  if (!argumentFuncType)
    return 0;

  return llvm::count_if(argumentFuncType->getParams(), [&](const AnyFunctionType::Param arg) {
    auto argType = cs.getFixedTypeRecursive(arg.getPlainType(), /*wantRValue=*/true);
    return !argType->isTypeVariableOrMember();
  });
}

bool Constraint::isExplicitConversion() const {
  assert(Kind == ConstraintKind::Disjunction);

  if (auto *locator = getLocator())
    return isExpr<CoerceExpr>(locator->getAnchor());

  return false;
}

Constraint *Constraint::create(ConstraintSystem &cs, ConstraintKind kind, 
                               Type first, Type second,
                               ConstraintLocator *locator,
                               ArrayRef<TypeVariableType *> extraTypeVars) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second && second->hasTypeVariable())
    second->getTypeVariables(typeVars);

  typeVars.insert(extraTypeVars.begin(), extraTypeVars.end());

  // Conformance constraints expect an existential on the right-hand side.
  assert((kind != ConstraintKind::ConformsTo &&
          kind != ConstraintKind::TransitivelyConformsTo &&
          kind != ConstraintKind::SelfObjectOfProtocol) ||
         second->isExistentialType());

  // Literal protocol conformances expect a protocol.
  assert((kind != ConstraintKind::LiteralConformsTo) ||
         second->is<ProtocolType>());

  // Create the constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return ::new (mem) Constraint(kind, first, second, locator, typeVars);
}

Constraint *Constraint::create(ConstraintSystem &cs, ConstraintKind kind,
                               Type first, Type second, Type third,
                               ConstraintLocator *locator,
                               ArrayRef<TypeVariableType *> extraTypeVars) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars(extraTypeVars.begin(),
                                              extraTypeVars.end());
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second->hasTypeVariable())
    second->getTypeVariables(typeVars);
  if (third->hasTypeVariable())
    third->getTypeVariables(typeVars);

  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return ::new (mem) Constraint(kind,
                                first, second, third,
                                locator, typeVars);
}

Constraint *Constraint::createMemberOrOuterDisjunction(
    ConstraintSystem &cs, ConstraintKind kind, Type first, Type second,
    DeclNameRef member, DeclContext *useDC, FunctionRefKind functionRefKind,
    ArrayRef<OverloadChoice> outerAlternatives, ConstraintLocator *locator) {
  auto memberConstraint = createMember(cs, kind, first, second, member,
                             useDC, functionRefKind, locator);

  if (outerAlternatives.empty())
    return memberConstraint;

  SmallVector<Constraint *, 4> constraints;
  constraints.push_back(memberConstraint);
  memberConstraint->setFavored();
  for (auto choice : outerAlternatives) {
    constraints.push_back(
        Constraint::createBindOverload(cs, first, choice, useDC, locator));
  }
  return Constraint::createDisjunction(cs, constraints, locator, ForgetChoice);
}

Constraint *Constraint::createMember(ConstraintSystem &cs, ConstraintKind kind, 
                                     Type first, Type second,
                                     DeclNameRef member, DeclContext *useDC,
                                     FunctionRefKind functionRefKind,
                                     ConstraintLocator *locator) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second->hasTypeVariable())
    second->getTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(kind, first, second, member, useDC,
                              functionRefKind, locator, typeVars);
}

Constraint *Constraint::createValueWitness(
    ConstraintSystem &cs, ConstraintKind kind, Type first, Type second,
    ValueDecl *requirement, DeclContext *useDC,
    FunctionRefKind functionRefKind, ConstraintLocator *locator) {
  assert(kind == ConstraintKind::ValueWitness);

  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second->hasTypeVariable())
    second->getTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(kind, first, second, requirement, useDC,
                              functionRefKind, locator, typeVars);
}

Constraint *Constraint::createBindOverload(ConstraintSystem &cs, Type type, 
                                           OverloadChoice choice, 
                                           DeclContext *useDC,
                                           ConstraintLocator *locator) {
  return createFixedChoice(cs, type, choice, useDC, /*fix=*/nullptr, locator);
}

Constraint *Constraint::createRestricted(ConstraintSystem &cs, 
                                         ConstraintKind kind, 
                                         ConversionRestrictionKind restriction,
                                         Type first, Type second, 
                                         ConstraintLocator *locator) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second->hasTypeVariable())
    second->getTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(kind, restriction, first, second, locator,
                              typeVars);
}

Constraint *Constraint::createFixed(ConstraintSystem &cs, ConstraintKind kind,
                                    ConstraintFix *fix, Type first, Type second,
                                    ConstraintLocator *locator) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second->hasTypeVariable())
    second->getTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(kind, fix, first, second, locator, typeVars);
}

Constraint *Constraint::createFixedChoice(ConstraintSystem &cs, Type type,
                                          OverloadChoice choice,
                                          DeclContext *useDC,
                                          ConstraintFix *fix,
                                          ConstraintLocator *locator) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (type->hasTypeVariable())
    type->getTypeVariables(typeVars);
  if (auto baseType = choice.getBaseType()) {
    baseType->getTypeVariables(typeVars);
  }

  // Create the constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType *>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(type, choice, useDC, fix, locator, typeVars);
}

Constraint *Constraint::createDisjunction(ConstraintSystem &cs,
                                          ArrayRef<Constraint *> constraints,
                                          ConstraintLocator *locator,
                                          RememberChoice_t rememberChoice) {
  // Unwrap any disjunctions inside the disjunction constraint; we only allow
  // disjunctions at the top level.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  bool unwrappedAny = false;
  SmallVector<Constraint *, 1> unwrapped;
  unsigned index = 0;
  for (auto constraint : constraints) {
    // Gather type variables from this constraint.
    gatherReferencedTypeVars(constraint, typeVars);

    // If we have a nested disjunction, unwrap it.
    if (constraint->getKind() == ConstraintKind::Disjunction) {
      // If we haven't unwrapped anything before, copy all of the constraints
      // we skipped.
      if (!unwrappedAny) {
        unwrapped.append(constraints.begin(), constraints.begin() + index);
        unwrappedAny = true;
      }

      // Add all of the constraints in the disjunction.
      unwrapped.append(constraint->getNestedConstraints().begin(),
                       constraint->getNestedConstraints().end());
    } else if (unwrappedAny) {
      // Since we unwrapped constraints before, add this constraint.
      unwrapped.push_back(constraint);
    }
    ++index;
  }

  // If we unwrapped anything, our list of constraints is the unwrapped list.
  if (unwrappedAny)
    constraints = unwrapped;

  assert(!constraints.empty() && "Empty disjunction constraint");

  // If there is a single constraint, this isn't a disjunction at all.
  if (constraints.size() == 1) {
    assert(!rememberChoice && "simplified an important disjunction?");
    return constraints.front();
  }

#ifndef NDEBUG
  assert(!constraints.empty());
  // Verify that all disjunction choices have the same left-hand side.
  Type commonType;
  assert(llvm::all_of(constraints, [&](const Constraint *choice) -> bool {
    // if this disjunction is formed from "fixed"
    // constraints let's not try to validate.
    if (choice->HasRestriction || choice->getFix())
      return true;

    auto currentType = choice->getFirstType();
    if (!commonType) {
      commonType = currentType;
      return true;
    }
    return commonType->isEqual(currentType);
  }));
#endif

  // Create the disjunction constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  auto disjunction = new (mem)
      Constraint(ConstraintKind::Disjunction, cs.allocateCopy(constraints),
                 /*isIsolated=*/false, locator, typeVars);
  disjunction->RememberChoice = (bool) rememberChoice;
  return disjunction;
}

Constraint *Constraint::createConjunction(
    ConstraintSystem &cs, ArrayRef<Constraint *> constraints, bool isIsolated,
    ConstraintLocator *locator, ArrayRef<TypeVariableType *> referencedVars) {
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  typeVars.insert(referencedVars.begin(), referencedVars.end());

  // Conjunctions don't gather constraints from either elements
  // because each have to be solved in isolation.

  assert(!constraints.empty() && "Empty conjunction constraint");
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  auto conjunction = new (mem)
      Constraint(ConstraintKind::Conjunction, cs.allocateCopy(constraints),
                 isIsolated, locator, typeVars);
  return conjunction;
}

Constraint *Constraint::createApplicableFunction(
    ConstraintSystem &cs, Type argumentFnType, Type calleeType,
    llvm::Optional<TrailingClosureMatching> trailingClosureMatching,
    ConstraintLocator *locator) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (argumentFnType->hasTypeVariable())
    argumentFnType->getTypeVariables(typeVars);
  if (calleeType->hasTypeVariable())
    calleeType->getTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = totalSizeToAlloc<TypeVariableType*>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  auto constraint = new (mem) Constraint(
      ConstraintKind::ApplicableFunction, argumentFnType, calleeType, locator,
      typeVars);

  // Encode the trailing closure matching.
  if (trailingClosureMatching) {
    switch (*trailingClosureMatching) {
      case TrailingClosureMatching::Forward:
        constraint->trailingClosureMatching = 1;
        break;

      case TrailingClosureMatching::Backward:
        constraint->trailingClosureMatching = 2;
        break;
    }
  } else {
    constraint->trailingClosureMatching = 0;
  }

  return constraint;
}

Constraint *Constraint::createSyntacticElement(ConstraintSystem &cs,
                                               ASTNode node,
                                               ConstraintLocator *locator,
                                               bool isDiscarded) {
  return createSyntacticElement(cs, node, ContextualTypeInfo(), locator,
                                isDiscarded);
}

Constraint *Constraint::createSyntacticElement(ConstraintSystem &cs,
                                               ASTNode node,
                                               ContextualTypeInfo context,
                                               ConstraintLocator *locator,
                                               bool isDiscarded) {
  // Collect type variables.
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  if (auto contextTy = context.getType())
    contextTy->getTypeVariables(typeVars);

  unsigned size = totalSizeToAlloc<TypeVariableType *>(typeVars.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(node, context, isDiscarded, locator, typeVars);
}

llvm::Optional<TrailingClosureMatching>
Constraint::getTrailingClosureMatching() const {
  assert(Kind == ConstraintKind::ApplicableFunction);
  switch (trailingClosureMatching) {
  case 0:
    return llvm::None;
  case 1: return TrailingClosureMatching::Forward;
  case 2: return TrailingClosureMatching::Backward;
  }

  llvm_unreachable("Bad trailing closure matching value");
}

void *Constraint::operator new(size_t bytes, ConstraintSystem& cs,
                               size_t alignment) {
  return ::operator new (bytes, cs, alignment);
}
