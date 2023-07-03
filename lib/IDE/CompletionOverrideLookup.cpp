//===--- CompletionOverrideLookup.cpp -------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CompletionOverrideLookup.h"

#include "CodeCompletionResultBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/IDE/CodeCompletionString.h"
#include "swift/IDE/CodeCompletionStringPrinter.h"

using namespace swift;
using namespace swift::ide;

bool CompletionOverrideLookup::addAccessControl(
    const ValueDecl *VD, CodeCompletionResultBuilder &Builder) {
  auto CurrentNominal = CurrDeclContext->getSelfNominalTypeDecl();
  assert(CurrentNominal);

  auto AccessOfContext = CurrentNominal->getFormalAccess();
  if (AccessOfContext < AccessLevel::Public)
    return false;

  auto Access = VD->getFormalAccess();
  // Use the greater access between the protocol requirement and the witness.
  // In case of:
  //
  //   public protocol P { func foo() }
  //   public class B { func foo() {} }
  //   public class C: B, P {
  //     <complete>
  //   }
  //
  // 'VD' is 'B.foo()' which is implicitly 'internal'. But as the overriding
  // declaration, the user needs to write both 'public' and 'override':
  //
  //   public class C: B {
  //     public override func foo() {}
  //   }
  if (Access < AccessLevel::Public &&
      !isa<ProtocolDecl>(VD->getDeclContext())) {
    for (auto Conformance : CurrentNominal->getAllConformances()) {
      Conformance->getRootConformance()->forEachValueWitness(
          [&](ValueDecl *req, Witness witness) {
            if (witness.getDecl() == VD)
              Access = std::max(Access,
                                Conformance->getProtocol()->getFormalAccess());
          });
    }
  }

  Access = std::min(Access, AccessOfContext);
  // Only emit 'public', not needed otherwise.
  if (Access < AccessLevel::Public)
    return false;

  Builder.addAccessControlKeyword(Access);
  return true;
}

Type CompletionOverrideLookup::getOpaqueResultType(
    const ValueDecl *VD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  if (Reason != DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal)
    return nullptr;

  auto currTy = CurrDeclContext->getDeclaredTypeInContext();
  if (!currTy)
    return nullptr;

  Type ResultT;
  if (auto *FD = dyn_cast<FuncDecl>(VD)) {
    if (FD->getGenericParams()) {
      // Generic function cannot have opaque result type.
      return nullptr;
    }
    ResultT = FD->getResultInterfaceType();
  } else if (auto *SD = dyn_cast<SubscriptDecl>(VD)) {
    if (SD->getGenericParams()) {
      // Generic subscript cannot have opaque result type.
      return nullptr;
    }
    ResultT = SD->getElementInterfaceType();
  } else if (auto *VarD = dyn_cast<VarDecl>(VD)) {
    ResultT = VarD->getInterfaceType();
  } else {
    return nullptr;
  }

  if (!ResultT->is<DependentMemberType>() ||
      !ResultT->castTo<DependentMemberType>()->getAssocType())
    // The result is not a valid associatedtype.
    return nullptr;

  // Try substitution to see if the associated type is resolved to concrete
  // type.
  auto substMap =
      currTy->getMemberSubstitutionMap(CurrDeclContext->getParentModule(), VD);
  if (!ResultT.subst(substMap)->is<DependentMemberType>())
    // If resolved print it.
    return nullptr;

  auto genericSig = VD->getDeclContext()->getGenericSignatureOfContext();

  if (genericSig->isConcreteType(ResultT))
    // If it has same type requrement, we will emit the concrete type.
    return nullptr;

  // Collect requirements on the associatedtype.
  SmallVector<Type, 2> opaqueTypes;
  bool hasExplicitAnyObject = false;
  if (auto superTy = genericSig->getSuperclassBound(ResultT))
    opaqueTypes.push_back(superTy);
  for (const auto proto : genericSig->getRequiredProtocols(ResultT))
    opaqueTypes.push_back(proto->getDeclaredInterfaceType());
  if (auto layout = genericSig->getLayoutConstraint(ResultT))
    hasExplicitAnyObject = layout->isClass();

  if (!hasExplicitAnyObject) {
    if (opaqueTypes.empty())
      return nullptr;
    if (opaqueTypes.size() == 1)
      return opaqueTypes.front();
  }
  return ProtocolCompositionType::get(VD->getASTContext(), opaqueTypes,
                                      hasExplicitAnyObject);
}

