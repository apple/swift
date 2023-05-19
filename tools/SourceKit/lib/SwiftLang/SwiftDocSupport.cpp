//===--- SwiftDocSupport.cpp ----------------------------------------------===//
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

#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"
#include "SwiftASTManager.h"
#include "SwiftEditorDiagConsumer.h"
#include "SwiftLangSupport.h"
#include "SourceKit/Support/Logging.h"
#include "SourceKit/Support/UIdent.h"

#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/CommentConversion.h"
#include "swift/IDE/ModuleInterfacePrinting.h"
#include "swift/IDE/SourceEntityWalker.h"
#include "swift/IDE/SyntaxModel.h"
#include "swift/Refactoring/Refactoring.h"
// This is included only for createLazyResolver(). Move to different header ?
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Config.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace SourceKit;
using namespace swift;
using namespace ide;

namespace {
struct TextRange {
  unsigned Offset;
  unsigned Length;
};

struct TextEntity {
  const Decl *Dcl = nullptr;
  TypeOrExtensionDecl SynthesizeTarget;
  const Decl *DefaultImplementationOf = nullptr;
  ModuleDecl *DeclaringModIfFromCrossImportOverlay = nullptr;
  StringRef Argument;
  TextRange Range;
  unsigned LocOffset = 0;
  std::vector<TextEntity> SubEntities;
  const bool IsSynthesizedExtension;

  TextEntity(const Decl *D, TypeOrExtensionDecl SynthesizeTarget,
             const Decl *DefaultImplementationOf, unsigned StartOffset,
             bool IsSynthesizedExtension)
      : Dcl(D), SynthesizeTarget(SynthesizeTarget),
        DefaultImplementationOf(DefaultImplementationOf), Range{StartOffset, 0},
        IsSynthesizedExtension(IsSynthesizedExtension) {}

  TextEntity(const Decl *D, TypeOrExtensionDecl SynthesizeTarget,
             const Decl *DefaultImplementationOf, TextRange TR,
             unsigned LocOffset, bool IsSynthesizedExtension)
      : Dcl(D), DefaultImplementationOf(DefaultImplementationOf), Range(TR),
        LocOffset(LocOffset), IsSynthesizedExtension(IsSynthesizedExtension) {}

  TextEntity(const Decl *D, TypeOrExtensionDecl SynthesizeTarget,
             const Decl *DefaultImplementationOf, StringRef Arg, TextRange TR,
             unsigned LocOffset, bool IsSynthesizedExtension)
      : Dcl(D), SynthesizeTarget(SynthesizeTarget),
        DefaultImplementationOf(DefaultImplementationOf), Argument(Arg),
        Range(TR), LocOffset(LocOffset),
        IsSynthesizedExtension(IsSynthesizedExtension) {}
};

struct TextReference {
  const ValueDecl *Dcl = nullptr;
  TextRange Range;
  const Type Ty;

  TextReference(const ValueDecl *D, unsigned Offset, unsigned Length,
                const Type Ty = Type()) : Dcl(D), Range{Offset, Length}, Ty(Ty) {}
};

class AnnotatingPrinter : public StreamPrinter {

  std::pair<const ExtensionDecl *, TypeOrExtensionDecl>
      SynthesizedExtensionInfo = {nullptr, {}};

  typedef llvm::SmallDenseMap<ValueDecl*, ValueDecl*> DefaultImplementMap;
  llvm::SmallDenseMap<ProtocolDecl*, DefaultImplementMap> AllDefaultMaps;
  DefaultImplementMap *DefaultMapToUse = nullptr;

  void initDefaultMapToUse(const Decl *D) {
    const auto *ED = dyn_cast<ExtensionDecl>(D);
    if (!ED)
      return;
    if (auto NTD = ED->getExtendedNominal()) {
      if (auto *PD = dyn_cast<ProtocolDecl>(NTD)) {
        auto Pair = AllDefaultMaps.insert({PD, DefaultImplementMap()});
        DefaultMapToUse = &Pair.first->getSecond();
        if (Pair.second) {
          swift::collectDefaultImplementationForProtocolMembers(PD,
                                                    Pair.first->getSecond());
        }
      }
    }
  }

  void deinitDefaultMapToUse(const Decl*D) {
    if (isa<ExtensionDecl>(D)) {
      DefaultMapToUse = nullptr;
    }
  }

  ValueDecl *getDefaultImplementation(const Decl *D) {
    if (!DefaultMapToUse)
      return nullptr;
    auto *VD = const_cast<ValueDecl*>(dyn_cast<ValueDecl>(D));
    auto Found = DefaultMapToUse->find(VD);
    if (Found != DefaultMapToUse->end()) {
      return Found->second;
    }
    return nullptr;
  }

public:
  std::vector<TextEntity> TopEntities;
  std::vector<TextEntity> EntitiesStack;
  std::vector<TextReference> References;

  using StreamPrinter::StreamPrinter;

  ~AnnotatingPrinter() override {
    assert(EntitiesStack.empty());
  }

  bool shouldContinuePre(const Decl *D, Optional<BracketOptions> Bracket) {
    assert(Bracket.has_value());
    if (!Bracket.value().shouldOpenExtension(D) &&
        isa<ExtensionDecl>(D))
      return false;
    return true;
  }

  bool shouldContinuePost(const Decl *D, Optional<BracketOptions> Bracket) {
    assert(Bracket.has_value());
    if (!Bracket.value().shouldCloseNominal(D) && isa<NominalTypeDecl>(D))
      return false;
    if (!Bracket.value().shouldCloseExtension(D) &&
        isa<ExtensionDecl>(D))
      return false;
    return true;
  }

  void printSynthesizedExtensionPre(const ExtensionDecl *ED,
                                    TypeOrExtensionDecl Target,
                                    Optional<BracketOptions> Bracket) override {
    assert(!SynthesizedExtensionInfo.first);
    SynthesizedExtensionInfo = {ED, Target};
    if (!shouldContinuePre(ED, Bracket))
      return;
    unsigned StartOffset = OS.tell();
    EntitiesStack.emplace_back(ED, Target, nullptr, StartOffset, true);
  }

  void
  printSynthesizedExtensionPost(const ExtensionDecl *ED,
                                TypeOrExtensionDecl Target,
                                Optional<BracketOptions> Bracket) override {
    assert(SynthesizedExtensionInfo.first);
    SynthesizedExtensionInfo = {nullptr, {}};
    if (!shouldContinuePost(ED, Bracket))
      return;
    TextEntity Entity = std::move(EntitiesStack.back());
    EntitiesStack.pop_back();
    unsigned EndOffset = OS.tell();
    Entity.Range.Length = EndOffset - Entity.Range.Offset;
    TopEntities.push_back(std::move(Entity));
  }

  void printDeclPre(const Decl *D, Optional<BracketOptions> Bracket) override {
    if (isa<ParamDecl>(D))
      return; // Parameters are handled specially in addParameters().
    if (!shouldContinuePre(D, Bracket))
      return;
    unsigned StartOffset = OS.tell();
    initDefaultMapToUse(D);
    // If D is declared in the extension, then the synthesized target is valid.
    TypeOrExtensionDecl SynthesizedTarget;
    assert(D->getDeclContext()->isModuleScopeContext() == EntitiesStack.empty());
    if (D->getDeclContext() == SynthesizedExtensionInfo.first)
      SynthesizedTarget = SynthesizedExtensionInfo.second;
    EntitiesStack.emplace_back(D, SynthesizedTarget,
                               getDefaultImplementation(D), StartOffset, false);
  }

  void printDeclLoc(const Decl *D) override {
    if (EntitiesStack.back().Dcl == D) {
      unsigned LocOffset = OS.tell();
      EntitiesStack.back().LocOffset = LocOffset;
    }
  }

