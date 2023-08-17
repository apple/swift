//===------------------------ ConstantnessSemaDiagnostics.cpp -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements checks for checking whether certain arguments to some
// specific APIs are compile-time constants (see below for the definition of
// constants). In particular, this code checks whether the new os_log APIs are
// invoked with constant arguments, and whether the primitive atomic operations
// are invoked with constant "orderings". These APIs are identified through
// @_semantics attributes.
//
// A "compile-time constant" is either a literal (including
// string/integer/float/boolean/string-interpolation literal) or a call to a
// "constant_evaluable" function (or property) with compile-time constant
// arguments. A closure expression is also considered a compile-time constant
// (it is a constant of a function type).
//===----------------------------------------------------------------------===//

#include "MiscDiagnostics.h"
#include "TypeChecker.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SemanticAttrs.h"
using namespace swift;

/// Check whether a given \p decl has a @_semantics attribute with the given
/// attribute name \c attrName.
static bool hasSemanticsAttr(ValueDecl *decl, StringRef attrName) {
  return decl->getAttrs().hasSemanticsAttr(attrName);
}

/// Return true iff  the given \p structDecl has a name that matches one of the
/// known atomic orderings structs.
static bool isAtomicOrderingDecl(StructDecl *structDecl) {
  ASTContext &astContext = structDecl->getASTContext();
  Identifier structName = structDecl->getName();
  return (structName == astContext.Id_AtomicLoadOrdering ||
          structName == astContext.Id_AtomicStoreOrdering ||
          structName == astContext.Id_AtomicUpdateOrdering);
}

/// Return true iff the given nominal type decl \p nominal has a name that
/// matches one of the known OSLog types that need not be a constant in the new
/// os_log APIs.
static bool isOSLogDynamicObject(NominalTypeDecl *nominal) {
  ASTContext &astContext = nominal->getASTContext();
  Identifier name = nominal->getName();
  return (name == astContext.Id_OSLog || name == astContext.Id_OSLogType);
}

/// Return true iff the parameter \p param of function \c funDecl is required to
/// be a constant. This is true if either the function is an os_log function or
/// it is an atomics operation and the parameter represents the ordering.
static bool isParamRequiredToBeConstant(AbstractFunctionDecl *funcDecl, ParamDecl *param) {
  assert(funcDecl && param && "funcDecl and param must not be null");
  Type paramType;
  NominalTypeDecl *nominal;
  StructDecl *structDecl;
  if (hasSemanticsAttr(funcDecl, semantics::OSLOG_REQUIRES_CONSTANT_ARGUMENTS))
    return true;
  if (hasSemanticsAttr(funcDecl, semantics::OSLOG_LOG_WITH_LEVEL)) {
    // We are looking at a top-level os_log function that accepts level and
    // possibly custom log object. Those need not be constants, but every other
    // parameter must be.
    paramType = param->getTypeInContext();
    nominal = paramType->getNominalOrBoundGenericNominal();
    return !nominal || !isOSLogDynamicObject(nominal);
  }
  if (!hasSemanticsAttr(funcDecl,
                        semantics::ATOMICS_REQUIRES_CONSTANT_ORDERINGS))
    return false;
  paramType = param->getTypeInContext();
  structDecl = paramType->getStructOrBoundGenericStruct();
  if (!structDecl)
    return false;
  return isAtomicOrderingDecl(structDecl);
}

/// Return true iff the \c decl is annotated as
/// @_semantics("constant_evaluable").
static bool hasConstantEvaluableAttr(ValueDecl *decl) {
  return hasSemanticsAttr(decl, semantics::CONSTANT_EVALUABLE);
}

/// Return true iff the \p decl is annotated with oslog.message.init semantics
/// attribute.
static bool isOSLogMessageInitializer(ValueDecl *decl) {
  return hasSemanticsAttr(decl, semantics::OSLOG_MESSAGE_INIT_STRING_LITERAL) ||
         hasSemanticsAttr(decl, semantics::OSLOG_MESSAGE_INIT_INTERPOLATION);
}

