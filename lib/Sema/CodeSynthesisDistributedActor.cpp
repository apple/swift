//===--- CodeSynthesisDistributedActor.cpp --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "TypeCheckDistributed.h"

#include "CodeSynthesis.h"
#include "DerivedConformances.h"
#include "TypeChecker.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Availability.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

using namespace swift;

/******************************************************************************/
/************************ PROPERTY SYNTHESIS **********************************/
/******************************************************************************/

static VarDecl*
 lookupDistributedActorProperty(NominalTypeDecl *decl, DeclName name) {
   assert(decl && "decl was null");
   auto &C = decl->getASTContext();

   auto clazz = dyn_cast<ClassDecl>(decl);
   if (!clazz)
     return nullptr;

   auto refs = decl->lookupDirect(name);
   if (refs.size() != 1)
     return nullptr;

   auto var = dyn_cast<VarDecl>(refs.front());
   if (!var)
     return nullptr;

   Type expectedType = Type();
   if (name == C.Id_id) {
     expectedType = getDistributedActorIDType(decl);
   } else if (name == C.Id_actorSystem) {
     expectedType = getDistributedActorSystemType(decl);
   } else {
     llvm_unreachable("Unexpected distributed actor property lookup!");
   }
   if (!expectedType)
     return nullptr;

   if (!var->getInterfaceType()->isEqual(expectedType))
     return nullptr;

   return var;
 }

// Note: This would be nice to implement in DerivedConformanceDistributedActor,
// but we can't since those are lazily triggered and an implementation exists
// for the 'id' property because 'Identifiable.id' has an extension that impls
// it for ObjectIdentifier, and we have to instead emit this stored property.
//
// The "derived" mechanisms are not really geared towards emitting for
// what already has a witness.
static VarDecl *addImplicitDistributedActorIDProperty(
    ClassDecl *nominal) {
  if (!nominal || !nominal->isDistributedActor())
    return nullptr;

  auto &C = nominal->getASTContext();

  // ==== Synthesize and add 'id' property to the actor decl
  Type propertyType = getDistributedActorIDType(nominal);
  if (!propertyType || propertyType->hasError())
    return nullptr;

  auto *propDecl = new (C)
      VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
              SourceLoc(), C.Id_id, nominal);
  propDecl->setImplicit();
  propDecl->setSynthesized();
  propDecl->copyFormalAccessFrom(nominal, /*sourceIsParentContext*/ true);
  propDecl->setInterfaceType(propertyType);

  Pattern *propPat = NamedPattern::createImplicit(C, propDecl, propertyType);
  propPat = TypedPattern::createImplicit(C, propPat, propertyType);
  propPat->setType(propertyType);

  PatternBindingDecl *pbDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, propPat, /*InitExpr*/ nullptr,
      nominal);

  // mark as nonisolated, allowing access to it from everywhere
  propDecl->getAttrs().add(
      new (C) NonisolatedAttr(/*unsafe=*/false, /*implicit=*/true));
  // mark as @_compilerInitialized, since we synthesize the initializing
  // assignment during SILGen.
  propDecl->getAttrs().add(
      new (C) CompilerInitializedAttr(/*IsImplicit=*/true));

  // IMPORTANT: The `id` MUST be the first field of any distributed actor,
  // because when we allocate remote proxy instances, we don't allocate memory
  // for anything except the first two fields: id and actorSystem, so they
  // MUST be those fields.
  //
  // Their specific order also matters, because it is enforced this way in IRGen
  // and how we emit them in AST MUST match what IRGen expects or cross-module
  // things could be using wrong offsets and manifest as reading trash memory on
  // id or system accesses.
  nominal->addMember(propDecl, /*hint=*/nullptr, /*insertAtHead=*/true);
  nominal->addMember(pbDecl, /*hint=*/nullptr, /*insertAtHead=*/true);
  return propDecl;
}

static VarDecl *addImplicitDistributedActorActorSystemProperty(
    ClassDecl *nominal) {
  if (!nominal)
    return nullptr;
  if (!nominal->isDistributedActor())
    return nullptr;

  auto &C = nominal->getASTContext();

  // ==== Synthesize and add 'actorSystem' property to the actor decl
  Type propertyType = getDistributedActorSystemType(nominal);

  auto *propDecl = new (C)
      VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
              SourceLoc(), C.Id_actorSystem, nominal);
  propDecl->setImplicit();
  propDecl->setSynthesized();
  propDecl->copyFormalAccessFrom(nominal, /*sourceIsParentContext*/ true);
  propDecl->setInterfaceType(propertyType);

  Pattern *propPat = NamedPattern::createImplicit(C, propDecl, propertyType);
  propPat = TypedPattern::createImplicit(C, propPat, propertyType);
  propPat->setType(propertyType);

  PatternBindingDecl *pbDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, propPat, /*InitExpr*/ nullptr,
      nominal);

  // mark as nonisolated, allowing access to it from everywhere
  propDecl->getAttrs().add(
      new (C) NonisolatedAttr(/*unsafe=*/false, /*implicit=*/true));

  auto idProperty = nominal->getDistributedActorIDProperty();
  // If the id was not yet synthesized, we need to ensure that eventually
  // the order of fields will be: id, actorSystem (because IRGen needs the
  // layouts to match with the AST we produce). We do this by inserting FIRST,
  // and then as the ID gets synthesized, it'll also force FIRST and therefore
  // the order will be okey -- ID and then system.
  auto insertAtHead = idProperty == nullptr;

  // IMPORTANT: The `id` MUST be the first field of any distributed actor.
  // So we find the property and add the system AFTER it using the hint.
  //
  // If the `id` was not synthesized yet, we'll end up inserting at head,
  // but the id synthesis will force itself to be FIRST anyway, so it works out.
  nominal->addMember(propDecl, /*hint=*/idProperty, /*insertAtHead=*/insertAtHead);
  nominal->addMember(pbDecl, /*hint=*/idProperty, /*insertAtHead=*/insertAtHead);
  return propDecl;
}