  void printDeclPost(const Decl *D, Optional<BracketOptions> Bracket) override {
    if (isa<ParamDecl>(D))
      return; // Parameters are handled specially in addParameters().
    if (!shouldContinuePost(D, Bracket))
      return;
    assert(!EntitiesStack.empty());
    TextEntity Entity = std::move(EntitiesStack.back());
    EntitiesStack.pop_back();
    unsigned EndOffset = OS.tell();
    Entity.Range.Length = EndOffset - Entity.Range.Offset;
    if (EntitiesStack.empty()) {
      assert (D->getDeclContext()->isModuleScopeContext());
      TopEntities.push_back(std::move(Entity));
    } else {
      assert (!D->getDeclContext()->isModuleScopeContext());
      EntitiesStack.back().SubEntities.push_back(std::move(Entity));
    }
    deinitDefaultMapToUse(D);
  }

  void printTypeRef(
      Type T, const TypeDecl *TD, Identifier Name,
      PrintNameContext NameContext = PrintNameContext::Normal) override {
    unsigned StartOffset = OS.tell();
    References.emplace_back(TD, StartOffset, Name.str().size());
    StreamPrinter::printTypeRef(T, TD, Name, NameContext);
  }
};

struct SourceTextInfo {
  std::string Text;
  std::vector<TextEntity> TopEntities;
  std::vector<TextReference> References;
};

} // end anonymous namespace

static void initDocGenericParams(const Decl *D, DocEntityInfo &Info,
                                  TypeOrExtensionDecl SynthesizedTarget,
                                  bool IsSynthesizedExt) {
  auto *GC = D->getAsGenericContext();
  if (!GC)
    return;

  GenericSignature GenericSig = GC->getGenericSignatureOfContext();
  if (!GenericSig)
    return;

  // The declaration may not be generic itself, but instead carry additional
  // generic requirements in a contextual where clause, so checking !isGeneric()
  // is insufficient.
  const auto ParentSig = GC->getParent()->getGenericSignatureOfContext();
  if (ParentSig && ParentSig->isEqual(GenericSig))
    return;


  // If we have a synthesized target, map from its base type into the this
  // declaration's innermost type context, or if we're dealing with the
  // synthesized extension itself rather than a member, into its extended
  // nominal (the extension's own requirements shouldn't be considered in the
  // substitution).
  unsigned TypeContextDepth = 0;
  SubstitutionMap SubMap;
  ModuleDecl *M = nullptr;
  Type BaseType;
  if (SynthesizedTarget) {
    BaseType = SynthesizedTarget.getBaseNominal()->getDeclaredInterfaceType();
    if (!BaseType->isExistentialType()) {
      DeclContext *DC;
      if (IsSynthesizedExt)
        DC = cast<ExtensionDecl>(D)->getExtendedNominal();
      else
        DC = D->getInnermostDeclContext()->getInnermostTypeContext();
      M = DC->getParentModule();
      SubMap = BaseType->getContextSubstitutionMap(M, DC);
      if (!SubMap.empty()) {
        TypeContextDepth = SubMap.getGenericSignature()
            .getGenericParams().back()->getDepth() + 1;
      }
    }
  }

  auto SubstTypes = [&](Type Ty) {
    if (SubMap.empty())
      return Ty;

    return Ty.subst(
      [&](SubstitutableType *type) -> Type {
        if (cast<GenericTypeParamType>(type)->getDepth() < TypeContextDepth)
          return Type(type).subst(SubMap);
        return type;
      },
      [&](CanType depType, Type substType, ProtocolDecl *proto) {
        return M->lookupConformance(substType, proto);
      },
      SubstFlags::DesugarMemberTypes);
  };

  // FIXME: Not right for extensions of nested generic types
  if (GC->isGeneric()) {
    for (auto *GP : GenericSig.getInnermostGenericParams()) {
      if (GP->getDecl()->isImplicit())
        continue;
      Type TypeToPrint = GP;
      if (!SubMap.empty()) {
        if (auto ArgTy = SubstTypes(GP)) {
          if (!ArgTy->hasError()) {
            // Ignore parameter that aren't generic after substitution
            if (!ArgTy->is<ArchetypeType>() && !ArgTy->isTypeParameter())
              continue;
            TypeToPrint = ArgTy;
          }
        }
      }
      DocGenericParam Param;
      Param.Name = TypeToPrint->getString();
      Info.GenericParams.push_back(Param);
    }
  }

  ProtocolDecl *Proto = nullptr;
  if (auto *typeDC = GC->getInnermostTypeContext())
    Proto = typeDC->getSelfProtocolDecl();

  for (auto Req: GenericSig.getRequirements()) {
    if (Proto &&
        Req.getKind() == RequirementKind::Conformance &&
        Req.getFirstType()->isEqual(Proto->getSelfInterfaceType()) &&
        Req.getProtocolDecl() == Proto)
      continue;

    auto First = Req.getFirstType();
    Type Second;
    if (Req.getKind() != RequirementKind::Layout)
      Second = Req.getSecondType();

    if (!SubMap.empty()) {
      Type SubFirst = SubstTypes(First);
      if (!SubFirst->hasError())
        First = SubFirst;
      if (Second) {
        Type SubSecond = SubstTypes(Second);
        if (!SubSecond->hasError())
          Second = SubSecond;
        // Ignore requirements that don't involve a generic after substitution.
        if (!(First->is<ArchetypeType>() || First->isTypeParameter()) &&
            !(Second->is<ArchetypeType>() || Second->isTypeParameter()))
          continue;
      }
    }

    std::string ReqStr;
    llvm::raw_string_ostream OS(ReqStr);
    PrintOptions Opts;
    if (Req.getKind() != RequirementKind::Layout) {
      Requirement(Req.getKind(), First, Second).print(OS, Opts);
    } else {
      Requirement(Req.getKind(), First, Req.getLayoutConstraint()).print(OS, Opts);
    }
    OS.flush();
    Info.GenericRequirements.push_back(std::move(ReqStr));
  }

  if (IsSynthesizedExt) {
    // If there's a conditional conformance on the base type that 'enabled' this
    // extension, we need to print its requirements too.
    if (auto *EnablingExt = dyn_cast<ExtensionDecl>(SynthesizedTarget.getAsDecl())) {
      if (EnablingExt->isConstrainedExtension()) {
        initDocGenericParams(EnablingExt, Info,
                             /*Target=*/EnablingExt->getExtendedNominal(),
                             /*IsSynthesizedExtension*/false);
      }
    }
  }
}

