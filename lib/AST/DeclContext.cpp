//===--- DeclContext.cpp - DeclContext implementation ---------------------===//
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

#include "swift/AST/DeclContext.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Expr.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Types.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "clang/AST/ASTContext.h"
using namespace swift;

#define DEBUG_TYPE "Name lookup"

STATISTIC(NumLazyIterableDeclContexts,
          "# of serialized iterable declaration contexts");
STATISTIC(NumUnloadedLazyIterableDeclContexts,
          "# of serialized iterable declaration contexts never loaded");

ASTContext &DeclContext::getASTContext() const {
  return getParentModule()->getASTContext();
}

GenericTypeDecl *DeclContext::getSelfTypeDecl() const {
  auto decl = const_cast<Decl*>(getAsDecl());
  if (!decl) return nullptr;

  auto ext = dyn_cast<ExtensionDecl>(decl);
  if (!ext) return dyn_cast<GenericTypeDecl>(decl);

  return ext->getExtendedNominal();
}

/// If this DeclContext is a NominalType declaration or an
/// extension thereof, return the NominalTypeDecl.
NominalTypeDecl *DeclContext::getSelfNominalTypeDecl() const {
  return dyn_cast_or_null<NominalTypeDecl>(getSelfTypeDecl());
}


ClassDecl *DeclContext::getSelfClassDecl() const {
  return dyn_cast_or_null<ClassDecl>(getSelfTypeDecl());
}

EnumDecl *DeclContext::getSelfEnumDecl() const {
  return dyn_cast_or_null<EnumDecl>(getSelfTypeDecl());
}

StructDecl *DeclContext::getSelfStructDecl() const {
  return dyn_cast_or_null<StructDecl>(getSelfTypeDecl());
}

ProtocolDecl *DeclContext::getSelfProtocolDecl() const {
  return dyn_cast_or_null<ProtocolDecl>(getSelfTypeDecl());
}

ProtocolDecl *DeclContext::getExtendedProtocolDecl() const {
  if (auto decl = const_cast<Decl*>(getAsDecl()))
    if (auto ED = dyn_cast<ExtensionDecl>(decl))
      return dyn_cast_or_null<ProtocolDecl>(ED->getExtendedNominal());
  return nullptr;
}

VarDecl *DeclContext::getNonLocalVarDecl() const {
  if (auto *init = dyn_cast<PatternBindingInitializer>(this)) {
    if (auto binding = init->getBinding()) {
      if (auto *var = binding->getAnchoringVarDecl(init->getBindingIndex())) {
        return var;
      }
    }
  }
  return nullptr;
}

GenericTypeParamType *DeclContext::getProtocolSelfType() const {
  assert(getSelfProtocolDecl() && "not a protocol");

  GenericParamList *genericParams;
  if (auto proto = dyn_cast<ProtocolDecl>(this)) {
    genericParams = proto->getGenericParams();
  } else {
    genericParams = cast<ExtensionDecl>(this)->getGenericParams();
  }

  if (genericParams == nullptr)
    return nullptr;

  return genericParams->getParams().front()
      ->getDeclaredInterfaceType()
      ->castTo<GenericTypeParamType>();
}

Type DeclContext::getDeclaredTypeInContext() const {
  if (auto declaredType = getDeclaredInterfaceType())
    return mapTypeIntoContext(declaredType);
  return Type();
}

Type DeclContext::getDeclaredInterfaceType() const {
  if (auto *ED = dyn_cast<ExtensionDecl>(this)) {
    auto *NTD = ED->getExtendedNominal();
    if (NTD == nullptr)
      return ErrorType::get(ED->getASTContext());
    return NTD->getDeclaredInterfaceType();
  }
  if (auto *NTD = dyn_cast<NominalTypeDecl>(this))
    return NTD->getDeclaredInterfaceType();
  return Type();
}

void DeclContext::forEachGenericContext(
    llvm::function_ref<void (GenericParamList *)> fn) const {
  auto dc = this;
  do {
    if (auto decl = dc->getAsDecl()) {
      // Extensions do not capture outer generic parameters.
      if (auto *ext = dyn_cast<ExtensionDecl>(decl)) {
        for (auto *gpList = ext->getGenericParams();
             gpList != nullptr;
             gpList = gpList->getOuterParameters()) {
          fn(gpList);
        }

        return;
      }

      if (auto genericCtx = decl->getAsGenericContext())
        if (auto *gpList = genericCtx->getGenericParams())
          fn(gpList);
    }
  } while ((dc = dc->getParentForLookup()));
}

unsigned DeclContext::getGenericContextDepth() const {
  unsigned depth = -1;
  forEachGenericContext([&](GenericParamList *) { ++depth; });
  return depth;
}

GenericSignature DeclContext::getGenericSignatureOfContext() const {
  auto dc = this;
  do {
    if (auto decl = dc->getAsDecl())
      if (auto GC = decl->getAsGenericContext())
        return GC->getGenericSignature();
  } while ((dc = dc->getParent()));

  return nullptr;
}

GenericEnvironment *DeclContext::getGenericEnvironmentOfContext() const {
  auto dc = this;
  do {
    if (auto decl = dc->getAsDecl())
      if (auto GC = decl->getAsGenericContext())
        return GC->getGenericEnvironment();
  } while ((dc = dc->getParent()));

  return nullptr;
}

Type DeclContext::mapTypeIntoContext(Type type) const {
  return GenericEnvironment::mapTypeIntoContext(
      getGenericEnvironmentOfContext(), type);
}

DeclContext *DeclContext::getLocalContext() {
  if (isLocalContext())
    return this;
  if (isModuleContext())
    return nullptr;
  return getParent()->getLocalContext();
}

AbstractFunctionDecl *DeclContext::getInnermostMethodContext() {
  auto dc = this;
  do {
    if (auto decl = dc->getAsDecl()) {
      auto func = dyn_cast<AbstractFunctionDecl>(decl);
      // If we found a non-func decl, we're done.
      if (func == nullptr)
        return nullptr;
      if (func->getDeclContext()->isTypeContext())
        return func;
    }
  } while ((dc = dc->getParent()));

  return nullptr;
}

AccessorDecl *DeclContext::getInnermostPropertyAccessorContext() {
  auto dc = this;
  do {
    if (auto decl = dc->getAsDecl()) {
      auto accessor = dyn_cast<AccessorDecl>(decl);
      // If we found a non-accessor decl, we're done.
      if (accessor == nullptr)
        return nullptr;

      auto *storage = accessor->getStorage();
      if (isa<VarDecl>(storage) && storage->getDeclContext()->isTypeContext())
        return accessor;
    }
  } while ((dc = dc->getParent()));

  return nullptr;
}