/******************************************************************************/
/*********************** DISTRIBUTED THUNK SYNTHESIS **************************/
/******************************************************************************/

static void forwardParameters(AbstractFunctionDecl *afd,
                              SmallVectorImpl<Expr*> &forwardingParams) {
  auto &C = afd->getASTContext();
  for (auto param : *afd->getParameters()) {
    forwardingParams.push_back(new (C) DeclRefExpr(
        ConcreteDeclRef(param), DeclNameLoc(), /*implicit=*/true,
        swift::AccessSemantics::Ordinary,
        afd->mapTypeIntoContext(param->getInterfaceType())));
  }
}

/// Mangle the target thunk in a way that we can look up the appropriate record.
static llvm::StringRef
mangleDistributedThunkForAccessorRecordName(
    ASTContext &C, AbstractFunctionDecl *thunk) {
  Mangle::ASTMangler mangler;

  // default mangling
  auto mangled =
      C.AllocateCopy(mangler.mangleDistributedThunkRef(cast<FuncDecl>(thunk)));
  return mangled;
}

static std::pair<BraceStmt *, bool>
deriveBodyDistributed_thunk(AbstractFunctionDecl *thunk, void *context) {
  auto implicit = true;
  ASTContext &C = thunk->getASTContext();

  // mock locations, we're a thunk and don't really need detailed locations
  const SourceLoc sloc = SourceLoc();
  const DeclNameLoc dloc = DeclNameLoc();

  auto func = static_cast<FuncDecl *>(context);
  auto funcDC = func->getDeclContext();
  assert(funcDC->getSelfNominalTypeDecl() &&
         funcDC->getSelfNominalTypeDecl()->isDistributedActor() &&
         "Distributed function must be part of distributed actor");

  auto selfDecl = thunk->getImplicitSelfDecl();
  selfDecl->getAttrs().add(new (C) KnownToBeLocalAttr(implicit));

  // === return type
  Type returnTy = func->getResultInterfaceType();
  auto isVoidReturn = returnTy->isVoid();

  // === Type:
  StructDecl *RCT = C.getRemoteCallTargetDecl();
  assert(RCT && "Missing RemoteCalLTarget declaration");
  Type remoteCallTargetTy = RCT->getDeclaredInterfaceType();

  // === __isRemoteActor(self)
  ArgumentList *isRemoteArgs = ArgumentList::forImplicitSingle(
      C, /*label=*/Identifier(), new (C) DeclRefExpr(selfDecl, dloc, implicit));

  FuncDecl *isRemoteFn = C.getIsRemoteDistributedActor();
  assert(isRemoteFn && "Could not find 'is remote' function, is the "
                       "'_Distributed' module available?");
  auto isRemoteDeclRef =
      UnresolvedDeclRefExpr::createImplicit(C, isRemoteFn->getName());
  auto isRemote =
      CallExpr::createImplicit(C, isRemoteDeclRef, isRemoteArgs);

  // === local branch ----------------------------------------------------------
  BraceStmt *localBranchStmt;
  if (auto accessor = dyn_cast<AccessorDecl>(func)) {
    auto selfRefExpr = new (C) DeclRefExpr(selfDecl, dloc, implicit);

    auto var = accessor->getStorage();

    Expr *localPropertyAccess = new (C) MemberRefExpr(
        selfRefExpr, sloc, ConcreteDeclRef(var), dloc, implicit);
    localPropertyAccess =
        AwaitExpr::createImplicit(C, sloc, localPropertyAccess);
    if (accessor->hasThrows()) {
      localPropertyAccess =
          TryExpr::createImplicit(C, sloc, localPropertyAccess);
    }

    auto returnLocalPropertyAccess =
        ReturnStmt::createImplicit(C, sloc, localPropertyAccess);
    localBranchStmt =
        BraceStmt::create(C, sloc, {returnLocalPropertyAccess}, sloc, implicit);
  } else {
    // normal function
    auto selfRefExpr = new (C) DeclRefExpr(selfDecl, dloc, implicit);

    // -- forward arguments
    SmallVector<Expr*, 4> forwardingParams;
    forwardParameters(thunk, forwardingParams);
    auto funcRef = UnresolvedDeclRefExpr::createImplicit(C, func->getName());
    auto forwardingArgList = ArgumentList::forImplicitCallTo(funcRef->getName(), forwardingParams, C);
    auto funcDeclRef =
        UnresolvedDotExpr::createImplicit(C, selfRefExpr, func->getBaseName());

    Expr *localFuncCall = CallExpr::createImplicit(C, funcDeclRef, forwardingArgList);
    localFuncCall = AwaitExpr::createImplicit(C, sloc, localFuncCall);
    if (func->hasThrows()) {
      localFuncCall = TryExpr::createImplicit(C, sloc, localFuncCall);
    }
    auto returnLocalFuncCall =
        ReturnStmt::createImplicit(C, sloc, localFuncCall);

    localBranchStmt =
        BraceStmt::create(C, sloc, {returnLocalFuncCall}, sloc, implicit);
  }
  // === remote branch  --------------------------------------------------------
  SmallVector<ASTNode, 8> remoteBranchStmts;
  // --- self.actorSystem
  auto systemRefExpr =
      UnresolvedDotExpr::createImplicit(
          C, new (C) DeclRefExpr(selfDecl, dloc, implicit), //  TODO: make createImplicit
          C.Id_actorSystem);

  auto *systemVar = new (C) VarDecl(
      /*isStatic=*/false, VarDecl::Introducer::Let, sloc, C.Id_system, thunk);
  systemVar->setImplicit();
  systemVar->setSynthesized();

  Pattern *systemPattern = NamedPattern::createImplicit(C, systemVar);

  auto systemPB = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, systemPattern, systemRefExpr,
      thunk);

  remoteBranchStmts.push_back(systemPB);
  remoteBranchStmts.push_back(systemVar);

  // --- invocationEncoder = system.makeInvocationEncoder()
  auto *invocationVar =
      new (C) VarDecl(/*isStatic=*/false, VarDecl::Introducer::Var, sloc,
                      C.Id_invocation, thunk);
  invocationVar->setImplicit();
  invocationVar->setSynthesized();

  {
    Pattern *invocationPattern = NamedPattern::createImplicit(C, invocationVar);

    auto makeInvocationExpr = UnresolvedDotExpr::createImplicit(
        C, new (C) DeclRefExpr(ConcreteDeclRef(systemVar), dloc, implicit),
        DeclName(C.Id_makeInvocationEncoder));
    auto *makeInvocationArgs = ArgumentList::createImplicit(C, {});
    auto makeInvocationCallExpr =
        CallExpr::createImplicit(C, makeInvocationExpr, makeInvocationArgs);
    makeInvocationCallExpr->setThrows(nullptr);

    auto invocationEncoderPB = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, invocationPattern, makeInvocationCallExpr,
        thunk);
    remoteBranchStmts.push_back(invocationEncoderPB);
    remoteBranchStmts.push_back(invocationVar);
  }

  // --- Recording invocation details
  // -- recordGenericSubstitution(s)
  if (auto genEnv = thunk->getGenericEnvironment()) {
    auto recordGenericSubstitutionName =
        DeclName(C, C.Id_recordGenericSubstitution,
                 /*labels=*/{Identifier()});
    auto recordGenericSubstitutionDeclRef =
        UnresolvedDeclRefExpr::createImplicit(C, recordGenericSubstitutionName);

    for (auto genParamType : genEnv->getGenericParams()) {
      auto tyExpr = TypeExpr::createImplicit(genEnv->mapTypeIntoContext(genParamType), C);
      auto subTypeExpr = new (C) DotSelfExpr(
          tyExpr,
          sloc, sloc, tyExpr->getType());

      auto recordGenericSubArgsList =
          ArgumentList::forImplicitCallTo(
              recordGenericSubstitutionDeclRef->getName(),
              {subTypeExpr},
              C);

      Expr *recordGenericSub = CallExpr::createImplicit(
          C,
          UnresolvedDotExpr::createImplicit(
              C,
              new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc,
                                  implicit, AccessSemantics::Ordinary),
              recordGenericSubstitutionName),
          recordGenericSubArgsList);
      recordGenericSub = TryExpr::createImplicit(C, sloc, recordGenericSub);

      remoteBranchStmts.push_back(recordGenericSub);
    }
  }

  // -- recordArgument(s)
  {
    auto recordArgumentName = DeclName(C, C.Id_recordArgument,
                                       /*labels=*/{Identifier()});
    if (auto params = thunk->getParameters()) {
      if (params->begin())
      for (auto param : *params) {
        auto argumentName = param->getArgumentName().str();
        LiteralExpr *argumentLabelArg;
        if (argumentName.empty()) {
          argumentLabelArg = new (C) NilLiteralExpr(sloc, implicit);
        } else {
          argumentLabelArg =
              new (C) StringLiteralExpr(argumentName, SourceRange(), implicit);
        }
        auto parameterName = param->getParameterName().str();


        // --- Prepare the RemoteCallArgument<Value> for the argument
        auto argumentVarName = C.getIdentifier("_" + parameterName.str());
        StructDecl *RCA = C.getRemoteCallArgumentDecl();
        VarDecl *callArgVar =
            new (C) VarDecl(/*isStatic=*/false, VarDecl::Introducer::Let, sloc,
                            argumentVarName, thunk);
        callArgVar->setImplicit();
        callArgVar->setSynthesized();

        Pattern *callArgPattern = NamedPattern::createImplicit(C, callArgVar);

        auto remoteCallArgumentInitDecl =
            RCA->getDistributedRemoteCallArgumentInitFunction();
        auto boundRCAType = BoundGenericType::get(
            RCA, Type(), {thunk->mapTypeIntoContext(param->getInterfaceType())});
        auto remoteCallArgumentInitDeclRef =
            TypeExpr::createImplicit(boundRCAType, C);

        auto initCallArgArgs = ArgumentList::forImplicitCallTo(
            DeclNameRef(remoteCallArgumentInitDecl->getEffectiveFullName()),
            {
             // label:
             argumentLabelArg,
             // name:
             new (C) StringLiteralExpr(parameterName, SourceRange(), implicit),
             // _ argument:
             new (C) DeclRefExpr(
                 ConcreteDeclRef(param), dloc, implicit,
                 AccessSemantics::Ordinary,
                 thunk->mapTypeIntoContext(param->getInterfaceType()))
            },
            C);

        auto initCallArgCallExpr =
            CallExpr::createImplicit(C, remoteCallArgumentInitDeclRef, initCallArgArgs);

        auto callArgPB = PatternBindingDecl::createImplicit(
            C, StaticSpellingKind::None, callArgPattern, initCallArgCallExpr, thunk);

        remoteBranchStmts.push_back(callArgPB);
        remoteBranchStmts.push_back(callArgVar);

        /// --- Pass the argumentRepr to the recordArgument function
        auto recordArgArgsList = ArgumentList::forImplicitCallTo(
            DeclNameRef(recordArgumentName),
            {new (C) DeclRefExpr(ConcreteDeclRef(callArgVar), dloc, implicit,
                                 AccessSemantics::Ordinary)},
            C);

        auto tryRecordArgExpr = TryExpr::createImplicit(
            C, sloc,
            CallExpr::createImplicit(
                C,
                UnresolvedDotExpr::createImplicit(
                    C,
                    new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc,
                                        implicit, AccessSemantics::Ordinary),
                    recordArgumentName),
                recordArgArgsList));

        remoteBranchStmts.push_back(tryRecordArgExpr);
      }
    }
  }

  // -- recordErrorType
  if (func->hasThrows()) {
    auto recordErrorTypeName = DeclName(C, C.Id_recordErrorType,
                                        /*labels=*/{Identifier()});
    // Error.self
    auto errorDecl = C.getErrorDecl();
    auto *errorTypeExpr = new (C) DotSelfExpr(
        UnresolvedDeclRefExpr::createImplicit(C, errorDecl->getName()), sloc,
        sloc, errorDecl->getDeclaredInterfaceType());

    auto recordArgsList = ArgumentList::forImplicitCallTo(
        DeclNameRef(recordErrorTypeName), {errorTypeExpr}, C);
    auto tryRecordErrorTyExpr = TryExpr::createImplicit(
        C, sloc,
        CallExpr::createImplicit(
            C,
            UnresolvedDotExpr::createImplicit(
                C,
                new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc,
                                    implicit, AccessSemantics::Ordinary),
                recordErrorTypeName),
            recordArgsList));

    remoteBranchStmts.push_back(tryRecordErrorTyExpr);
  }

  // -- recordReturnType
  if (!isVoidReturn) {
    auto recordReturnTypeName = DeclName(C, C.Id_recordReturnType,
                                         /*labels=*/{Identifier()});

    // Result.self
    // Watch out and always map into thunk context
    auto resultType = thunk->mapTypeIntoContext(func->getResultInterfaceType());
    auto *metaTypeRef = TypeExpr::createImplicit(resultType, C);
    auto *resultTypeExpr =
        new (C) DotSelfExpr(metaTypeRef, sloc, sloc, resultType);

    auto recordArgsList = ArgumentList::forImplicitCallTo(
        DeclNameRef(recordReturnTypeName), {resultTypeExpr}, C);
    auto tryRecordReturnTyExpr = TryExpr::createImplicit(
        C, sloc,
        CallExpr::createImplicit(
            C,
            UnresolvedDotExpr::createImplicit(
                C,
                new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc,
                                    implicit, AccessSemantics::Ordinary),
                recordReturnTypeName),
            recordArgsList));

    remoteBranchStmts.push_back(tryRecordReturnTyExpr);
  }

  // -- doneRecording
  {
    DeclName doneRecordingName(C.Id_doneRecording);
    auto argsList =
        ArgumentList::forImplicitCallTo(DeclNameRef(doneRecordingName), {}, C);
    auto tryDoneRecordingExpr = TryExpr::createImplicit(
        C, sloc,
        CallExpr::createImplicit(
            C,
            UnresolvedDotExpr::createImplicit(
                C,
                new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc,
                                    implicit, AccessSemantics::Ordinary,
                                    invocationVar->getInterfaceType()),
                doneRecordingName),
            argsList));

    remoteBranchStmts.push_back(tryDoneRecordingExpr);
  }

  // === Prepare the 'RemoteCallTarget'
  auto *targetVar = new (C) VarDecl(
      /*isStatic=*/false, VarDecl::Introducer::Let, sloc, C.Id_target, thunk);

  {
    // --- Mangle the thunk name
    auto mangledAccessorRecordName =
        mangleDistributedThunkForAccessorRecordName(C, thunk);

    StringLiteralExpr *mangledTargetStringLiteral =
        new (C) StringLiteralExpr(mangledAccessorRecordName,
                                  SourceRange(), implicit);

    // --- let target = RemoteCallTarget(<mangled name>)
    targetVar->setInterfaceType(remoteCallTargetTy);
    targetVar->setImplicit();
    targetVar->setSynthesized();

    Pattern *targetPattern = NamedPattern::createImplicit(C, targetVar);

    auto remoteCallTargetInitDecl =
        RCT->getDistributedRemoteCallTargetInitFunction();
    auto remoteCallTargetInitDeclRef = UnresolvedDeclRefExpr::createImplicit(
        C, remoteCallTargetInitDecl->getEffectiveFullName());

    auto initTargetExpr = UnresolvedDeclRefExpr::createImplicit(
        C, RCT->getName());
    auto initTargetArgs = ArgumentList::forImplicitCallTo(
        remoteCallTargetInitDeclRef->getName(),
        {mangledTargetStringLiteral}, C);

    auto initTargetCallExpr =
        CallExpr::createImplicit(C, initTargetExpr, initTargetArgs);

    auto targetPB = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, targetPattern, initTargetCallExpr, thunk);

    remoteBranchStmts.push_back(targetPB);
    remoteBranchStmts.push_back(targetVar);
  }

  // === Make the 'remoteCall(Void)(...)'
  {
    DeclName remoteCallName;
    if (isVoidReturn) {
      remoteCallName =
          DeclName(C, C.Id_remoteCallVoid,
                   {C.Id_on, C.Id_target, C.Id_invocation, C.Id_throwing});
    } else {
      remoteCallName = DeclName(C, C.Id_remoteCall,
                                {C.Id_on, C.Id_target, C.Id_invocation,
                                 C.Id_throwing, C.Id_returning});
    }

    auto systemRemoteCallRef = UnresolvedDotExpr::createImplicit(
        C, new (C) DeclRefExpr(ConcreteDeclRef(systemVar), dloc, implicit),
        remoteCallName);

    SmallVector<Expr *, 5> args;
    // -- on actor: Act
    args.push_back(new (C) DeclRefExpr(selfDecl, dloc, implicit,
                                       swift::AccessSemantics::Ordinary,
                                       selfDecl->getInterfaceType()));
    // -- target: RemoteCallTarget
    args.push_back(new (C) DeclRefExpr(ConcreteDeclRef(targetVar), dloc, implicit,
                                       AccessSemantics::Ordinary,
                                       RCT->getDeclaredInterfaceType()));
    // -- invocation: inout InvocationEncoder
    args.push_back(new (C) InOutExpr(
        sloc,
        new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc, implicit,
                            AccessSemantics::Ordinary),
        Type(), implicit));

    // -- throwing: Err.Type
    if (func->hasThrows()) {
      // Error.self
      auto errorDecl = C.getErrorDecl();
      auto *errorTypeExpr = new (C) DotSelfExpr(
          UnresolvedDeclRefExpr::createImplicit(C, errorDecl->getName()), sloc,
          sloc, errorDecl->getDeclaredInterfaceType());

      args.push_back(errorTypeExpr);
    } else {
      // Never.self
      auto neverDecl = C.getNeverDecl();
      auto *neverTypeExpr = new (C) DotSelfExpr(
          UnresolvedDeclRefExpr::createImplicit(C, neverDecl->getName()), sloc,
          sloc, neverDecl->getDeclaredInterfaceType());
      args.push_back(neverTypeExpr);
    }

    // -- returning: Res.Type
    if (!isVoidReturn) {
      // Result.self
      auto resultType =
          func->mapTypeIntoContext(func->getResultInterfaceType());
      auto *metaTypeRef = TypeExpr::createImplicit(resultType, C);
      auto *resultTypeExpr =
          new (C) DotSelfExpr(metaTypeRef, sloc, sloc, resultType);

      args.push_back(resultTypeExpr);
    }

    assert(args.size() == (isVoidReturn ? 4 : 5));
    auto remoteCallArgs = ArgumentList::forImplicitCallTo(
        systemRemoteCallRef->getName(), args, C);

    Expr *remoteCallExpr =
        CallExpr::createImplicit(C, systemRemoteCallRef, remoteCallArgs);
    remoteCallExpr = AwaitExpr::createImplicit(C, sloc, remoteCallExpr);
    remoteCallExpr = TryExpr::createImplicit(C, sloc, remoteCallExpr);
    auto returnRemoteCall = ReturnStmt::createImplicit(C, sloc, remoteCallExpr);
    remoteBranchStmts.push_back(returnRemoteCall);
  }

  // ---------------------------------------------------------------------------
  auto remoteBranchStmt =
      BraceStmt::create(C, sloc, remoteBranchStmts, sloc, implicit);

  // ---------------------------------------------------------------------------
  // === if (isRemote(...) <remote branch> else <local branch>
  auto ifStmt = new (C) IfStmt(sloc, /*condition=*/isRemote,
                               /*then=*/remoteBranchStmt, sloc,
                               /*else=*/localBranchStmt, implicit, C);

  auto body = BraceStmt::create(C, sloc, {ifStmt}, sloc, implicit);
  return {body, /*isTypeChecked=*/false};
}