static bool initDocEntityInfo(const Decl *D,
                              TypeOrExtensionDecl SynthesizedTarget,
                              const Decl *DefaultImplementationOf, bool IsRef,
                              bool IsSynthesizedExtension, DocEntityInfo &Info,
                              StringRef Arg = StringRef(),
                              ModuleDecl *DeclaringModForCrossImport = nullptr){
  if (!IsRef && D->isImplicit())
    return true;
  if (!D || isa<ParamDecl>(D) ||
      (isa<VarDecl>(D) && D->getDeclContext()->isLocalContext())) {
    Info.Kind = SwiftLangSupport::getUIDForLocalVar(IsRef);
    if (D) {
      llvm::raw_svector_ostream OS(Info.Name);
      SwiftLangSupport::printDisplayName(cast<ValueDecl>(D), OS);
    } else {
      Info.Name = "_";
    }

    if (!Arg.empty())
      Info.Argument = Arg.str();

    return false;
  }

  auto SynthesizedTargetNTD =
      SynthesizedTarget ? SynthesizedTarget.getBaseNominal() : nullptr;

  if (IsSynthesizedExtension) {
    Info.Kind =
        SwiftLangSupport::getUIDForExtensionOfDecl(SynthesizedTargetNTD);
  } else
    Info.Kind = SwiftLangSupport::getUIDForDecl(D, IsRef);

  if (Info.Kind.isInvalid())
    return true;
  if (const auto *VD = dyn_cast<ValueDecl>(D)) {
    llvm::raw_svector_ostream NameOS(Info.Name);
    SwiftLangSupport::printDisplayName(VD, NameOS);
    {
      llvm::raw_svector_ostream OS(Info.USR);
      SwiftLangSupport::printUSR(VD, OS);
      if (SynthesizedTarget) {
        OS << SwiftLangSupport::SynthesizedUSRSeparator;
        SwiftLangSupport::printUSR(SynthesizedTargetNTD, OS);
        {
          llvm::raw_svector_ostream OS(Info.OriginalUSR);
          SwiftLangSupport::printUSR(VD, OS);
        }
      }
    }
  }

  if (DefaultImplementationOf) {
    llvm::raw_svector_ostream OS(Info.ProvideImplementationOfUSR);
    SwiftLangSupport::printUSR((const ValueDecl*)DefaultImplementationOf, OS);
  }

  Info.IsUnavailable = AvailableAttr::isUnavailable(D);
  Info.IsDeprecated = D->getAttrs().getDeprecated(D->getASTContext()) != nullptr;
  Info.IsOptional = D->getAttrs().hasAttribute<OptionalAttr>();
  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
    Info.IsAsync = AFD->hasAsync();
  } else if (auto *Storage = dyn_cast<AbstractStorageDecl>(D)) {
    if (auto *Getter = Storage->getAccessor(AccessorKind::Get))
      Info.IsAsync = Getter->hasAsync();
  }

  if (!IsRef) {
    llvm::raw_svector_ostream OS(Info.DocComment);

    {
      llvm::SmallString<128> DocBuffer;
      {
        llvm::raw_svector_ostream OSS(DocBuffer);
        ide::getDocumentationCommentAsXML(D, OSS, SynthesizedTarget);
      }
      OS << DocBuffer;
    }

    initDocGenericParams(D, Info, SynthesizedTarget, IsSynthesizedExtension);

    llvm::raw_svector_ostream LocalizationKeyOS(Info.LocalizationKey);
    ide::getLocalizationKey(D, LocalizationKeyOS);

    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      llvm::raw_svector_ostream OS(Info.FullyAnnotatedDecl);
      if (SynthesizedTarget)
        SwiftLangSupport::printFullyAnnotatedSynthesizedDeclaration(
            VD, SynthesizedTarget, OS);
      else
        SwiftLangSupport::printFullyAnnotatedDeclaration(VD, Type(), OS);
    } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
      llvm::raw_svector_ostream OS(Info.FullyAnnotatedDecl);
      if (SynthesizedTarget)
        SwiftLangSupport::printFullyAnnotatedSynthesizedDeclaration(
            ED, SynthesizedTarget, OS);
      else
        SwiftLangSupport::printFullyAnnotatedDeclaration(ED, OS);
    }

    if (DeclaringModForCrossImport) {
      ModuleDecl *MD = D->getModuleContext();
      SmallVector<Identifier, 1> Bystanders;
      if (MD->getRequiredBystandersIfCrossImportOverlay(
          DeclaringModForCrossImport, Bystanders)) {
        llvm::transform(
            Bystanders, std::back_inserter(Info.RequiredBystanders),
            [](Identifier Bystander) { return Bystander.str().str(); });
      } else {
        llvm_unreachable("DeclaringModForCrossImport not correct?");
      }
    }
  }

  switch(D->getDeclContext()->getContextKind()) {
    case DeclContextKind::AbstractClosureExpr:
    case DeclContextKind::TopLevelCodeDecl:
    case DeclContextKind::AbstractFunctionDecl:
    case DeclContextKind::SubscriptDecl:
    case DeclContextKind::EnumElementDecl:
    case DeclContextKind::Initializer:
    case DeclContextKind::SerializedLocal:
    case DeclContextKind::ExtensionDecl:
    case DeclContextKind::GenericTypeDecl:
    case DeclContextKind::MacroDecl:
    case DeclContextKind::Package:
      break;

    // We report sub-module information only for top-level decls.
    case DeclContextKind::Module:
    case DeclContextKind::FileUnit: {
      if (auto *CD = D->getClangDecl()) {
        if (auto *M = CD->getImportedOwningModule()) {
          if (M->isSubModule()) {
            llvm::raw_svector_ostream OS(Info.SubModuleName);
            ModuleDecl::ReverseFullNameIterator(M).printForward(OS);
          }
        }
      }
      break;
    }
  }

  return false;
}

static bool initDocEntityInfo(const TextEntity &Entity,
                              DocEntityInfo &Info) {
  if (initDocEntityInfo(Entity.Dcl, Entity.SynthesizeTarget,
                        Entity.DefaultImplementationOf,
                        /*IsRef=*/false, Entity.IsSynthesizedExtension,
                        Info, Entity.Argument,
                        Entity.DeclaringModIfFromCrossImportOverlay))
    return true;
  Info.Offset = Entity.Range.Offset;
  Info.Length = Entity.Range.Length;
  return false;
}

static const TypeDecl *getTypeDeclFromType(Type Ty) {
  if (auto alias = dyn_cast<TypeAliasType>(Ty.getPointer()))
    return alias->getDecl();
  return Ty->getAnyNominal();
}

static void passInherits(const ValueDecl *D, DocInfoConsumer &Consumer) {
  DocEntityInfo EntInfo;
  if (initDocEntityInfo(D, {}, nullptr, /*IsRef=*/true, false, EntInfo))
    return;
  Consumer.handleInheritsEntity(EntInfo);
}
static void passConforms(const ValueDecl *D, DocInfoConsumer &Consumer) {
  DocEntityInfo EntInfo;
  if (initDocEntityInfo(D, {}, nullptr, /*IsRef=*/true, false, EntInfo))
    return;
  Consumer.handleConformsToEntity(EntInfo);
}
static void passInherits(ArrayRef<InheritedEntry> InheritedTypes,
                         DocInfoConsumer &Consumer) {
  for (auto Inherited : InheritedTypes) {
    if (!Inherited.getType())
      continue;

    if (auto Proto = Inherited.getType()->getAs<ProtocolType>()) {
      passConforms(Proto->getDecl(), Consumer);
      continue;
    }

    if (auto ProtoComposition
               = Inherited.getType()->getAs<ProtocolCompositionType>()) {
      for (auto T : ProtoComposition->getMembers())
        passInherits(InheritedEntry(TypeLoc::withoutLoc(T)), Consumer);
      continue;
    }

    if (auto TD = getTypeDeclFromType(Inherited.getType())) {
      passInherits(TD, Consumer);
      continue;
    }
  }
}

static void passConforms(ArrayRef<ValueDecl *> Dcls,
                         DocInfoConsumer &Consumer) {
  for (auto D : Dcls)
    passConforms(D, Consumer);
}
static void passExtends(const ValueDecl *D, DocInfoConsumer &Consumer) {
  DocEntityInfo EntInfo;
  if (initDocEntityInfo(D, {}, nullptr, /*IsRef=*/true, false, EntInfo))
    return;
  Consumer.handleExtendsEntity(EntInfo);
}

static void passInheritsAndConformancesForValueDecl(const ValueDecl *VD,
                                                    DocInfoConsumer &Consumer) {
  if (auto Overridden = VD->getOverriddenDecl())
    passInherits(Overridden, Consumer);
  passConforms(VD->getSatisfiedProtocolRequirements(/*Sorted=*/true),
               Consumer);
}

