//===--- SILGenFunction.cpp - Top-level lowering for functions ------------===//
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
//  This file defines the primary routines for creating and emitting
//  functions.
//
//===----------------------------------------------------------------------===//

#include "SILGenFunction.h"
#include "Cleanup.h"
#include "RValue.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTScope.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILProfiler.h"
#include "swift/SIL/SILUndef.h"

using namespace swift;
using namespace Lowering;

#define DEBUG_TYPE "silscopes"

//===----------------------------------------------------------------------===//
// SILGenFunction Class implementation
//===----------------------------------------------------------------------===//

SILGenFunction::SILGenFunction(SILGenModule &SGM, SILFunction &F,
                               DeclContext *DC, bool IsEmittingTopLevelCode)
    : SGM(SGM), F(F), silConv(SGM.M), FunctionDC(DC),
      StartOfPostmatter(F.end()), B(*this),
      SF(DC ? DC->getParentSourceFile() : nullptr), Cleanups(*this),
      StatsTracer(SGM.M.getASTContext().Stats, "SILGen-function", &F),
      IsEmittingTopLevelCode(IsEmittingTopLevelCode) {
  assert(DC && "creating SGF without a DeclContext?");
  B.setInsertionPoint(createBasicBlock());
  B.setCurrentDebugScope(F.getDebugScope());

  // Populate VarDeclScopeMap.
  SourceLoc SLoc = F.getLocation().getSourceLoc();
  if (SF && SLoc) {
    FnASTScope = ast_scope::ASTScopeImpl::findStartingScopeForLookup(SF, SLoc);
    ScopeMap.insert({{FnASTScope, nullptr}, F.getDebugScope()});

    // Collect all variable declarations in this scope.
    struct Consumer : public namelookup::AbstractASTScopeDeclConsumer {
      const ast_scope::ASTScopeImpl *ASTScope;
      VarDeclScopeMapTy &VarDeclScopeMap;
      Consumer(const ast_scope::ASTScopeImpl *ASTScope,
               VarDeclScopeMapTy &VarDeclScopeMap)
          : ASTScope(ASTScope), VarDeclScopeMap(VarDeclScopeMap) {}

      bool consume(ArrayRef<ValueDecl *> values,
                   NullablePtr<DeclContext> baseDC) override {
        LLVM_DEBUG(ASTScope->print(llvm::errs(), 0, false, false));
        for (auto &value : values) {
          LLVM_DEBUG({
            if (value->hasName())
              llvm::dbgs() << "+ " << value->getBaseIdentifier() << "\n";
          });

          // FIXME: ASTs coming out of the autodiff transformation trigger this.
          // assert((VarDeclScopeMap.count(value) == 0 ||
          //         VarDeclScopeMap[value] == ASTScope) &&
          //        "VarDecl appears twice");
          VarDeclScopeMap.insert({value, ASTScope});
        }
        return false;
      }
      bool lookInMembers(const DeclContext *) const override { return false; }
#ifndef NDEBUG
      void startingNextLookupStep() override {}
      void finishingLookup(std::string) const override {}
      bool isTargetLookup() const override { return false; }
#endif
    };
    const_cast<ast_scope::ASTScopeImpl *>(FnASTScope)
        ->preOrderChildrenDo([&](ast_scope::ASTScopeImpl *ASTScope) {
          if (!ASTScope->ignoreInDebugInfo()) {
            Consumer consumer(ASTScope, VarDeclScopeMap);
            ASTScope->lookupLocalsOrMembers(consumer);
          }
        });
  }
}

/// SILGenFunction destructor - called after the entire function's AST has been
/// visited.  This handles "falling off the end of the function" logic.
SILGenFunction::~SILGenFunction() {
  // If the end of the function isn't terminated, we screwed up somewhere.
  assert(!B.hasValidInsertionPoint() &&
         "SILGenFunction did not terminate function?!");

  // If we didn't clean up the rethrow destination, we screwed up somewhere.
  assert(!ThrowDest.isValid() &&
         "SILGenFunction did not emit throw destination");
}

//===----------------------------------------------------------------------===//
// Function emission
//===----------------------------------------------------------------------===//

// Get the #function name for a declaration.
DeclName SILGenModule::getMagicFunctionName(DeclContext *dc) {
  // For closures, use the parent name.
  if (auto closure = dyn_cast<AbstractClosureExpr>(dc)) {
    return getMagicFunctionName(closure->getParent());
  }
  if (auto absFunc = dyn_cast<AbstractFunctionDecl>(dc)) {
    // If this is an accessor, use the name of the storage.
    if (auto accessor = dyn_cast<AccessorDecl>(absFunc))
      return accessor->getStorage()->getName();
    if (auto func = dyn_cast<FuncDecl>(absFunc)) {
      // If this is a defer body, use the parent name.
      if (func->isDeferBody()) {
        return getMagicFunctionName(func->getParent());
      }
    }

    return absFunc->getName();
  }
  if (auto init = dyn_cast<Initializer>(dc)) {
    return getMagicFunctionName(init->getParent());
  }
  if (auto nominal = dyn_cast<NominalTypeDecl>(dc)) {
    return nominal->getName();
  }
  if (auto tl = dyn_cast<TopLevelCodeDecl>(dc)) {
    return tl->getModuleContext()->getName();
  }
  if (auto fu = dyn_cast<FileUnit>(dc)) {
    return fu->getParentModule()->getName();
  }
  if (auto m = dyn_cast<ModuleDecl>(dc)) {
    return m->getName();
  }
  if (auto e = dyn_cast<ExtensionDecl>(dc)) {
    assert(e->getExtendedNominal() && "extension for nonnominal");
    return e->getExtendedNominal()->getName();
  }
  if (auto EED = dyn_cast<EnumElementDecl>(dc)) {
    return EED->getName();
  }
  if (auto SD = dyn_cast<SubscriptDecl>(dc)) {
    return SD->getName();
  }
  llvm_unreachable("unexpected #function context");
}

DeclName SILGenModule::getMagicFunctionName(SILDeclRef ref) {
  switch (ref.kind) {
  case SILDeclRef::Kind::Func:
    if (auto closure = ref.getAbstractClosureExpr())
      return getMagicFunctionName(closure);
    return getMagicFunctionName(cast<FuncDecl>(ref.getDecl()));
  case SILDeclRef::Kind::Initializer:
  case SILDeclRef::Kind::Allocator:
    return getMagicFunctionName(cast<ConstructorDecl>(ref.getDecl()));
  case SILDeclRef::Kind::Deallocator:
  case SILDeclRef::Kind::Destroyer:
    return getMagicFunctionName(cast<DestructorDecl>(ref.getDecl()));
  case SILDeclRef::Kind::GlobalAccessor:
    return getMagicFunctionName(cast<VarDecl>(ref.getDecl())->getDeclContext());
  case SILDeclRef::Kind::DefaultArgGenerator:
    return getMagicFunctionName(cast<DeclContext>(ref.getDecl()));
  case SILDeclRef::Kind::StoredPropertyInitializer:
  case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
    return getMagicFunctionName(cast<VarDecl>(ref.getDecl())->getDeclContext());
  case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue:
    return getMagicFunctionName(cast<VarDecl>(ref.getDecl())->getDeclContext());
  case SILDeclRef::Kind::IVarInitializer:
    return getMagicFunctionName(cast<ClassDecl>(ref.getDecl()));
  case SILDeclRef::Kind::IVarDestroyer:
    return getMagicFunctionName(cast<ClassDecl>(ref.getDecl()));
  case SILDeclRef::Kind::EnumElement:
    return getMagicFunctionName(cast<EnumElementDecl>(ref.getDecl())
                                  ->getDeclContext());
  case SILDeclRef::Kind::AsyncEntryPoint:
  case SILDeclRef::Kind::EntryPoint: {
    auto *file = ref.getDecl()->getDeclContext()->getParentSourceFile();
    return getMagicFunctionName(file);
  }
  }

  llvm_unreachable("Unhandled SILDeclRefKind in switch.");
}

SILDebugLocation SILGenFunction::getSILDebugLocation(
    SILBuilder &B, SILLocation Loc,
    std::optional<SILLocation> CurDebugLocOverride, bool ForMetaInstruction) {
  const SILDebugScope *Scope = B.getCurrentDebugScope();
  if (!Scope)
    Scope = F.getDebugScope();
  if (auto *SILScope = getScopeOrNull(Loc, ForMetaInstruction)) {
    Scope = SILScope;
    // Metainstructions such as a debug_value may break the flow of scopes and
    // should not change the state of the builder.
    if (!ForMetaInstruction)
      B.setCurrentDebugScope(Scope);
  }
  auto overriddenLoc = CurDebugLocOverride ? *CurDebugLocOverride : Loc;
  return SILDebugLocation(overriddenLoc, Scope);
}

const SILDebugScope *SILGenFunction::getScopeOrNull(SILLocation Loc,
                                                    bool ForMetaInstruction) {
  if (!ForMetaInstruction) {
    if (Loc.getKind() == SILLocation::CleanupKind ||
        Loc.getKind() == SILLocation::ImplicitReturnKind ||
        // The source locations produced by the ResultBuilder transformation are
        // all over the place.
        Loc.isImplicit() || Loc.isAutoGenerated())
      return nullptr;
  }

  SourceLoc SLoc = Loc.getSourceLoc();
  if (!SF || LastSourceLoc == SLoc)
    return nullptr;
  if (ForMetaInstruction)
    if (ValueDecl *ValDecl = Loc.getAsASTNode<ValueDecl>()) {
      // The source location of a VarDecl isn't necessarily in the same scope
      // that the variable resides in for name lookup purposes.
      auto ValueScope = VarDeclScopeMap.find(ValDecl);
      if (ValueScope != VarDeclScopeMap.end())
        return getOrCreateScope(ValueScope->second, F.getDebugScope());
    }
  return getOrCreateScope(SLoc);
}