/// Create a new FuncDecl that has the same signature as the passed in func.
/// This is used both to create stub witnesses as well as distributed thunks.
///
/// \param DC The declaration context of the newly created function
static FuncDecl *createSameSignatureDistributedThunkDecl(DeclContext *DC,
                                                         FuncDecl *func) {
  auto &C = func->getASTContext();

  // --- Prepare generic parameters
  GenericParamList *genericParamList = nullptr;
  if (auto genericParams = func->getGenericParams()) {
    genericParamList = genericParams->clone(DC);
  }

  GenericSignature baseSignature = func->getGenericSignature();

  // --- Prepare parameters
  auto funcParams = func->getParameters();
  SmallVector<ParamDecl*, 2> paramDecls;
  for (unsigned i : indices(*func->getParameters())) {
    auto funcParam = funcParams->get(i);

    auto paramName = funcParam->getParameterName();
    // If internal name is empty it could only mean either
    // `_:` or `x _: ...`, so let's auto-generate a name
    // to be used in the body of a thunk.
    if (paramName.empty()) {
      paramName = C.getIdentifier("p" + llvm::utostr(i));
    }

    auto paramDecl = new (C)
        ParamDecl(SourceLoc(),
                  /*argumentNameLoc=*/SourceLoc(), funcParam->getArgumentName(),
                  /*parameterNameLoc=*/SourceLoc(), paramName, DC);

    paramDecl->setImplicit();
    paramDecl->setSending();
    paramDecl->setSpecifier(funcParam->getSpecifier());
    paramDecl->setInterfaceType(funcParam->getInterfaceType());

    paramDecls.push_back(paramDecl);
  }
  ParameterList *params = ParameterList::create(C, paramDecls);

  FuncDecl *thunk;
  if (auto accessor = dyn_cast<AccessorDecl>(func)) {
    auto accessorThunk = AccessorDecl::createImplicit(
        C, AccessorKind::DistributedGet,
        /*storage=*/accessor->getStorage(),
        /*async=*/true, /*throws=*/true, // since it's a distributed thunk
        /*thrownType=*/TypeLoc::withoutLoc(Type()),
        func->getResultInterfaceType(),
        DC);
    accessorThunk->setParameters(params);
    // An accessor does not have a name; the `var` does though,
    // and we'll be mangling the accessor based on the Storage name (the var)
    thunk = accessorThunk;
  } else {
    // Let's use the name of a 'distributed func'
    DeclName thunkName = func->getName();

    thunk = FuncDecl::createImplicit(
        C, swift::StaticSpellingKind::None,
        thunkName, SourceLoc(),
        /*async=*/true, /*throws=*/true, // since it's a distributed thunk
        /*thrownType=*/Type(),
        genericParamList,
        params, func->getResultInterfaceType(), DC);
  }
  thunk->setSynthesized(true);

  if (isa<ClassDecl>(DC))
    thunk->getAttrs().add(new (C) FinalAttr(/*isImplicit=*/true));

  thunk->setGenericSignature(baseSignature);
  thunk->copyFormalAccessFrom(func, /*sourceIsParentContext=*/false);

  thunk->setSynthesized(true);
  thunk->setDistributedThunk(true);
  thunk->getAttrs().add(
      new (C) NonisolatedAttr(/*unsafe=*/false, /*implicit=*/true));

  return thunk;
}