static void reportRelated(ASTContext &Ctx, const Decl *D,
                          TypeOrExtensionDecl SynthesizedTarget,
                          DocInfoConsumer &Consumer) {
  if (!D || isa<ParamDecl>(D))
    return;
  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    if (SynthesizedTarget) {
      auto Extends = SynthesizedTarget.getBaseNominal();
      passExtends(Extends, Consumer);
    } else if (Type T = ED->getExtendedType()) {
      if (auto TD = getTypeDeclFromType(T))
        passExtends(TD, Consumer);
    }

    passInherits(ED->getInherited(), Consumer);

  } else if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {

    if (auto Ty = TAD->getDeclaredInterfaceType()) {
      // If underlying type exists, report the inheritance and conformance of the
      // underlying type.
      if (auto NM = Ty->getAnyNominal()) {
        passInherits(NM->getInherited(), Consumer);
        passConforms(NM->getSatisfiedProtocolRequirements(/*Sorted=*/true),
                     Consumer);
        return;
      }
    }

    // Otherwise, report the inheritance of the type alias itself.
    passInheritsAndConformancesForValueDecl(TAD, Consumer);
  } else if (const auto *TD = dyn_cast<TypeDecl>(D)) {
    llvm::SmallVector<InheritedEntry, 4> AllInheritsForPrinting;
    getInheritedForPrinting(TD, PrintOptions(), AllInheritsForPrinting);
    passInherits(AllInheritsForPrinting, Consumer);
    passConforms(TD->getSatisfiedProtocolRequirements(/*Sorted=*/true),
                 Consumer);
  } else if (auto *VD = dyn_cast<ValueDecl>(D)) {
    passInheritsAndConformancesForValueDecl(VD, Consumer);
  }
}

static ArrayRef<const DeclAttribute*>
getDeclAttributes(const Decl *D, std::vector<const DeclAttribute*> &Scratch) {
  for (auto Attr : D->getAttrs()) {
    Scratch.push_back(Attr);
  }
  // For enum elements, inherit their parent enum decls' deprecated attributes.
  if (auto *DE = dyn_cast<EnumElementDecl>(D)) {
    for (auto Attr : DE->getParentEnum()->getAttrs()) {
      if (auto Avail = dyn_cast<AvailableAttr>(Attr)) {
        if (Avail->Deprecated || Avail->isUnconditionallyDeprecated()) {
          Scratch.push_back(Attr);
        }
      }
    }
  }

  return llvm::makeArrayRef(Scratch);
}

// Only reports @available.
// FIXME: Handle all attributes.
static void reportAttributes(ASTContext &Ctx,
                             const Decl *D,
                             DocInfoConsumer &Consumer) {
  static UIdent AvailableAttrKind("source.lang.swift.attribute.availability");
  static UIdent PlatformIOS("source.availability.platform.ios");
  static UIdent PlatformMacCatalyst("source.availability.platform.maccatalyst");
  static UIdent PlatformOSX("source.availability.platform.osx");
  static UIdent PlatformtvOS("source.availability.platform.tvos");
  static UIdent PlatformWatchOS("source.availability.platform.watchos");
  static UIdent PlatformIOSAppExt("source.availability.platform.ios_app_extension");
  static UIdent PlatformMacCatalystAppExt("source.availability.platform.maccatalyst_app_extension");
  static UIdent PlatformOSXAppExt("source.availability.platform.osx_app_extension");
  static UIdent PlatformtvOSAppExt("source.availability.platform.tvos_app_extension");
  static UIdent PlatformWatchOSAppExt("source.availability.platform.watchos_app_extension");
  static UIdent PlatformOpenBSD("source.availability.platform.openbsd");
  static UIdent PlatformWindows("source.availability.platform.windows");
  std::vector<const DeclAttribute*> Scratch;

  for (auto Attr : getDeclAttributes(D, Scratch)) {
    if (auto Av = dyn_cast<AvailableAttr>(Attr)) {
      UIdent PlatformUID;
      switch (Av->Platform) {
      case PlatformKind::none:
        PlatformUID = UIdent(); break;
      case PlatformKind::iOS:
        PlatformUID = PlatformIOS; break;
      case PlatformKind::macCatalyst:
        PlatformUID = PlatformMacCatalyst; break;
      case PlatformKind::macOS:
        PlatformUID = PlatformOSX; break;
      case PlatformKind::tvOS:
        PlatformUID = PlatformtvOS; break;
      case PlatformKind::watchOS:
        PlatformUID = PlatformWatchOS; break;
      case PlatformKind::iOSApplicationExtension:
        PlatformUID = PlatformIOSAppExt; break;
      case PlatformKind::macCatalystApplicationExtension:
        PlatformUID = PlatformMacCatalystAppExt; break;
      case PlatformKind::macOSApplicationExtension:
        PlatformUID = PlatformOSXAppExt; break;
      case PlatformKind::tvOSApplicationExtension:
        PlatformUID = PlatformtvOSAppExt; break;
      case PlatformKind::watchOSApplicationExtension:
        PlatformUID = PlatformWatchOSAppExt; break;
      case PlatformKind::OpenBSD:
        PlatformUID = PlatformOpenBSD; break;
      case PlatformKind::Windows:
        PlatformUID = PlatformWindows; break;
      }

      AvailableAttrInfo Info;
      Info.AttrKind = AvailableAttrKind;
      Info.IsUnavailable = Av->isUnconditionallyUnavailable();
      Info.IsDeprecated = Av->isUnconditionallyDeprecated();
      Info.Platform = PlatformUID;
      Info.Message = Av->Message;
      if (Av->Introduced)
        Info.Introduced = *Av->Introduced;
      if (Av->Deprecated)
        Info.Deprecated = *Av->Deprecated;
      if (Av->Obsoleted)
        Info.Obsoleted = *Av->Obsoleted;

      Consumer.handleAvailableAttribute(Info);
    }
  }
}

static void reportDocEntities(ASTContext &Ctx,
                              ArrayRef<TextEntity> Entities,
                              DocInfoConsumer &Consumer) {
  for (auto &Entity : Entities) {
    DocEntityInfo EntInfo;
    if (initDocEntityInfo(Entity, EntInfo))
      continue;
    Consumer.startSourceEntity(EntInfo);
    reportRelated(Ctx, Entity.Dcl,
                  Entity.IsSynthesizedExtension ? Entity.SynthesizeTarget
                                                : TypeOrExtensionDecl(),
                  Consumer);
    reportDocEntities(Ctx, Entity.SubEntities, Consumer);
    reportAttributes(Ctx, Entity.Dcl, Consumer);
    Consumer.finishSourceEntity(EntInfo.Kind);
  }
}

namespace {
class DocSyntaxWalker : public SyntaxModelWalker {
  SourceManager &SM;
  unsigned BufferID;
  ArrayRef<TextReference> References;
  DocInfoConsumer &Consumer;
  SourceLoc LastArgLoc;
  SourceLoc LastParamLoc;

public:
  DocSyntaxWalker(SourceManager &SM, unsigned BufferID,
                  ArrayRef<TextReference> References,
                  DocInfoConsumer &Consumer)
    : SM(SM), BufferID(BufferID), References(References), Consumer(Consumer) {}