const SILDebugScope *SILGenFunction::getOrCreateScope(SourceLoc SLoc) {
  if (const SILDebugScope *macroScope = getMacroScope(SLoc))
    return macroScope;
  auto *astScope =
      ast_scope::ASTScopeImpl::findStartingScopeForLookup(SF, SLoc);

  // At the call site of a closure, the ASTScope created for the ClosureExpr
  // has no parents, so filter it out here.
  if (!astScope->getParent())
    return nullptr;

  const SILDebugScope *Scope = getOrCreateScope(astScope, F.getDebugScope());
  assert(Scope && "failed to construct SILDebugScope from ASTScope");
  return Scope;
}

namespace {
struct MacroInfo {
  MacroInfo(SourceLoc SLoc, SourceLoc ExpansionSLoc)
      : SLoc(SLoc), ExpansionSLoc(ExpansionSLoc) {}
  SourceLoc SLoc;
  SourceLoc ExpansionSLoc;
  RegularLocation ExpansionLoc = RegularLocation((Decl*)nullptr);
  std::string Name = "__unknown_macro__";
  bool Freestanding = false;
};
}

static DeclContext *getInnermostFunctionContext(DeclContext *DC) {
  for (; DC; DC = DC->getParent())
    if (DC->getContextKind() == DeclContextKind::AbstractFunctionDecl)
      return DC;
  return nullptr;
}

/// Return location of the macro expansion and the macro name.
static MacroInfo getMacroInfo(GeneratedSourceInfo &Info,
                              DeclContext *FunctionDC) {
  MacroInfo Result(Info.generatedSourceRange.getStart(),
                   Info.originalSourceRange.getStart());
  if (!Info.astNode)
    return Result;
  // Keep this in sync with ASTMangler::appendMacroExpansionContext().
  Mangle::ASTMangler mangler;
  switch (Info.kind) {
  case GeneratedSourceInfo::ExpressionMacroExpansion: {
    auto parent = ASTNode::getFromOpaqueValue(Info.astNode);
    if (auto expr =
            cast_or_null<MacroExpansionExpr>(parent.dyn_cast<Expr *>())) {
      Result.ExpansionLoc = RegularLocation(expr);
      Result.Name = mangler.mangleMacroExpansion(expr);
    } else {
      auto decl = cast<MacroExpansionDecl>(parent.get<Decl *>());
      Result.ExpansionLoc = RegularLocation(decl);
      Result.Name = mangler.mangleMacroExpansion(decl);
    }
    // If the parent function of the macro expansion expression is not the
    // current function, then the macro expanded to a closure or nested
    // function. As far as the generated SIL is concerned this is the same as a
    // function generated from a freestanding macro expansion.
    DeclContext *MacroContext = getInnermostFunctionContext(Info.declContext);
    if (MacroContext != FunctionDC)
      Result.Freestanding = true;
    break;
  }
  case GeneratedSourceInfo::DeclarationMacroExpansion: 
  case GeneratedSourceInfo::CodeItemMacroExpansion: {
    auto expansion = cast<MacroExpansionDecl>(
        ASTNode::getFromOpaqueValue(Info.astNode).get<Decl *>());
    Result.ExpansionLoc = RegularLocation(expansion);
    Result.Name = mangler.mangleMacroExpansion(expansion);
    Result.Freestanding = true;
    break;
  }

#define FREESTANDING_MACRO_EXPANSION(Name, Description)
#define ATTACHED
  case GeneratedSourceInfo::AccessorMacroExpansion:
  case GeneratedSourceInfo::MemberAttributeMacroExpansion:
  case GeneratedSourceInfo::MemberMacroExpansion:
  case GeneratedSourceInfo::PeerMacroExpansion:
  case GeneratedSourceInfo::ConformanceMacroExpansion:
  case GeneratedSourceInfo::ExtensionMacroExpansion:
  case GeneratedSourceInfo::PreambleMacroExpansion:
  case GeneratedSourceInfo::BodyMacroExpansion: {
    auto decl = ASTNode::getFromOpaqueValue(Info.astNode).get<Decl *>();
    auto attr = Info.attachedMacroCustomAttr;
    if (auto *macroDecl = decl->getResolvedMacro(attr)) {
      Result.ExpansionLoc = RegularLocation(macroDecl);
      Result.Name = macroDecl->getBaseName().userFacingName();
      Result.Freestanding = true;
    }
    break;
  }
  case GeneratedSourceInfo::PrettyPrinted:
  case GeneratedSourceInfo::ReplacedFunctionBody:
  case GeneratedSourceInfo::DefaultArgument:
    break;
  }
  return Result;
}

const SILDebugScope *SILGenFunction::getMacroScope(SourceLoc SLoc) {
  auto &SM = getSourceManager();
  unsigned BufferID = SM.findBufferContainingLoc(SLoc);
  auto GeneratedSourceInfo = SM.getGeneratedSourceInfo(BufferID);
  if (!GeneratedSourceInfo)
    return nullptr;

  // There is no good way to represent freestanding macros as inlined functions,
  // because entire function would need to be "inlined" into a top-level
  // declaration that isn't part of a real function. By not handling them here,
  // source locations will still point into the macro expansion buffer, but
  // debug info doesn't know what macro that buffer was expanded from.
  auto Macro = getMacroInfo(*GeneratedSourceInfo, FunctionDC);
  if (Macro.Freestanding)
    return nullptr;
  
  const SILDebugScope *TopLevelScope;
  auto It = InlinedScopeMap.find(BufferID);
  if (It != InlinedScopeMap.end())
    TopLevelScope = It->second;
  else {
    // Recursively create one inlined function + scope per layer of generated
    // sources.  Chains of Macro expansions are represented as flat
    // function-level scopes.
    SILGenFunctionBuilder B(SGM);
    auto &ASTContext = SGM.M.getASTContext();
    auto ExtInfo = SILFunctionType::ExtInfo::getThin();
    auto FunctionType =
        SILFunctionType::get(nullptr, ExtInfo, SILCoroutineKind::None,
                             ParameterConvention::Direct_Unowned, /*Params*/ {},
                             /*yields*/
                             {},
                             /*Results*/ {}, std::nullopt, SubstitutionMap(),
                             SubstitutionMap(), ASTContext);
    StringRef MacroName = ASTContext.getIdentifier(Macro.Name).str();
    RegularLocation MacroLoc(Macro.SLoc);
    // Use the ExpansionLoc as the location so IRGenDebugInfo can extract the
    // human-readable macro name from the MacroExpansionDecl.
    SILFunction *MacroFn = B.getOrCreateFunction(
        Macro.ExpansionLoc, MacroName,
        SILLinkage::DefaultForDeclaration, FunctionType, IsNotBare,
        IsNotTransparent, IsNotSerialized, IsNotDynamic, IsNotDistributed,
        IsNotRuntimeAccessible);
    // At the end of the chain ExpansionLoc should be a macro expansion node.
    const SILDebugScope *InlinedAt = nullptr;
    const SILDebugScope *ExpansionScope = getOrCreateScope(Macro.ExpansionSLoc);

    // Inject an extra scope to hold the inlined call site.
    if (ExpansionScope)
      InlinedAt = new (SGM.M)
          SILDebugScope(Macro.ExpansionLoc, nullptr, ExpansionScope,
                        ExpansionScope->InlinedCallSite);

    TopLevelScope =
        new (SGM.M) SILDebugScope(MacroLoc, MacroFn, nullptr, InlinedAt);

    InlinedScopeMap.insert({BufferID, TopLevelScope});
  }

  // Create the scope hierarchy inside the macro expansion.
  auto *MacroAstScope =
      ast_scope::ASTScopeImpl::findStartingScopeForLookup(SF, Macro.SLoc);
  return getOrCreateScope(MacroAstScope, TopLevelScope,
                          TopLevelScope->InlinedCallSite);
}

const SILDebugScope *
SILGenFunction::getOrCreateScope(const ast_scope::ASTScopeImpl *ASTScope,
                                 const SILDebugScope *FnScope,
                                 const SILDebugScope *InlinedAt) {
  if (!ASTScope)
    return FnScope;

  // Top-level function scope?
  if (ASTScope == FnASTScope)
    return FnScope;

  auto It = ScopeMap.find({ASTScope, InlinedAt});
  if (It != ScopeMap.end())
    return It->second;

  LLVM_DEBUG(ASTScope->print(llvm::errs(), 0, false, false));

  auto cache = [&](const SILDebugScope *SILScope) {
    ScopeMap.insert({{ASTScope, InlinedAt}, SILScope});
    assert(SILScope->getParentFunction() == &F &&
           "inlinedAt points to other function");
    return SILScope;
  };

  // Decide whether to pick a parent scope instead.
  if (ASTScope->ignoreInDebugInfo()) {
    LLVM_DEBUG(llvm::dbgs() << "ignored\n");
    auto *ParentScope = getOrCreateScope(ASTScope->getParent().getPtrOrNull(),
                                         FnScope, InlinedAt);
    return ParentScope->InlinedCallSite != InlinedAt ? FnScope : ParentScope;
  }

  // Collapse BraceStmtScopes whose parent is a .*BodyScope.
  if (auto Parent = ASTScope->getParent().getPtrOrNull())
    if (Parent->getSourceRangeOfThisASTNode() ==
        ASTScope->getSourceRangeOfThisASTNode())
      return cache(getOrCreateScope(Parent, FnScope, InlinedAt));

  // The calls to defer closures have cleanup source locations pointing to the
  // defer. Reparent them into the current debug scope.
  auto *AncestorScope = ASTScope->getParent().getPtrOrNull();
  while (AncestorScope && AncestorScope != FnASTScope &&
         !ScopeMap.count({AncestorScope, InlinedAt})) {
    if (auto *FD = dyn_cast_or_null<FuncDecl>(
            AncestorScope->getDeclIfAny().getPtrOrNull())) {
      if (cast<DeclContext>(FD) != FunctionDC)
        return cache(B.getCurrentDebugScope());

      // This is this function's own scope.
      // If this is the outermost BraceStmt scope, ignore it.
      if (AncestorScope == ASTScope->getParent().getPtrOrNull())
        return cache(FnScope);
      break;
    }

    AncestorScope = AncestorScope->getParent().getPtrOrNull();
  };

  // Create the scope and recursively its parents.  getLookupParent implements a
  // special case for GuardBlockStmt, which is nested incorrectly.
  auto *ParentScope = ASTScope->getLookupParent().getPtrOrNull();
  const SILDebugScope *Parent =
      getOrCreateScope(ParentScope, FnScope, InlinedAt);
  SourceLoc SLoc = ASTScope->getSourceRangeOfThisASTNode().Start;
  RegularLocation Loc(SLoc);
  auto *SILScope = new (SGM.M)
      SILDebugScope(Loc, FnScope->getParentFunction(), Parent, InlinedAt);
  return cache(SILScope);
}