void CompletionOverrideLookup::addValueOverride(
    const ValueDecl *VD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo, CodeCompletionResultBuilder &Builder,
    bool hasDeclIntroducer) {
  Type opaqueResultType = getOpaqueResultType(VD, Reason, dynamicLookupInfo);

  class DeclPrinter : public CodeCompletionStringPrinter {
    Type OpaqueBaseTy;

  public:
    DeclPrinter(CodeCompletionResultBuilder &Builder, Type OpaqueBaseTy)
        : CodeCompletionStringPrinter(Builder), OpaqueBaseTy(OpaqueBaseTy) {}

    // As for FuncDecl, SubscriptDecl, and VarDecl, substitute the result type
    // with 'OpaqueBaseTy' if specified.
    void printDeclResultTypePre(ValueDecl *VD, TypeLoc &TL) override {
      if (!OpaqueBaseTy.isNull()) {
        setNextChunkKind(ChunkKind::Keyword);
        printText("some ");
        setNextChunkKind(ChunkKind::Text);
        TL = TypeLoc::withoutLoc(OpaqueBaseTy);
      }
      CodeCompletionStringPrinter::printDeclResultTypePre(VD, TL);
    }
  };

  DeclPrinter Printer(Builder, opaqueResultType);
  Printer.startPreamble();

  bool modifierAdded = false;

  // 'public' if needed.
  modifierAdded |= !hasAccessModifier && addAccessControl(VD, Builder);

  // 'override' if needed
  if (missingOverride(Reason)) {
    Builder.addOverrideKeyword();
    modifierAdded |= true;
  }

  // Erase existing introducer (e.g. 'func') if any modifiers are added.
  if (hasDeclIntroducer && modifierAdded) {
    auto dist = Ctx.SourceMgr.getByteDistance(
        introducerLoc, Ctx.SourceMgr.getIDEInspectionTargetLoc());
    if (dist <= CodeCompletionResult::MaxNumBytesToErase) {
      Builder.setNumBytesToErase(dist);
      hasDeclIntroducer = false;
    }
  }

  PrintOptions PO;
  if (auto transformType = CurrDeclContext->getDeclaredTypeInContext())
    PO.setBaseType(transformType);
  PO.PrintPropertyAccessors = false;
  PO.PrintSubscriptAccessors = false;

  PO.SkipUnderscoredKeywords = true;
  PO.PrintImplicitAttrs = false;
  PO.ExclusiveAttrList.push_back(TAK_escaping);
  PO.ExclusiveAttrList.push_back(TAK_autoclosure);
  // Print certain modifiers only when the introducer is not written.
  // Otherwise, the user can add it after the completion.
  if (!hasDeclIntroducer) {
    PO.ExclusiveAttrList.push_back(DAK_Nonisolated);
  }

  PO.PrintAccess = false;
  PO.PrintOverrideKeyword = false;
  PO.PrintSelfAccessKindKeyword = false;

  PO.PrintStaticKeyword = !hasStaticOrClass && !hasDeclIntroducer;
  PO.SkipIntroducerKeywords = hasDeclIntroducer;
  VD->print(Printer, PO);
}

void CompletionOverrideLookup::addMethodOverride(
    const FuncDecl *FD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResultKind::Declaration,
                                      SemanticContextKind::Super);
  Builder.setResultTypeNotApplicable();
  Builder.setAssociatedDecl(FD);
  addValueOverride(FD, Reason, dynamicLookupInfo, Builder, hasFuncIntroducer);
  Builder.addBraceStmtWithCursor();
}