  bool walkToNodePre(SyntaxNode Node) override {
    // Ignore things that don't come from this buffer.
    if (!SM.getRangeForBuffer(BufferID).contains(Node.Range.getStart()))
      return false;

    unsigned Offset = SM.getLocOffsetInBuffer(Node.Range.getStart(), BufferID);
    unsigned Length = Node.Range.getByteLength();

    reportRefsUntil(Offset);
    if (!References.empty() && References.front().Range.Offset == Offset)
      return true;

    switch (Node.Kind) {
    case SyntaxNodeKind::EditorPlaceholder:
      return true;

    case SyntaxNodeKind::Keyword:
    case SyntaxNodeKind::Identifier:
      if (Node.Range.getStart() == LastArgLoc ||
          Node.Range.getStart() == LastParamLoc)
        return true;
      break;

    case SyntaxNodeKind::Operator:
    case SyntaxNodeKind::DollarIdent:
    case SyntaxNodeKind::Integer:
    case SyntaxNodeKind::Floating:
    case SyntaxNodeKind::String:
    case SyntaxNodeKind::StringInterpolationAnchor:
    case SyntaxNodeKind::CommentLine:
    case SyntaxNodeKind::CommentBlock:
    case SyntaxNodeKind::CommentMarker:
    case SyntaxNodeKind::CommentURL:
    case SyntaxNodeKind::DocCommentLine:
    case SyntaxNodeKind::DocCommentBlock:
    case SyntaxNodeKind::DocCommentField:
    case SyntaxNodeKind::TypeId:
    case SyntaxNodeKind::BuildConfigKeyword:
    case SyntaxNodeKind::BuildConfigId:
    case SyntaxNodeKind::PoundDirectiveKeyword:
    case SyntaxNodeKind::AttributeId:
    case SyntaxNodeKind::AttributeBuiltin:
    case SyntaxNodeKind::ObjectLiteral:
      break;
    }

    DocEntityInfo Info;
    Info.Kind = SwiftLangSupport::getUIDForSyntaxNodeKind(Node.Kind);
    Info.Offset = Offset;
    Info.Length = Length;
    Consumer.handleAnnotation(Info);
    return true;
  }

  void finished() {
    reportRefsUntil(std::numeric_limits<unsigned>::max());
  }

  bool walkToSubStructurePre(SyntaxStructureNode Node) override {
    if (Node.Kind == SyntaxStructureKind::Parameter) {
      auto Param = dyn_cast<ParamDecl>(Node.Dcl);

      auto passAnnotation = [&](UIdent Kind, SourceLoc Loc, Identifier Name) {
        if (Loc.isInvalid())
          return;

        // Ignore things that don't come from this buffer.
        if (!SM.getRangeForBuffer(BufferID).contains(Loc))
          return;

        unsigned Offset = SM.getLocOffsetInBuffer(Loc, BufferID);
        unsigned Length = Name.empty() ? 1 : Name.getLength();
        reportRefsUntil(Offset);

        DocEntityInfo Info;
        Info.Kind = Kind;
        Info.Offset = Offset;
        Info.Length = Length;
        Consumer.handleAnnotation(Info);
      };

      // Argument
      static UIdent KindArgument("source.lang.swift.syntaxtype.argument");
      passAnnotation(KindArgument, Param->getArgumentNameLoc(),
                     Param->getArgumentName());
      LastArgLoc = Param->getArgumentNameLoc();

      // Parameter
      static UIdent KindParameter("source.lang.swift.syntaxtype.parameter");
      passAnnotation(KindParameter, Param->getNameLoc(), Param->getName());
      LastParamLoc = Param->getNameLoc();
    }

    return true;
  }

private:
  void reportRefsUntil(unsigned Offset) {
    while (!References.empty() && References.front().Range.Offset < Offset) {
      const TextReference &Ref = References.front();
      References = References.slice(1);
      DocEntityInfo Info;
      if (initDocEntityInfo(Ref.Dcl, {}, nullptr, /*IsRef=*/true, false, Info))
        continue;
      Info.Offset = Ref.Range.Offset;
      Info.Length = Ref.Range.Length;
      Info.Ty = Ref.Ty;
      Consumer.handleAnnotation(Info);
    }
  }
};
} // end anonymous namespace

static bool makeParserAST(CompilerInstance &CI, StringRef Text,
                          CompilerInvocation Invocation) {
  Invocation.getFrontendOptions().InputsAndOutputs.clearInputs();
  Invocation.setModuleName("main");
  Invocation.getLangOptions().DisablePoundIfEvaluation = true;

  std::unique_ptr<llvm::MemoryBuffer> Buf;
  Buf = llvm::MemoryBuffer::getMemBuffer(Text, "<module-interface>");
  Invocation.getFrontendOptions().InputsAndOutputs.addInput(
      InputFile(Buf.get()->getBufferIdentifier(), /*isPrimary*/false, Buf.get(),
                file_types::TY_Swift));
  std::string InstanceSetupError;
  return CI.setup(Invocation, InstanceSetupError);
}

static void collectFuncEntities(std::vector<TextEntity> &Ents,
                                std::vector<TextEntity*> &FuncEntities) {
  for (TextEntity &Ent : Ents) {
    if (isa<AbstractFunctionDecl>(Ent.Dcl) || isa<SubscriptDecl>(Ent.Dcl)) {
      // We are getting the entities via a pointer and later adding to their
      // subentities; make sure it doesn't have subentities now or we are going
      // to invalidate the pointers.
      assert(Ent.SubEntities.empty());
      FuncEntities.push_back(&Ent);
    }
    collectFuncEntities(Ent.SubEntities, FuncEntities);
  }
}

static void addParameters(ArrayRef<Identifier> &ArgNames,
                          const ParameterList *paramList,
                          TextEntity &Ent,
                          SourceManager &SM,
                          unsigned BufferID) {
  for (auto &param : *paramList) {
    StringRef Arg;
    if (!ArgNames.empty()) {
      Identifier Id = ArgNames.front();
      Arg = Id.empty() ? "_" : Id.str();
      ArgNames = ArgNames.slice(1);
    }

    if (auto typeRepr = param->getTypeRepr()) {
      SourceRange TypeRange = typeRepr->getSourceRange();
      if (auto OwnTyR = dyn_cast_or_null<OwnershipTypeRepr>(typeRepr))
        TypeRange = OwnTyR->getBase()->getSourceRange();
      if (TypeRange.isInvalid())
        continue;

      // Ignore things that don't come from this buffer.
      if (!SM.getRangeForBuffer(BufferID).contains(TypeRange.Start))
        continue;

      unsigned StartOffs = SM.getLocOffsetInBuffer(TypeRange.Start, BufferID);
      unsigned EndOffs =
        SM.getLocOffsetInBuffer(Lexer::getLocForEndOfToken(SM, TypeRange.End),
                                BufferID);
      TextRange TR{ StartOffs, EndOffs-StartOffs };
      TextEntity Param(param, {}, nullptr, Arg, TR, StartOffs, false);
      Ent.SubEntities.push_back(std::move(Param));
    }
  }
}

static void addParameters(const AbstractFunctionDecl *FD,
                          TextEntity &Ent,
                          SourceManager &SM,
                          unsigned BufferID) {
  ArrayRef<Identifier> ArgNames;
  DeclName Name = FD->getName();
  if (Name) {
    ArgNames = Name.getArgumentNames();
  }
  auto paramList = FD->getParameters();
  addParameters(ArgNames, paramList, Ent, SM, BufferID);
}

static void addParameters(const SubscriptDecl *D,
                          TextEntity &Ent,
                          SourceManager &SM,
                          unsigned BufferID) {
  ArrayRef<Identifier> ArgNames;
  DeclName Name = D->getName();
  if (Name) {
    ArgNames = Name.getArgumentNames();
  }
  addParameters(ArgNames, D->getIndices(), Ent, SM, BufferID);
}

namespace {
class FuncWalker : public ASTWalker {
  SourceManager &SM;
  unsigned BufferID;
  llvm::MutableArrayRef<TextEntity*> FuncEnts;

public:
  FuncWalker(SourceManager &SM, unsigned BufferID,
             llvm::MutableArrayRef<TextEntity*> FuncEnts)
    : SM(SM), BufferID(BufferID), FuncEnts(FuncEnts) {}

  /// Only walk the arguments of a macro, to represent the source as written.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (D->isImplicit())
      return Action::SkipChildren(); // Skip body.

    if (FuncEnts.empty())
      return Action::SkipChildren();

    if (!isa<AbstractFunctionDecl>(D) && !isa<SubscriptDecl>(D))
      return Action::Continue();

    // Ignore things that don't come from this buffer.
    if (!SM.getRangeForBuffer(BufferID).contains(D->getLoc()))
      return Action::SkipChildren();