static FuncDecl *createDistributedThunkFunction(FuncDecl *func) {
  auto DC = func->getDeclContext();

  FuncDecl *thunk =
      createSameSignatureDistributedThunkDecl(DC, func);
  assert(thunk && "couldn't create a distributed thunk");

  // Protocol requirements don't have bodies.
  if (func->hasBody())
    thunk->setBodySynthesizer(deriveBodyDistributed_thunk, func);

  return thunk;
}

/******************************************************************************/
/*********************** CODABLE CONFORMANCE **********************************/
/******************************************************************************/

static NormalProtocolConformance*
addDistributedActorCodableConformance(
    ClassDecl *actor, ProtocolDecl *proto) {
  assert(proto->isSpecificProtocol(swift::KnownProtocolKind::Decodable) ||
         proto->isSpecificProtocol(swift::KnownProtocolKind::Encodable));
  auto &C = actor->getASTContext();
  auto module = actor->getParentModule();

  // === Only Distributed actors can gain this implicit conformance
  if (!actor->isDistributedActor()) {
    return nullptr;
  }

  // === Does the actor explicitly conform to the protocol already?
  auto explicitConformance =
      module->lookupConformance(actor->getInterfaceType(), proto);
  if (!explicitConformance.isInvalid()) {
    // ok, it was conformed explicitly -- let's not synthesize;
    return nullptr;
  }

  // Check whether we can infer conformance at all.
  if (auto *file = dyn_cast<FileUnit>(actor->getModuleScopeContext())) {
    switch (file->getKind()) {
    case FileUnitKind::Source:
      // Check what kind of source file we have.
      if (auto sourceFile = actor->getParentSourceFile()) {
        switch (sourceFile->Kind) {
        case SourceFileKind::Interface:
          return nullptr;

        case SourceFileKind::Library:
        case SourceFileKind::Main:
        case SourceFileKind::MacroExpansion:
        case SourceFileKind::SIL:
        case SourceFileKind::DefaultArgument:
          break;
        }
      }
      break;

    case FileUnitKind::Builtin:
    case FileUnitKind::SerializedAST:
    case FileUnitKind::Synthesized:
      // Explicitly-handled modules don't infer Sendable conformances.
      return nullptr;

    case FileUnitKind::ClangModule:
    case FileUnitKind::DWARFModule:
      // Infer conformances for imported modules.
      break;
    }
  } else {
    return nullptr;
  }

  auto conformance = C.getNormalConformance(
      actor->getDeclaredInterfaceType(), proto,
      actor->getLoc(), /*dc=*/actor,
      ProtocolConformanceState::Incomplete,
      /*isUnchecked=*/false,
      /*isPreconcurrency=*/false);
  conformance->setSourceKindAndImplyingConformance(
      ConformanceEntryKind::Synthesized, nullptr);
  actor->registerProtocolConformance(conformance, /*synthesized=*/true);
  return conformance;
}