/// Check whether \p expr is a compile-time constant. It must either be a
/// literal_expr, which does not include array and dictionary literal, or a
/// closure expression, which is considered a compile-time constant of a
/// function type, or a call to a "constant_evaluable" function (or property)
/// whose arguments are themselves compile-time constants.
static Expr *checkConstantness(Expr *expr) {
  SmallVector<Expr *, 4> expressionsToCheck;
  expressionsToCheck.push_back(expr);
  while (!expressionsToCheck.empty()) {
    Expr *expr = expressionsToCheck.pop_back_val();
    // Lookthrough identity_expr, tuple, binary_expr and inject_into_optional expressions.
    if (IdentityExpr *identityExpr = dyn_cast<IdentityExpr>(expr)) {
      expressionsToCheck.push_back(identityExpr->getSubExpr());
      continue;
    }
    if (TupleExpr *tupleExpr = dyn_cast<TupleExpr>(expr)) {
      for (Expr *element : tupleExpr->getElements())
        expressionsToCheck.push_back(element);
      continue;
    }
    if (BinaryExpr *binaryExpr = dyn_cast<BinaryExpr>(expr)) {
      expressionsToCheck.push_back(binaryExpr->getLHS());
      expressionsToCheck.push_back(binaryExpr->getRHS());
      continue;
    }
    if (InjectIntoOptionalExpr *optionalExpr =
            dyn_cast<InjectIntoOptionalExpr>(expr)) {
      expressionsToCheck.push_back(optionalExpr->getSubExpr());
      continue;
    }
    // Literal expressions also includes InterpolatedStringLiteralExpr.
    if (isa<LiteralExpr>(expr))
      continue;
    if (isa<TypeExpr>(expr))
      continue;
    // Closure expressions are always treated as constants. They are
    // constants of function types.
    if (isa<AbstractClosureExpr>(expr))
      continue;
    // Default argument expressions of a constant_evaluable or a
    // requires_constant function must be ensured to be a constant by the
    // definition of the function.
    if (isa<DefaultArgumentExpr>(expr))
      continue;

    // If this is a member-ref, it has to be annotated constant evaluable.
    if (MemberRefExpr *memberRef = dyn_cast<MemberRefExpr>(expr)) {
      if (ValueDecl *memberDecl = memberRef->getMember().getDecl()) {
        if (hasConstantEvaluableAttr(memberDecl))
          continue;
      }
      return expr;
    }

    // If this is a variable, it has to be a known constant parameter of the
    // enclosing function.
    if (DeclRefExpr *declRef = dyn_cast<DeclRefExpr>(expr)) {
      ValueDecl *decl = declRef->getDecl();
      if (!decl)
        return expr;
      ParamDecl *paramDecl = dyn_cast<ParamDecl>(decl);
      if (!paramDecl)
        return expr;
      Decl *declContext = paramDecl->getDeclContext()->getAsDecl();
      if (!declContext)
        return expr;
      AbstractFunctionDecl *funcDecl = dyn_cast<AbstractFunctionDecl>(declContext);
      if (!funcDecl || !isParamRequiredToBeConstant(funcDecl, paramDecl))
        return expr;
      continue;
    }

    if (!isa<ApplyExpr>(expr))
      return expr;

    ApplyExpr *apply = cast<ApplyExpr>(expr);
    ValueDecl *calledValue = apply->getCalledValue();
    if (!calledValue)
      return expr;

    // If this is an enum case, check whether the arguments are constants.
    if (isa<EnumElementDecl>(calledValue)) {
      for (auto arg : *apply->getArgs())
        expressionsToCheck.push_back(arg.getExpr());
      continue;
    }

    AbstractFunctionDecl *callee = dyn_cast<AbstractFunctionDecl>(calledValue);
    if (!callee)
      return expr;

    // If this is an application of OSLogMessage initializer, fail the check
    // as this type must be created from string interpolations.
    if (isOSLogMessageInitializer(callee))
      return expr;

    // If this is a constant_evaluable function, check whether the arguments are
    // constants.
    if (!hasConstantEvaluableAttr(callee))
      return expr;

    for (auto arg : *apply->getArgs())
      expressionsToCheck.push_back(arg.getExpr());
  }
  return nullptr;
}