    unsigned Offset = SM.getLocOffsetInBuffer(D->getLoc(), BufferID);
    auto Found = FuncEnts.end();
    if (FuncEnts.front()->LocOffset == Offset) {
      Found = FuncEnts.begin();
    } else {
      Found = std::lower_bound(FuncEnts.begin(), FuncEnts.end(), Offset,
        [](TextEntity *Ent, unsigned Offs) {
          return Ent->LocOffset < Offs;
        });
    }
    if (Found == FuncEnts.end() || (*Found)->LocOffset != Offset)
      return Action::SkipChildren();
    if (auto FD = dyn_cast<AbstractFunctionDecl>(D)) {
      addParameters(FD, **Found, SM, BufferID);
    } else {
      addParameters(cast<SubscriptDecl>(D), **Found, SM, BufferID);
    }
    FuncEnts = llvm::MutableArrayRef<TextEntity*>(Found+1, FuncEnts.end());
    return Action::SkipChildren(); // skip body.
  }
};
} // end anonymous namespace

static void addParameterEntities(CompilerInstance &CI,
                                 SourceTextInfo &IFaceInfo) {
  std::vector<TextEntity*> FuncEntities;
  collectFuncEntities(IFaceInfo.TopEntities, FuncEntities);
  llvm::MutableArrayRef<TextEntity*> FuncEnts(FuncEntities.data(),
                                              FuncEntities.size());
  for (auto Unit : CI.getMainModule()->getFiles()) {
    auto SF = dyn_cast<SourceFile>(Unit);
    if (!SF)
      continue;
    FuncWalker Walker(CI.getSourceMgr(), *SF->getBufferID(), FuncEnts);
    SF->walk(Walker);
  }
}

static void reportSourceAnnotations(const SourceTextInfo &IFaceInfo,
                                    CompilerInstance &CI,
                                    DocInfoConsumer &Consumer) {
  for (auto Unit : CI.getMainModule()->getFiles()) {
    auto SF = dyn_cast<SourceFile>(Unit);
    if (!SF)
      continue;

    SyntaxModelContext SyntaxContext(*SF);
    DocSyntaxWalker SyntaxWalker(CI.getSourceMgr(), *SF->getBufferID(),
                                 IFaceInfo.References, Consumer);
    SyntaxContext.walk(SyntaxWalker);
    SyntaxWalker.finished();
  }
}

static bool getModuleInterfaceInfo(ASTContext &Ctx, StringRef ModuleName,
                                   SourceTextInfo &Info) {
  // Load standard library so that Clang importer can use it.
  auto *Stdlib = Ctx.getModuleByIdentifier(Ctx.StdlibModuleName);
  if (!Stdlib)
    return true;

  auto *M = Ctx.getModuleByName(ModuleName);
  if (!M)
    return true;

  PrintOptions Options = PrintOptions::printDocInterface();
  ModuleTraversalOptions TraversalOptions = None;
  TraversalOptions |= ModuleTraversal::VisitSubmodules;
  TraversalOptions |= ModuleTraversal::VisitHidden;

  SmallString<128> Text;
  llvm::raw_svector_ostream OS(Text);
  AnnotatingPrinter Printer(OS);

  printModuleInterface(M, None, TraversalOptions, Printer, Options, true);

  Info.Text = std::string(OS.str());
  Info.TopEntities = std::move(Printer.TopEntities);
  Info.References = std::move(Printer.References);

  // Add a reference to the main module on any entities from cross-import
  // overlay modules (used to determine their bystanders later).
  for (auto &Entity: Info.TopEntities) {
    auto *EntityMod = Entity.Dcl->getModuleContext();
    if (!EntityMod || EntityMod == M)
      continue;
    if (EntityMod->isCrossImportOverlayOf(M))
      Entity.DeclaringModIfFromCrossImportOverlay = M;
  }
  return false;
}

static bool reportModuleDocInfo(CompilerInvocation Invocation,
                                StringRef ModuleName,
                                DocInfoConsumer &Consumer) {
  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  std::string InstanceSetupError;
  if (CI.setup(Invocation, InstanceSetupError)) {
    Consumer.failed(InstanceSetupError);
    return true;
  }

  ASTContext &Ctx = CI.getASTContext();
  registerIDERequestFunctions(Ctx.evaluator);

  // Load implicit imports so that Clang importer can use it.
  for (auto unloadedImport :
       CI.getMainModule()->getImplicitImportInfo().AdditionalUnloadedImports) {
    (void)Ctx.getModule(unloadedImport.module.getModulePath());
  }

  SourceTextInfo IFaceInfo;
  if (getModuleInterfaceInfo(Ctx, ModuleName, IFaceInfo))
    return true;

  CompilerInstance ParseCI;
  if (makeParserAST(ParseCI, IFaceInfo.Text, Invocation))
    return true;
  addParameterEntities(ParseCI, IFaceInfo);

  Consumer.handleSourceText(IFaceInfo.Text);
  reportDocEntities(Ctx, IFaceInfo.TopEntities, Consumer);
  reportSourceAnnotations(IFaceInfo, ParseCI, Consumer);

  return false;
}

namespace {
class SourceDocASTWalker : public SourceEntityWalker {
public:
  SourceManager &SM;
  unsigned BufferID;

  std::vector<TextEntity> TopEntities;
  std::vector<TextEntity> EntitiesStack;
  std::vector<TextReference> References;

  SourceDocASTWalker(SourceManager &SM, unsigned BufferID)
    : SM(SM), BufferID(BufferID) {}

  ~SourceDocASTWalker() override {
    assert(EntitiesStack.empty());
  }

  bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
    if (!isa<ValueDecl>(D) && !isa<ExtensionDecl>(D))
      return true;
    if (isLocal(D))
      return true;

    // Ignore things that don't come from this buffer.
    if (!SM.getRangeForBuffer(BufferID).contains(D->getSourceRange().Start))
      return false;

    TextRange TR = getTextRange(D->getSourceRange());
    unsigned LocOffset = getOffset(Range.getStart());
    EntitiesStack.emplace_back(D, TypeOrExtensionDecl(), nullptr, TR, LocOffset,
                               false);
    return true;
  }

  bool walkToDeclPost(Decl *D) override {
    if (EntitiesStack.empty() || EntitiesStack.back().Dcl != D)
      return true;

    TextEntity Entity = std::move(EntitiesStack.back());
    EntitiesStack.pop_back();
    if (EntitiesStack.empty())
      TopEntities.push_back(Entity);
    else
      EntitiesStack.back().SubEntities.push_back(Entity);
    return true;
  }

  bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                          TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef, Type Ty,
                          ReferenceMetaData Data) override {
    if (Data.isImplicit || !Range.isValid())
      return true;
    // Ignore things that don't come from this buffer.
    if (!SM.getRangeForBuffer(BufferID).contains(Range.getStart()))
      return false;

    unsigned StartOffset = getOffset(Range.getStart());
    References.emplace_back(D, StartOffset, Range.getByteLength(), Ty);
    return true;
  }

  bool visitSubscriptReference(ValueDecl *D, CharSourceRange Range,
                               ReferenceMetaData Data,
                               bool IsOpenBracket) override {
    // Treat both open and close brackets equally
    return visitDeclReference(D, Range, nullptr, nullptr, Type(), Data);
  }

  bool isLocal(Decl *D) const {
    return isa<ParamDecl>(D) || D->getDeclContext()->getLocalContext();
  }

  unsigned getOffset(SourceLoc Loc) const {
    return SM.getLocOffsetInBuffer(Loc, BufferID);
  }

  TextRange getTextRange(SourceRange R) const {
    unsigned Start = getOffset(R.Start);
    unsigned End = getOffset(R.End);
    return TextRange{ Start, End-Start };
  }
};
} // end anonymous namespace