/******************************************************************************/
/******************************************************************************/

void swift::assertRequiredSynthesizedPropertyOrder(ASTContext &Context,
                                                   NominalTypeDecl *nominal) {
#ifndef NDEBUG
  if (auto id = nominal->getDistributedActorIDProperty()) {
    if (auto system = nominal->getDistributedActorSystemProperty()) {
      if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
        if (auto unownedExecutor = classDecl->getUnownedExecutorProperty()) {
          int idIdx, actorSystemIdx, unownedExecutorIdx = 0;
          int idx = 0;
          for (auto member : nominal->getMembers()) {
            if (auto binding = dyn_cast<PatternBindingDecl>(member)) {
              if (binding->getSingleVar()->getName() == Context.Id_id) {
                idIdx = idx;
              } else if (binding->getSingleVar()->getName() ==
                         Context.Id_actorSystem) {
                actorSystemIdx = idx;
              } else if (binding->getSingleVar()->getName() ==
                         Context.Id_unownedExecutor) {
                unownedExecutorIdx = idx;
              }
              idx += 1;
            }
          }
          if (idIdx + actorSystemIdx + unownedExecutorIdx >= 0 + 1 + 2) {
            // we have found all the necessary fields, let's assert their order
            assert(idIdx < actorSystemIdx < unownedExecutorIdx &&
                   "order of fields MUST be exact.");
          }
        }
      }
    }
  }