void CompletionOverrideLookup::addVarOverride(
    const VarDecl *VD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  // Overrides cannot use 'let', but if the 'override' keyword is specified
  // then the intention is clear, so provide the results anyway.  The compiler
  // can then provide an error telling you to use 'var' instead.
  // If we don't need override then it's a protocol requirement, so show it.
  if (missingOverride(Reason) && hasVarIntroducer && isKeywordSpecified("let"))
    return;

  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResultKind::Declaration,
                                      SemanticContextKind::Super);
  Builder.setAssociatedDecl(VD);
  addValueOverride(VD, Reason, dynamicLookupInfo, Builder, hasVarIntroducer);
}

void CompletionOverrideLookup::addSubscriptOverride(
    const SubscriptDecl *SD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResultKind::Declaration,
                                      SemanticContextKind::Super);
  Builder.setResultTypeNotApplicable();
  Builder.setAssociatedDecl(SD);
  addValueOverride(SD, Reason, dynamicLookupInfo, Builder, false);
  Builder.addBraceStmtWithCursor();
}

void CompletionOverrideLookup::addTypeAlias(
    const AssociatedTypeDecl *ATD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResultKind::Declaration,
                                      SemanticContextKind::Super);
  Builder.setResultTypeNotApplicable();
  Builder.setAssociatedDecl(ATD);
  if (!hasTypealiasIntroducer && !hasAccessModifier)
    (void)addAccessControl(ATD, Builder);
  if (!hasTypealiasIntroducer)
    Builder.addDeclIntroducer("typealias ");
  Builder.addBaseName(ATD->getName().str());
  Builder.addTextChunk(" = ");
  Builder.addSimpleNamedParameter("Type");
}

void CompletionOverrideLookup::addConstructor(
    const ConstructorDecl *CD, DeclVisibilityKind Reason,
    DynamicLookupInfo dynamicLookupInfo) {
  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResultKind::Declaration,
                                      SemanticContextKind::Super);
  Builder.setResultTypeNotApplicable();
  Builder.setAssociatedDecl(CD);

  CodeCompletionStringPrinter printer(Builder);
  printer.startPreamble();

  if (!hasAccessModifier)
    (void)addAccessControl(CD, Builder);

  if (missingOverride(Reason) && CD->isDesignatedInit() && !CD->isRequired())
    Builder.addOverrideKeyword();

  // Emit 'required' if we're in class context, 'required' is not specified,
  // and 1) this is a protocol conformance and the class is not final, or 2)
  // this is subclass and the initializer is marked as required.
  bool needRequired = false;
  auto C = CurrDeclContext->getSelfClassDecl();
  if (C && !isKeywordSpecified("required")) {
    switch (Reason) {
    case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
    case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
      if (!C->isSemanticallyFinal())
        needRequired = true;
      break;
    case DeclVisibilityKind::MemberOfSuper:
      if (CD->isRequired())
        needRequired = true;
      break;
    default:
      break;
    }
  }
  if (needRequired)
    Builder.addRequiredKeyword();

  {
    PrintOptions Options;
    if (auto transformType = CurrDeclContext->getDeclaredTypeInContext())
      Options.setBaseType(transformType);
    Options.PrintImplicitAttrs = false;
    Options.SkipAttributes = true;
    CD->print(printer, Options);
  }
  printer.flush();

  Builder.addBraceStmtWithCursor();
}