bool DeclContext::isTypeContext() const {
  if (auto decl = getAsDecl())
    return isa<NominalTypeDecl>(decl) || isa<ExtensionDecl>(decl);
  return false;
}

DeclContext *DeclContext::getInnermostTypeContext() {
  auto dc = this;
  do {
    if (dc->isTypeContext())
      return dc;
  } while ((dc = dc->getParent()));

  return nullptr;
}

Decl *DeclContext::getInnermostDeclarationDeclContext() {
  auto DC = this;
  do {
    if (auto decl = DC->getAsDecl())
      return isa<ModuleDecl>(decl) ? nullptr : decl;
  } while ((DC = DC->getParent()));

  return nullptr;
}

Decl *DeclContext::getTopmostDeclarationDeclContext() {
  auto *dc = this;
  Decl *topmost = nullptr;
  while (auto *decl = dc->getInnermostDeclarationDeclContext()) {
    topmost = decl;
    dc = decl->getDeclContext();
  }
  return topmost;
}

DeclContext *DeclContext::getInnermostSkippedFunctionContext() {
  auto dc = this;
  do {
    if (auto afd = dyn_cast<AbstractFunctionDecl>(dc))
      if (afd->isBodySkipped())
        return afd;
  } while ((dc = dc->getParent()));

  return nullptr;
}

DeclContext *DeclContext::getParentForLookup() const {
  if (isa<ProtocolDecl>(this) || isa<ExtensionDecl>(this)) {
    // If we are inside a protocol or an extension, skip directly
    // to the module scope context, without looking at any (invalid)
    // outer types.
    return getModuleScopeContext();
  }
  if (isa<NominalTypeDecl>(this)) {
    // If we are inside a nominal type that is inside a protocol,
    // skip the protocol.
    if (isa<ProtocolDecl>(getParent()))
      return getModuleScopeContext();
  }
  return getParent();
}

PackageUnit *DeclContext::getPackageContext(bool lookupIfNotCurrent) const {
  // Current decl context might be PackageUnit, which is not in the
  // DeclContext hierarchy, so check for it first
  if (isPackageContext())
    return const_cast<PackageUnit *>(cast<PackageUnit>(this));

  // If the current context is not PackageUnit, look it up via
  // the parent module if needed
  if (lookupIfNotCurrent) {
    auto mdecl = getParentModule();
    return mdecl->getPackage();
  }
  return nullptr;
}

ModuleDecl *DeclContext::getParentModule() const {
  // If the current context is PackageUnit, return the module
  // decl context pointing to the current context. This check
  // needs to be done first as PackageUnit is not in the DeclContext
  // hierarchy.
  if (auto pkg = getPackageContext()) {
    auto &mdecl = pkg->getSourceModule();
    return &mdecl;
  }

  const DeclContext *DC = this;
  while (!DC->isModuleContext())
    DC = DC->getParent();
  return const_cast<ModuleDecl *>(cast<ModuleDecl>(DC));
}

SourceFile *DeclContext::getParentSourceFile() const {
  const DeclContext *DC = this;
  SourceLoc loc;
  while (DC && !DC->isModuleScopeContext()) {
    // If we don't have a source location yet, try to grab one from this
    // context.
    if (loc.isInvalid()) {
      switch (DC->getContextKind()) {
      case DeclContextKind::AbstractClosureExpr:
        loc = cast<AbstractClosureExpr>(DC)->getLoc();
        break;

      case DeclContextKind::AbstractFunctionDecl:
      case DeclContextKind::EnumElementDecl:
      case DeclContextKind::ExtensionDecl:
      case DeclContextKind::GenericTypeDecl:
      case DeclContextKind::MacroDecl:
      case DeclContextKind::SubscriptDecl:
      case DeclContextKind::TopLevelCodeDecl:
        loc = DC->getAsDecl()->getLoc(/*SerializedOK=*/false);
        break;

      case DeclContextKind::Initializer:
      case DeclContextKind::FileUnit:
      case DeclContextKind::Module:
      case DeclContextKind::Package:
      case DeclContextKind::SerializedLocal:
        break;
      }
    }

    DC = DC->getParent();
  }

  if (!DC)
    return nullptr;

  auto fallbackSF = const_cast<SourceFile *>(dyn_cast<SourceFile>(DC));
  if (auto module = DC->getParentModule()) {
    if (auto sf = module->getSourceFileContainingLocation(loc))
      return sf;
  }

  return fallbackSF;
}

SourceFile *DeclContext::getOutermostParentSourceFile() const {
  auto sf = getParentSourceFile();
  if (!sf)
    return nullptr;

  // Find the originating source file.
  while (auto enclosingSF = sf->getEnclosingSourceFile())
    sf = enclosingSF;

  return sf;
}

DeclContext *DeclContext::getModuleScopeContext() const {
  // If the current context is PackageUnit, return the module
  // decl context pointing to the current context. This check
  // needs to be done first as PackageUnit is not in the DeclContext
  // hierarchy.
  if (auto pkg = getPackageContext()) {
    auto &mdecl = pkg->getSourceModule();
    return &mdecl;
  }

  auto DC = const_cast<DeclContext *>(this);
  while (true) {
    if (DC->ParentAndKind.getInt() == ASTHierarchy::FileUnit)
      return DC;
    if (auto NextDC = DC->getParent()) {
      DC = NextDC;
    } else {
      assert(DC && isa<ModuleDecl>(DC->getAsDecl()) &&
             "no module decl context found!");
      return DC;
    }
  }
}

void DeclContext::getSeparatelyImportedOverlays(
    ModuleDecl *declaring, SmallVectorImpl<ModuleDecl *> &overlays) const {
  if (auto SF = getParentSourceFile())
    SF->getSeparatelyImportedOverlays(declaring, overlays);
}

/// Determine whether the given context is generic at any level.
bool DeclContext::isGenericContext() const {
  auto dc = this;
  do {
    if (auto decl = dc->getAsDecl()) {
      if (auto GC = decl->getAsGenericContext()) {
        if (GC->getGenericParams())
          return true;
      }
    }
  } while ((dc = dc->getParentForLookup()));

  return false;
}

ResilienceExpansion DeclContext::getResilienceExpansion() const {
  auto fragileKind = getFragileFunctionKind();
  switch (fragileKind.kind) {
  case FragileFunctionKind::Transparent:
  case FragileFunctionKind::Inlinable:
  case FragileFunctionKind::AlwaysEmitIntoClient:
  case FragileFunctionKind::DefaultArgument:
  case FragileFunctionKind::PropertyInitializer:
  case FragileFunctionKind::BackDeploy:
    return ResilienceExpansion::Minimal;
  case FragileFunctionKind::None:
    return ResilienceExpansion::Maximal;
  }

  llvm_unreachable("Bad fragile function kind");
}