static bool getSourceTextInfo(CompilerInstance &CI,
                              SourceTextInfo &Info) {
  SourceManager &SM = CI.getSourceMgr();
  unsigned BufID = CI.getInputBufferIDs().back();

  SourceDocASTWalker Walker(SM, BufID);
  Walker.walk(*CI.getMainModule());

  CharSourceRange FullRange = SM.getRangeForBuffer(BufID);
  Info.Text = SM.extractText(FullRange).str();
  Info.TopEntities = std::move(Walker.TopEntities);
  Info.References = std::move(Walker.References);
  return false;
}

static bool reportSourceDocInfo(CompilerInvocation Invocation,
                                llvm::MemoryBuffer *InputBuf,
                                DocInfoConsumer &Consumer) {
  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  EditorDiagConsumer DiagConsumer;
  CI.addDiagnosticConsumer(&DiagConsumer);
  Invocation.getFrontendOptions().InputsAndOutputs.addInput(
      InputFile(InputBuf->getBufferIdentifier(), false, InputBuf));
  std::string InstanceSetupError;
  if (CI.setup(Invocation, InstanceSetupError)) {
    Consumer.failed(InstanceSetupError);
    return true;
  }

  ASTContext &Ctx = CI.getASTContext();
  CloseClangModuleFiles scopedCloseFiles(*Ctx.getClangModuleLoader());
  CI.performSema();

  SourceTextInfo SourceInfo;
  if (getSourceTextInfo(CI, SourceInfo))
    return true;
  addParameterEntities(CI, SourceInfo);

  reportDocEntities(Ctx, SourceInfo.TopEntities, Consumer);
  reportSourceAnnotations(SourceInfo, CI, Consumer);

  auto BufferID = CI.getInputBufferIDs().back();
  Consumer.handleDiagnostics(DiagConsumer.getDiagnosticsForBuffer(BufferID));
  return false;
}

class RequestRefactoringEditConsumer::Implementation {
public:
  CategorizedEditsReceiver Receiver;
  std::vector<Edit> AllEdits;
  std::vector<std::pair<unsigned, unsigned>> StartEnds;
  std::vector<UIdent> UIds;
  SmallString<64> ErrBuffer;
  llvm::raw_svector_ostream OS;
  PrintingDiagnosticConsumer DiagConsumer;
  Implementation(CategorizedEditsReceiver Receiver):
    Receiver(std::move(Receiver)), OS(ErrBuffer), DiagConsumer(OS) {}
  ~Implementation() {
    if (DiagConsumer.didErrorOccur()) {
      Receiver(RequestResult<ArrayRef<CategorizedEdits>>::fromError(OS.str()));
      return;
    }
    assert(UIds.size() == StartEnds.size());
    std::vector<CategorizedEdits> Results;
    for (unsigned I = 0, N = UIds.size(); I < N; I ++) {
      auto Pair = StartEnds[I];
      Results.push_back({UIds[I],
                         llvm::makeArrayRef(AllEdits.data() + Pair.first,
                                             Pair.second - Pair.first)});
    }
    Receiver(RequestResult<ArrayRef<CategorizedEdits>>::fromResult(Results));
  }
  void accept(SourceManager &SM, RegionType RegionType,
              ArrayRef<Replacement> Replacements) {
    unsigned Start = AllEdits.size();
    llvm::transform(
        Replacements, std::back_inserter(AllEdits),
        [&](const Replacement &R) -> Edit {
          auto Start = SM.getLineAndColumnInBuffer(R.Range.getStart());
          auto End = SM.getLineAndColumnInBuffer(R.Range.getEnd());
          SmallVector<NoteRegion, 4> SubRanges;
          auto RawRanges = R.RegionsWorthNote;
          llvm::transform(
              RawRanges, std::back_inserter(SubRanges),
              [](swift::ide::NoteRegion R) -> SourceKit::NoteRegion {
                return {SwiftLangSupport::getUIDForRefactoringRangeKind(R.Kind),
                        R.StartLine,
                        R.StartColumn,
                        R.EndLine,
                        R.EndColumn,
                        R.ArgIndex};
              });
          return {R.Path.str(), Start.first,         Start.second,
                  End.first,    End.second,          R.BufferName.str(),
                  R.Text.str(), std::move(SubRanges)};
        });
    unsigned End = AllEdits.size();
    StartEnds.emplace_back(Start, End);
    UIds.push_back(SwiftLangSupport::getUIDForRegionType(RegionType));
  }
};

RequestRefactoringEditConsumer::
RequestRefactoringEditConsumer(CategorizedEditsReceiver Receiver) :
  Impl(*new Implementation(Receiver)) {}

RequestRefactoringEditConsumer::
~RequestRefactoringEditConsumer() { delete &Impl; }

void RequestRefactoringEditConsumer::
accept(SourceManager &SM, RegionType RegionType,
       ArrayRef<Replacement> Replacements) {
  Impl.accept(SM, RegionType, Replacements);
}

void RequestRefactoringEditConsumer::handleDiagnostic(
    SourceManager &SM, const DiagnosticInfo &Info) {
  Impl.DiagConsumer.handleDiagnostic(SM, Info);
}

class RequestRenameRangeConsumer::Implementation {
  CategorizedRenameRangesReceiver Receiver;
  std::string ErrBuffer;
  llvm::raw_string_ostream OS;
  std::vector<CategorizedRenameRanges> CategorizedRanges;

public:
  PrintingDiagnosticConsumer DiagConsumer;

public:
  Implementation(CategorizedRenameRangesReceiver Receiver)
      : Receiver(Receiver), OS(ErrBuffer), DiagConsumer(OS) {}

  ~Implementation() {
    if (DiagConsumer.didErrorOccur()) {
      Receiver(RequestResult<ArrayRef<CategorizedRenameRanges>>::fromError(OS.str()));
      return;
    }
    Receiver(RequestResult<ArrayRef<CategorizedRenameRanges>>::fromResult(CategorizedRanges));
  }

  void accept(SourceManager &SM, RegionType RegionType,
              ArrayRef<ide::RenameRangeDetail> Ranges) {
    CategorizedRenameRanges Results;
    Results.Category = SwiftLangSupport::getUIDForRegionType(RegionType);
    for (const auto &R : Ranges) {
      SourceKit::RenameRangeDetail Result;
      std::tie(Result.StartLine, Result.StartColumn) =
          SM.getLineAndColumnInBuffer(R.Range.getStart());
      std::tie(Result.EndLine, Result.EndColumn) =
          SM.getLineAndColumnInBuffer(R.Range.getEnd());
      Result.ArgIndex = R.Index;
      Result.Kind =
          SwiftLangSupport::getUIDForRefactoringRangeKind(R.RangeKind);
      Results.Ranges.push_back(std::move(Result));
    }
    CategorizedRanges.push_back(std::move(Results));
  }
};

RequestRenameRangeConsumer::RequestRenameRangeConsumer(
    CategorizedRenameRangesReceiver Receiver)
    : Impl(*new Implementation(Receiver)) {}
RequestRenameRangeConsumer::~RequestRenameRangeConsumer() { delete &Impl; }

void RequestRenameRangeConsumer::accept(
    SourceManager &SM, RegionType RegionType,
    ArrayRef<ide::RenameRangeDetail> Ranges) {
  Impl.accept(SM, RegionType, Ranges);
}

void RequestRenameRangeConsumer::handleDiagnostic(SourceManager &SM,
                                                  const DiagnosticInfo &Info) {
  Impl.DiagConsumer.handleDiagnostic(SM, Info);
}

static NameUsage getNameUsage(RenameType Type) {
  switch (Type) {
  case RenameType::Definition:
    return NameUsage::Definition;
  case RenameType::Reference:
    return NameUsage::Reference;
  case RenameType::Call:
    return NameUsage::Call;
  case RenameType::Unknown:
    return NameUsage::Unknown;
  }
}