void CompletionOverrideLookup::foundDecl(ValueDecl *D,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo) {
  if (Reason == DeclVisibilityKind::MemberOfCurrentNominal)
    return;

  if (D->shouldHideFromEditor())
    return;

  if (D->isSemanticallyFinal())
    return;

  bool hasIntroducer =
      hasFuncIntroducer || hasVarIntroducer || hasTypealiasIntroducer;

  if (hasStaticOrClass && !D->isStatic())
    return;

  // As per the current convention, only instance members are
  // suggested if an introducer is not accompanied by a 'static' or
  // 'class' modifier.
  if (hasIntroducer && !hasStaticOrClass && D->isStatic())
    return;

  if (auto *FD = dyn_cast<FuncDecl>(D)) {
    // We cannot override operators as members.
    if (FD->isBinaryOperator() || FD->isUnaryOperator())
      return;

    // We cannot override individual accessors.
    if (isa<AccessorDecl>(FD))
      return;

    if (hasFuncIntroducer || (!hasIntroducer && !hasInitializerModifier))
      addMethodOverride(FD, Reason, dynamicLookupInfo);
    return;
  }

  if (auto *VD = dyn_cast<VarDecl>(D)) {
    if (hasVarIntroducer || (!hasIntroducer && !hasInitializerModifier))
      addVarOverride(VD, Reason, dynamicLookupInfo);
    return;
  }

  if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
    if (!hasIntroducer && !hasInitializerModifier)
      addSubscriptOverride(SD, Reason, dynamicLookupInfo);
  }

  if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
    if (!isa<ProtocolDecl>(CD->getDeclContext()))
      return;
    if (hasIntroducer || hasOverride || hasOverridabilityModifier ||
        hasStaticOrClass)
      return;
    if (CD->isRequired() || CD->isDesignatedInit())
      addConstructor(CD, Reason, dynamicLookupInfo);
    return;
  }
}

void CompletionOverrideLookup::addDesignatedInitializers(NominalTypeDecl *NTD) {
  if (hasFuncIntroducer || hasVarIntroducer || hasTypealiasIntroducer ||
      hasOverridabilityModifier || hasStaticOrClass)
    return;

  const auto *CD = dyn_cast<ClassDecl>(NTD);
  if (!CD)
    return;
  if (!CD->hasSuperclass())
    return;
  CD = CD->getSuperclassDecl();
  for (const auto *Member : CD->getMembers()) {
    const auto *Constructor = dyn_cast<ConstructorDecl>(Member);
    if (!Constructor)
      continue;
    if (Constructor->hasStubImplementation())
      continue;
    if (Constructor->isDesignatedInit())
      addConstructor(Constructor, DeclVisibilityKind::MemberOfSuper, {});
  }
}

void CompletionOverrideLookup::addAssociatedTypes(NominalTypeDecl *NTD) {
  if (!hasTypealiasIntroducer &&
      (hasFuncIntroducer || hasVarIntroducer || hasInitializerModifier ||
       hasOverride || hasOverridabilityModifier || hasStaticOrClass))
    return;

  if (isa<ProtocolDecl>(NTD))
    return;

  for (auto Conformance : NTD->getAllConformances()) {
    auto Proto = Conformance->getProtocol();
    if (!Proto->isAccessibleFrom(CurrDeclContext))
      continue;
    for (auto *ATD : Proto->getAssociatedTypeMembers()) {
      // FIXME: Also exclude the type alias that has already been specified.
      if (!Conformance->hasTypeWitness(ATD) || ATD->hasDefaultDefinitionType())
        continue;
      addTypeAlias(
          ATD, DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal,
          {});
    }
  }
}

StringRef CompletionOverrideLookup::getResultBuilderDocComment(
    ResultBuilderBuildFunction function) {
  switch (function) {
  case ResultBuilderBuildFunction::BuildArray:
    return "Enables support for..in loops in a result builder by "
           "combining the results of all iterations into a single result";

  case ResultBuilderBuildFunction::BuildBlock:
    return "Required by every result builder to build combined results "
           "from statement blocks";

  case ResultBuilderBuildFunction::BuildEitherFirst:
    return "With buildEither(second:), enables support for 'if-else' and "
           "'switch' statements by folding conditional results into a single "
           "result";

  case ResultBuilderBuildFunction::BuildEitherSecond:
    return "With buildEither(first:), enables support for 'if-else' and "
           "'switch' statements by folding conditional results into a single "
           "result";

  case ResultBuilderBuildFunction::BuildExpression:
    return "If declared, provides contextual type information for statement "
           "expressions to translate them into partial results";

  case ResultBuilderBuildFunction::BuildFinalResult:
    return "If declared, this will be called on the partial result from the "
           "outermost block statement to produce the final returned result";

  case ResultBuilderBuildFunction::BuildLimitedAvailability:
    return "If declared, this will be called on the partial result of "
           "an 'if #available' block to allow the result builder to erase "
           "type information";

  case ResultBuilderBuildFunction::BuildOptional:
    return "Enables support for `if` statements that do not have an `else`";
  case ResultBuilderBuildFunction::BuildPartialBlockFirst:
    return "Builds a partial result component from the first component";
  case ResultBuilderBuildFunction::BuildPartialBlockAccumulated:
    return "Builds a partial result component by combining an accumulated "
           "component and a new component";
  }
}