void SILGenFunction::enterDebugScope(SILLocation Loc, bool isBindingScope) {
  // Initialize the builder with a default SILDebugScope for this scope.
  if (const SILDebugScope *Scope = getScopeOrNull(Loc))
    B.setCurrentDebugScope(Scope);
}

/// Return to the previous debug scope.
void SILGenFunction::leaveDebugScope() {}

std::tuple<ManagedValue, SILType>
SILGenFunction::emitSiblingMethodRef(SILLocation loc,
                                     SILValue selfValue,
                                     SILDeclRef methodConstant,
                                     SubstitutionMap subMap) {
  SILValue methodValue;

  // If the method is dynamic, access it through runtime-hookable virtual
  // dispatch (viz. objc_msgSend for now).
  if (methodConstant.hasDecl()
      && methodConstant.getDecl()->shouldUseObjCDispatch()) {
    methodValue =
        emitDynamicMethodRef(
            loc, methodConstant,
            SGM.Types.getConstantInfo(getTypeExpansionContext(), methodConstant)
                .SILFnType)
            .getValue();
  } else {
    methodValue = emitGlobalFunctionRef(loc, methodConstant);
  }

  SILType methodTy = methodValue->getType();

  // Specialize the generic method.
  methodTy =
      methodTy.substGenericArgs(SGM.M, subMap, getTypeExpansionContext());

  return std::make_tuple(
      ManagedValue::forObjectRValueWithoutOwnership(methodValue), methodTy);
}