#endif
}

static bool canSynthesizeDistributedThunk(AbstractFunctionDecl *distributedTarget) {
  // `distributed` protocol requirements are allowed without additional checks.
  if (isa<ProtocolDecl>(distributedTarget->getDeclContext()))
    return true;

  if (getConcreteReplacementForProtocolActorSystemType(distributedTarget)) {
    return true;
  }

  auto serializationTy =
      getDistributedActorSerializationType(distributedTarget->getDeclContext());
  return serializationTy && !serializationTy->hasDependentMember();
}

/******************************************************************************/
/*********************** SYNTHESIS ENTRY POINTS *******************************/
/******************************************************************************/

FuncDecl *GetDistributedThunkRequest::evaluate(Evaluator &evaluator,
                                               Originator originator) const {
  AbstractFunctionDecl *distributedTarget = nullptr;
  if (auto *storage = originator.dyn_cast<AbstractStorageDecl *>()) {
    if (!storage->isDistributed())
      return nullptr;

    if (auto *var = dyn_cast<VarDecl>(storage)) {
      if (checkDistributedActorProperty(var, /*diagnose=*/false))
        return nullptr;

      distributedTarget = var->getAccessor(AccessorKind::Get);
    } else {
      llvm_unreachable("unsupported storage kind");
    }
  } else {
    distributedTarget = originator.get<AbstractFunctionDecl *>();
    if (!distributedTarget->isDistributed())
      return nullptr;
  }
  assert(distributedTarget);

  // This evaluation type-check by now was already computed and cached;
  // We need to check in order to avoid emitting a THUNK for a distributed func
  // which had errors; as the thunk then may also cause un-addressable issues and confusion.
  if (swift::checkDistributedFunction(distributedTarget)) {
    return nullptr;
  }

  auto &C = distributedTarget->getASTContext();

  if (!canSynthesizeDistributedThunk(distributedTarget)) {
    return nullptr;
  }

  // If the target function signature has errors, or if it is illegal in other
  // ways, such as e.g. parameters not conforming to SerializationRequirement,
  // we must avoid synthesis of the thunk because it'd also have errors,
  // giving an ugly user experience (errors in implicit code).
  if (distributedTarget->getInterfaceType()->hasError() ||
      (!isa<AccessorDecl>(distributedTarget) &&
       checkDistributedFunction(distributedTarget))) {
    return nullptr;
  }

  if (auto func = dyn_cast<FuncDecl>(distributedTarget)) {
    // not via `ensureDistributedModuleLoaded` to avoid generating a warning,
    // we won't be emitting the offending decl after all.
    if (!C.getLoadedModule(C.Id_Distributed))
      return nullptr;

    // --- Prepare the "distributed thunk" which does the "maybe remote" dance:
    return createDistributedThunkFunction(func);
  }

  llvm_unreachable("Unable to synthesize distributed thunk");
}

VarDecl *GetDistributedActorIDPropertyRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *actor) const {
  if (!actor->isDistributedActor())
    return nullptr;

  auto &C = actor->getASTContext();

  // not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed))
    return nullptr;

  auto classDecl = dyn_cast<ClassDecl>(actor);
  if (!classDecl)
    return nullptr;

  // We may enter this request multiple times, e.g. in multi-file projects,
  // so in order to avoid synthesizing a property many times, first perform
  // a lookup and return if it already exists.
  if (auto existingProp = lookupDistributedActorProperty(classDecl, C.Id_id)) {
    return existingProp;
  }

  return addImplicitDistributedActorIDProperty(classDecl);
}

VarDecl *GetDistributedActorSystemPropertyRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *nominal) const {
  auto &C = nominal->getASTContext();
  auto module = nominal->getParentModule();

  auto DAS = C.getDistributedActorSystemDecl();

  // not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed))
    return nullptr;

  if (!nominal->isDistributedActor())
    return nullptr;

  if (auto proto = dyn_cast<ProtocolDecl>(nominal)) {
    auto DistributedActorProto = C.getDistributedActorDecl();
    for (auto system : DistributedActorProto->lookupDirect(C.Id_actorSystem)) {
      if (auto var = dyn_cast<VarDecl>(system)) {
        auto conformance = module->checkConformance(
            proto->mapTypeIntoContext(var->getInterfaceType()),
            DAS);

        if (conformance.isInvalid())
          continue;

        return var;
      }
    }

    return nullptr;
  }

  auto classDecl = dyn_cast<ClassDecl>(nominal);
  if (!classDecl)
    return nullptr;

  // We may be triggered after synthesis was handled via `DerivedConformances`,
  // in which case we should locate the existing property, rather than add
  // another one. Generally derived conformances are triggered early and are right
  // but for some reason sometimes we get a request before synthesis was triggered
  // there... so this is to workaround that issue, and ensure we're always
  // synthesising correctly, regardless of entry-point.
  if (auto existingProp = lookupDistributedActorProperty(classDecl, C.Id_actorSystem)) {
    return existingProp;
  }

  return addImplicitDistributedActorActorSystemProperty(classDecl);
}