FragileFunctionKind DeclContext::getFragileFunctionKind() const {
  auto &context = getASTContext();
  return evaluateOrDefault(
      context.evaluator,
      FragileFunctionKindRequest{const_cast<DeclContext *>(this)},
      {FragileFunctionKind::None});
}

FragileFunctionKind
swift::FragileFunctionKindRequest::evaluate(Evaluator &evaluator,
                                            DeclContext *context) const {
  for (const auto *dc = context->getLocalContext(); dc && dc->isLocalContext();
       dc = dc->getParent()) {
    // Default argument initializer contexts have their resilience expansion
    // set when they're type checked.
    if (isa<DefaultArgumentInitializer>(dc)) {
      dc = dc->getParent();

      auto *VD = cast<ValueDecl>(dc->getAsDecl());
      assert(VD->hasParameterList());

      if (VD->getDeclContext()->isLocalContext()) {
        auto kind = VD->getDeclContext()->getFragileFunctionKind();
        if (kind.kind != FragileFunctionKind::None)
          return {FragileFunctionKind::DefaultArgument};
      }

      auto effectiveAccess =
          VD->getFormalAccessScope(/*useDC=*/nullptr,
                                   /*treatUsableFromInlineAsPublic=*/true);
      if (effectiveAccess.isPublic()) {
        return {FragileFunctionKind::DefaultArgument};
      }

      return {FragileFunctionKind::None};
    }

    // Stored property initializer contexts use minimal resilience expansion
    // if the type is formally fixed layout.
    if (auto *init = dyn_cast <PatternBindingInitializer>(dc)) {
      auto bindingIndex = init->getBindingIndex();
      if (auto *varDecl = init->getBinding()->getAnchoringVarDecl(bindingIndex)) {
        if (varDecl->isInitExposedToClients()) {
          return {FragileFunctionKind::PropertyInitializer};
        }
      }

      return {FragileFunctionKind::None};
    }

    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(dc)) {
      // If the function is a nested function, we will serialize its body if
      // we serialize the parent's body.
      if (AFD->getDeclContext()->isLocalContext())
        continue;

      auto funcAccess =
        AFD->getFormalAccessScope(/*useDC=*/nullptr,
                                  /*treatUsableFromInlineAsPublic=*/true);

      // If the function is not externally visible, we will not be serializing
      // its body.
      if (!funcAccess.isPublic()) {
        return {FragileFunctionKind::None};
      }

      // If the function is public, @_transparent implies @inlinable.
      if (AFD->isTransparent()) {
        return {FragileFunctionKind::Transparent};
      }

      if (AFD->getAttrs().hasAttribute<InlinableAttr>()) {
        return {FragileFunctionKind::Inlinable};
      }

      if (AFD->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>()) {
        return {FragileFunctionKind::AlwaysEmitIntoClient};
      }

      if (AFD->isBackDeployed(context->getASTContext())) {
        return {FragileFunctionKind::BackDeploy};
      }

      // Property and subscript accessors inherit @_alwaysEmitIntoClient,
      // @backDeployed, and @inlinable from their storage declarations.
      if (auto accessor = dyn_cast<AccessorDecl>(AFD)) {
        auto *storage = accessor->getStorage();
        if (storage->getAttrs().getAttribute<InlinableAttr>()) {
          return {FragileFunctionKind::Inlinable};
        }
        if (storage->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>()) {
          return {FragileFunctionKind::AlwaysEmitIntoClient};
        }
        if (storage->isBackDeployed(context->getASTContext())) {
          return {FragileFunctionKind::BackDeploy};
        }
      }
    }
  }

  return {FragileFunctionKind::None};
}

/// Determine whether the innermost context is generic.
bool DeclContext::isInnermostContextGeneric() const {
  if (auto Decl = getAsDecl())
    if (auto GC = Decl->getAsGenericContext())
      return GC->isGeneric();
  return false;
}

unsigned DeclContext::getSyntacticDepth() const {
  // Module scope == depth 0.
  if (isModuleScopeContext())
    return 0;

  return 1 + getParent()->getSyntacticDepth();
}

unsigned DeclContext::getSemanticDepth() const {
  // For extensions, count the depth of the nominal type being extended.
  if (isa<ExtensionDecl>(this)) {
    if (auto nominal = getSelfNominalTypeDecl())
      return nominal->getSemanticDepth();

    return 1;
  }

  // Module scope == depth 0.
  if (isModuleScopeContext())
    return 0;

  return 1 + getParent()->getSemanticDepth();
}

bool DeclContext::mayContainMembersAccessedByDynamicLookup() const {
  // Members of non-generic classes and class extensions can be found by
  /// dynamic lookup.
  if (auto *CD = getSelfClassDecl())
    return !CD->isGenericContext();

  // Members of @objc protocols (but not protocol extensions) can be
  // found by dynamic lookup.
  if (auto *PD = dyn_cast<ProtocolDecl>(this))
      return PD->getAttrs().hasAttribute<ObjCAttr>();

  return false;
}

bool DeclContext::canBeParentOfExtension() const {
  return isa<SourceFile>(this);
}

bool DeclContext::walkContext(ASTWalker &Walker) {
  switch (getContextKind()) {
  case DeclContextKind::Package:
    return false;
  case DeclContextKind::Module:
    return cast<ModuleDecl>(this)->walk(Walker);
  case DeclContextKind::FileUnit:
    return cast<FileUnit>(this)->walk(Walker);
  case DeclContextKind::AbstractClosureExpr:
    return cast<AbstractClosureExpr>(this)->walk(Walker);
  case DeclContextKind::GenericTypeDecl:
    return cast<GenericTypeDecl>(this)->walk(Walker);
  case DeclContextKind::ExtensionDecl:
    return cast<ExtensionDecl>(this)->walk(Walker);
  case DeclContextKind::TopLevelCodeDecl:
    return cast<TopLevelCodeDecl>(this)->walk(Walker);
  case DeclContextKind::AbstractFunctionDecl:
    return cast<AbstractFunctionDecl>(this)->walk(Walker);
  case DeclContextKind::SubscriptDecl:
    return cast<SubscriptDecl>(this)->walk(Walker);
  case DeclContextKind::EnumElementDecl:
    return cast<EnumElementDecl>(this)->walk(Walker);
  case DeclContextKind::MacroDecl:
    return cast<MacroDecl>(this)->walk(Walker);
  case DeclContextKind::SerializedLocal:
    llvm_unreachable("walk is unimplemented for deserialized contexts");
  case DeclContextKind::Initializer:
    // Is there any point in trying to walk the expression?
    return false;
  }
  llvm_unreachable("bad DeclContextKind");
}

void DeclContext::dumpContext() const {
  printContext(llvm::errs());
}