static std::vector<RenameLoc>
getSyntacticRenameLocs(ArrayRef<RenameLocations> RenameLocations);

void SwiftLangSupport::
syntacticRename(llvm::MemoryBuffer *InputBuf,
                ArrayRef<RenameLocations> RenameLocations,
                ArrayRef<const char*> Args,
                CategorizedEditsReceiver Receiver) {
  std::string Error;
  CompilerInstance ParseCI;
  PrintingDiagnosticConsumer PrintDiags;
  ParseCI.addDiagnosticConsumer(&PrintDiags);
  SourceFile *SF = getSyntacticSourceFile(InputBuf, Args, ParseCI, Error);
  if (!SF) {
    Receiver(RequestResult<ArrayRef<CategorizedEdits>>::fromError(Error));
    return;
  }

  auto RenameLocs = getSyntacticRenameLocs(RenameLocations);
  RequestRefactoringEditConsumer EditConsumer(Receiver);
  swift::ide::syntacticRename(SF, RenameLocs, EditConsumer, EditConsumer);
}

void SwiftLangSupport::findRenameRanges(
    llvm::MemoryBuffer *InputBuf, ArrayRef<RenameLocations> RenameLocations,
    ArrayRef<const char *> Args, CategorizedRenameRangesReceiver Receiver) {
  std::string Error;
  CompilerInstance ParseCI;
  PrintingDiagnosticConsumer PrintDiags;
  ParseCI.addDiagnosticConsumer(&PrintDiags);
  SourceFile *SF = getSyntacticSourceFile(InputBuf, Args, ParseCI, Error);
  if (!SF) {
    Receiver(RequestResult<ArrayRef<CategorizedRenameRanges>>::fromError(Error));
    return;
  }

  auto RenameLocs = getSyntacticRenameLocs(RenameLocations);
  RequestRenameRangeConsumer Consumer(Receiver);
  swift::ide::findSyntacticRenameRanges(SF, RenameLocs, Consumer, Consumer);
}

void SwiftLangSupport::findLocalRenameRanges(
    StringRef Filename, unsigned Line, unsigned Column, unsigned Length,
    ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
    CategorizedRenameRangesReceiver Receiver) {
  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, Filename, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<ArrayRef<CategorizedRenameRanges>>::fromError(Error));
    return;
  }

  struct LocalRenameRangeASTConsumer : public SwiftASTConsumer {
    unsigned Line, Column, Length;
    CategorizedRenameRangesReceiver Receiver;

    LocalRenameRangeASTConsumer(unsigned Line, unsigned Column, unsigned Length,
                                CategorizedRenameRangesReceiver Receiver)
        : Line(Line), Column(Column), Length(Length),
          Receiver(std::move(Receiver)) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      auto &SF = AstUnit->getPrimarySourceFile();
      swift::ide::RangeConfig Range{*SF.getBufferID(), Line, Column, Length};
      RequestRenameRangeConsumer Consumer(std::move(Receiver));
      swift::ide::findLocalRenameRanges(&SF, Range, Consumer, Consumer);
    }

    void cancelled() override {
      Receiver(RequestResult<ArrayRef<CategorizedRenameRanges>>::cancelled());
    }

    void failed(StringRef Error) override {
      Receiver(RequestResult<ArrayRef<CategorizedRenameRanges>>::fromError(Error));
    }
  };

  auto ASTConsumer = std::make_shared<LocalRenameRangeASTConsumer>(
      Line, Column, Length, std::move(Receiver));
  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  getASTManager()->processASTAsync(Invok, ASTConsumer, &OncePerASTToken,
                                   CancellationToken,
                                   llvm::vfs::getRealFileSystem());
}

SourceFile *SwiftLangSupport::getSyntacticSourceFile(
    llvm::MemoryBuffer *InputBuf, ArrayRef<const char *> Args,
    CompilerInstance &ParseCI, std::string &Error) {
  CompilerInvocation Invocation;

  bool Failed = getASTManager()->initCompilerInvocationNoInputs(
      Invocation, Args, FrontendOptions::ActionType::Parse, ParseCI.getDiags(),
      Error);
  if (Failed) {
    Error = "Compiler invocation init failed";
    return nullptr;
  }
  Invocation.getFrontendOptions().InputsAndOutputs.addInput(
      InputFile(InputBuf->getBufferIdentifier(), /*isPrimary*/false, InputBuf,
                file_types::TY_Swift));

  if (ParseCI.setup(Invocation, Error)) {
    return nullptr;
  }

  SourceFile *SF = nullptr;
  unsigned BufferID = ParseCI.getInputBufferIDs().back();
  for (auto Unit : ParseCI.getMainModule()->getFiles()) {
    if (auto Current = dyn_cast<SourceFile>(Unit)) {
      if (Current->getBufferID().value() == BufferID) {
        SF = Current;
        break;
      }
    }
  }
  if (!SF)
    Error = "Failed to determine SourceFile for input buffer";
  return SF;
}

static std::vector<RenameLoc>
getSyntacticRenameLocs(ArrayRef<RenameLocations> RenameLocations) {
  std::vector<RenameLoc> RenameLocs;
  for(const auto &Locations: RenameLocations) {
    for(const auto &Location: Locations.LineColumnLocs) {
      RenameLocs.push_back({Location.Line, Location.Column,
        getNameUsage(Location.Type), Locations.OldName, Locations.NewName,
        Locations.IsFunctionLike, Locations.IsNonProtocolType});
    }
  }
  return RenameLocs;
}

void SwiftLangSupport::getDocInfo(llvm::MemoryBuffer *InputBuf,
                                  StringRef ModuleName,
                                  ArrayRef<const char *> Args,
                                  DocInfoConsumer &Consumer) {
  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  CompilerInvocation Invocation;
  std::string Error;
  bool Failed = getASTManager()->initCompilerInvocationNoInputs(
      Invocation, Args, FrontendOptions::ActionType::Typecheck, CI.getDiags(),
      Error, /*AllowInputs=*/false);

  if (Failed) {
    Consumer.failed(Error);
    return;
  }

  Invocation.getClangImporterOptions().ImportForwardDeclarations = true;

  if (!ModuleName.empty()) {
    bool Error = reportModuleDocInfo(Invocation, ModuleName, Consumer);
    if (Error)
      Consumer.failed("Error occurred");
    return;
  }

  Failed = reportSourceDocInfo(Invocation, InputBuf, Consumer);
  if (Failed)
    Consumer.failed("Error occurred");
}

void SwiftLangSupport::
findModuleGroups(StringRef ModuleName, ArrayRef<const char *> Args,
                 std::function<void(const RequestResult<ArrayRef<StringRef>> &)> Receiver) {
  CompilerInvocation Invocation;
  Invocation.getClangImporterOptions().ImportForwardDeclarations = true;
  Invocation.getFrontendOptions().InputsAndOutputs.clearInputs();

  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);
  std::string Error;
  if (getASTManager()->initCompilerInvocationNoInputs(
          Invocation, Args, FrontendOptions::ActionType::Typecheck,
          CI.getDiags(), Error)) {
    Receiver(RequestResult<ArrayRef<StringRef>>::fromError(Error));
    return;
  }
  if (CI.setup(Invocation, Error)) {
    Receiver(RequestResult<ArrayRef<StringRef>>::fromError(Error));
    return;
  }

  // Load standard library so that Clang importer can use it.
  ASTContext &Ctx = CI.getASTContext();
  auto *M = Ctx.getModuleByName(ModuleName);
  if (!M) {
    Error = "Cannot find the module.";
    Receiver(RequestResult<ArrayRef<StringRef>>::fromError(Error));
    return;
  }

  llvm::SmallVector<StringRef, 0> Groups;
  collectModuleGroups(M, Groups);
  Receiver(RequestResult<ArrayRef<StringRef>>::fromResult(Groups));
}