void SILGenFunction::emitCaptures(SILLocation loc,
                                  SILDeclRef closure,
                                  CaptureEmission purpose,
                                  SmallVectorImpl<ManagedValue> &capturedArgs) {
  loc.markAutoGenerated();
  auto captureInfo = SGM.Types.getLoweredLocalCaptures(closure);
  // For boxed captures, we need to mark the contained variables as having
  // escaped for DI diagnostics.
  SmallVector<SILValue, 2> escapesToMark;
  
  // Partial applications take ownership of the context parameters, so we'll
  // need to pass ownership rather than merely guaranteeing parameters.
  bool canGuarantee;
  bool captureCanEscape = true;
  switch (purpose) {
  case CaptureEmission::PartialApplication:
    canGuarantee = false;
    break;
  case CaptureEmission::ImmediateApplication:
    canGuarantee = true;
    break;
  case CaptureEmission::AssignByWrapper:
    canGuarantee = false;
    captureCanEscape = false;
    break;
  }

  auto expansion = getTypeExpansionContext();

  for (auto capture : captureInfo.getCaptures()) {
    if (capture.isDynamicSelfMetadata()) {
      // The parameter type is the static Self type, but the value we
      // want to pass is the dynamic Self type, so upcast it.
      auto dynamicSelfMetatype = MetatypeType::get(
        captureInfo.getDynamicSelfType());
      SILType dynamicSILType = getLoweredType(dynamicSelfMetatype);

      SILValue value = B.createMetatype(loc, dynamicSILType);
      capturedArgs.push_back(
          ManagedValue::forObjectRValueWithoutOwnership(value));
      continue;
    }

    if (capture.isOpaqueValue() || capture.isPackElement()) {
      capturedArgs.push_back(
          emitRValueAsSingleValue(capture.getExpr()).ensurePlusOne(*this, loc));
      continue;
    }

    auto *vd = cast<VarDecl>(capture.getDecl());

    auto interfaceType = vd->getInterfaceType();

    bool isPack = false;
    if (interfaceType->is<PackExpansionType>()) {
      assert(!vd->supportsMutation() &&
             "Cannot capture a pack as an lvalue");

      SmallVector<TupleTypeElt, 1> elts;
      elts.push_back(interfaceType);
      interfaceType = TupleType::get(elts, getASTContext());

      isPack = true;
    }

    auto type = FunctionDC->mapTypeIntoContext(interfaceType);
    auto valueType = FunctionDC->mapTypeIntoContext(
      interfaceType->getReferenceStorageReferent());

    //
    // If we haven't emitted the captured value yet, we're forming a closure
    // to a local function before all of its captures have been emitted. Eg,
    //
    // func f() { g() } // transitive capture of 'x'
    // f() // closure formed here
    // var x = 123 // 'x' defined here
    // func g() { print(x) } // 'x' captured here
    //
    auto found = VarLocs.find(vd);
    if (found == VarLocs.end()) {
      auto &Diags = getASTContext().Diags;

      SourceLoc loc;
      if (closure.kind == SILDeclRef::Kind::DefaultArgGenerator) {
        auto *param = getParameterAt(closure.getDecl(),
                                     closure.defaultArgIndex);
        assert(param);
        loc = param->getLoc();
      } else {
        auto f = *closure.getAnyFunctionRef();
        loc = f.getLoc();
      }

      Diags.diagnose(loc, diag::capture_before_declaration,
                     vd->getBaseIdentifier());
      Diags.diagnose(vd->getLoc(), diag::captured_value_declared_here);
      Diags.diagnose(capture.getLoc(), diag::value_captured_here);

      // Emit an 'undef' of the correct type.
      auto captureKind = SGM.Types.getDeclCaptureKind(capture, expansion);
      switch (captureKind) {
      case CaptureKind::Constant:
        capturedArgs.push_back(emitUndef(getLoweredType(type)));
        break;
      case CaptureKind::Immutable:
      case CaptureKind::StorageAddress: {
        auto ty = getLoweredType(type);
        if (SGM.M.useLoweredAddresses())
          ty = ty.getAddressType();
        capturedArgs.push_back(emitUndef(ty));
        break;
      }
      case CaptureKind::ImmutableBox:
      case CaptureKind::Box: {
        bool isMutable = captureKind == CaptureKind::Box;
        auto boxTy = SGM.Types.getContextBoxTypeForCapture(
            vd,
            SGM.Types.getLoweredRValueType(TypeExpansionContext::minimal(),
                                           type),
            FunctionDC->getGenericEnvironmentOfContext(),
            /*mutable*/ isMutable);
        capturedArgs.push_back(emitUndef(boxTy));
        break;
      }
      }
      continue;
    }

    // Get an address value for a SILValue if it is address only in an type
    // expansion context without opaque archetype substitution.
    auto getAddressValue = [&](SILValue entryValue, bool forceCopy,
                               bool forLValue) -> SILValue {
      if (!SGM.M.useLoweredAddresses() && !forLValue && !isPack) {
        // In opaque values mode, addresses aren't used except by lvalues.
        auto &lowering = getTypeLowering(entryValue->getType());
        if (entryValue->getType().isAddress()) {
          // If the value is currently an address, load it, copying if needed.
          if (lowering.isTrivial()) {
            SILValue result = lowering.emitLoad(
                B, loc, entryValue, LoadOwnershipQualifier::Trivial);
            return result;
          }
          if (forceCopy) {
            SILValue result =
                lowering.emitLoadOfCopy(B, loc, entryValue, IsNotTake);
            enterDestroyCleanup(result);
            return result;
          } else {
            auto load = B.createLoadBorrow(
                             loc, ManagedValue::forBorrowedRValue(entryValue))
                            .getValue();
            return load;
          }
        } else {
          // Otherwise, just return it, copying if needed.
          if (forceCopy && !lowering.isTrivial()) {
            auto result = B.emitCopyValueOperation(loc, entryValue);
            return result;
          }
          return entryValue;
        }
      } else if (SGM.M.useLoweredAddresses() &&
                 SGM.Types
                     .getTypeLowering(
                         valueType, TypeExpansionContext::
                                        noOpaqueTypeArchetypesSubstitution(
                                            expansion.getResilienceExpansion()))
                     .isAddressOnly() &&
                 !entryValue->getType().isAddress()) {

        assert(!isPack);

        auto addr = emitTemporaryAllocation(vd, entryValue->getType(),
                                            DoesNotHaveDynamicLifetime,
                                            IsNotLexical, IsNotFromVarDecl,
                                            /*generateDebugInfo*/ false);
        auto val = B.emitCopyValueOperation(loc, entryValue);
        auto &lowering = getTypeLowering(entryValue->getType());
        lowering.emitStore(B, loc, val, addr, StoreOwnershipQualifier::Init);

        if (!forceCopy)
          enterDestroyCleanup(addr);
        return addr;

      } else if (isPack) {
        SILType ty = getLoweredType(valueType).getObjectType();
        auto addr = B.createAllocStack(loc, ty);
        enterDeallocStackCleanup(addr);

        auto formalPackType = cast<TupleType>(valueType->getCanonicalType())
            .getInducedPackType();
        copyPackElementsToTuple(loc, addr, entryValue, formalPackType);

        if (!forceCopy)
          enterDestroyCleanup(addr);
        return addr;
      } else if (forceCopy) {
        // We cannot pass a valid SILDebugVariable while creating the temp here
        // See rdar://60425582
        auto addr = B.createAllocStack(loc, entryValue->getType().getObjectType());
        enterDeallocStackCleanup(addr);
        B.createCopyAddr(loc, entryValue, addr, IsNotTake, IsInitialization);
        return addr;

      } else {
        return entryValue;
      }
    };

    auto Entry = found->second;
    auto val = Entry.value;

    switch (SGM.Types.getDeclCaptureKind(capture, expansion)) {
    case CaptureKind::Constant: {
      assert(!isPack);

      // let declarations.
      auto &tl = getTypeLowering(valueType);
      bool eliminateMoveOnlyWrapper =
          val->getType().isMoveOnlyWrapped() &&
          !interfaceType->is<SILMoveOnlyWrappedType>();

      if (!val->getType().isAddress()) {
        // Our 'let' binding can guarantee the lifetime for the callee,
        // if we don't need to do anything more to it.
        if (canGuarantee && !vd->getInterfaceType()->is<ReferenceStorageType>()) {
          auto guaranteed = B.borrowObjectRValue(
              *this, loc, val, ManagedValue::ScopeKind::Lexical);
          if (eliminateMoveOnlyWrapper)
            guaranteed = B.createGuaranteedMoveOnlyWrapperToCopyableValue(
                loc, guaranteed);
          capturedArgs.push_back(guaranteed);
          break;
        }

        // Just copy a by-val let.
        val = B.emitCopyValueOperation(loc, val);
        // If we need to unwrap a moveonlywrapped value, do so now but in an
        // owned way to ensure that the partial apply is viewed as a semantic
        // use of the value.
        if (eliminateMoveOnlyWrapper)
          val = B.createOwnedMoveOnlyWrapperToCopyableValue(loc, val);
      } else {
        // If we have a mutable binding for a 'let', such as 'self' in an
        // 'init' method, load it.
        if (val->getType().isMoveOnly()) {
          val = B.createMarkUnresolvedNonCopyableValueInst(
              loc, val,
              MarkUnresolvedNonCopyableValueInst::CheckKind::
                  NoConsumeOrAssign);
        }
        val = emitLoad(loc, val, tl, SGFContext(), IsNotTake).forward(*this);
      }

      // If we're capturing an unowned pointer by value, we will have just
      // loaded it into a normal retained class pointer, but we capture it as
      // an unowned pointer.  Convert back now.
      if (interfaceType->is<ReferenceStorageType>())
        val = emitConversionFromSemanticValue(loc, val, getLoweredType(type));

      capturedArgs.push_back(emitManagedRValueWithCleanup(val));
      break;
    }
    case CaptureKind::Immutable: {
      if (canGuarantee) {
        // No-escaping stored declarations are captured as the
        // address of the value.
        auto addr =
            getAddressValue(val, /*forceCopy=*/false, /*forLValue=*/false);
        capturedArgs.push_back(
            addr->getOwnershipKind() == OwnershipKind::Owned
                ? ManagedValue::forOwnedRValue(addr, CleanupHandle::invalid())
                : ManagedValue::forBorrowedRValue(addr));
      } else {
        auto addr =
            getAddressValue(val, /*forceCopy=*/true, /*forLValue=*/false);
        if (!useLoweredAddresses()) {
          auto &lowering = getTypeLowering(addr->getType());
          auto rvalue =
              lowering.isTrivial()
                  ? ManagedValue::forObjectRValueWithoutOwnership(addr)
                  : ManagedValue::forOwnedRValue(addr,
                                                 CleanupHandle::invalid());
          capturedArgs.push_back(rvalue);
          break;
        }
        // If our address is move only wrapped, unwrap it.
        if (addr->getType().isMoveOnlyWrapped()) {
          addr = B.createMoveOnlyWrapperToCopyableAddr(loc, addr);
        }
        capturedArgs.push_back(ManagedValue::forOwnedAddressRValue(
            addr, CleanupHandle::invalid()));
      }
      break;
    }
    case CaptureKind::StorageAddress: {
      assert(!isPack);

      auto addr = getAddressValue(val, /*forceCopy=*/false, /*forLValue=*/true);

      // No-escaping stored declarations are captured as the
      // address of the value.
      assert(addr->getType().isAddress() && "no address for captured var!");

      // If we have a moveonlywrapped address type, unwrap it.
      if (addr->getType().isMoveOnlyWrapped())
        addr = B.createMoveOnlyWrapperToCopyableAddr(loc, addr);

      capturedArgs.push_back(ManagedValue::forLValue(addr));
      break;
    }

    case CaptureKind::Box: {
      assert(!isPack);

      assert(val->getType().isAddress() &&
             "no address for captured var!");
      // Boxes of opaque return values stay opaque.
      auto minimalLoweredType = SGM.Types.getLoweredRValueType(
          TypeExpansionContext::minimal(), type->getCanonicalType());
      // If this is a boxed variable, we can use it directly.
      if (Entry.box &&
          val->getType().getASTType() == minimalLoweredType) {
        ManagedValue box;
        // We can guarantee our own box to the callee.
        if (canGuarantee) {
          box = B.borrowObjectRValue(*this, loc, Entry.box,
                                     ManagedValue::ScopeKind::Lexical);
        } else {
          box = B.copyOwnedObjectRValue(loc, Entry.box,
                                        ManagedValue::ScopeKind::Lexical);
        }
        assert(box);

        // If our captured value is a box with a moveonlywrapped type inside,
        // unwrap it.
        if (box.getType().isBoxedMoveOnlyWrappedType(&F)) {
          CleanupCloner cloner(*this, box);
          box = cloner.clone(
              B.createMoveOnlyWrapperToCopyableBox(loc, box.forward(*this)));
        }

        capturedArgs.push_back(box);

        if (captureCanEscape)
          escapesToMark.push_back(val);
      } else {
        // Address only 'let' values are passed by box.  This isn't great, in
        // that a variable captured by multiple closures will be boxed for each
        // one.  This could be improved by doing an "isCaptured" analysis when
        // emitting address-only let constants, and emit them into an alloc_box
        // like a variable instead of into an alloc_stack.
        //
        // TODO: This might not be profitable anymore with guaranteed captures,
        // since we could conceivably forward the copied value into the
        // closure context and pass it down to the partially applied function
        // in-place.
        // TODO: Use immutable box for immutable captures.
        auto boxTy = SGM.Types.getContextBoxTypeForCapture(
            vd, minimalLoweredType,
            FunctionDC->getGenericEnvironmentOfContext(),
            /*mutable*/ true);

        AllocBoxInst *allocBox = B.createAllocBox(loc, boxTy);
        ProjectBoxInst *boxAddress = B.createProjectBox(loc, allocBox, 0);
        B.createCopyAddr(loc, val, boxAddress, IsNotTake,
                         IsInitialization);
        if (canGuarantee)
          capturedArgs.push_back(
              emitManagedRValueWithCleanup(allocBox).borrow(*this, loc));
        else
          capturedArgs.push_back(emitManagedRValueWithCleanup(allocBox));
      }

      break;
    }
    case CaptureKind::ImmutableBox: {
      assert(!isPack);

      assert(val->getType().isAddress() &&
             "no address for captured var!");
      // Boxes of opaque return values stay opaque.
      auto minimalLoweredType = SGM.Types.getLoweredRValueType(
          TypeExpansionContext::minimal(), type->getCanonicalType());
      // If this is a boxed variable, we can use it directly.
      if (Entry.box &&
          val->getType().getASTType() == minimalLoweredType) {
        // We can guarantee our own box to the callee.
        if (canGuarantee) {
          capturedArgs.push_back(B.borrowObjectRValue(
              *this, loc, Entry.box, ManagedValue::ScopeKind::Lexical));
        } else {
          capturedArgs.push_back(emitManagedCopy(loc, Entry.box));
        }
        if (captureCanEscape)
          escapesToMark.push_back(val);
      } else {
        // Address only 'let' values are passed by box.  This isn't great, in
        // that a variable captured by multiple closures will be boxed for each
        // one.  This could be improved by doing an "isCaptured" analysis when
        // emitting address-only let constants, and emit them into an alloc_box
        // like a variable instead of into an alloc_stack.
        //
        // TODO: This might not be profitable anymore with guaranteed captures,
        // since we could conceivably forward the copied value into the
        // closure context and pass it down to the partially applied function
        // in-place.
        // TODO: Use immutable box for immutable captures.
        auto boxTy = SGM.Types.getContextBoxTypeForCapture(
            vd, minimalLoweredType,
            FunctionDC->getGenericEnvironmentOfContext(),
            /*mutable*/ false);

        AllocBoxInst *allocBox = B.createAllocBox(loc, boxTy);
        ProjectBoxInst *boxAddress = B.createProjectBox(loc, allocBox, 0);
        B.createCopyAddr(loc, val, boxAddress, IsNotTake,
                         IsInitialization);
        if (canGuarantee)
          capturedArgs.push_back(
              emitManagedRValueWithCleanup(allocBox).borrow(*this, loc));
        else
          capturedArgs.push_back(emitManagedRValueWithCleanup(allocBox));
      }

      break;
    }
    }
  }
  
  // Mark box addresses as captured for DI purposes. The values must have
  // been fully initialized before we close over them.
  if (!escapesToMark.empty()) {
    B.createMarkFunctionEscape(loc, escapesToMark);
  }
}

