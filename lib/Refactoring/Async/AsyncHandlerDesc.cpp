//===----------------------------------------------------------------------===//
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

#include "AsyncRefactoring.h"
#include "swift/Basic/Assertions.h"

using namespace swift;
using namespace swift::refactoring::asyncrefactorings;

/// Whether the given type is (or conforms to) the stdlib Error type
static bool isErrorType(Type Ty, ModuleDecl *MD) {
  if (!Ty)
    return false;
  return !MD->checkConformance(Ty, Ty->getASTContext().getErrorDecl())
              .isInvalid();
}

AsyncHandlerDesc AsyncHandlerDesc::get(const ValueDecl *Handler,
                                       bool RequireName) {
  AsyncHandlerDesc HandlerDesc;
  if (auto Var = dyn_cast<VarDecl>(Handler)) {
    HandlerDesc.Handler = Var;
  } else if (auto Func = dyn_cast<AbstractFunctionDecl>(Handler)) {
    HandlerDesc.Handler = Func;
  } else {
    // The handler must be a variable or function
    return AsyncHandlerDesc();
  }

  // Callback must have a completion-like name
  if (RequireName && !isCompletionHandlerParamName(HandlerDesc.getNameStr()))
    return AsyncHandlerDesc();

  // Callback must be a function type and return void. Doesn't need to have
  // any parameters - may just be a "I'm done" callback
  auto *HandlerTy = HandlerDesc.getType()->getAs<AnyFunctionType>();
  if (!HandlerTy || !HandlerTy->getResult()->isVoid())
    return AsyncHandlerDesc();

  // Find the type of result in the handler (eg. whether it's a Result<...>,
  // just parameters, or nothing).
  auto HandlerParams = HandlerTy->getParams();
  if (HandlerParams.size() == 1) {
    auto ParamTy =
        HandlerParams.back().getPlainType()->getAs<BoundGenericType>();
    if (ParamTy && ParamTy->isResult()) {
      auto GenericArgs = ParamTy->getGenericArgs();
      assert(GenericArgs.size() == 2 && "Result should have two params");
      HandlerDesc.Type = HandlerType::RESULT;
      HandlerDesc.HasError = !GenericArgs.back()->isUninhabited();
    }
  }

  if (HandlerDesc.Type != HandlerType::RESULT) {
    // Only handle non-result parameters
    for (auto &Param : HandlerParams) {
      if (Param.getPlainType() && Param.getPlainType()->isResult())
        return AsyncHandlerDesc();
    }

    HandlerDesc.Type = HandlerType::PARAMS;
    if (!HandlerParams.empty()) {
      auto LastParamTy = HandlerParams.back().getParameterType();
      HandlerDesc.HasError = isErrorType(LastParamTy->getOptionalObjectType(),
                                         Handler->getModuleContext());
    }
  }

  return HandlerDesc;
}

const ValueDecl *AsyncHandlerDesc::getHandler() const {
  if (!Handler) {
    return nullptr;
  }
  if (auto Var = Handler.dyn_cast<const VarDecl *>()) {
    return Var;
  } else if (auto Func = Handler.dyn_cast<const AbstractFunctionDecl *>()) {
    return Func;
  } else {
    llvm_unreachable("Unknown handler type");
  }
}

StringRef AsyncHandlerDesc::getNameStr() const {
  if (auto Var = Handler.dyn_cast<const VarDecl *>()) {
    return Var->getNameStr();
  } else if (auto Func = Handler.dyn_cast<const AbstractFunctionDecl *>()) {
    return Func->getNameStr();
  } else {
    llvm_unreachable("Unknown handler type");
  }
}

swift::Type AsyncHandlerDesc::getType() const {
  if (auto Var = Handler.dyn_cast<const VarDecl *>()) {
    return Var->getTypeInContext();
  } else if (auto Func = Handler.dyn_cast<const AbstractFunctionDecl *>()) {
    auto Type = Func->getInterfaceType();
    // Undo the self curry thunk if we are referencing a member function.
    if (Func->hasImplicitSelfDecl()) {
      assert(Type->is<AnyFunctionType>());
      Type = Type->getAs<AnyFunctionType>()->getResult();
    }
    return Type;
  } else {
    llvm_unreachable("Unknown handler type");
  }
}

ArrayRef<AnyFunctionType::Param> AsyncHandlerDesc::params() const {
  auto Ty = getType()->getAs<AnyFunctionType>();
  assert(Ty && "Type must be a function type");
  return Ty->getParams();
}

ArrayRef<AnyFunctionType::Param> AsyncHandlerDesc::getSuccessParams() const {
  if (HasError && Type == HandlerType::PARAMS)
    return params().drop_back();
  return params();
}

std::optional<AnyFunctionType::Param> AsyncHandlerDesc::getErrorParam() const {
  if (HasError && Type == HandlerType::PARAMS)
    return params().back();
  return std::nullopt;
}

std::optional<swift::Type> AsyncHandlerDesc::getErrorType() const {
  if (HasError) {
    switch (Type) {
    case HandlerType::INVALID:
      return std::nullopt;
    case HandlerType::PARAMS:
      // The last parameter of the completion handler is the error param
      return params().back().getPlainType()->lookThroughSingleOptionalType();
    case HandlerType::RESULT:
      assert(
          params().size() == 1 &&
          "Result handler should have the Result type as the only parameter");
      auto ResultType =
          params().back().getPlainType()->getAs<BoundGenericType>();
      auto GenericArgs = ResultType->getGenericArgs();
      assert(GenericArgs.size() == 2 && "Result should have two params");
      // The second (last) generic parameter of the Result type is the error
      // type.
      return GenericArgs.back();
    }
  } else {
    return std::nullopt;
  }
}