/// Return true iff the given \p type is a Stdlib integer type.
static bool isIntegerType(Type type) {
  return type->isInt() || type->isInt8() || type->isInt16() ||
         type->isInt32() || type->isInt64() || type->isUInt() ||
         type->isUInt8() || type->isUInt16() || type->isUInt32() ||
         type->isUInt64();
}

/// Return true iff the given \p type is a Float type.
static bool isFloatType(Type type) {
  return type->isFloat() || type->isDouble() || type->isFloat80();
}

/// Given an error expression \p errorExpr, diagnose the error based on the type
/// of the expression. For instance, if the expression's type is expressible by
/// a literal e.g. integer, boolean etc. report that it must be a literal.
/// Otherwise, if the expression is a nominal type, report that it must be
/// static member of the type.
static void diagnoseError(Expr *errorExpr, const ASTContext &astContext,
                          AbstractFunctionDecl *funcDecl) {
  DiagnosticEngine &diags = astContext.Diags;
  Type exprType = errorExpr->getType();
  SourceLoc errorLoc = errorExpr->getLoc();

  // Diagnose atomics ordering related error here.
  if (hasSemanticsAttr(funcDecl,
                       semantics::ATOMICS_REQUIRES_CONSTANT_ORDERINGS)) {
    NominalTypeDecl *nominalDecl = exprType->getNominalOrBoundGenericNominal();
    if (!nominalDecl) {
      // This case should normally not happen. This is a safe guard against
      // possible mismatch between the atomics library and the compiler.
      diags.diagnose(errorLoc, diag::argument_must_be_constant);
    }
    diags.diagnose(errorLoc, diag::atomics_ordering_must_be_constant,
                   nominalDecl->getName());
    return;
  }

  // Diagnose os_log specific errors here.

  // Diagnose primitive stdlib types.
  if (exprType->isBool()) {
    diags.diagnose(errorLoc, diag::oslog_arg_must_be_bool_literal);
    return;
  }
  if (exprType->isString()) {
    diags.diagnose(errorLoc, diag::oslog_arg_must_be_string_literal);
    return;
  }
  if (isIntegerType(exprType)) {
    diags.diagnose(errorLoc, diag::oslog_arg_must_be_integer_literal);
    return;
  }
  if (isFloatType(exprType)) {
    diags.diagnose(errorLoc, diag::oslog_arg_must_be_float_literal);
    return;
  }
  if (exprType->is<MetatypeType>()) {
    diags.diagnose(errorLoc, diag::oslog_arg_must_be_metatype_literal);
    return;
  }
  if (exprType->is<AnyFunctionType>()) {
    diags.diagnose(errorLoc, diag::oslog_arg_must_be_closure);
    return;
  }
  if (EnumDecl *enumDecl = exprType->getEnumOrBoundGenericEnum()) {
    diags.diagnose(errorLoc, diag::oslog_arg_must_be_enum_case,
                   enumDecl->getName());
    return;
  }
  NominalTypeDecl *nominalDecl = exprType->getNominalOrBoundGenericNominal();
  if (!nominalDecl) {
    // This case should normally not happen. This is a safe guard against
    // possible mismatch between the os overlay and the compiler.
    diags.diagnose(errorLoc, diag::argument_must_be_constant);
    return;
  }
  // If this is OSLogMessage, it should be a string-interpolation literal.
  Identifier declName = nominalDecl->getName();
  if (declName == astContext.Id_OSLogMessage ||
      nominalDecl->hasSemanticsAttr(semantics::OSLOG_MESSAGE_TYPE)) {
    diags.diagnose(errorLoc, diag::oslog_message_must_be_string_interpolation);
    return;
  }
  diags.diagnose(errorLoc, diag::oslog_arg_must_be_type_member_access,
                 declName);
}