ManagedValue
SILGenFunction::emitClosureValue(SILLocation loc, SILDeclRef constant,
                                 const FunctionTypeInfo &typeContext,
                                 SubstitutionMap subs) {
  auto loweredCaptureInfo = SGM.Types.getLoweredLocalCaptures(constant);
  SGM.Types.setCaptureTypeExpansionContext(constant, SGM.M);
  
  auto constantInfo = getConstantInfo(getTypeExpansionContext(), constant);
  SILValue functionRef = emitGlobalFunctionRef(loc, constant, constantInfo);
  SILType functionTy = functionRef->getType();

  // Apply substitutions.
  auto pft = constantInfo.SILFnType;

  if (constant.getAbstractClosureExpr()) {
    // If we have a closure expression in generic context, Sema won't give
    // us substitutions, so we just use the forwarding substitutions from
    // context.
    std::tie(std::ignore, std::ignore, subs)
        = SGM.Types.getForwardingSubstitutionsForLowering(constant);
  } else {
    subs = SGM.Types.getSubstitutionMapWithCapturedEnvironments(
        constant, loweredCaptureInfo, subs);
  }

  // We completely drop the generic signature if all generic parameters were
  // concrete.
  if (!pft->isPolymorphic()) {
    subs = SubstitutionMap();
  } else {
    assert(!subs.getGenericSignature()->areAllParamsConcrete());

    auto specialized =
        pft->substGenericArgs(F.getModule(), subs, getTypeExpansionContext());
    functionTy = SILType::getPrimitiveObjectType(specialized);
  }

  auto closure = *constant.getAnyFunctionRef();
  auto *dc = closure.getAsDeclContext()->getParent();

  // If we're in top-level code, we don't need to physically capture script
  // globals, but we still need to mark them as escaping so that DI can flag
  // uninitialized uses.
  if (isEmittingTopLevelCode() && dc->getParentSourceFile()) {
    auto captureInfo = closure.getCaptureInfo();
    emitMarkFunctionEscapeForTopLevelCodeGlobals(loc, captureInfo);
  }

  bool hasErasedIsolation =
    typeContext.ExpectedLoweredType->hasErasedIsolation();

  ManagedValue result;
  if (loweredCaptureInfo.getCaptures().empty() && !subs &&
      !hasErasedIsolation) {
    result = ManagedValue::forObjectRValueWithoutOwnership(functionRef);
  } else {
    SmallVector<ManagedValue, 4> capturedArgs;
    emitCaptures(loc, constant, CaptureEmission::PartialApplication,
                 capturedArgs);

    // Compute the erased isolation
    ManagedValue isolation;
    if (hasErasedIsolation) {
      isolation = emitClosureIsolation(loc, constant, capturedArgs);
    }

    // The partial application takes ownership of the context parameters.
    SmallVector<SILValue, 4> forwardedArgs;
    if (hasErasedIsolation)
      forwardedArgs.push_back(isolation.forward(*this));
    for (auto capture : capturedArgs)
      forwardedArgs.push_back(capture.forward(*this));

    auto calleeConvention = ParameterConvention::Direct_Guaranteed;

    auto resultIsolation =
        (hasErasedIsolation ? SILFunctionTypeIsolation::Erased
                            : SILFunctionTypeIsolation::Unknown);
    auto toClosure =
      B.createPartialApply(loc, functionRef, subs, forwardedArgs,
                           calleeConvention, resultIsolation);
    result = emitManagedRValueWithCleanup(toClosure);
  }

  // Generalize if necessary.
  if (result.getType().getASTType() != typeContext.ExpectedLoweredType) {
    result = emitTransformedValue(loc, result,
                                  AbstractionPattern(subs, constantInfo.LoweredType),
                                  typeContext.FormalType,
                                  typeContext.OrigType,
                                  typeContext.FormalType,
              SILType::getPrimitiveObjectType(typeContext.ExpectedLoweredType));

    auto resultType = cast<SILFunctionType>(result.getType().getASTType());

    // Check if we performed Sendable/sending type compensation in
    // emitTransformedValue for a closure. If we did, insert some fixup code to
    // convert from an @Sendable to a not-@Sendable value.
    //
    // DISCUSSION: We cannot do this internally to emitTransformedValue since it
    // does not have access to our SILDeclRef.
    if (auto *e = constant.getClosureExpr()) {
      auto actualType = cast<AnyFunctionType>(e->getType()->getCanonicalType());
      if (e->inheritsActorContext() &&
          e->getActorIsolation().isActorIsolated() && actualType->isAsync() &&
          !actualType->isSendable() && resultType->isSendable()) {
        auto extInfo = resultType->getExtInfo().withSendable(false);
        resultType = resultType->getWithExtInfo(extInfo);
        result = B.createConvertFunction(
            loc, result, SILType::getPrimitiveObjectType(resultType));
      }
    }
  }

  return result;
}

void SILGenFunction::emitFunction(FuncDecl *fd) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(fd);

  auto captureInfo = SGM.M.Types.getLoweredLocalCaptures(SILDeclRef(fd));
  emitProlog(fd, captureInfo, fd->getParameters(), fd->getImplicitSelfDecl(),
             fd->getResultInterfaceType(), fd->getEffectiveThrownErrorType(),
             fd->getThrowsLoc());

  if (fd->isDistributedActorFactory()) {
    // Synthesize the factory function body
    emitDistributedActorFactory(fd);
  } else {
    prepareEpilog(fd,
                  fd->getResultInterfaceType(),
                  fd->getEffectiveThrownErrorType(),
                  CleanupLocation(fd));

    if (fd->requiresUnavailableDeclABICompatibilityStubs())
      emitApplyOfUnavailableCodeReached();

    assert(!fd->isBodySkipped());
    emitProfilerIncrement(fd->getTypecheckedBody());

    // Emit the actual function body as usual
    emitStmt(fd->getTypecheckedBody());

    emitEpilog(fd);
  }

  mergeCleanupBlocks();
}

void SILGenFunction::emitClosure(AbstractClosureExpr *ace) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(ace);

  auto &closureInfo = SGM.M.Types.getClosureTypeInfo(ace);
  TypeContext = closureInfo;

  auto resultIfaceTy = ace->getResultType()->mapTypeOutOfContext();
  std::optional<Type> errorIfaceTy;
  if (auto optErrorTy = ace->getEffectiveThrownType())
    errorIfaceTy = (*optErrorTy)->mapTypeOutOfContext();
  auto captureInfo = SGM.M.Types.getLoweredLocalCaptures(
    SILDeclRef(ace));
  emitProlog(ace, captureInfo, ace->getParameters(), /*selfParam=*/nullptr,
             resultIfaceTy, errorIfaceTy, ace->getLoc());
  prepareEpilog(ace, resultIfaceTy, errorIfaceTy,
                CleanupLocation(ace));

  emitProfilerIncrement(ace);
  if (auto *ce = dyn_cast<ClosureExpr>(ace)) {
    emitStmt(ce->getBody());
  } else {
    auto *autoclosure = cast<AutoClosureExpr>(ace);
    // Closure expressions implicitly return the result of their body
    // expression.
    if (B.hasValidInsertionPoint()) {
      emitReturnExpr(ImplicitReturnLocation(ace),
                     autoclosure->getSingleExpressionBody());
    }
  }
  emitEpilog(ace);
}