void AccessScope::dump() const {
  llvm::errs() << getAccessLevelSpelling(accessLevelForDiagnostics()) << ": ";

  if (isPublic() || isPackage()) {
    llvm::errs() << "(null)\n";
    return;
  }

  if (auto *file = dyn_cast<SourceFile>(getDeclContext())) {
    llvm::errs() << "file '" << file->getFilename() << "'\n";
    return;
  }

  if (auto *decl = getDeclContext()->getAsDecl()) {
    llvm::errs() << Decl::getKindName(decl->getKind()) << " ";
    if (auto *ext = dyn_cast<ExtensionDecl>(decl)) {
      auto *extended = ext->getExtendedNominal();
      if (extended)
        llvm::errs() << extended->getName();
      else
        llvm::errs() << "(null)";
    } else if (auto *named = dyn_cast<ValueDecl>(decl)) {
      llvm::errs() << named->getName();
    } else {
      llvm::errs() << (const void *)decl;
    }

    SourceLoc loc = decl->getLoc();
    if (loc.isValid()) {
      llvm::errs() << " at ";
      loc.print(llvm::errs(), decl->getASTContext().SourceMgr);
    }
    llvm::errs() << "\n";

    return;
  }

  // If all else fails, dump the DeclContext tree.
  getDeclContext()->printContext(llvm::errs());
}

template <typename DCType>
static unsigned getLineNumber(DCType *DC) {
  SourceLoc loc = DC->getLoc();
  if (loc.isInvalid())
    return 0;

  const ASTContext &ctx = static_cast<const DeclContext *>(DC)->getASTContext();
  return ctx.SourceMgr.getPresumedLineAndColumnForLoc(loc).first;
}

unsigned DeclContext::printContext(raw_ostream &OS, const unsigned indent,
                                   const bool onlyAPartialLine) const {
  unsigned Depth = 0;
  if (!onlyAPartialLine)
    if (auto *P = getParent())
      Depth = P->printContext(OS, indent);

  const char *Kind;
  switch (getContextKind()) {
  case DeclContextKind::Package:          Kind = "Package"; break;
  case DeclContextKind::Module:           Kind = "Module"; break;
  case DeclContextKind::FileUnit:         Kind = "FileUnit"; break;
  case DeclContextKind::SerializedLocal:  Kind = "Serialized Local"; break;
  case DeclContextKind::AbstractClosureExpr:
    Kind = "AbstractClosureExpr";
    break;
  case DeclContextKind::GenericTypeDecl:
    switch (cast<GenericTypeDecl>(this)->getKind()) {
#define DECL(ID, PARENT) \
    case DeclKind::ID: Kind = #ID "Decl"; break;
#include "swift/AST/DeclNodes.def"
    }
    break;
  case DeclContextKind::ExtensionDecl:    Kind = "ExtensionDecl"; break;
  case DeclContextKind::TopLevelCodeDecl: Kind = "TopLevelCodeDecl"; break;
  case DeclContextKind::Initializer:      Kind = "Initializer"; break;
  case DeclContextKind::AbstractFunctionDecl:
    Kind = "AbstractFunctionDecl";
    break;
  case DeclContextKind::SubscriptDecl:    Kind = "SubscriptDecl"; break;
  case DeclContextKind::EnumElementDecl:  Kind = "EnumElementDecl"; break;
  case DeclContextKind::MacroDecl:    Kind = "MacroDecl"; break;
  }
  OS.indent(Depth*2 + indent) << (void*)this << " " << Kind;

  switch (getContextKind()) {
  case DeclContextKind::Package:
    OS << " name=" << cast<PackageUnit>(this)->getName();
    break;
  case DeclContextKind::Module:
    OS << " name=" << cast<ModuleDecl>(this)->getName();
    break;
  case DeclContextKind::FileUnit:
    switch (cast<FileUnit>(this)->getKind()) {
    case FileUnitKind::Builtin:
      OS << " Builtin";
      break;
    case FileUnitKind::Source:
      OS << " file=\"" << cast<SourceFile>(this)->getFilename() << "\"";
      break;
    case FileUnitKind::Synthesized:
      OS << " synthesized file";
      break;
    case FileUnitKind::SerializedAST:
    case FileUnitKind::ClangModule:
    case FileUnitKind::DWARFModule:
      OS << " file=\"" << cast<LoadedFile>(this)->getFilename() << "\"";
      break;
    }
    break;
  case DeclContextKind::AbstractClosureExpr:
    OS << " line=" << getLineNumber(cast<AbstractClosureExpr>(this));
    OS << " : " << cast<AbstractClosureExpr>(this)->getType();
    break;
  case DeclContextKind::GenericTypeDecl:
    OS << " name=" << cast<GenericTypeDecl>(this)->getName();
    break;
  case DeclContextKind::ExtensionDecl:
    OS << " line=" << getLineNumber(cast<ExtensionDecl>(this));
    OS << " base=" << cast<ExtensionDecl>(this)->getExtendedType();
    break;
  case DeclContextKind::TopLevelCodeDecl:
    OS << " line=" << getLineNumber(cast<TopLevelCodeDecl>(this));
    break;
  case DeclContextKind::AbstractFunctionDecl: {
    auto *AFD = cast<AbstractFunctionDecl>(this);
    OS << " name=" << AFD->getName();
    if (AFD->hasInterfaceType())
      OS << " : " << AFD->getInterfaceType();
    else
      OS << " : (no type set)";
    break;
  }
  case DeclContextKind::SubscriptDecl: {
    auto *SD = cast<SubscriptDecl>(this);
    OS << " name=" << SD->getBaseName();
    if (SD->hasInterfaceType())
      OS << " : " << SD->getInterfaceType();
    else
      OS << " : (no type set)";
    break;
  }
  case DeclContextKind::EnumElementDecl: {
    auto *EED = cast<EnumElementDecl>(this);
    OS << " name=" << EED->getBaseName();
    if (EED->hasInterfaceType())
      OS << " : " << EED->getInterfaceType();
    else
      OS << " : (no type set)";
    break;
  }
  case DeclContextKind::MacroDecl: {
    auto *MD = cast<MacroDecl>(this);
    OS << " name=" << MD->getBaseName();
    if (MD->hasInterfaceType())
      OS << " : " << MD->getInterfaceType();
    else
      OS << " : (no type set)";
    break;
  }
  case DeclContextKind::Initializer:
    switch (cast<Initializer>(this)->getInitializerKind()) {
    case InitializerKind::PatternBinding: {
      auto init = cast<PatternBindingInitializer>(this);
      OS << " PatternBinding 0x" << (void*) init->getBinding()
         << " #" << init->getBindingIndex();
      break;
    }
    case InitializerKind::DefaultArgument: {
      auto init = cast<DefaultArgumentInitializer>(this);
      OS << " DefaultArgument index=" << init->getIndex();
      break;
    }
    case InitializerKind::PropertyWrapper: {
      auto init = cast<PropertyWrapperInitializer>(this);
      OS << "PropertyWrapper 0x" << (void*)init->getWrappedVar() << ", kind=";
      switch (init->getKind()) {
      case PropertyWrapperInitializer::Kind::WrappedValue:
        OS << "wrappedValue";
        break;
      case PropertyWrapperInitializer::Kind::ProjectedValue:
          OS << "projectedValue";
        break;
      }
      break;
    }

    case InitializerKind::RuntimeAttribute: {
      auto init = cast<RuntimeAttributeInitializer>(this);
      auto *decl = init->getAttachedToDecl();

      OS << "RuntimeAttribute attachedTo="
         << init->getAttachedToDecl()->getName() << ", attribute=";
      init->getAttr()->print(OS, decl);
    }
    }
    break;

  case DeclContextKind::SerializedLocal: {
    auto local = cast<SerializedLocalDeclContext>(this);
    switch (local->getLocalDeclContextKind()) {
    case LocalDeclContextKind::AbstractClosure: {
      auto serializedClosure = cast<SerializedAbstractClosureExpr>(local);
      OS << " closure : " << serializedClosure->getType();
      break;
    }
    case LocalDeclContextKind::DefaultArgumentInitializer: {
      auto init = cast<SerializedDefaultArgumentInitializer>(local);
      OS << "DefaultArgument index=" << init->getIndex();
      break;
    }
    case LocalDeclContextKind::PatternBindingInitializer: {
      auto init = cast<SerializedPatternBindingInitializer>(local);
      OS << " PatternBinding 0x" << (void*) init->getBinding()
         << " #" << init->getBindingIndex();
      break;
    }
    case LocalDeclContextKind::TopLevelCodeDecl:
      OS << " TopLevelCode";
      break;
    }
  }
  }

  if (auto decl = getAsDecl())
    if (decl->getClangNode().getLocation().isValid()) {
      auto &clangSM = getASTContext().getClangModuleLoader()
                          ->getClangASTContext().getSourceManager();
      OS << " clang_loc=";
      decl->getClangNode().getLocation().print(OS, clangSM);
    }

  if (!onlyAPartialLine)
    OS << "\n";
  return Depth + 1;
}