/// Given a call \c callExpr, if some or all of its arguments are required to be
/// constants, check that property on the arguments.
static void diagnoseConstantArgumentRequirementOfCall(const CallExpr *callExpr,
                                                      const ASTContext &ctx) {
  assert(callExpr && callExpr->getType() &&
         "callExpr should have a valid type");
  ValueDecl *calledDecl = callExpr->getCalledValue();
  if (!calledDecl || !isa<AbstractFunctionDecl>(calledDecl))
    return;
  AbstractFunctionDecl *callee = cast<AbstractFunctionDecl>(calledDecl);

  // Collect argument indices that are required to be constants.
  SmallVector<unsigned, 4> constantArgumentIndices;
  auto paramList = callee->getParameters();
  for (unsigned i = 0; i < paramList->size(); ++i) {
    ParamDecl *param = paramList->get(i);
    if (isParamRequiredToBeConstant(callee, param))
      constantArgumentIndices.push_back(i);
  }
  if (constantArgumentIndices.empty())
    return;

  // Check that the arguments at the constantArgumentIndices are constants.
  SmallVector<Expr *, 4> arguments;
  for (auto arg : *callExpr->getArgs())
    arguments.push_back(arg.getExpr());

  for (unsigned constantIndex : constantArgumentIndices) {
    assert(constantIndex < arguments.size() &&
           "constantIndex exceeds the number of arguments to the function");
    Expr *argument = arguments[constantIndex];
    Expr *errorExpr = checkConstantness(argument);
    if (errorExpr)
      diagnoseError(errorExpr, ctx, callee);
  }
}

void swift::diagnoseConstantArgumentRequirement(
    const Expr *expr, const DeclContext *declContext) {
  class ConstantReqCallWalker : public ASTWalker {
    DeclContext *DC;
    bool insideClosure;

  public:
    ConstantReqCallWalker(DeclContext *DC) : DC(DC), insideClosure(false) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    // Descend until we find a call expressions. Note that the input expression
    // could be an assign expression or another expression that contains the
    // call.
    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      // Handle closure expressions separately as we may need to
      // manually descend into the body.
      if (auto *closureExpr = dyn_cast<ClosureExpr>(expr)) {
        return walkToClosureExprPre(closureExpr);
      }

      if (!expr || isa<ErrorExpr>(expr) || !expr->getType())
        return Action::SkipChildren(expr);
      if (auto *callExpr = dyn_cast<CallExpr>(expr)) {
        diagnoseConstantArgumentRequirementOfCall(callExpr, DC->getASTContext());
      }
      return Action::Continue(expr);
    }

    PreWalkResult<Expr *> walkToClosureExprPre(ClosureExpr *closure) {
      DC = closure;
      insideClosure = true;
      return Action::Continue(closure);
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      if (auto *closureExpr = dyn_cast<ClosureExpr>(expr)) {
        // Reset the DeclContext to the outer scope if we descended
        // into a closure expr and check whether or not we are still
        // within a closure context.
        DC = closureExpr->getParent();
        insideClosure = isa<ClosureExpr>(DC);
      }
      return Action::Continue(expr);
    }
  };

  // We manually check closure bodies from their outer contexts,
  // so bail early if we are being called directly on expressions
  // inside of a closure body.
  if (isa<ClosureExpr>(declContext)) {
    return;
  }

  ConstantReqCallWalker walker(const_cast<DeclContext *>(declContext));
  const_cast<Expr *>(expr)->walk(walker);
}