void SILGenFunction::emitArtificialTopLevel(Decl *mainDecl) {
  // Create the argc and argv arguments.
  auto entry = B.getInsertionBB();
  auto paramTypeIter = F.getConventions()
                           .getParameterSILTypes(getTypeExpansionContext())
                           .begin();

  SILValue argc;
  SILValue argv;
  const bool isAsyncFunc =
      isa<FuncDecl>(mainDecl) && static_cast<FuncDecl *>(mainDecl)->hasAsync();
  if (!isAsyncFunc) {
    argc = entry->createFunctionArgument(*paramTypeIter);
    argv = entry->createFunctionArgument(*std::next(paramTypeIter));
  }

  switch (mainDecl->getArtificialMainKind()) {
  case ArtificialMainKind::UIApplicationMain: {
    // Emit a UIKit main.
    // return UIApplicationMain(C_ARGC, C_ARGV, nil, ClassName);

    auto *mainClass = cast<NominalTypeDecl>(mainDecl);

    CanType NSStringTy = SGM.Types.getNSStringType();
    CanType OptNSStringTy
      = OptionalType::get(NSStringTy)->getCanonicalType();

    // Look up UIApplicationMain.
    // FIXME: Doing an AST lookup here is gross and not entirely sound;
    // we're getting away with it because the types are guaranteed to already
    // be imported.
    ASTContext &ctx = getASTContext();
    
    ImportPath::Element UIKitName =
      {ctx.getIdentifier("UIKit"), SourceLoc()};

    ModuleDecl *UIKit = ctx.getClangModuleLoader()->loadModule(
        SourceLoc(), ImportPath::Module(llvm::ArrayRef(UIKitName)));
    assert(UIKit && "couldn't find UIKit objc module?!");
    SmallVector<ValueDecl *, 2> results;
    UIKit->lookupQualified(UIKit,
                           DeclNameRef(ctx.getIdentifier("UIApplicationMain")),
                           SourceLoc(), NL_QualifiedDefault,
                           results);

    // As the comment above alludes, using a qualified lookup into UIKit is
    // *not* sound. In particular, it's possible for the lookup to find the
    // (deprecated) Swift copy of UIApplicationMain in UIKit and try to call
    // that instead of the C entrypoint. Let's try to force this to happen.
    auto FoundUIApplicationMain = llvm::find_if(results, [](const ValueDecl *VD) {
      return !VD->getClangNode().isNull();
    });
    assert(FoundUIApplicationMain != results.end() &&
           "Could not find a UIApplicationMain to call!");
    ValueDecl *UIApplicationMainDecl = *FoundUIApplicationMain;

    auto mainRef = SILDeclRef(UIApplicationMainDecl).asForeign();
    SILGenFunctionBuilder builder(SGM);
    auto UIApplicationMainFn =
        builder.getOrCreateFunction(mainClass, mainRef, NotForDefinition);
    auto fnTy = UIApplicationMainFn->getLoweredFunctionType();
    SILFunctionConventions fnConv(fnTy, SGM.M);

    // Get the class name as a string using NSStringFromClass.
    CanType mainClassTy = mainClass->getDeclaredInterfaceType()
        ->getCanonicalType();
    CanType mainClassMetaty = CanMetatypeType::get(mainClassTy,
                                                   MetatypeRepresentation::ObjC);
    CanType anyObjectTy = ctx.getAnyObjectType();
    CanType anyObjectMetaTy = CanExistentialMetatypeType::get(anyObjectTy,
                                                  MetatypeRepresentation::ObjC);

    auto conformances =
        SGM.SwiftModule->collectExistentialConformances(mainClassMetaty,
                                                        anyObjectMetaTy);

    auto paramConvention = ParameterConvention::Direct_Unowned;
    auto params = {SILParameterInfo(anyObjectMetaTy, paramConvention)};
    std::array<SILResultInfo, 1> resultInfos = {
        SILResultInfo(OptNSStringTy, ResultConvention::Autoreleased)};
    auto repr = SILFunctionType::Representation::CFunctionPointer;
    auto *clangFnType =
        ctx.getCanonicalClangFunctionType(params, resultInfos[0], repr);
    auto extInfo = SILFunctionType::ExtInfoBuilder()
                       .withRepresentation(repr)
                       .withClangFunctionType(clangFnType)
                       .build();

    auto NSStringFromClassType = SILFunctionType::get(
        nullptr, extInfo, SILCoroutineKind::None, paramConvention, params,
        /*yields*/ {}, resultInfos, /*error result*/ std::nullopt,
        SubstitutionMap(), SubstitutionMap(), ctx);

    auto NSStringFromClassFn = builder.getOrCreateFunction(
        mainClass, "NSStringFromClass", SILLinkage::PublicExternal,
        NSStringFromClassType, IsBare, IsTransparent, IsNotSerialized,
        IsNotDynamic, IsNotDistributed, IsNotRuntimeAccessible);
    auto NSStringFromClass = B.createFunctionRef(mainClass, NSStringFromClassFn);
    SILValue metaTy = B.createMetatype(mainClass,
                             SILType::getPrimitiveObjectType(mainClassMetaty));
    metaTy = B.createInitExistentialMetatype(mainClass, metaTy,
                          SILType::getPrimitiveObjectType(anyObjectMetaTy),
                          conformances);
    SILValue optNameValue = B.createApply(
        mainClass, NSStringFromClass, {}, metaTy);
    ManagedValue optName = emitManagedRValueWithCleanup(optNameValue);

    // Fix up the string parameters to have the right type.
    SILType nameArgTy =
        fnConv.getSILArgumentType(3, B.getTypeExpansionContext());
    assert(nameArgTy ==
           fnConv.getSILArgumentType(2, B.getTypeExpansionContext()));
    (void)nameArgTy;
    assert(optName.getType() == nameArgTy);
    SILValue nilValue =
        getOptionalNoneValue(mainClass, getTypeLowering(OptNSStringTy));

    // Fix up argv to have the right type.
    auto argvTy = fnConv.getSILArgumentType(1, B.getTypeExpansionContext());

    SILType unwrappedTy = argvTy;
    if (Type innerTy = argvTy.getASTType()->getOptionalObjectType()) {
      auto canInnerTy = innerTy->getCanonicalType();
      unwrappedTy = SILType::getPrimitiveObjectType(canInnerTy);
    }

    auto managedArgv = ManagedValue::forObjectRValueWithoutOwnership(argv);

    if (unwrappedTy != argv->getType()) {
      auto converted =
          emitPointerToPointer(mainClass, managedArgv,
                               argv->getType().getASTType(),
                               unwrappedTy.getASTType());
      managedArgv = std::move(converted).getAsSingleValue(*this, mainClass);
    }

    if (unwrappedTy != argvTy) {
      managedArgv = getOptionalSomeValue(mainClass, managedArgv,
                                         getTypeLowering(argvTy));
    }

    auto UIApplicationMain = B.createFunctionRef(mainClass, UIApplicationMainFn);

    SILValue args[] = {argc, managedArgv.getValue(), nilValue,
                       optName.getValue()};

    B.createApply(mainClass, UIApplicationMain, SubstitutionMap(), args);
    SILValue r = B.createIntegerLiteral(mainClass,
                        SILType::getBuiltinIntegerType(32, ctx), 0);
    auto rType =
        F.getConventions().getSingleSILResultType(B.getTypeExpansionContext());
    if (r->getType() != rType)
      r = B.createStruct(mainClass, rType, r);

    Cleanups.emitCleanupsForReturn(mainClass, NotForUnwind);
    B.createReturn(mainClass, r);
    return;
  }

  case ArtificialMainKind::NSApplicationMain: {
    // Emit an AppKit main.
    // return NSApplicationMain(C_ARGC, C_ARGV);

    auto *mainClass = cast<NominalTypeDecl>(mainDecl);

    SILParameterInfo argTypes[] = {
      SILParameterInfo(argc->getType().getASTType(),
                       ParameterConvention::Direct_Unowned),
      SILParameterInfo(argv->getType().getASTType(),
                       ParameterConvention::Direct_Unowned),
    };
    auto NSApplicationMainType = SILFunctionType::get(
        nullptr,
        // Should be C calling convention, but NSApplicationMain
        // has an overlay to fix the type of argv.
        SILFunctionType::ExtInfo::getThin(), SILCoroutineKind::None,
        ParameterConvention::Direct_Unowned, argTypes,
        /*yields*/ {},
        SILResultInfo(argc->getType().getASTType(), ResultConvention::Unowned),
        /*error result*/ std::nullopt, SubstitutionMap(), SubstitutionMap(),
        getASTContext());

    SILGenFunctionBuilder builder(SGM);
    auto NSApplicationMainFn = builder.getOrCreateFunction(
        mainClass, "NSApplicationMain", SILLinkage::PublicExternal,
        NSApplicationMainType, IsBare, IsTransparent, IsNotSerialized,
        IsNotDynamic, IsNotDistributed, IsNotRuntimeAccessible);

    auto NSApplicationMain = B.createFunctionRef(mainClass, NSApplicationMainFn);
    SILValue args[] = { argc, argv };

    B.createApply(mainClass, NSApplicationMain, SubstitutionMap(), args);
    SILValue r = B.createIntegerLiteral(mainClass,
                        SILType::getBuiltinIntegerType(32, getASTContext()), 0);
    auto rType =
        F.getConventions().getSingleSILResultType(B.getTypeExpansionContext());
    if (r->getType() != rType)
      r = B.createStruct(mainClass, rType, r);
    B.createReturn(mainClass, r);
    return;
  }

  case ArtificialMainKind::TypeMain: {
    // Emit a call to the main static function.
    emitCallToMain(cast<FuncDecl>(mainDecl));
    return;
  }
  }
}

void SILGenFunction::emitAsyncMainThreadStart(SILDeclRef entryPoint) {
  auto moduleLoc = entryPoint.getAsRegularLocation();
  auto *entryBlock = B.getInsertionBB();
  auto paramTypeIter = F.getConventions()
                           .getParameterSILTypes(getTypeExpansionContext())
                           .begin();

  entryBlock->createFunctionArgument(*paramTypeIter);            // argc
  entryBlock->createFunctionArgument(*std::next(paramTypeIter)); // argv

  // Lookup necessary functions
  swift::ASTContext &ctx = entryPoint.getASTContext();

  B.setInsertionPoint(entryBlock);

  auto wrapCallArgs = [this, &moduleLoc](SILValue originalValue, FuncDecl *fd,
                            uint32_t paramIndex) -> SILValue {
    Type parameterType = fd->getParameters()->get(paramIndex)->getTypeInContext();
    SILType paramSILType = SILType::getPrimitiveObjectType(parameterType->getCanonicalType());
    // If the types are the same, we don't need to do anything!
    if (paramSILType == originalValue->getType())
      return originalValue;
    return this->B.createStruct(moduleLoc, paramSILType, originalValue);
  };

  // Call CreateAsyncTask
  FuncDecl *builtinDecl = cast<FuncDecl>(getBuiltinValueDecl(
      ctx,
      ctx.getIdentifier(getBuiltinName(BuiltinValueKind::CreateAsyncTask))));
  auto subs = SubstitutionMap::get(builtinDecl->getGenericSignature(),
                                   {TupleType::getEmpty(ctx)},
                                   ArrayRef<ProtocolConformanceRef>{});

  SILValue mainFunctionRef = emitGlobalFunctionRef(moduleLoc, entryPoint);

  // Emit the CreateAsyncTask builtin
  TaskCreateFlags taskCreationFlagMask(0);
  taskCreationFlagMask.setInheritContext(true);
  SILValue taskFlags =
      emitWrapIntegerLiteral(moduleLoc, getLoweredType(ctx.getIntType()),
                             taskCreationFlagMask.getOpaqueValue());

  SILValue task = emitCreateAsyncMainTask(
          moduleLoc, subs,
          ManagedValue::forObjectRValueWithoutOwnership(taskFlags),
          ManagedValue::forObjectRValueWithoutOwnership(mainFunctionRef))
          .forward(*this);
  DestructureTupleInst *structure = B.createDestructureTuple(moduleLoc, task);
  task = structure->getResult(0);

  // Get swiftJobRun
  FuncDecl *swiftJobRunFuncDecl = SGM.getSwiftJobRun();
  assert(swiftJobRunFuncDecl && "Failed to find swift_job_run function decl");
  SILFunction *swiftJobRunSILFunc =
      SGM.getFunction(SILDeclRef(swiftJobRunFuncDecl, SILDeclRef::Kind::Func),
                      NotForDefinition);
  SILValue swiftJobRunFunc =
      B.createFunctionRefFor(moduleLoc, swiftJobRunSILFunc);

  // Convert task to job
  SILType JobType = SILType::getPrimitiveObjectType(
      getBuiltinType(ctx, "Job")->getCanonicalType());
  SILValue jobResult = B.createBuiltin(
      moduleLoc,
      ctx.getIdentifier(getBuiltinName(BuiltinValueKind::ConvertTaskToJob)),
      JobType, {}, {task});
  jobResult = wrapCallArgs(jobResult, swiftJobRunFuncDecl, 0);

  ModuleDecl * moduleDecl = entryPoint.getModuleContext();

  SILValue mainExecutor = emitMainExecutor(moduleLoc);
  mainExecutor = wrapCallArgs(mainExecutor, swiftJobRunFuncDecl, 1);

  // Run first part synchronously
  B.createApply(moduleLoc, swiftJobRunFunc, {}, {jobResult, mainExecutor});

  // Start Main loop!
  FuncDecl *drainQueueFuncDecl = SGM.getAsyncMainDrainQueue();
  if (!drainQueueFuncDecl) {
    // If it doesn't exist, we can conjure one up instead of crashing
    // @available(SwiftStdlib 5.5, *)
    // @_silgen_name("swift_task_asyncMainDrainQueue")
    // internal func _asyncMainDrainQueue() -> Never
    ParameterList *emptyParams = ParameterList::createEmpty(getASTContext());
    drainQueueFuncDecl = FuncDecl::createImplicit(
        getASTContext(), StaticSpellingKind::None,
        DeclName(
            getASTContext(),
            DeclBaseName(getASTContext().getIdentifier("_asyncMainDrainQueue")),
            /*Arguments*/ emptyParams),
        {}, /*async*/ false, /*throws*/ false, /*thrownType*/Type(), {},
        emptyParams,
        getASTContext().getNeverType(), moduleDecl);
    drainQueueFuncDecl->getAttrs().add(new (getASTContext()) SILGenNameAttr(
        "swift_task_asyncMainDrainQueue", /*raw*/ false, /*implicit*/ true));
  }

  SILFunction *drainQueueSILFunc = SGM.getFunction(
      SILDeclRef(drainQueueFuncDecl, SILDeclRef::Kind::Func), NotForDefinition);
  SILValue drainQueueFunc =
      B.createFunctionRefFor(moduleLoc, drainQueueSILFunc);
  B.createApply(moduleLoc, drainQueueFunc, {}, {});
  B.createUnreachable(moduleLoc);
  return;
}