NormalProtocolConformance *GetDistributedActorImplicitCodableRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *nominal,
    KnownProtocolKind protoKind) const {
  assert(nominal->isDistributedActor());
  assert(protoKind == KnownProtocolKind::Encodable ||
         protoKind == KnownProtocolKind::Decodable);
  auto &C = nominal->getASTContext();

  // not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed))
    return nullptr;

  auto classDecl = dyn_cast<ClassDecl>(nominal);
  if (!classDecl) {
    // we only synthesize the conformance for concrete actors
    return nullptr;
  }

  return addDistributedActorCodableConformance(classDecl,
                                               C.getProtocol(protoKind));
}

bool CanSynthesizeDistributedActorCodableConformanceRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *actor) const {

  if (actor && !isa<ClassDecl>(actor))
    return false;

  if (!actor->isDistributedActor())
    return false;

  auto systemTy = getConcreteReplacementForProtocolActorSystemType(actor);
  if (!systemTy)
    return false;

  if (!systemTy->getAnyNominal())
    return false;

  auto idTy = getDistributedActorSystemActorIDType(systemTy->getAnyNominal());
  if (!idTy)
    return false;

  return TypeChecker::conformsToKnownProtocol(
             idTy, KnownProtocolKind::Decodable, actor->getParentModule()) &&
         TypeChecker::conformsToKnownProtocol(
             idTy, KnownProtocolKind::Encodable, actor->getParentModule());
}

NormalProtocolConformance *
GetDistributedActorAsActorConformanceRequest::evaluate(
    Evaluator &evaluator, ProtocolDecl *distributedActorProto) const {
  auto &ctx = distributedActorProto->getASTContext();
  auto swiftModule = ctx.getStdlibModule();

  auto actorProto = ctx.getProtocol(KnownProtocolKind::Actor);

  auto ext = findDistributedActorAsActorExtension(
      distributedActorProto, swiftModule);
  if (!ext)
    return nullptr;

  auto genericParam = GenericTypeParamType::get(/*isParameterPack=*/false,
                                                /*depth=*/0, /*index=*/0, ctx);

  auto distributedActorAsActorConformance = ctx.getNormalConformance(
      Type(genericParam), actorProto, SourceLoc(), ext,
      ProtocolConformanceState::Incomplete, /*isUnchecked=*/false,
      /*isPreconcurrency=*/false);
  // NOTE: Normally we "register" a conformance, but here we don't
  // because we cannot (currently) register them in a protocol,
  // since they do not have conformance tables.

  return distributedActorAsActorConformance;
}