const Decl *
IterableDeclContext::getDecl() const {
  switch (getIterableContextKind()) {
  case IterableDeclContextKind::NominalTypeDecl:
    return cast<NominalTypeDecl>(this);
    break;

  case IterableDeclContextKind::ExtensionDecl:
    return cast<ExtensionDecl>(this);
    break;
  }
  llvm_unreachable("Unhandled IterableDeclContextKind in switch.");
}

GenericContext *IterableDeclContext::getAsGenericContext() {
  switch (getIterableContextKind()) {
  case IterableDeclContextKind::NominalTypeDecl:
    return cast<NominalTypeDecl>(this);
  case IterableDeclContextKind::ExtensionDecl:
    return cast<ExtensionDecl>(this);
  }
  llvm_unreachable("Unhandled IterableDeclContextKind in switch.");
}

ASTContext &IterableDeclContext::getASTContext() const {
  return getDecl()->getASTContext();
}

DeclRange IterableDeclContext::getCurrentMembersWithoutLoading() const {
  return DeclRange(FirstDeclAndLazyMembers.getPointer(), nullptr);
}

DeclRange IterableDeclContext::getMembers() const {
  return getCurrentMembers();
}

DeclRange IterableDeclContext::getCurrentMembers() const {
  loadAllMembers();

  return getCurrentMembersWithoutLoading();
}

ArrayRef<Decl *> IterableDeclContext::getParsedMembers() const {
  ASTContext &ctx = getASTContext();
  auto mutableThis = const_cast<IterableDeclContext *>(this);
  return evaluateOrDefault(
      ctx.evaluator, ParseMembersRequest{mutableThis},
      FingerprintAndMembers())
    .members;
}

ArrayRef<Decl *> IterableDeclContext::getABIMembers() const {
  ASTContext &ctx = getASTContext();
  return evaluateOrDefault(
      ctx.evaluator,
      ABIMembersRequest{const_cast<IterableDeclContext *>(this)},
      ArrayRef<Decl *>());
}

ArrayRef<Decl *> IterableDeclContext::getAllMembers() const {
  ASTContext &ctx = getASTContext();
  return evaluateOrDefault(
      ctx.evaluator,
      AllMembersRequest{const_cast<IterableDeclContext *>(this)},
      ArrayRef<Decl *>());
}

void IterableDeclContext::addMemberPreservingSourceOrder(Decl *member) {
  auto &SM = getASTContext().SourceMgr;

  SourceLoc start = member->getStartLoc();
  Decl *hint = nullptr;

  for (auto *existingMember : getMembers()) {
    if (existingMember->isImplicit())
      continue;

    // An EnumCaseDecl contains one or more EnumElementDecls,
    // but the EnumElementDecls are also added as members of
    // the parent enum. We ignore the EnumCaseDecl since its
    // source range overlaps with that of the EnumElementDecls.
    if (isa<EnumCaseDecl>(existingMember))
      continue;

    // The elements of the active clause of an IfConfigDecl
    // are added to the parent type. We ignore the IfConfigDecl
    // since its source range overlaps with the source ranges
    // of the active elements.
    if (isa<IfConfigDecl>(existingMember))
      continue;

    if (!SM.isBeforeInBuffer(existingMember->getEndLoc(), start))
      break;

    hint = existingMember;
  }

  addMember(member, hint, /*insertAtHead=*/hint == nullptr);
}

/// Add a member to this context.
void IterableDeclContext::addMember(Decl *member, Decl *hint, bool insertAtHead) {
  // Add the member to the list of declarations without notification.
  addMemberSilently(member, hint, insertAtHead);

  // Notify our parent declaration that we have added the member, which can
  // be used to update the lookup tables.
  switch (getIterableContextKind()) {
  case IterableDeclContextKind::NominalTypeDecl: {
    auto nominal = cast<NominalTypeDecl>(this);
    nominal->addedMember(member);
    assert(member->getDeclContext() == nominal &&
           "Added member to the wrong context");
    break;
  }

  case IterableDeclContextKind::ExtensionDecl: {
    auto ext = cast<ExtensionDecl>(this);
    ext->addedMember(member);
    assert(member->getDeclContext() == ext &&
           "Added member to the wrong context");
    break;
  }
  }
}