void SILGenFunction::emitGeneratorFunction(SILDeclRef function, Expr *value,
                                           bool EmitProfilerIncrement) {
  auto *const topLevelValue = value;
  auto *dc = function.getDecl()->getInnermostDeclContext();
  MagicFunctionName = SILGenModule::getMagicFunctionName(function);

  RegularLocation Loc(value);
  Loc.markAutoGenerated();

  // If a default argument or stored property initializer value is a noescape
  // function type, strip the escape to noescape function conversion.
  if (function.kind == SILDeclRef::Kind::DefaultArgGenerator ||
      function.kind == SILDeclRef::Kind::StoredPropertyInitializer) {
    if (auto funType = value->getType()->getAs<AnyFunctionType>()) {
      if (funType->getExtInfo().isNoEscape()) {
        auto conv = cast<FunctionConversionExpr>(value);
        value = conv->getSubExpr();
        assert(funType->withExtInfo(funType->getExtInfo().withNoEscape(false))
                   ->isEqual(value->getType()));
      }
    }
  }

  // For a property wrapper backing initializer, form a parameter list
  // containing the wrapped or projected value.
  ParameterList *params = nullptr;
  if (function.kind == SILDeclRef::Kind::PropertyWrapperBackingInitializer ||
      function.kind == SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue) {
    auto &ctx = getASTContext();
    auto param = new (ctx) ParamDecl(SourceLoc(), SourceLoc(),
                                     ctx.getIdentifier("$input_value"),
                                     SourceLoc(),
                                     ctx.getIdentifier("$input_value"),
                                     dc);
    param->setSpecifier(ParamSpecifier::LegacyOwned);
    param->setImplicit();
    auto vd = cast<VarDecl>(function.getDecl());
    if (function.kind == SILDeclRef::Kind::PropertyWrapperBackingInitializer) {
      param->setInterfaceType(vd->getPropertyWrapperInitValueInterfaceType());
    } else {
      auto *placeholder =
          vd->getPropertyWrapperInitializerInfo().getProjectedValuePlaceholder();
      auto interfaceType = placeholder->getType();
      if (interfaceType->hasArchetype())
        interfaceType = interfaceType->mapTypeOutOfContext();

      param->setInterfaceType(interfaceType);
    }

    params = ParameterList::create(ctx, SourceLoc(), {param}, SourceLoc());
  }

  auto captureInfo = SGM.M.Types.getLoweredLocalCaptures(function);
  auto interfaceType = value->getType()->mapTypeOutOfContext();
  emitProlog(dc, captureInfo, params, /*selfParam=*/nullptr, interfaceType,
             /*errorType=*/std::nullopt, SourceLoc());
  if (EmitProfilerIncrement) {
    // Emit a profiler increment for the top-level value, not looking through
    // any function conversions. This is necessary as the counter would have
    // been recorded for this expression, not the sub-expression.
    emitProfilerIncrement(topLevelValue);
  }
  prepareEpilog(dc, interfaceType, std::nullopt, CleanupLocation(Loc));

  {
    std::optional<SILGenFunction::OpaqueValueRAII> opaqueValue;

    // For a property wrapper backing initializer, bind the opaque value used
    // in the initializer expression to the given parameter.
    if (function.kind == SILDeclRef::Kind::PropertyWrapperBackingInitializer) {
      auto var = cast<VarDecl>(function.getDecl());
      auto initInfo = var->getPropertyWrapperInitializerInfo();
      auto param = params->get(0);
      auto *placeholder = initInfo.getWrappedValuePlaceholder();
      opaqueValue.emplace(
          *this, placeholder->getOpaqueValuePlaceholder(),
          maybeEmitValueOfLocalVarDecl(param, AccessKind::Read));

      assert(value == initInfo.getInitFromWrappedValue());
    } else if (function.kind == SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue) {
      auto var = cast<VarDecl>(function.getDecl());
      auto initInfo = var->getPropertyWrapperInitializerInfo();
      auto param = params->get(0);
      auto *placeholder = initInfo.getProjectedValuePlaceholder();
      opaqueValue.emplace(
          *this, placeholder->getOpaqueValuePlaceholder(),
          maybeEmitValueOfLocalVarDecl(param, AccessKind::Read));

      assert(value == initInfo.getInitFromProjectedValue());
    }

    emitReturnExpr(Loc, value);
  }

  emitEpilog(Loc);
  mergeCleanupBlocks();
}

void SILGenFunction::emitGeneratorFunction(SILDeclRef function, VarDecl *var) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(function);

  RegularLocation loc(var);
  loc.markAutoGenerated();

  auto decl = function.getAbstractFunctionDecl();
  auto *dc = decl->getInnermostDeclContext();
  auto interfaceType = var->getValueInterfaceType();

  // If this is the backing storage for a property with an attached
  // wrapper that was initialized with '=', the stored property initializer
  // will be in terms of the original property's type.
  if (auto originalProperty = var->getOriginalWrappedProperty()) {
    if (originalProperty->isPropertyMemberwiseInitializedWithWrappedType()) {
      interfaceType = originalProperty->getPropertyWrapperInitValueInterfaceType();

      if (auto fnType = interfaceType->getAs<AnyFunctionType>()) {
        auto newExtInfo = fnType->getExtInfo().withNoEscape(false);
        interfaceType = fnType->withExtInfo(newExtInfo);
      }
    }
  }

  emitBasicProlog(dc,
                  /*paramList*/ nullptr,
                  /*selfParam*/ nullptr, interfaceType,
                  /*errorType=*/std::nullopt,
                  /*throwsLoc=*/SourceLoc(),
                  /*ignored parameters*/ 0);
  prepareEpilog(dc, interfaceType, std::nullopt, CleanupLocation(loc));

  auto pbd = var->getParentPatternBinding();
  const auto i = pbd->getPatternEntryIndexForVarDecl(var);
  auto *anchorVar = pbd->getAnchoringVarDecl(i);
  auto subs = getForwardingSubstitutionMap();
  auto contextualType = dc->mapTypeIntoContext(interfaceType);
  auto resultType = contextualType->getCanonicalType();
  auto origResultType = AbstractionPattern(resultType);

  SmallVector<SILValue, 4> directResults;

  if (F.getConventions().hasIndirectSILResults()) {
    Scope scope(Cleanups, CleanupLocation(var));

    SmallVector<CleanupHandle, 4> cleanups;
    auto init = prepareIndirectResultInit(loc, AbstractionPattern(resultType),
                                          resultType, directResults, cleanups);

    emitApplyOfStoredPropertyInitializer(loc, anchorVar, subs, resultType,
                                         origResultType,
                                         SGFContext(init.get()));

    for (auto cleanup : cleanups) {
      Cleanups.forwardCleanup(cleanup);
    }
  } else {
    Scope scope(Cleanups, CleanupLocation(var));

    // If we have no indirect results, just return the result.
    auto result = emitApplyOfStoredPropertyInitializer(loc, anchorVar, subs,
                                                       resultType,
                                                       origResultType,
                                                       SGFContext())
                    .ensurePlusOne(*this, loc);
    std::move(result).forwardAll(*this, directResults);
  }

  Cleanups.emitBranchAndCleanups(ReturnDest, loc, directResults);
  emitEpilog(loc);
}

void SILGenFunction::emitGeneratorFunction(
    SILDeclRef function, Type resultInterfaceType, BraceStmt *body) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(function);

  RegularLocation loc(function.getDecl());
  loc.markAutoGenerated();

  auto *dc = function.getDecl()->getInnermostDeclContext();
  auto captureInfo = SGM.M.Types.getLoweredLocalCaptures(function);
  emitProlog(dc, captureInfo, ParameterList::createEmpty(getASTContext()),
             /*selfParam=*/nullptr, resultInterfaceType,
             /*errorType=*/std::nullopt, SourceLoc());

  prepareEpilog(dc, resultInterfaceType, std::nullopt, CleanupLocation(loc));

  emitStmt(body);

  emitEpilog(loc);
  mergeCleanupBlocks();
}

std::unique_ptr<Initialization> SILGenFunction::getSingleValueStmtInit(Expr *E) {
  if (SingleValueStmtInitStack.empty())
    return nullptr;

  // Check to see if this is an expression branch of an active
  // SingleValueStmtExpr initialization.
  if (!SingleValueStmtInitStack.back().Exprs.contains(E))
    return nullptr;
  
  auto resultAddr = SingleValueStmtInitStack.back().InitializationBuffer;
  return std::make_unique<KnownAddressInitialization>(resultAddr);
}

void SILGenFunction::emitProfilerIncrement(ASTNode Node) {
  emitProfilerIncrement(ProfileCounterRef::node(Node));
}

