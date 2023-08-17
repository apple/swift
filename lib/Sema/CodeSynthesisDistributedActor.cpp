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

#include "TypeChecker.h"
#include "TypeCheckType.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Availability.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/Basic/Defer.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "DerivedConformances.h"

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
  if (!nominal)
    return nullptr;
  if (!nominal->isDistributedActor())
    return nullptr;

  auto &C = nominal->getASTContext();

  // ==== Synthesize and add 'id' property to the actor decl
  Type propertyType = getDistributedActorIDType(nominal);

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
      new (C) NonisolatedAttr(/*IsImplicit=*/true));
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
      new (C) NonisolatedAttr(/*IsImplicit=*/true));

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

static std::pair<BraceStmt *, bool>
deriveBodyDistributed_thunk(AbstractFunctionDecl *thunk, void *context) {
  auto implicit = true;
  ASTContext &C = thunk->getASTContext();
  auto module = thunk->getParentModule();

  // mock locations, we're a thunk and don't really need detailed locations
  const SourceLoc sloc = SourceLoc();
  const DeclNameLoc dloc = DeclNameLoc();

  auto func = static_cast<FuncDecl *>(context);
  auto funcDC = func->getDeclContext();
  NominalTypeDecl *nominal = funcDC->getSelfNominalTypeDecl();
  assert(nominal && nominal->isDistributedActor() &&
         "Distributed function must be part of distributed actor");

  auto selfDecl = thunk->getImplicitSelfDecl();
  selfDecl->getAttrs().add(new (C) KnownToBeLocalAttr(implicit));

  // === return type
  Type returnTy = func->getResultInterfaceType();
  auto isVoidReturn = returnTy->isVoid();

  // === self.actorSystem
  ProtocolDecl *DAS = C.getDistributedActorSystemDecl();
  Type systemTy = getConcreteReplacementForProtocolActorSystemType(thunk);
  assert(systemTy && "distributed thunk can only be synthesized with concrete "
                     "actor system types");
  NominalTypeDecl *systemDecl = systemTy->getAnyNominal();
  assert(systemDecl);
  auto systemConfRef = module->lookupConformance(systemTy, DAS);
  assert(systemConfRef && "ActorSystem must conform to DistributedActorSystem");

  // === ActorSystem.InvocationEncoder
  Type invocationEncoderTy =
      getDistributedActorSystemInvocationEncoderType(systemDecl);
  NominalTypeDecl *invocationEncoderDecl = invocationEncoderTy->getAnyNominal();

  // === Type:
  StructDecl *RCT = C.getRemoteCallTargetDecl();
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

    auto returnLocalPropertyAccess = new (C) ReturnStmt(sloc, localPropertyAccess, implicit);
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
    auto returnLocalFuncCall = new (C) ReturnStmt(sloc, localFuncCall, implicit);

    localBranchStmt =
        BraceStmt::create(C, sloc, {returnLocalFuncCall}, sloc, implicit);
  }
  // === remote branch  --------------------------------------------------------
  SmallVector<ASTNode, 8> remoteBranchStmts;
  // --- self.actorSystem
  auto systemProperty = nominal->getDistributedActorSystemProperty();
  assert(systemProperty && "Unable to find 'actorSystem' property");
  auto systemRefExpr =
      UnresolvedDotExpr::createImplicit(
          C, new (C) DeclRefExpr(selfDecl, dloc, implicit), //  TODO: make createImplicit
          C.Id_actorSystem);

  auto *systemVar =
      new (C) VarDecl(/*isStatic=*/false, VarDecl::Introducer::Let, sloc,
                      C.Id_system, thunk);
  systemVar->setInterfaceType(systemProperty->getInterfaceType());
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
  invocationVar->setInterfaceType(invocationEncoderTy);
  invocationVar->setImplicit();
  invocationVar->setSynthesized();

  {
    Pattern *invocationPattern = NamedPattern::createImplicit(C, invocationVar);

    FuncDecl *makeInvocationEncoderDecl =
        C.getMakeInvocationEncoderOnDistributedActorSystem(func);
    auto makeInvocationExpr = UnresolvedDotExpr::createImplicit(
        C, new (C) DeclRefExpr(ConcreteDeclRef(systemVar), dloc, implicit),
        makeInvocationEncoderDecl->getName());
    auto *makeInvocationArgs = ArgumentList::createImplicit(C, {});
    auto makeInvocationCallExpr =
        CallExpr::createImplicit(C, makeInvocationExpr, makeInvocationArgs);
    makeInvocationCallExpr->setThrows(false);

    auto invocationEncoderPB = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, invocationPattern, makeInvocationCallExpr,
        thunk);
    remoteBranchStmts.push_back(invocationEncoderPB);
    remoteBranchStmts.push_back(invocationVar);
  }

  // --- Recording invocation details
  // -- recordGenericSubstitution(s)
  if (thunk->isGeneric() || nominal->isGeneric()) {
    auto recordGenericSubstitutionDecl =
        C.getRecordGenericSubstitutionOnDistributedInvocationEncoder(invocationEncoderDecl);
    assert(recordGenericSubstitutionDecl);
    auto recordGenericSubstitutionDeclRef =
        UnresolvedDeclRefExpr::createImplicit(
            C, recordGenericSubstitutionDecl->getName());

    auto signature = thunk->getGenericSignature();
    for (auto genParamType : signature.getGenericParams()) {

      auto tyExpr = TypeExpr::createImplicit(thunk->mapTypeIntoContext(genParamType), C);
      auto subTypeExpr = new (C) DotSelfExpr(
          tyExpr,
          sloc, sloc, tyExpr->getType());

      auto recordGenericSubArgsList =
          ArgumentList::forImplicitCallTo(
              recordGenericSubstitutionDeclRef->getName(),
              {subTypeExpr},
              C);

      Expr *recordGenericSub =
          CallExpr::createImplicit(C,
              UnresolvedDotExpr::createImplicit(C,
                  new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc, implicit, AccessSemantics::Ordinary),
                  recordGenericSubstitutionDecl->getName()),
                  recordGenericSubArgsList);
      recordGenericSub = TryExpr::createImplicit(C, sloc, recordGenericSub);

      remoteBranchStmts.push_back(recordGenericSub);
    }
  }

  // -- recordArgument(s)
  {
    auto recordArgumentDecl =
        C.getRecordArgumentOnDistributedInvocationEncoder(invocationEncoderDecl);
    assert(recordArgumentDecl);

    for (auto param : *thunk->getParameters()) {
      auto recordArgumentDeclRef = UnresolvedDeclRefExpr::createImplicit(
          C, recordArgumentDecl->getName());

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
      initCallArgCallExpr->setImplicit();

      auto callArgPB = PatternBindingDecl::createImplicit(
          C, StaticSpellingKind::None, callArgPattern, initCallArgCallExpr, thunk);

      remoteBranchStmts.push_back(callArgPB);
      remoteBranchStmts.push_back(callArgVar);

      /// --- Pass the argumentRepr to the recordArgument function
      auto recordArgArgsList = ArgumentList::forImplicitCallTo(
          recordArgumentDeclRef->getName(),
          {
              new (C) DeclRefExpr(
                  ConcreteDeclRef(callArgVar), dloc, implicit,
                  AccessSemantics::Ordinary)
          }, C);

      auto tryRecordArgExpr = TryExpr::createImplicit(C, sloc,
        CallExpr::createImplicit(C,
          UnresolvedDotExpr::createImplicit(C,
          new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc, implicit, AccessSemantics::Ordinary),
          recordArgumentDecl->getName()),
          recordArgArgsList));

      remoteBranchStmts.push_back(tryRecordArgExpr);
    }
  }

  // -- recordErrorType
  if (func->hasThrows()) {
    // Error.self
    auto errorDecl = C.getErrorDecl();
    auto *errorTypeExpr = new (C) DotSelfExpr(
        UnresolvedDeclRefExpr::createImplicit(C, errorDecl->getName()), sloc,
        sloc, errorDecl->getDeclaredInterfaceType());

    auto recordErrorDecl = C.getRecordErrorTypeOnDistributedInvocationEncoder(
        invocationEncoderDecl);
    assert(recordErrorDecl);
    auto recordErrorDeclRef =
        UnresolvedDeclRefExpr::createImplicit(C, recordErrorDecl->getName());

    auto recordArgsList = ArgumentList::forImplicitCallTo(
        recordErrorDeclRef->getName(),
        {errorTypeExpr},
        C);
    auto tryRecordErrorTyExpr = TryExpr::createImplicit(C, sloc,
      CallExpr::createImplicit(C,
        UnresolvedDotExpr::createImplicit(C,
          new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc, implicit, AccessSemantics::Ordinary),
          recordErrorDecl->getName()),
        recordArgsList));

    remoteBranchStmts.push_back(tryRecordErrorTyExpr);
  }

  // -- recordReturnType
  if (!isVoidReturn) {
    // Result.self
    // Watch out and always map into thunk context
    auto resultType = thunk->mapTypeIntoContext(func->getResultInterfaceType());
    auto *metaTypeRef = TypeExpr::createImplicit(resultType, C);
    auto *resultTypeExpr =
        new (C) DotSelfExpr(metaTypeRef, sloc, sloc, resultType);

    auto recordReturnTypeDecl =
        C.getRecordReturnTypeOnDistributedInvocationEncoder(
            invocationEncoderDecl);
    auto recordReturnTypeDeclRef =
        UnresolvedDeclRefExpr::createImplicit(C, recordReturnTypeDecl->getName());

    auto recordArgsList = ArgumentList::forImplicitCallTo(
        recordReturnTypeDeclRef->getName(),
        {resultTypeExpr},
        C);
    auto tryRecordReturnTyExpr = TryExpr::createImplicit(C, sloc,
      CallExpr::createImplicit(C,
        UnresolvedDotExpr::createImplicit(C,
          new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc, implicit,
                              AccessSemantics::Ordinary),
          recordReturnTypeDecl->getName()),
        recordArgsList));

    remoteBranchStmts.push_back(tryRecordReturnTyExpr);
  }

  // -- doneRecording
  {
    auto doneRecordingDecl =
        C.getDoneRecordingOnDistributedInvocationEncoder(invocationEncoderDecl);
    auto doneRecordingDeclRef =
        UnresolvedDeclRefExpr::createImplicit(C, doneRecordingDecl->getName());

    auto argsList =
        ArgumentList::forImplicitCallTo(doneRecordingDeclRef->getName(), {}, C);
    auto tryDoneRecordingExpr = TryExpr::createImplicit(C, sloc,
      CallExpr::createImplicit(C,
        UnresolvedDotExpr::createImplicit(C,
          new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc, implicit,
                              AccessSemantics::Ordinary,
                              invocationVar->getInterfaceType()),
          doneRecordingDecl->getName()),
        argsList));

    remoteBranchStmts.push_back(tryDoneRecordingExpr);
  }

  // === Prepare the 'RemoteCallTarget'
  auto *targetVar = new (C) VarDecl(
      /*isStatic=*/false, VarDecl::Introducer::Let, sloc, C.Id_target, thunk);

  {
    // --- Mangle the thunk name
    Mangle::ASTMangler mangler;
    auto mangled =
        C.AllocateCopy(mangler.mangleDistributedThunk(cast<FuncDecl>(thunk)));
    auto mangledTargetStringLiteral =
        new (C) StringLiteralExpr(mangled, SourceRange(), implicit);

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
    auto remoteCallDecl =
        C.getRemoteCallOnDistributedActorSystem(systemDecl, isVoidReturn);
    auto systemRemoteCallRef =
        UnresolvedDotExpr::createImplicit(
            C, new (C) DeclRefExpr(ConcreteDeclRef(systemVar), dloc, implicit),
            remoteCallDecl->getName());

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
    args.push_back(new (C) InOutExpr(sloc,
        new (C) DeclRefExpr(ConcreteDeclRef(invocationVar), dloc,
        implicit, AccessSemantics::Ordinary, invocationEncoderTy), invocationEncoderTy, implicit));

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

    assert(args.size() == remoteCallDecl->getParameters()->size());
    auto remoteCallArgs = ArgumentList::forImplicitCallTo(
        systemRemoteCallRef->getName(), args, C);

    Expr *remoteCallExpr =
        CallExpr::createImplicit(C, systemRemoteCallRef, remoteCallArgs);
    remoteCallExpr = AwaitExpr::createImplicit(C, sloc, remoteCallExpr);
    remoteCallExpr = TryExpr::createImplicit(C, sloc, remoteCallExpr);
    auto returnRemoteCall =
                new (C) ReturnStmt(sloc, remoteCallExpr, implicit);
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

static FuncDecl *createDistributedThunkFunction(FuncDecl *func) {
  auto &C = func->getASTContext();
  auto DC = func->getDeclContext();

  // NOTE: So we don't need a thunk in the protocol, we should call the underlying
  // thing instead, which MUST have a thunk, since it must be a distributed func as well...
  if (isa<ProtocolDecl>(DC)) {
    return nullptr;
  }

  assert(getConcreteReplacementForProtocolActorSystemType(func) &&
         "Thunk synthesis must have concrete actor system type available");

  DeclName thunkName;

  // Since accessors don't have names, let's generate one based on
  // the computed property.
  if (auto *accessor = dyn_cast<AccessorDecl>(func)) {
    auto *var = accessor->getStorage();
    thunkName = DeclName(C, var->getBaseName(),
                         /*argumentNames=*/ArrayRef<Identifier>());
  } else {
    // Let's use the name of a 'distributed func'
    thunkName = func->getName();
  }

  // --- Prepare generic parameters
  GenericParamList *genericParamList = nullptr;
  if (auto genericParams = func->getGenericParams()) {
    genericParamList = genericParams->clone(DC);
  }

  GenericSignature baseSignature =
      buildGenericSignature(C, func->getGenericSignature(),
                            /*addedParameters=*/{},
                            /*addedRequirements=*/{});

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

    paramDecl->setImplicit(true);
    paramDecl->setSpecifier(funcParam->getSpecifier());
    paramDecl->setInterfaceType(funcParam->getInterfaceType());

    paramDecls.push_back(paramDecl);
  }
  ParameterList *params = ParameterList::create(C, paramDecls); // = funcParams->clone(C);

  FuncDecl *thunk = FuncDecl::createImplicit(
      C, swift::StaticSpellingKind::None, thunkName, SourceLoc(),
      /*async=*/true, /*throws=*/true, genericParamList, params,
      func->getResultInterfaceType(), DC);

  assert(thunk && "couldn't create a distributed thunk");

  thunk->setSynthesized(true);
  thunk->setDistributedThunk(true);
  thunk->getAttrs().add(new (C) NonisolatedAttr(/*isImplicit=*/true));

  if (isa<ClassDecl>(DC))
    thunk->getAttrs().add(new (C) FinalAttr(/*isImplicit=*/true));

  thunk->setGenericSignature(baseSignature);
  thunk->copyFormalAccessFrom(func, /*sourceIsParentContext=*/false);
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
      /*isUnchecked=*/false);
  conformance->setSourceKindAndImplyingConformance(
      ConformanceEntryKind::Synthesized, nullptr);
  actor->registerProtocolConformance(conformance, /*synthesized=*/true);
  return conformance;
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

  auto &C = distributedTarget->getASTContext();

  if (!getConcreteReplacementForProtocolActorSystemType(distributedTarget)) {
    // Don't synthesize thunks, unless there is a *concrete* ActorSystem.
    // TODO(distributed): we should be able to lift this eventually,
    // and allow resolving distributed actor protocols.
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
        auto conformance = module->conformsToProtocol(
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