void IterableDeclContext::addMemberSilently(Decl *member, Decl *hint,
                                            bool insertAtHead) const {
  assert(!isa<AccessorDecl>(member) && "Accessors should not be added here");
  assert(!member->NextDecl && "Already added to a container");

#ifndef NDEBUG
  // Assert that new declarations are always added in source order.
  auto checkSourceRange = [&](Decl *prev, Decl *next) {
    // SKip these checks for imported and deserialized decls.
    if (!member->getDeclContext()->getParentSourceFile())
      return;

    auto shouldSkip = [](Decl *d) {
      // PatternBindingDecl source ranges overlap with VarDecls,
      // EnumCaseDecl source ranges overlap with EnumElementDecls,
      // and IfConfigDecl source ranges overlap with the elements
      // of the active clause. Skip them all here to avoid
      // spurious assertions.
      if (isa<PatternBindingDecl>(d) ||
          isa<EnumCaseDecl>(d) ||
          isa<IfConfigDecl>(d))
        return true;

      // Ignore source location information of implicit declarations.
      if (d->isImplicit())
        return true;

      // Imported decls won't have complete location info.
      if (d->hasClangNode())
        return true;

      return false;
    };

    if (shouldSkip(prev) || shouldSkip(next))
      return;

    SourceLoc prevEnd = prev->getEndLoc();
    SourceLoc nextStart = next->getStartLoc();

    assert(prevEnd.isValid() &&
           "Only implicit decls can have invalid source location");
    assert(nextStart.isValid() &&
           "Only implicit decls can have invalid source location");

    if (getASTContext().SourceMgr.isBeforeInBuffer(prevEnd, nextStart))
      return;

    // Synthesized member macros can add new members in a macro expansion buffer.
    SourceFile *memberSourceFile = member->getLoc()
        ? member->getModuleContext()
                ->getSourceFileContainingLocation(member->getLoc())
        : member->getInnermostDeclContext()->getParentSourceFile();
    if (memberSourceFile->getFulfilledMacroRole() == MacroRole::Member ||
        memberSourceFile->getFulfilledMacroRole() == MacroRole::Peer)
      return;

    llvm::errs() << "Source ranges out of order in addMember():\n";
    prev->dump(llvm::errs());
    next->dump(llvm::errs());
    abort();
  };
#endif

  // Empty list.
  if (!FirstDeclAndLazyMembers.getPointer()) {
    assert(hint == nullptr);

    FirstDeclAndLazyMembers.setPointer(member);
    LastDeclAndKind.setPointer(member);

  // Insertion at the head.
  } else if (insertAtHead) {
    assert(hint == nullptr);

    member->NextDecl = FirstDeclAndLazyMembers.getPointer();
    FirstDeclAndLazyMembers.setPointer(member);

  // Insertion at the tail.
  } else if (hint == nullptr) {
    auto *last = LastDeclAndKind.getPointer();

#ifndef NDEBUG
    checkSourceRange(last, member);
#endif

    last->NextDecl = member;
    LastDeclAndKind.setPointer(member);

  // Insertion after 'hint' (which may be the tail).
  } else {
#ifndef NDEBUG
    checkSourceRange(hint, member);
#endif

    member->NextDecl = hint->NextDecl;
    hint->NextDecl = member;

    // Handle case where the 'hint' is the tail.
    if (LastDeclAndKind.getPointer() == hint)
      LastDeclAndKind.setPointer(member);
  }
}

void IterableDeclContext::setMemberLoader(LazyMemberLoader *loader,
                                          uint64_t contextData) {
  assert(!hasLazyMembers() && "already have lazy members");

  ASTContext &ctx = getASTContext();
  auto contextInfo = ctx.getOrCreateLazyIterableContextData(this, loader);
  FirstDeclAndLazyMembers.setInt(true);
  contextInfo->memberData = contextData;

  ++NumLazyIterableDeclContexts;
  ++NumUnloadedLazyIterableDeclContexts;
  // FIXME: (transitional) increment the redundant "always-on" counter.
  if (auto s = ctx.Stats) {
    ++s->getFrontendCounters().NumLazyIterableDeclContexts;
    ++s->getFrontendCounters().NumUnloadedLazyIterableDeclContexts;
  }
}
bool IterableDeclContext::hasUnparsedMembers() const {
  if (AddedParsedMembers)
    return false;

  if (!getAsGenericContext()->getParentSourceFile()) {
    // There will never be any parsed members to add, so set the flag to say
    // we are done so we can short-circuit next time.
    const_cast<IterableDeclContext *>(this)->AddedParsedMembers = 1;
    return false;
  }

  return true;
}

void IterableDeclContext::setHasLazyMembers(bool hasLazyMembers) const {
  FirstDeclAndLazyMembers.setInt(hasLazyMembers);
}

void IterableDeclContext::loadAllMembers() const {
  ASTContext &ctx = getASTContext();

  // For contexts within a source file, get the list of parsed members.
  if (getAsGenericContext()->getParentSourceFile()) {
    // Retrieve the parsed members. Even if we've already added the parsed
    // members to this context, this call is important for recording the
    // dependency edge.
    auto mutableThis = const_cast<IterableDeclContext *>(this);
    auto members = getParsedMembers();

    // If we haven't already done so, add these members to this context.
    if (!AddedParsedMembers) {
      mutableThis->AddedParsedMembers = 1;
      for (auto member : members) {
        mutableThis->addMember(member);
      }
    }
  }

  if (!hasLazyMembers())
    return;

  // Don't try to load all members re-entrant-ly.
  setHasLazyMembers(false);

  const Decl *container = getDecl();
  auto contextInfo = ctx.getOrCreateLazyIterableContextData(this,
    /*lazyLoader=*/nullptr);
  contextInfo->loader->loadAllMembers(const_cast<Decl *>(container),
                                      contextInfo->memberData);

  --NumUnloadedLazyIterableDeclContexts;
  // FIXME: (transitional) decrement the redundant "always-on" counter.
  if (auto s = ctx.Stats)
    --s->getFrontendCounters().NumUnloadedLazyIterableDeclContexts;
}

bool IterableDeclContext::wasDeserialized() const {
  const DeclContext *DC = getAsGenericContext();
  if (auto F = dyn_cast<FileUnit>(DC->getModuleScopeContext())) {
    return F->getKind() == FileUnitKind::SerializedAST;
  }
  return false;
}

bool IterableDeclContext::classof(const Decl *D) {
  switch (D->getKind()) {
  default: return false;
#define DECL(ID, PARENT) // See previous line
#define ITERABLE_DECL(ID, PARENT) \
  case DeclKind::ID: return true;
#include "swift/AST/DeclNodes.def"
  }
}