void SILGenFunction::emitProfilerIncrement(ProfileCounterRef Ref) {
  // Ignore functions which aren't set up for instrumentation.
  SILProfiler *SP = F.getProfiler();
  if (!SP)
    return;
  if (!SP->hasRegionCounters() || !getModule().getOptions().UseProfile.empty())
    return;

  auto CounterIdx = SP->getCounterIndexFor(Ref);

  // If we're at an unreachable point, the increment can be elided as the
  // counter cannot be incremented.
  if (!B.hasValidInsertionPoint())
    return;

  B.createIncrementProfilerCounter(
      Ref.getLocation(), CounterIdx, SP->getPGOFuncName(),
      SP->getNumRegionCounters(), SP->getPGOFuncHash());
}

ProfileCounter SILGenFunction::loadProfilerCount(ASTNode Node) const {
  if (SILProfiler *SP = F.getProfiler())
    return SP->getExecutionCount(Node);
  return ProfileCounter();
}

std::optional<ASTNode> SILGenFunction::getPGOParent(ASTNode Node) const {
  if (SILProfiler *SP = F.getProfiler())
    return SP->getPGOParent(Node);
  return std::nullopt;
}

SILValue SILGenFunction::emitUnwrapIntegerResult(SILLocation loc,
                                                 SILValue value) {
  // This is a loop because we want to handle types that wrap integer types,
  // like ObjCBool (which may be Bool or Int8).
  while (!value->getType().is<BuiltinIntegerType>()) {
    auto structDecl = value->getType().getStructOrBoundGenericStruct();
    assert(structDecl && "value for error result wasn't of struct type!");
    assert(structDecl->getStoredProperties().size() == 1);
    auto property = structDecl->getStoredProperties()[0];
    value = B.createStructExtract(loc, value, property);
  }

  return value;
}

SILValue SILGenFunction::emitWrapIntegerLiteral(SILLocation loc,
                                                SILType ty,
                                                unsigned value) {
  // Create a builtin integer literal value.
  if (auto intTy = ty.getAs<BuiltinIntegerType>()) {
    return B.createIntegerLiteral(loc, ty, value);
  }
  
  // Or wrap a value in a struct, potentially multiple times to handle types
  // that wrap integer types like ObjCBool (which may be Bool or Int8).
  auto structDecl = ty.getStructOrBoundGenericStruct();
  assert(structDecl && "value for error result wasn't of struct type!");
  assert(structDecl->getStoredProperties().size() == 1);
  auto property = structDecl->getStoredProperties()[0];
  auto propertyTy = ty.getFieldType(property, SGM.Types, getTypeExpansionContext());
  auto propertyValue = emitWrapIntegerLiteral(loc, propertyTy, value);
  return B.createStruct(loc, ty, propertyValue);
}

ParamDecl *SILGenFunction::isMappedToInitAccessorArgument(VarDecl *property) {
  assert(isa<AccessorDecl>(FunctionDC) &&
         cast<AccessorDecl>(FunctionDC)->isInitAccessor());

  auto arg = InitAccessorArgumentMappings.find(property);
  if (arg == InitAccessorArgumentMappings.end())
    return nullptr;

  return arg->second;
}

SILValue
SILGenFunction::emitApplyOfSetterToBase(SILLocation loc, SILDeclRef setter,
                                        ManagedValue base,
                                        SubstitutionMap substitutions) {
  auto setterFRef = [&]() -> SILValue {
    auto setterInfo = getConstantInfo(getTypeExpansionContext(), setter);
    if (setter.hasDecl() && setter.getDecl()->shouldUseObjCDispatch()) {
      // Emit a thunk we might have to bridge arguments.
      auto foreignSetterThunk = setter.asForeign(false);
      return emitDynamicMethodRef(
                 loc, foreignSetterThunk,
                 SGM.Types
                     .getConstantInfo(getTypeExpansionContext(),
                                      foreignSetterThunk)
                     .SILFnType)
          .getValue();
    }

    return emitGlobalFunctionRef(loc, setter, setterInfo);
  }();

  auto getSetterType = [&](SILValue setterFRef) {
    CanSILFunctionType setterTy =
        setterFRef->getType().castTo<SILFunctionType>();
    return setterTy->substGenericArgs(SGM.M, substitutions,
                                      getTypeExpansionContext());
  };

  SILFunctionConventions setterConv(getSetterType(setterFRef), SGM.M);

  // Emit captures for the setter
  SmallVector<SILValue, 4> capturedArgs;
  auto captureInfo = SGM.Types.getLoweredLocalCaptures(setter);
  if (!captureInfo.getCaptures().empty()) {
    SmallVector<ManagedValue, 4> captures;
    emitCaptures(loc, setter, CaptureEmission::AssignByWrapper, captures);

    llvm::transform(captures, std::back_inserter(capturedArgs),
                    [](auto &capture) { return capture.getValue(); });
  } else {
    assert(base);

    SILValue capturedBase;
    unsigned argIdx = setterConv.getNumSILArguments() - 1;

    if (setterConv.getSILArgumentConvention(argIdx).isInoutConvention()) {
      capturedBase = base.getValue();
    } else if (base.getType().isAddress() &&
               base.getType().getObjectType() ==
                   setterConv.getSILArgumentType(argIdx,
                                                 getTypeExpansionContext())) {
      // If the base is a reference and the setter expects a value, emit a
      // load. This pattern is emitted for property wrappers with a
      // nonmutating setter, for example.
      capturedBase = B.createTrivialLoadOr(loc, base.getValue(),
                                           LoadOwnershipQualifier::Copy);
      // On-stack partial apply doesn't take ownership of the base, so
      // we have to destroy it manually.
      enterDestroyCleanup(capturedBase);
    } else {
      capturedBase = base.borrow(*this, loc).getValue();
    }

    capturedArgs.push_back(capturedBase);
  }

  PartialApplyInst *setterPAI =
      B.createPartialApply(loc, setterFRef, substitutions, capturedArgs,
                           ParameterConvention::Direct_Guaranteed,
                           SILFunctionTypeIsolation::Unknown,
                           PartialApplyInst::OnStackKind::OnStack);
  return emitManagedRValueWithCleanup(setterPAI).getValue();
}

void SILGenFunction::emitAssignOrInit(SILLocation loc, ManagedValue selfValue,
                                      VarDecl *field, ManagedValue newValue,
                                      SubstitutionMap substitutions) {
  auto fieldTy = field->getValueInterfaceType();
  if (!substitutions.empty())
    fieldTy = fieldTy.subst(substitutions);

  auto *initAccessor = field->getOpaqueAccessor(AccessorKind::Init);

  // Emit the init accessor function partially applied to the base.
  SILValue initFRef = emitGlobalFunctionRef(
      loc, getAccessorDeclRef(initAccessor));

  auto initTy = initFRef->getType().castTo<SILFunctionType>();

  // If there are substitutions we need to emit partial apply to
  // apply substitutions to the init accessor reference type.
  initTy = initTy->substGenericArgs(SGM.M, substitutions,
                                    getTypeExpansionContext());

  // Emit partial apply with self metatype argument to produce a substituted
  // init accessor reference.
  auto selfTy = selfValue.getType().getASTType();
  auto metatypeTy = MetatypeType::get(selfTy);

  SILValue selfMetatype;
  if (selfTy->getClassOrBoundGenericClass()) {
    selfMetatype = B.createValueMetatype(loc, getLoweredType(metatypeTy),
                                         selfValue).getValue();
  } else {
    selfMetatype = B.createMetatype(loc, getLoweredType(metatypeTy));
  }

  auto expectedSelfTy = initAccessor->getDeclContext()->getSelfInterfaceType()
      .subst(substitutions);

  // This should only happen in the invalid case where we attempt to initialize
  // superclass storage from a subclass initializer. However, we shouldn't
  // crash, so emit the appropriate cast so that we can recover and diagnose
  // later.
  if (!expectedSelfTy->isEqual(selfTy)) {
    selfMetatype = B.createUpcast(loc, selfMetatype,
                             getLoweredType(MetatypeType::get(expectedSelfTy)));
  }
  PartialApplyInst *initPAI =
      B.createPartialApply(loc, initFRef, substitutions, selfMetatype,
                           ParameterConvention::Direct_Guaranteed,
                           SILFunctionTypeIsolation::Unknown,
                           PartialApplyInst::OnStackKind::OnStack);
  initFRef = emitManagedRValueWithCleanup(initPAI).getValue();

  // Check whether value is supposed to be passed indirectly and
  // materialize if required.
  {
    SILFunctionConventions initConv(initTy, SGM.M);

    auto newValueArgIdx = initConv.getSILArgIndexOfFirstParam();
    // If we need the argument in memory, materialize an address.
    if (initConv.getSILArgumentConvention(newValueArgIdx)
            .isIndirectConvention() &&
        !newValue.getType().isAddress()) {
      newValue = newValue.materialize(*this, loc);
    }
  }

  SILValue setterFRef;
  if (auto *setter = field->getOpaqueAccessor(AccessorKind::Set)) {
    setterFRef = emitApplyOfSetterToBase(loc, SILDeclRef(setter), selfValue,
                                         substitutions);
  } else {
    setterFRef = SILUndef::get(F, initFRef->getType());
  }

  auto isValueSelf = !selfValue.getType().getASTType()->mayHaveSuperclass();
  // If we are emitting `assign_or_init` instruction for a value
  // type, we need to make sure that "self" is always a l-value
  // reference to "rootself" because `nonmutating set` loads "self"
  // and referencing `selfValue` in such case is incorrect because
  // it's a copy which is going to be skipped by DI.
  auto selfRef = selfValue;
  if (isValueSelf && !selfRef.isLValue()) {
    auto *ctor = cast<ConstructorDecl>(FunctionDC->getAsDecl());
    selfRef = maybeEmitValueOfLocalVarDecl(ctor->getImplicitSelfDecl(),
                                           AccessKind::ReadWrite);
  }

  B.createAssignOrInit(loc, field, selfRef.getValue(),
                       newValue.forward(*this), initFRef, setterFRef,
                       AssignOrInitInst::Unknown);
}