void CompletionOverrideLookup::addResultBuilderBuildCompletion(
    NominalTypeDecl *builder, Type componentType,
    ResultBuilderBuildFunction function) {
  CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Pattern,
                                      SemanticContextKind::CurrentNominal);
  Builder.setResultTypeNotApplicable();

  if (!hasFuncIntroducer) {
    if (!hasAccessModifier && builder->getFormalAccess() >= AccessLevel::Public)
      Builder.addAccessControlKeyword(AccessLevel::Public);

    if (!hasStaticOrClass)
      Builder.addTextChunk("static ");

    Builder.addTextChunk("func ");
  }

  std::string declStringWithoutFunc;
  {
    llvm::raw_string_ostream out(declStringWithoutFunc);
    printResultBuilderBuildFunction(builder, componentType, function,
                                    llvm::None, out);
  }
  Builder.addTextChunk(declStringWithoutFunc);
  Builder.addBraceStmtWithCursor();
  Builder.setBriefDocComment(getResultBuilderDocComment(function));
}

void CompletionOverrideLookup::addResultBuilderBuildCompletions(
    NominalTypeDecl *builder) {
  Type componentType = inferResultBuilderComponentType(builder);

  addResultBuilderBuildCompletion(builder, componentType,
                                  ResultBuilderBuildFunction::BuildBlock);
  addResultBuilderBuildCompletion(builder, componentType,
                                  ResultBuilderBuildFunction::BuildExpression);
  addResultBuilderBuildCompletion(builder, componentType,
                                  ResultBuilderBuildFunction::BuildOptional);
  addResultBuilderBuildCompletion(builder, componentType,
                                  ResultBuilderBuildFunction::BuildEitherFirst);
  addResultBuilderBuildCompletion(
      builder, componentType, ResultBuilderBuildFunction::BuildEitherSecond);
  addResultBuilderBuildCompletion(builder, componentType,
                                  ResultBuilderBuildFunction::BuildArray);
  addResultBuilderBuildCompletion(
      builder, componentType,
      ResultBuilderBuildFunction::BuildLimitedAvailability);
  addResultBuilderBuildCompletion(builder, componentType,
                                  ResultBuilderBuildFunction::BuildFinalResult);
  addResultBuilderBuildCompletion(
      builder, componentType,
      ResultBuilderBuildFunction::BuildPartialBlockFirst);
  addResultBuilderBuildCompletion(
      builder, componentType,
      ResultBuilderBuildFunction::BuildPartialBlockAccumulated);
}

void CompletionOverrideLookup::getOverrideCompletions(SourceLoc Loc) {
  if (!CurrDeclContext->isTypeContext())
    return;
  if (isa<ProtocolDecl>(CurrDeclContext))
    return;

  Type CurrTy = CurrDeclContext->getSelfTypeInContext();
  auto *NTD = CurrDeclContext->getSelfNominalTypeDecl();
  if (CurrTy && !CurrTy->is<ErrorType>()) {
    // Look for overridable static members too.
    Type Meta = MetatypeType::get(CurrTy);
    lookupVisibleMemberDecls(*this, Meta, introducerLoc, CurrDeclContext,
                             /*includeInstanceMembers=*/true,
                             /*includeDerivedRequirements*/ true,
                             /*includeProtocolExtensionMembers*/ false);
    addDesignatedInitializers(NTD);
    addAssociatedTypes(NTD);
  }

  if (NTD && NTD->getAttrs().hasAttribute<ResultBuilderAttr>()) {
    addResultBuilderBuildCompletions(NTD);
  }
}