IterableDeclContext *
IterableDeclContext::castDeclToIterableDeclContext(const Decl *D) {
  switch (D->getKind()) {
  default: llvm_unreachable("Decl is not a IterableDeclContext.");
#define DECL(ID, PARENT) // See previous line
#define ITERABLE_DECL(ID, PARENT) \
  case DeclKind::ID: \
    return const_cast<IterableDeclContext *>( \
        static_cast<const IterableDeclContext*>(cast<ID##Decl>(D)));
#include "swift/AST/DeclNodes.def"
  }
}

llvm::Optional<Fingerprint> IterableDeclContext::getBodyFingerprint() const {
  auto fileUnit = dyn_cast<FileUnit>(getAsGenericContext()->getModuleScopeContext());
  if (!fileUnit)
    return llvm::None;

  if (isa<SourceFile>(fileUnit)) {
    auto mutableThis = const_cast<IterableDeclContext *>(this);
    return evaluateOrDefault(getASTContext().evaluator,
                             ParseMembersRequest{mutableThis},
                             FingerprintAndMembers())
        .fingerprint;
  }

  if (getDecl()->isImplicit())
    return llvm::None;

  return fileUnit->loadFingerprint(this);
}

/// Return the DeclContext to compare when checking private access in
/// Swift 4 mode. The context returned is the type declaration if the context
/// and the type declaration are in the same file, otherwise it is the types
/// last extension in the source file. If the context does not refer to a
/// declaration or extension, the supplied context is returned.
static const DeclContext *
getPrivateDeclContext(const DeclContext *DC, const SourceFile *useSF) {
  auto NTD = DC->getSelfNominalTypeDecl();
  if (!NTD)
    return DC;

  // use the type declaration as the private scope if it is in the same
  // file as useSF. This occurs for both extensions and declarations.
  if (NTD->getParentSourceFile() == useSF)
    return NTD;

  // Otherwise use the last extension declaration in the same file.
  const DeclContext *lastExtension = nullptr;
  for (ExtensionDecl *ED : NTD->getExtensions())
    if (ED->getParentSourceFile() == useSF)
      lastExtension = ED;

  // If there's no last extension, return the supplied context.
  return lastExtension ? lastExtension : DC;
}

AccessScope::AccessScope(const DeclContext *DC, bool isPrivate)
    : Value(DC, isPrivate) {
  if (isPrivate) {
    DC = getPrivateDeclContext(DC, DC->getParentSourceFile());
    Value.setPointer(DC);
  }
  if (!DC || isa<ModuleDecl>(DC) || isa<PackageUnit>(DC))
    assert(!isPrivate && "public, package, or internal scope can't be private");
}

bool AccessScope::isFileScope() const {
  auto DC = getDeclContext();
  return DC && isa<FileUnit>(DC);
}

bool AccessScope::isInternal() const {
  auto DC = getDeclContext();
  return DC && isa<ModuleDecl>(DC);
}

bool AccessScope::isPackage() const {
  auto DC = getDeclContext();
  return DC && isa<PackageUnit>(DC);
}

AccessLevel AccessScope::accessLevelForDiagnostics() const {
  if (isPublic())
    return AccessLevel::Public;
  if (isPackage())
    return AccessLevel::Package;
  if (isa<ModuleDecl>(getDeclContext()))
    return AccessLevel::Internal;
  if (getDeclContext()->isModuleScopeContext()) {
    return isPrivate() ? AccessLevel::Private : AccessLevel::FilePrivate;
  }

  return AccessLevel::Private;
}

bool AccessScope::allowsPrivateAccess(const DeclContext *useDC, const DeclContext *sourceDC) {
  // Check the lexical scope.
  if (useDC->isChildContextOf(sourceDC))
    return true;

  // If the decl site has package acl, but the use site
  // has internal or less acl, check if it belongs to
  // the same package as the decl site's to allow access.
  if (auto srcPkg = sourceDC->getPackageContext()) {
    if (auto usePkg = useDC->getPackageContext(/*lookupIfNotCurrent*/ true)) {
      return usePkg->isSamePackageAs(srcPkg);
    }
  }
  // Do not allow access if the sourceDC is in a different file
  auto useSF = useDC->getParentSourceFile();
  if (useSF != sourceDC->getParentSourceFile())
    return false;

  // Do not allow access if the sourceDC does not represent a type.
  auto sourceNTD = sourceDC->getSelfNominalTypeDecl();
  if (!sourceNTD)
    return false;

  // Compare the private scopes and iterate over the parent types.
  sourceDC = getPrivateDeclContext(sourceDC, useSF);
  while (!useDC->isModuleContext()) {
    useDC = getPrivateDeclContext(useDC, useSF);
    if (useDC == sourceDC)
      return true;

    // Get the parent type. If the context represents a type, look at the types
    // declaring context instead of the contexts parent. This will crawl up
    // the type hierarchy in nested extensions correctly.
    if (auto NTD = useDC->getSelfNominalTypeDecl())
      useDC = NTD->getDeclContext();
    else
      useDC = useDC->getParent();
  }

  return false;
}

DeclContext *Decl::getDeclContextForModule() const {
  if (auto module = dyn_cast<ModuleDecl>(this))
    return const_cast<ModuleDecl *>(module);

  return nullptr;
}

DeclContextKind DeclContext::getContextKind() const {
  switch (ParentAndKind.getInt()) {
  case ASTHierarchy::Expr:
    return DeclContextKind::AbstractClosureExpr;
  case ASTHierarchy::Initializer:
    return DeclContextKind::Initializer;
  case ASTHierarchy::SerializedLocal:
    return DeclContextKind::SerializedLocal;
  case ASTHierarchy::FileUnit:
    return DeclContextKind::FileUnit;
  case ASTHierarchy::Package:
    return DeclContextKind::Package;
  case ASTHierarchy::Decl: {
    auto decl = reinterpret_cast<const Decl*>(this + 1);
    if (isa<AbstractFunctionDecl>(decl))
      return DeclContextKind::AbstractFunctionDecl;
    if (isa<GenericTypeDecl>(decl))
      return DeclContextKind::GenericTypeDecl;
    switch (decl->getKind()) {
    case DeclKind::Module:
      return DeclContextKind::Module;
    case DeclKind::TopLevelCode:
      return DeclContextKind::TopLevelCodeDecl;
    case DeclKind::Subscript:
      return DeclContextKind::SubscriptDecl;
    case DeclKind::EnumElement:
      return DeclContextKind::EnumElementDecl;
    case DeclKind::Extension:
      return DeclContextKind::ExtensionDecl;
    case DeclKind::Macro:
      return DeclContextKind::MacroDecl;
    default:
      llvm_unreachable("Unhandled Decl kind");
    }
  }
  }
  llvm_unreachable("Unhandled DeclContext ASTHierarchy");
}

bool DeclContext::hasValueSemantics() const {
  if (getExtendedProtocolDecl())
    return !isClassConstrainedProtocolExtension();
  return !getDeclaredInterfaceType()->hasReferenceSemantics();
}

bool DeclContext::isClassConstrainedProtocolExtension() const {
  if (getExtendedProtocolDecl()) {
    auto ED = cast<ExtensionDecl>(this);
    if (auto sig = ED->getGenericSignature()) {
      return sig->requiresClass(ED->getSelfInterfaceType());
    }
  }
  return false;
}

bool DeclContext::isAsyncContext() const {
  switch (getContextKind()) {
  case DeclContextKind::Initializer:
  case DeclContextKind::EnumElementDecl:
  case DeclContextKind::ExtensionDecl:
  case DeclContextKind::SerializedLocal:
  case DeclContextKind::Package:
  case DeclContextKind::Module:
  case DeclContextKind::GenericTypeDecl:
  case DeclContextKind::MacroDecl:
    return false;
  case DeclContextKind::FileUnit:
    if (const SourceFile *sf = dyn_cast<SourceFile>(this))
      return sf->isAsyncTopLevelSourceFile();
    return false;
  case DeclContextKind::TopLevelCodeDecl:
    return getParent()->isAsyncContext();
  case DeclContextKind::AbstractClosureExpr:
    return cast<AbstractClosureExpr>(this)->isBodyAsync();
  case DeclContextKind::AbstractFunctionDecl: {
    const AbstractFunctionDecl *function = cast<AbstractFunctionDecl>(this);
    return function->hasAsync();
  }
  case DeclContextKind::SubscriptDecl: {
    AccessorDecl *getter =
        cast<SubscriptDecl>(this)->getAccessor(AccessorKind::Get);
    return getter != nullptr && getter->hasAsync();
  }
  }
  llvm_unreachable("Unhandled DeclContextKind switch");
}

SourceLoc swift::extractNearestSourceLoc(const DeclContext *dc) {
  switch (dc->getContextKind()) {
  case DeclContextKind::Package:
  case DeclContextKind::Module:
    return SourceLoc();
  case DeclContextKind::AbstractFunctionDecl:
  case DeclContextKind::EnumElementDecl:
  case DeclContextKind::ExtensionDecl:
  case DeclContextKind::GenericTypeDecl:
  case DeclContextKind::SubscriptDecl:
  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::MacroDecl:
    return extractNearestSourceLoc(dc->getAsDecl());

  case DeclContextKind::AbstractClosureExpr: {
    SourceLoc loc = cast<AbstractClosureExpr>(dc)->getLoc();
    if (loc.isValid())
      return loc;
    return extractNearestSourceLoc(dc->getParent());
  }

  case DeclContextKind::FileUnit:
    return SourceLoc();

  case DeclContextKind::Initializer:
  case DeclContextKind::SerializedLocal:
    return extractNearestSourceLoc(dc->getParent());
  }
  llvm_unreachable("Unhandled DeclContextKindIn switch");
}

#define DECL(Id, Parent) \
  static_assert(!std::is_base_of<DeclContext, Id##Decl>::value, \
                "Non-context Decl node has context?");
#define CONTEXT_DECL(Id, Parent) \
  static_assert(alignof(DeclContext) == alignof(Id##Decl), "Alignment error"); \
  static_assert(std::is_base_of<DeclContext, Id##Decl>::value, \
                "CONTEXT_DECL nodes must inherit from DeclContext");
#define CONTEXT_VALUE_DECL(Id, Parent) \
  static_assert(alignof(DeclContext) == alignof(Id##Decl), "Alignment error"); \
  static_assert(std::is_base_of<DeclContext, Id##Decl>::value, \
                "CONTEXT_VALUE_DECL nodes must inherit from DeclContext");
#include "swift/AST/DeclNodes.def"

#define EXPR(Id, Parent) \
  static_assert(!std::is_base_of<DeclContext, Id##Expr>::value, \
                "Non-context Expr node has context?");
#define CONTEXT_EXPR(Id, Parent) \
  static_assert(alignof(DeclContext) == alignof(Id##Expr), "Alignment error"); \
  static_assert(std::is_base_of<DeclContext, Id##Expr>::value, \
                "CONTEXT_EXPR nodes must inherit from DeclContext");
#include "swift/AST/ExprNodes.def"

#ifndef NDEBUG
// XXX -- static_cast is not static enough for use with static_assert().
// DO verify this by temporarily breaking a Decl or Expr.
// DO NOT assume that the compiler will emit this code blindly.
SWIFT_CONSTRUCTOR
static void verify_DeclContext_is_start_of_node() {
  auto decl = reinterpret_cast<Decl*>(0x1000 + sizeof(DeclContext));
#define DECL(Id, Parent)
#define CONTEXT_DECL(Id, Parent) \
  assert(reinterpret_cast<DeclContext*>(0x1000) == \
         static_cast<Id##Decl*>(decl));
#define CONTEXT_VALUE_DECL(Id, Parent) \
  assert(reinterpret_cast<DeclContext*>(0x1000) == \
         static_cast<Id##Decl*>(decl));
#include "swift/AST/DeclNodes.def"

  auto expr = reinterpret_cast<Expr*>(0x1000 + sizeof(DeclContext));
#define EXPR(Id, Parent)
#define CONTEXT_EXPR(Id, Parent) \
  assert(reinterpret_cast<DeclContext*>(0x1000) == \
         static_cast<Id##Expr*>(expr));
#include "swift/AST/ExprNodes.def"
}
#endif

void swift::simple_display(llvm::raw_ostream &out,
                           const IterableDeclContext *idc) {
  simple_display(out, idc->getDecl());
}

SourceLoc swift::extractNearestSourceLoc(const IterableDeclContext *idc) {
  return extractNearestSourceLoc(idc->getDecl());
}

static bool isSpecializeExtensionContext(const DeclContext *dc) {
  if (dc->isModuleScopeContext())
    return false;
  if (auto *extCtx = dyn_cast<ExtensionDecl>(dc)) {
    // and has specialized attr ...
    return extCtx->getAttrs().hasAttribute<SpecializeExtensionAttr>();
  }
  auto *parentDecl = dc->getParent();
  return isSpecializeExtensionContext(parentDecl);
}

bool DeclContext::isInSpecializeExtensionContext() const {
   return isSpecializeExtensionContext(this);
}

bool DeclContext::isAlwaysAvailableConformanceContext() const {
  auto *ext = dyn_cast<ExtensionDecl>(this);
  if (ext == nullptr)
    return true;

  if (AvailableAttr::isUnavailable(ext))
    return false;

  auto &ctx = getASTContext();

  AvailabilityContext conformanceAvailability{
      AvailabilityInference::availableRange(ext, ctx)};

  auto deploymentTarget =
      AvailabilityContext::forDeploymentTarget(ctx);

  return deploymentTarget.isContainedIn(conformanceAvailability);
}