CallExpr *AsyncHandlerDesc::getAsHandlerCall(ASTNode Node) const {
  if (!isValid())
    return nullptr;

  if (auto E = Node.dyn_cast<Expr *>()) {
    if (auto *CE = dyn_cast<CallExpr>(E->getSemanticsProvidingExpr())) {
      if (CE->getFn()->getReferencedDecl().getDecl() == getHandler()) {
        return CE;
      }
    }
  }
  return nullptr;
}

bool AsyncHandlerDesc::isAmbiguousCallToParamHandler(const CallExpr *CE) const {
  if (!HasError || Type != HandlerType::PARAMS) {
    // Only param handlers with an error can pass both an error AND a result.
    return false;
  }
  auto Args = CE->getArgs()->getArgExprs();
  if (!isa<NilLiteralExpr>(Args.back())) {
    // We've got an error parameter. If any of the success params is not nil,
    // the call is ambiguous.
    for (auto &Arg : Args.drop_back()) {
      if (!isa<NilLiteralExpr>(Arg)) {
        return true;
      }
    }
  }
  return false;
}

HandlerResult
AsyncHandlerDesc::extractResultArgs(const CallExpr *CE,
                                    bool ReturnErrorArgsIfAmbiguous) const {
  auto *ArgList = CE->getArgs();
  SmallVector<Argument, 2> Scratch(ArgList->begin(), ArgList->end());
  auto Args = llvm::ArrayRef(Scratch);

  if (Type == HandlerType::PARAMS) {
    bool IsErrorResult;
    if (isAmbiguousCallToParamHandler(CE)) {
      IsErrorResult = ReturnErrorArgsIfAmbiguous;
    } else {
      // If there's an error parameter and the user isn't passing nil to it,
      // assume this is the error path.
      IsErrorResult = (HasError && !isa<NilLiteralExpr>(Args.back().getExpr()));
    }
    if (IsErrorResult)
      return HandlerResult(Args.back(), true);

    // We can drop the args altogether if they're just Void.
    if (willAsyncReturnVoid())
      return HandlerResult();

    return HandlerResult(HasError ? Args.drop_back() : Args);
  } else if (Type == HandlerType::RESULT) {
    if (Args.size() != 1)
      return HandlerResult(Args);

    auto *ResultCE = dyn_cast<CallExpr>(Args[0].getExpr());
    if (!ResultCE)
      return HandlerResult(Args);

    auto *DSC = dyn_cast<DotSyntaxCallExpr>(ResultCE->getFn());
    if (!DSC)
      return HandlerResult(Args);

    auto *D =
        dyn_cast<EnumElementDecl>(DSC->getFn()->getReferencedDecl().getDecl());
    if (!D)
      return HandlerResult(Args);

    auto ResultArgList = ResultCE->getArgs();
    auto isFailure = D->getNameStr() == StringRef("failure");

    // We can drop the arg altogether if it's just Void.
    if (!isFailure && willAsyncReturnVoid())
      return HandlerResult();

    // Otherwise the arg gets the .success() or .failure() call dropped.
    return HandlerResult(ResultArgList->get(0), isFailure);
  }

  llvm_unreachable("Unhandled result type");
}

swift::Type
AsyncHandlerDesc::getSuccessParamAsyncReturnType(swift::Type Ty) const {
  switch (Type) {
  case HandlerType::PARAMS: {
    // If there's an Error parameter in the handler, the success branch can
    // be unwrapped.
    if (HasError)
      Ty = Ty->lookThroughSingleOptionalType();

    return Ty;
  }
  case HandlerType::RESULT: {
    // Result<T, U> maps to T.
    return Ty->castTo<BoundGenericType>()->getGenericArgs()[0];
  }
  case HandlerType::INVALID:
    llvm_unreachable("Invalid handler type");
  }
}

Identifier AsyncHandlerDesc::getAsyncReturnTypeLabel(size_t Index) const {
  assert(Index < getSuccessParams().size());
  if (getSuccessParams().size() <= 1) {
    // There can't be any labels if the async function doesn't return a tuple.
    return Identifier();
  } else {
    return getSuccessParams()[Index].getInternalLabel();
  }
}

ArrayRef<LabeledReturnType> AsyncHandlerDesc::getAsyncReturnTypes(
    SmallVectorImpl<LabeledReturnType> &Scratch) const {
  for (size_t I = 0; I < getSuccessParams().size(); ++I) {
    auto Ty = getSuccessParams()[I].getParameterType();
    Scratch.emplace_back(getAsyncReturnTypeLabel(I),
                         getSuccessParamAsyncReturnType(Ty));
  }
  return Scratch;
}

bool AsyncHandlerDesc::willAsyncReturnVoid() const {
  // If all of the success params will be converted to Void return types,
  // this will be a Void async function.
  return llvm::all_of(getSuccessParams(), [&](auto &param) {
    auto Ty = param.getParameterType();
    return getSuccessParamAsyncReturnType(Ty)->isVoid();
  });
}
