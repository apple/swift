//===--- Index.cpp --------------------------------------------------------===//
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

#include "swift/Index/Index.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/Comment.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/Types.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/StringExtras.h"
#include "swift/IDE/SourceEntityWalker.h"
#include "swift/IDE/Utils.h"
#include "swift/Markup/Markup.h"
#include "swift/Sema/IDETypeChecking.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include <tuple>

using namespace swift;
using namespace swift::index;

static bool
printArtificialName(const swift::AbstractStorageDecl *ASD, AccessorKind AK, llvm::raw_ostream &OS) {
  switch (AK) {
  case AccessorKind::Get:
    OS << "getter:" << ASD->getName();
    return false;
  case AccessorKind::Set:
    OS << "setter:" << ASD->getName();
    return false;
  case AccessorKind::DidSet:
    OS << "didSet:" << ASD->getName();
    return false;
  case AccessorKind::WillSet:
    OS << "willSet:" << ASD->getName() ;
    return false;

  case AccessorKind::Address:
  case AccessorKind::MutableAddress:
  case AccessorKind::Read:
  case AccessorKind::Modify:
    return true;
  }

  llvm_unreachable("Unhandled AccessorKind in switch.");
}

static bool printDisplayName(const swift::ValueDecl *D, llvm::raw_ostream &OS) {
  if (!D->hasName() && !isa<ParamDecl>(D)) {
    auto *FD = dyn_cast<AccessorDecl>(D);
    if (!FD)
      return true;
    return printArtificialName(FD->getStorage(), FD->getAccessorKind(), OS);
  }

  OS << D->getName();
  return false;
}

static bool isMemberwiseInit(swift::ValueDecl *D) {
  if (auto AFD = dyn_cast<AbstractFunctionDecl>(D))
    return AFD->getBodyKind() == AbstractFunctionDecl::BodyKind::MemberwiseInitializer;
  return false;
}

static SourceLoc getLocForExtension(ExtensionDecl *D) {
  // Use the 'End' token of the range, in case it is a compound name, e.g.
  //   extension A.B {}
  // we want the location of 'B' token.
  if (auto *repr = D->getExtendedTypeRepr()) {
    return repr->getSourceRange().End;
  }
  return SourceLoc();
}

namespace {
// Adapter providing a common interface for a SourceFile/Module.
class SourceFileOrModule {
  llvm::PointerUnion<SourceFile *, ModuleDecl *> SFOrMod;

public:
  SourceFileOrModule(SourceFile &SF) : SFOrMod(&SF) {}
  SourceFileOrModule(ModuleDecl &Mod) : SFOrMod(&Mod) {}

  SourceFile *getAsSourceFile() const {
    return SFOrMod.dyn_cast<SourceFile *>();
  }

  ModuleDecl *getAsModule() const { return SFOrMod.dyn_cast<ModuleDecl *>(); }

  ModuleDecl &getModule() const {
    if (auto SF = SFOrMod.dyn_cast<SourceFile *>())
      return *SF->getParentModule();
    return *SFOrMod.get<ModuleDecl *>();
  }

  ArrayRef<FileUnit *> getFiles() const {
    return SFOrMod.is<SourceFile *>() ? *SFOrMod.getAddrOfPtr1()
                                      : SFOrMod.get<ModuleDecl *>()->getFiles();
  }

  StringRef getFilename() const {
    if (auto *SF = SFOrMod.dyn_cast<SourceFile *>())
      return SF->getFilename();
    return SFOrMod.get<ModuleDecl *>()->getModuleFilename();
  }

  void
  getImportedModules(SmallVectorImpl<ImportedModule> &Modules) const {
    constexpr ModuleDecl::ImportFilter ImportFilter = {
        ModuleDecl::ImportFilterKind::Exported,
        ModuleDecl::ImportFilterKind::Default,
        ModuleDecl::ImportFilterKind::ImplementationOnly};

    if (auto *SF = SFOrMod.dyn_cast<SourceFile *>()) {
      SF->getImportedModules(Modules, ImportFilter);
    } else {
      SFOrMod.get<ModuleDecl *>()->getImportedModules(Modules, ImportFilter);
    }
  }
};

struct IndexedWitness {
  ValueDecl *Member;
  ValueDecl *Requirement;
};

class IndexSwiftASTWalker : public SourceEntityWalker {
  IndexDataConsumer &IdxConsumer;
  SourceManager &SrcMgr;
  unsigned BufferID;
  bool enableWarnings;

  bool IsModuleFile = false;
  bool isSystemModule = false;
  struct Entity {
    Decl *D;
    SymbolInfo SymInfo;
    SymbolRoleSet Roles;
    SmallVector<IndexedWitness, 6> ExplicitWitnesses;
    SmallVector<SourceLoc, 6> RefsToSuppress;
  };
  SmallVector<Entity, 6> EntitiesStack;
  SmallVector<Expr *, 8> ExprStack;
  SmallVector<const AccessorDecl *, 4> ManuallyVisitedAccessorStack;
  bool Cancelled = false;

  struct NameAndUSR {
    StringRef USR;
    StringRef name;
  };
  typedef llvm::PointerIntPair<Decl *, 3> DeclAccessorPair;
  llvm::DenseMap<void *, NameAndUSR> nameAndUSRCache;
  llvm::DenseMap<DeclAccessorPair, NameAndUSR> accessorNameAndUSRCache;
  StringScratchSpace stringStorage;

  bool getNameAndUSR(ValueDecl *D, ExtensionDecl *ExtD,
                     StringRef &name, StringRef &USR) {
    auto &result = nameAndUSRCache[ExtD ? (Decl*)ExtD : D];
    if (result.USR.empty()) {
      SmallString<128> storage;
      {
        llvm::raw_svector_ostream OS(storage);
        if (ExtD) {
          if (ide::printExtensionUSR(ExtD, OS))
            return true;
        } else {
          if (ide::printValueDeclUSR(D, OS))
            return true;
        }
        result.USR = stringStorage.copyString(OS.str());
      }

      storage.clear();
      {
        llvm::raw_svector_ostream OS(storage);
        printDisplayName(D, OS);
        result.name = stringStorage.copyString(OS.str());
      }
    }

    name = result.name;
    USR = result.USR;
    return false;
  }

  bool getModuleNameAndUSR(ModuleEntity Mod, StringRef &name, StringRef &USR) {
    auto &result = nameAndUSRCache[Mod.getOpaqueValue()];
    if (result.USR.empty()) {
      SmallString<128> storage;
      {
        llvm::raw_svector_ostream OS(storage);
        if (ide::printModuleUSR(Mod, OS))
          return true;
        result.USR = stringStorage.copyString(OS.str());
      }
      storage.clear();
      {
        llvm::raw_svector_ostream OS(storage);
        OS << Mod.getFullName();
        result.name = stringStorage.copyString(OS.str());
      }
    }
    name = result.name;
    USR = result.USR;
    return false;
  }

  bool getPseudoAccessorNameAndUSR(AbstractStorageDecl *D, AccessorKind AK, StringRef &Name, StringRef &USR) {
    assert(static_cast<int>(AK) < 0x111 && "AccessorKind too big for pair");
    DeclAccessorPair key(D, static_cast<int>(AK));
    auto &result = accessorNameAndUSRCache[key];
    if (result.USR.empty()) {
      SmallString<128> storage;
      {
        llvm::raw_svector_ostream OS(storage);
        if (ide::printAccessorUSR(D, AK, OS))
          return true;
        result.USR = stringStorage.copyString(OS.str());
      }

      storage.clear();
      {
        llvm::raw_svector_ostream OS(storage);
        printArtificialName(D, AK, OS);
        result.name = stringStorage.copyString(OS.str());
      }
    }

    Name = result.name;
    USR = result.USR;
    return false;
  }

  bool addRelation(IndexSymbol &Info, SymbolRoleSet RelationRoles, Decl *D) {
    assert(D);
    auto Match = std::find_if(Info.Relations.begin(), Info.Relations.end(),
                              [D](IndexRelation R) { return R.decl == D; });
    if (Match != Info.Relations.end()) {
      Match->roles |= RelationRoles;
      Info.roles |= RelationRoles;
      return false;
    }

    StringRef Name, USR;
    SymbolInfo SymInfo = getSymbolInfoForDecl(D);

    if (SymInfo.Kind == SymbolKind::Unknown)
      return true;
    if (auto *ExtD = dyn_cast<ExtensionDecl>(D)) {
      NominalTypeDecl *NTD = ExtD->getExtendedNominal();
      if (getNameAndUSR(NTD, ExtD, Name, USR))
        return true;
    } else {
      if (getNameAndUSR(cast<ValueDecl>(D), /*ExtD=*/nullptr, Name, USR))
        return true;
    }

    Info.Relations.push_back(IndexRelation(RelationRoles, D, SymInfo, Name, USR));
    Info.roles |= RelationRoles;
    return false;
  }

public:
  IndexSwiftASTWalker(IndexDataConsumer &IdxConsumer, ASTContext &Ctx,
                      SourceFile *SF = nullptr)
      : IdxConsumer(IdxConsumer), SrcMgr(Ctx.SourceMgr),
        BufferID(SF ? SF->getBufferID().getValueOr(-1) : -1),
        enableWarnings(IdxConsumer.enableWarnings()) {}

  ~IndexSwiftASTWalker() override {
    assert(Cancelled || EntitiesStack.empty());
    assert(Cancelled || ManuallyVisitedAccessorStack.empty());
  }

  void visitModule(ModuleDecl &Mod);
  void visitDeclContext(DeclContext *DC);

private:
  bool visitImports(SourceFileOrModule Mod,
                    llvm::SmallPtrSetImpl<ModuleDecl *> &Visited);

  bool handleSourceOrModuleFile(SourceFileOrModule SFOrMod);

  bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
    // Do not handle unavailable decls from other modules.
    if (IsModuleFile && AvailableAttr::isUnavailable(D))
      return false;

    if (!handleCustomAttrInitRefs(D))
      return false;

    if (auto *AD = dyn_cast<AccessorDecl>(D)) {
      if (ManuallyVisitedAccessorStack.empty() ||
          ManuallyVisitedAccessorStack.back() != AD)
        return false; // already handled as part of the var decl.
    }
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (!report(VD))
        return false;
    }
    if (auto *ED = dyn_cast<ExtensionDecl>(D))
      return reportExtension(ED);
    return true;
  }

  bool walkToDeclPost(Decl *D) override {
    if (Cancelled)
      return false;

    if (getParentDecl() == D)
      return finishCurrentEntity();

    return true;
  }

  /// Report calls to the initializers of property wrapper types on wrapped properties.
  ///
  /// These may be either explicit:
  ///     `\@Wrapper(initialialValue: 42) var x: Int`
  /// or implicit:
  ///     `\@Wrapper var x = 10`
  bool handleCustomAttrInitRefs(Decl * D) {
    for (auto *customAttr : D->getAttrs().getAttributes<CustomAttr, true>()) {
      if (customAttr->isImplicit())
        continue;

      if (auto *semanticInit = dyn_cast_or_null<CallExpr>(customAttr->getSemanticInit())) {
        if (auto *CD = semanticInit->getCalledValue()) {
          if (!shouldIndex(CD, /*IsRef*/true))
            continue;
          IndexSymbol Info;
          const auto reprLoc = customAttr->getTypeRepr()->getLoc();
          if (initIndexSymbol(CD, reprLoc, /*IsRef=*/true, Info))
            continue;
          Info.roles |= (unsigned)SymbolRole::Call;
          if (semanticInit->isImplicit())
            Info.roles |= (unsigned)SymbolRole::Implicit;
          if (!startEntity(CD, Info, /*IsRef=*/true) || !finishCurrentEntity())
            return false;
        }
      }
    }
    return true;
  }

  void handleMemberwiseInitRefs(Expr *E) {
    if (!isa<ApplyExpr>(E))
      return;

    auto *DeclRef = dyn_cast<DeclRefExpr>(cast<ApplyExpr>(E)->getFn());
    if (!DeclRef || !isMemberwiseInit(DeclRef->getDecl()))
      return;

    // get label locations
    auto *MemberwiseInit = DeclRef->getDecl();
    std::vector<SourceLoc> LabelLocs;
    ArrayRef<Identifier> Labels;
    auto NameLoc = DeclRef->getNameLoc();
    if (NameLoc.isCompound()) {
      size_t LabelIndex = 0;
      SourceLoc ArgLoc;
      while ((ArgLoc = NameLoc.getArgumentLabelLoc(LabelIndex++)).isValid()) {
        LabelLocs.push_back(ArgLoc);
      }
      Labels = MemberwiseInit->getName().getArgumentNames();
    } else if (auto *CallParent = dyn_cast_or_null<CallExpr>(getParentExpr())) {
      LabelLocs = CallParent->getArgumentLabelLocs();
      Labels = CallParent->getArgumentLabels();
    }

    if (LabelLocs.empty())
      return;

    assert(Labels.size() == LabelLocs.size());

    // match labels to properties
    auto *TypeContext =
        MemberwiseInit->getDeclContext()->getSelfNominalTypeDecl();
    if (!TypeContext || !shouldIndex(TypeContext, false))
      return;

    unsigned CurLabel = 0;
    for (auto Member : TypeContext->getMembers()) {
      auto Prop = dyn_cast<VarDecl>(Member);
      if (!Prop)
        continue;

      if (!Prop->isMemberwiseInitialized(/*preferDeclaredProperties=*/true))
        continue;

      if (CurLabel == LabelLocs.size())
        break;

      if (Labels[CurLabel] != Prop->getName())
        continue;

      IndexSymbol Info;
      if (initIndexSymbol(Prop, LabelLocs[CurLabel++], /*IsRef=*/true, Info))
        continue;
      if (startEntity(Prop, Info, /*IsRef=*/true))
        finishCurrentEntity();
    }
  }

  bool walkToExprPre(Expr *E) override {
    if (Cancelled)
      return false;
    ExprStack.push_back(E);

    handleMemberwiseInitRefs(E);
    return true;
  }

  bool walkToExprPost(Expr *E) override {
    if (Cancelled)
      return false;
    assert(ExprStack.back() == E);
    ExprStack.pop_back();
    return true;
  }

  /// Extensions redeclare all generic parameters of their extended type to add
  /// their additional restrictions. There are two issues with this model for
  /// indexing:
  ///  - The generic paramter declarations of the extension are implicit so we
  ///    wouldn't report them in the index. Any usage of the generic param in
  ///    the extension references this implicit declaration so we don't include
  ///    it in the index either.
  ///  - The implicit re-declarations have their own USRs so any usage of a
  ///    generic parameter inside an extension would use a different USR than
  ///    declaration of the param in the extended type.
  ///
  /// To fix these issues, we replace the reference to the implicit generic
  /// parameter defined in the extension by a reference to the generic paramter
  /// defined in the extended type.
  ///
  /// \returns the canonicalized replaced generic param decl if it can be found
  ///          or \p GenParam otherwise.
  ValueDecl *
  canonicalizeGenericTypeParamDeclForIndex(GenericTypeParamDecl *GenParam) {
    auto Extension = dyn_cast_or_null<ExtensionDecl>(
        GenParam->getDeclContext()->getAsDecl());
    if (!Extension) {
      // We are not referencing a generic paramter defined in an extension.
      // Nothing to do.
      return GenParam;
    }
    assert(GenParam->isImplicit() &&
           "Generic param decls in extension should always be implicit and "
           "shadow a generic param in the extended type.");
    assert(Extension->getExtendedNominal() &&
           "The implict generic types on the extension should only be created "
           "if the extended type was found");

    auto ExtendedTypeGenSig =
        Extension->getExtendedNominal()->getGenericSignature();
    assert(ExtendedTypeGenSig && "Extension is generic but extended type not?");

    // The generic parameter in the extension has the same depths and index
    // as the one in the extended type.
    for (auto ExtendedTypeGenParam : ExtendedTypeGenSig->getGenericParams()) {
      if (ExtendedTypeGenParam->getIndex() == GenParam->getIndex() &&
          ExtendedTypeGenParam->getDepth() == GenParam->getDepth()) {
        assert(ExtendedTypeGenParam->getDecl() &&
               "The generic parameter defined on the extended type cannot be "
               "implicit.");
        return ExtendedTypeGenParam->getDecl();
      }
    }
    llvm_unreachable("Can't find the generic parameter in the extended type");
  }

  bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                          TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef, Type T,
                          ReferenceMetaData Data) override {
    SourceLoc Loc = Range.getStart();

    if (isRepressed(Loc) || Loc.isInvalid())
      return true;

    IndexSymbol Info;

    if (Data.isImplicit)
      Info.roles |= (unsigned)SymbolRole::Implicit;

    if (CtorTyRef)
      if (!reportRef(CtorTyRef, Loc, Info, Data.AccKind))
        return false;

    if (auto *GenParam = dyn_cast<GenericTypeParamDecl>(D)) {
      D = canonicalizeGenericTypeParamDeclForIndex(GenParam);
    }

    if (!reportRef(D, Loc, Info, Data.AccKind))
      return false;

    // If this is a reference to a property wrapper backing property or
    // projected value, report a reference to the wrapped property too (i.e.
    // report an occurrence of `foo` in `_foo` and '$foo').
    if (auto *VD = dyn_cast<VarDecl>(D)) {
      if (auto *Wrapped = VD->getOriginalWrappedProperty()) {
        assert(Range.getByteLength() > 1 &&
               (Range.str().front() == '_' || Range.str().front() == '$'));
        auto AfterDollar = Loc.getAdvancedLoc(1);
        reportRef(Wrapped, AfterDollar, Info, None);
      }
    }

    return true;
  }

  bool visitModuleReference(ModuleEntity Mod, CharSourceRange Range) override {
    SourceLoc Loc = Range.getStart();

    if (Loc.isInvalid())
      return true;

    IndexSymbol Info;
    std::tie(Info.line, Info.column, Info.offset) = getLineColAndOffset(Loc);
    Info.roles |= (unsigned)SymbolRole::Reference;
    Info.symInfo = getSymbolInfoForModule(Mod);
    getModuleNameAndUSR(Mod, Info.name, Info.USR);

    if (auto Container = getContainingDecl())
      addRelation(Info, (unsigned)SymbolRole::RelationContainedBy, Container);

    if (!IdxConsumer.startSourceEntity(Info)) {
      Cancelled = true;
      return true;
    }

    return finishSourceEntity(Info.symInfo, Info.roles);
  }

  bool visitCallAsFunctionReference(ValueDecl *D, CharSourceRange Range,
                                    ReferenceMetaData Data) override {
    // Index implicit callAsFunction reference.
    return visitDeclReference(D, Range, /*CtorTyRef*/ nullptr,
                              /*ExtTyRef*/ nullptr, Type(), Data);
  }

  Decl *getParentDecl() const {
    if (!EntitiesStack.empty())
      return EntitiesStack.back().D;
    return nullptr;
  }

  Decl *getContainingDecl() const {
    for (const auto &Entity: EntitiesStack) {
      if (isa<AbstractFunctionDecl>(Entity.D) &&
          (Entity.Roles & (SymbolRoleSet)SymbolRole::Definition)) {
        return Entity.D;
      }
    }
    return nullptr;
  }

  void repressRefAtLoc(SourceLoc Loc) {
    if (Loc.isInvalid()) return;
    assert(!EntitiesStack.empty());
    EntitiesStack.back().RefsToSuppress.push_back(Loc);
  }

  bool isRepressed(SourceLoc Loc) const {
    if (EntitiesStack.empty() || Loc.isInvalid())
      return false;
    auto &Suppressed = EntitiesStack.back().RefsToSuppress;
    return std::find(Suppressed.begin(), Suppressed.end(), Loc) != Suppressed.end();

  }

  Expr *getContainingExpr(size_t index) const {
    if (ExprStack.size() > index)
      return ExprStack.end()[-std::ptrdiff_t(index + 1)];
    return nullptr;
  }

  Expr *getCurrentExpr() const {
    return ExprStack.empty() ? nullptr : ExprStack.back();
  }

  Expr *getParentExpr() const {
    return getContainingExpr(1);
  }


  bool report(ValueDecl *D);
  bool reportExtension(ExtensionDecl *D);
  bool reportRef(ValueDecl *D, SourceLoc Loc, IndexSymbol &Info,
                 Optional<AccessKind> AccKind);
  bool reportImplicitConformance(ValueDecl *witness, ValueDecl *requirement,
                                 Decl *container);

  bool startEntity(Decl *D, IndexSymbol &Info, bool IsRef);
  bool startEntityDecl(ValueDecl *D);

  bool reportRelatedRef(ValueDecl *D, SourceLoc Loc, bool isImplicit, SymbolRoleSet Relations, Decl *Related);
  bool reportRelatedTypeRef(const TypeLoc &Ty, SymbolRoleSet Relations, Decl *Related);
  bool reportInheritedTypeRefs(
      ArrayRef<InheritedEntry> Inherited, Decl *Inheritee);
  NominalTypeDecl *getTypeLocAsNominalTypeDecl(const TypeLoc &Ty);

  bool reportPseudoGetterDecl(VarDecl *D) {
    return reportPseudoAccessor(D, AccessorKind::Get, /*IsRef=*/false,
                                D->getLoc());
  }
  bool reportPseudoSetterDecl(VarDecl *D) {
    return reportPseudoAccessor(D, AccessorKind::Set, /*IsRef=*/false,
                                D->getLoc());
  }
  bool reportPseudoAccessor(AbstractStorageDecl *D, AccessorKind AccKind,
                            bool IsRef, SourceLoc Loc);

  bool finishCurrentEntity() {
    Entity CurrEnt = EntitiesStack.pop_back_val();
    assert(CurrEnt.SymInfo.Kind != SymbolKind::Unknown);
    return finishSourceEntity(CurrEnt.SymInfo, CurrEnt.Roles);
  }

  bool finishSourceEntity(SymbolInfo symInfo, SymbolRoleSet roles) {
    if (!IdxConsumer.finishSourceEntity(symInfo, roles)) {
      Cancelled = true;
      return false;
    }
    return true;
  }

  bool initIndexSymbol(ValueDecl *D, SourceLoc Loc, bool IsRef,
                       IndexSymbol &Info);
  bool initIndexSymbol(ExtensionDecl *D, ValueDecl *ExtendedD, SourceLoc Loc,
                       IndexSymbol &Info);
  bool initFuncDeclIndexSymbol(FuncDecl *D, IndexSymbol &Info);
  bool initFuncRefIndexSymbol(ValueDecl *D, SourceLoc Loc, IndexSymbol &Info);
  bool initVarRefIndexSymbols(Expr *CurrentE, ValueDecl *D, SourceLoc Loc,
                              IndexSymbol &Info, Optional<AccessKind> AccKind);

  bool indexComment(const Decl *D);

  std::tuple<unsigned, unsigned, Optional<unsigned>>
  getLineColAndOffset(SourceLoc Loc) {
    if (Loc.isInvalid())
      return std::make_tuple(0, 0, None);
    auto lineAndColumn = SrcMgr.getLineAndColumnInBuffer(Loc, BufferID);
    unsigned offset = SrcMgr.getLocOffsetInBuffer(Loc, BufferID);
    return std::make_tuple(lineAndColumn.first, lineAndColumn.second, offset);
  }

  bool shouldIndex(ValueDecl *D, bool IsRef) const {
    if (D->isImplicit() && isa<VarDecl>(D) && IsRef) {
      // Bypass the implicit VarDecls introduced in CaseStmt bodies by using the
      // canonical VarDecl for these checks instead.
      D = cast<VarDecl>(D)->getCanonicalVarDecl();
    }

    if (D->isImplicit() && !isa<ConstructorDecl>(D))
      return false;

    // Do not handle non-public imported decls.
    if (IsModuleFile && !D->isAccessibleFrom(nullptr))
      return false;

    if (!IdxConsumer.indexLocals() && isLocalSymbol(D))
      return isa<ParamDecl>(D) && !IsRef &&
        D->getDeclContext()->getContextKind() != DeclContextKind::AbstractClosureExpr;

    if (D->isPrivateStdlibDecl())
      return false;

    return true;
  }

  // Are there members or conformances in \c D that should be indexed?
  bool shouldIndexMembers(ExtensionDecl *D) {
    for (auto Member : D->getMembers())
      if (auto VD = dyn_cast<ValueDecl>(Member))
        if (shouldIndex(VD, /*IsRef=*/false))
          return true;

    for (auto Inherit : D->getInherited())
      if (auto T = Inherit.getType())
        if (T->getAnyNominal() &&
            shouldIndex(T->getAnyNominal(), /*IsRef=*/false))
          return true;

    return false;
  }

  /// Reports all implicit member value decl conformances that \p D introduces
  /// as implicit overrides at the source location of \p D, and returns the
  /// explicit ones so we can check against them later on when visiting them as
  /// members.
  ///
  /// \returns false if AST visitation should stop.
  bool handleWitnesses(Decl *D, SmallVectorImpl<IndexedWitness> &explicitWitnesses);

  void getRecursiveModuleImports(ModuleDecl &Mod,
                                 SmallVectorImpl<ModuleDecl *> &Imports);
  void
  collectRecursiveModuleImports(ModuleDecl &Mod,
                                llvm::SmallPtrSetImpl<ModuleDecl *> &Visited);

  template <typename F>
  void warn(F log) {
    if (!enableWarnings)
      return;

    SmallString<128> warning;
    llvm::raw_svector_ostream OS(warning);
    log(OS);
  }

  // This maps a module to all its imports, recursively.
  llvm::DenseMap<ModuleDecl *, llvm::SmallVector<ModuleDecl *, 4>> ImportsMap;
};
} // anonymous namespace

void IndexSwiftASTWalker::visitDeclContext(DeclContext *Context) {
  IsModuleFile = false;
  isSystemModule = Context->getParentModule()->isSystemModule();
  auto accessor = dyn_cast<AccessorDecl>(Context);
  if (accessor)
    ManuallyVisitedAccessorStack.push_back(accessor);
  walk(Context);
  if (accessor)
    ManuallyVisitedAccessorStack.pop_back();
}

void IndexSwiftASTWalker::visitModule(ModuleDecl &Mod) {
  SourceFile *SrcFile = nullptr;
  for (auto File : Mod.getFiles()) {
    if (auto SF = dyn_cast<SourceFile>(File)) {
      auto BufID = SF->getBufferID();
      if (BufID.hasValue() && *BufID == BufferID) {
        SrcFile = SF;
        break;
      }
    }
  }

  if (SrcFile != nullptr) {
    IsModuleFile = false;
    if (!handleSourceOrModuleFile(*SrcFile))
      return;
    walk(*SrcFile);
  } else {
    IsModuleFile = true;
    isSystemModule = Mod.isSystemModule();
    if (!handleSourceOrModuleFile(Mod))
      return;
    walk(Mod);
  }
}

bool IndexSwiftASTWalker::handleSourceOrModuleFile(SourceFileOrModule SFOrMod) {
  // Common reporting for TU/module file.

  llvm::SmallPtrSet<ModuleDecl *, 16> Visited;
  return visitImports(SFOrMod, Visited);
}

bool IndexSwiftASTWalker::visitImports(
    SourceFileOrModule TopMod, llvm::SmallPtrSetImpl<ModuleDecl *> &Visited) {
  // Dependencies of the stdlib module (like SwiftShims module) are
  // implementation details.
  if (TopMod.getModule().isStdlibModule())
    return true;

  bool IsNew = Visited.insert(&TopMod.getModule()).second;
  if (!IsNew)
    return true;

  SmallVector<ImportedModule, 8> Imports;
  TopMod.getImportedModules(Imports);

  llvm::SmallPtrSet<ModuleDecl *, 8> Reported;
  for (auto Import : Imports) {
    ModuleDecl *Mod = Import.importedModule;
    bool NewReport = Reported.insert(Mod).second;
    if (!NewReport)
      continue;

    // FIXME: Handle modules with multiple source files; these will fail on
    // getModuleFilename() (by returning an empty path). Note that such modules
    // may be heterogeneous.
    StringRef Path = Mod->getModuleFilename();
    if (Path.empty() || Path == TopMod.getFilename())
      continue; // this is a submodule.

    Optional<bool> IsClangModuleOpt;
    for (auto File : Mod->getFiles()) {
      switch (File->getKind()) {
      case FileUnitKind::Source:
      case FileUnitKind::Builtin:
      case FileUnitKind::Synthesized:
        break;
      case FileUnitKind::SerializedAST:
        assert(!IsClangModuleOpt.hasValue() &&
               "cannot handle multi-file modules");
        IsClangModuleOpt = false;
        break;
      case FileUnitKind::ClangModule:
      case FileUnitKind::DWARFModule:
        assert(!IsClangModuleOpt.hasValue() &&
               "cannot handle multi-file modules");
        IsClangModuleOpt = true;
        break;
      }
    }
    if (!IsClangModuleOpt.hasValue())
      continue;
    bool IsClangModule = *IsClangModuleOpt;

    StringRef ModuleName = Mod->getNameStr();

    // If this module is an underscored cross-import overlay, use the name
    // of the underlying module that declared it instead.
    if (ModuleDecl *Declaring = Mod->getDeclaringModuleIfCrossImportOverlay())
      ModuleName = Declaring->getNameStr();

    if (!IdxConsumer.startDependency(ModuleName, Path, IsClangModule,
                                     Mod->isSystemModule()))
      return false;
    if (!IsClangModule)
      if (!visitImports(*Mod, Visited))
        return false;
    if (!IdxConsumer.finishDependency(IsClangModule))
      return false;
  }

  return true;
}

bool IndexSwiftASTWalker::handleWitnesses(Decl *D, SmallVectorImpl<IndexedWitness> &explicitWitnesses) {
  const auto *const IDC = dyn_cast<IterableDeclContext>(D);
  if (!IDC)
    return true;

  const auto DC = IDC->getAsGenericContext();
  for (auto *conf : IDC->getLocalConformances()) {
    if (conf->isInvalid())
      continue;

    // Ignore self-conformances; they're not interesting to show to users.
    auto normal = dyn_cast<NormalProtocolConformance>(conf->getRootConformance());
    if (!normal)
      continue;

    normal->forEachValueWitness([&](ValueDecl *req, Witness witness) {
      if (Cancelled)
        return;

      auto *decl = witness.getDecl();
      if (decl == nullptr)
        return;

      if (decl->getDeclContext() == DC) {
        explicitWitnesses.push_back({decl, req});
      } else {
        reportImplicitConformance(decl, req, D);
      }
    });

    normal->forEachTypeWitness(
                [&](AssociatedTypeDecl *assoc, Type type, TypeDecl *typeDecl) {
      if (Cancelled)
        return true;
      if (typeDecl == nullptr)
        return false;

      if (typeDecl->getDeclContext() == DC) {
        explicitWitnesses.push_back({typeDecl, assoc});
      } else {
        // Report the implicit conformance.
        reportImplicitConformance(typeDecl, assoc, D);
      }
      return false;
    });
  }

  if (Cancelled)
    return false;

  return true;
}

bool IndexSwiftASTWalker::startEntity(Decl *D, IndexSymbol &Info, bool IsRef) {
  switch (IdxConsumer.startSourceEntity(Info)) {
    case swift::index::IndexDataConsumer::Abort:
      Cancelled = true;
      LLVM_FALLTHROUGH;
    case swift::index::IndexDataConsumer::Skip:
      return false;
    case swift::index::IndexDataConsumer::Continue: {
      SmallVector<IndexedWitness, 6> explicitWitnesses;
      if (!IsRef) {
        if (!handleWitnesses(D, explicitWitnesses))
          return false;
      }
      EntitiesStack.push_back({D, Info.symInfo, Info.roles, std::move(explicitWitnesses), {}});
      return true;
    }
  }

  llvm_unreachable("Unhandled IndexDataConsumer in switch.");
}

bool IndexSwiftASTWalker::startEntityDecl(ValueDecl *D) {
  if (!shouldIndex(D, /*IsRef=*/false))
    return false;

  SourceLoc Loc = D->getLoc(/*SerializedOK*/false);
  if (Loc.isInvalid() && !IsModuleFile)
    return false;

  if (!IsModuleFile) {
    if (!indexComment(D))
      return false;
  }

  IndexSymbol Info;
  if (auto FD = dyn_cast<FuncDecl>(D)) {
    if (initFuncDeclIndexSymbol(FD, Info))
      return false;
  } else {
    if (initIndexSymbol(D, Loc, /*IsRef=*/false, Info))
      return false;
  }

  for (auto Overriden: collectAllOverriddenDecls(D, /*IncludeProtocolReqs=*/false)) {
    addRelation(Info, (SymbolRoleSet) SymbolRole::RelationOverrideOf, Overriden);
  }

  if (auto Parent = getParentDecl()) {
    for (const IndexedWitness &witness : EntitiesStack.back().ExplicitWitnesses) {
      if (witness.Member == D)
        addRelation(Info, (SymbolRoleSet) SymbolRole::RelationOverrideOf, witness.Requirement);
    }
    if (auto ParentVD = dyn_cast<ValueDecl>(Parent)) {
      SymbolRoleSet RelationsToParent = (SymbolRoleSet)SymbolRole::RelationChildOf;
      if (Info.symInfo.SubKind == SymbolSubKind::AccessorGetter ||
          Info.symInfo.SubKind == SymbolSubKind::AccessorSetter ||
          (Info.symInfo.SubKind >= SymbolSubKind::SwiftAccessorWillSet &&
           Info.symInfo.SubKind <= SymbolSubKind::SwiftAccessorMutableAddressor))
        RelationsToParent |= (SymbolRoleSet)SymbolRole::RelationAccessorOf;
      if (addRelation(Info, RelationsToParent, ParentVD))
        return false;

    } else if (auto ParentED = dyn_cast<ExtensionDecl>(Parent)) {
      if (ParentED->getExtendedNominal()) {
        if (addRelation(Info, (SymbolRoleSet) SymbolRole::RelationChildOf, ParentED))
          return false;
      }
    }
  }

  return startEntity(D, Info, /*IsRef=*/false);
}

bool IndexSwiftASTWalker::reportRelatedRef(ValueDecl *D, SourceLoc Loc, bool isImplicit,
                                           SymbolRoleSet Relations, Decl *Related) {
  if (!shouldIndex(D, /*IsRef=*/true))
    return true;

  IndexSymbol Info;
  if (addRelation(Info, Relations, Related))
    return true;
  if (isImplicit)
    Info.roles |= (unsigned)SymbolRole::Implicit;

  // don't report this ref again when visitDeclReference reports it
  repressRefAtLoc(Loc);

  if (!reportRef(D, Loc, Info, None)) {
    Cancelled = true;
    return false;
  }

  return !Cancelled;
}

bool IndexSwiftASTWalker::reportInheritedTypeRefs(ArrayRef<InheritedEntry> Inherited, Decl *Inheritee) {
  for (auto Base : Inherited) {
    if (!reportRelatedTypeRef(Base, (SymbolRoleSet) SymbolRole::RelationBaseOf, Inheritee))
      return false;
  }
  return true;
}

bool IndexSwiftASTWalker::reportRelatedTypeRef(const TypeLoc &Ty, SymbolRoleSet Relations, Decl *Related) {

  if (auto *T = dyn_cast_or_null<IdentTypeRepr>(Ty.getTypeRepr())) {
    auto Comps = T->getComponentRange();
    SourceLoc IdLoc = Comps.back()->getLoc();
    NominalTypeDecl *NTD = nullptr;
    bool isImplicit = false;
    if (auto *VD = Comps.back()->getBoundDecl()) {
      if (auto *TAD = dyn_cast<TypeAliasDecl>(VD)) {
        IndexSymbol Info;
        if (!reportRef(TAD, IdLoc, Info, None))
          return false;
        if (auto Ty = TAD->getUnderlyingType()) {
          NTD = Ty->getAnyNominal();
          isImplicit = true;
        }
      } else {
        NTD = dyn_cast<NominalTypeDecl>(VD);
      }
    }
    if (NTD) {
      if (!reportRelatedRef(NTD, IdLoc, isImplicit, Relations, Related))
        return false;
    }
    return true;
  }

  if (Ty.getType()) {
    if (auto nominal = Ty.getType()->getAnyNominal())
      if (!reportRelatedRef(nominal, Ty.getLoc(), /*isImplicit=*/false, Relations, Related))
        return false;
  }
  return true;
}

static bool isDynamicVarAccessorOrFunc(ValueDecl *D, SymbolInfo symInfo) {
  if (auto NTD = D->getDeclContext()->getSelfNominalTypeDecl()) {
    bool isClassOrProtocol = isa<ClassDecl>(NTD) || isa<ProtocolDecl>(NTD);
    bool isInternalAccessor =
      symInfo.SubKind == SymbolSubKind::SwiftAccessorWillSet ||
      symInfo.SubKind == SymbolSubKind::SwiftAccessorDidSet ||
      symInfo.SubKind == SymbolSubKind::SwiftAccessorAddressor ||
      symInfo.SubKind == SymbolSubKind::SwiftAccessorMutableAddressor;
    if (isClassOrProtocol &&
        symInfo.Kind != SymbolKind::StaticMethod &&
        !isInternalAccessor &&
        !D->isFinal()) {
      return true;
    }
  }
  return false;
}

bool IndexSwiftASTWalker::reportPseudoAccessor(AbstractStorageDecl *D,
                                               AccessorKind AccKind, bool IsRef,
                                               SourceLoc Loc) {
  if (!shouldIndex(D, IsRef))
    return true; // continue walking.

  auto updateInfo = [this, D, AccKind](IndexSymbol &Info) {
    if (getPseudoAccessorNameAndUSR(D, AccKind, Info.name, Info.USR))
      return true;
    Info.symInfo.Kind = SymbolKind::Function;
    if (D->getDeclContext()->isTypeContext()) {
      if (D->isStatic()) {
        if (D->getCorrectStaticSpelling() == StaticSpellingKind::KeywordClass)
          Info.symInfo.Kind = SymbolKind::ClassMethod;
        else
          Info.symInfo.Kind = SymbolKind::StaticMethod;
      } else {
        Info.symInfo.Kind = SymbolKind::InstanceMethod;
      }
    }
    Info.symInfo.SubKind = getSubKindForAccessor(AccKind);
    Info.roles |= (SymbolRoleSet)SymbolRole::Implicit;
    Info.group = "";
    if (isDynamicVarAccessorOrFunc(D, Info.symInfo)) {
      Info.roles |= (SymbolRoleSet)SymbolRole::Dynamic;
    }
    return false;
  };

  if (IsRef) {
    IndexSymbol Info;

    // initFuncRefIndexSymbol uses the top of the entities stack as the caller,
    // but in this case the top of the stack is the referenced
    // AbstractStorageDecl.
    assert(getParentDecl() == D);
    auto PreviousTop = EntitiesStack.pop_back_val();
    bool initFailed = initFuncRefIndexSymbol(D, Loc, Info);
    EntitiesStack.push_back(PreviousTop);

    if (initFailed)
      return true; // continue walking.
    if (updateInfo(Info))
      return true;

    if (!IdxConsumer.startSourceEntity(Info) || !IdxConsumer.finishSourceEntity(Info.symInfo, Info.roles))
      Cancelled = true;
  } else {
    IndexSymbol Info;
    if (initIndexSymbol(D, Loc, IsRef, Info))
      return true; // continue walking.
    if (updateInfo(Info))
      return true;
    if (addRelation(Info, (SymbolRoleSet)SymbolRole::RelationAccessorOf |
                    (SymbolRoleSet)SymbolRole::RelationChildOf , D))
      return true;

    if (!IdxConsumer.startSourceEntity(Info) || !IdxConsumer.finishSourceEntity(Info.symInfo, Info.roles))
      Cancelled = true;
  }
  return !Cancelled;
}

NominalTypeDecl *
IndexSwiftASTWalker::getTypeLocAsNominalTypeDecl(const TypeLoc &Ty) {
  if (Type T = Ty.getType())
    return T->getAnyNominal();
  if (auto *T = dyn_cast_or_null<IdentTypeRepr>(Ty.getTypeRepr())) {
    auto Comp = T->getComponentRange().back();
    if (auto NTD = dyn_cast_or_null<NominalTypeDecl>(Comp->getBoundDecl()))
      return NTD;
  }
  return nullptr;
}

bool IndexSwiftASTWalker::reportExtension(ExtensionDecl *D) {
  SourceLoc Loc = getLocForExtension(D);
  NominalTypeDecl *NTD = D->getExtendedNominal();
  if (!NTD)
    return true;
  if (!shouldIndex(NTD, /*IsRef=*/false))
    return true;

  // Don't index "empty" extensions in imported modules.
  if (IsModuleFile && !shouldIndexMembers(D))
    return true;

  IndexSymbol Info;
  if (initIndexSymbol(D, NTD, Loc, Info))
    return true;

  if (!startEntity(D, Info, /*IsRef=*/false))
    return false;

  if (!reportRelatedRef(NTD, Loc, /*isImplicit=*/false,
                        (SymbolRoleSet)SymbolRole::RelationExtendedBy, D))
      return false;
  if (!reportInheritedTypeRefs(D->getInherited(), D))
      return false;

  return true;
}

bool IndexSwiftASTWalker::report(ValueDecl *D) {
  if (startEntityDecl(D)) {
    // Pass accessors.
    if (auto StoreD = dyn_cast<AbstractStorageDecl>(D)) {
      bool usedPseudoAccessors = false;
      if (isa<VarDecl>(D) &&
          !StoreD->getParsedAccessor(AccessorKind::Get) &&
          !StoreD->getParsedAccessor(AccessorKind::Set)) {
        usedPseudoAccessors = true;
        auto VarD = cast<VarDecl>(D);
        // No actual getter or setter, pass 'pseudo' accessors.
        // We create accessor entities so we can implement the functionality
        // of libclang, which reports implicit method property accessor
        // declarations, invocations, and overrides for properties.
        // Note that an ObjC class subclassing from a Swift class, may still
        // be able to override its non-computed-property-accessors via a
        // method.
        if (!reportPseudoGetterDecl(VarD))
          return false;
        if (!reportPseudoSetterDecl(VarD))
          return false;
      }

      for (auto accessor : StoreD->getAllAccessors()) {
        // Don't include the implicit getter and setter if we added pseudo
        // accessors above.
        if (usedPseudoAccessors &&
            (accessor->getAccessorKind() == AccessorKind::Get ||
             accessor->getAccessorKind() == AccessorKind::Set))
          continue;

        ManuallyVisitedAccessorStack.push_back(accessor);
        SourceEntityWalker::walk(cast<Decl>(accessor));
        ManuallyVisitedAccessorStack.pop_back();
        if (Cancelled)
          return false;
      }
    } else if (auto NTD = dyn_cast<NominalTypeDecl>(D)) {
      if (!reportInheritedTypeRefs(NTD->getInherited(), NTD))
        return false;
    }
  } else {
    // Even if we don't record a local property we still need to walk its
    // accessor bodies.
    if (auto StoreD = dyn_cast<AbstractStorageDecl>(D)) {
      StoreD->visitParsedAccessors([&](AccessorDecl *accessor) {
        if (Cancelled)
          return;
        ManuallyVisitedAccessorStack.push_back(accessor);
        SourceEntityWalker::walk(cast<Decl>(accessor));
        ManuallyVisitedAccessorStack.pop_back();
      });
    }
  }

  return !Cancelled;
}

static bool hasUsefulRoleInSystemModule(SymbolRoleSet roles) {
  return roles & ((SymbolRoleSet)SymbolRole::Definition |
  (SymbolRoleSet)SymbolRole::Declaration |
  (SymbolRoleSet)SymbolRole::RelationChildOf |
  (SymbolRoleSet)SymbolRole::RelationBaseOf |
  (SymbolRoleSet)SymbolRole::RelationOverrideOf |
  (SymbolRoleSet)SymbolRole::RelationExtendedBy |
  (SymbolRoleSet)SymbolRole::RelationAccessorOf |
  (SymbolRoleSet)SymbolRole::RelationIBTypeOf);
}

bool IndexSwiftASTWalker::reportRef(ValueDecl *D, SourceLoc Loc,
                                    IndexSymbol &Info,
                                    Optional<AccessKind> AccKind) {
  if (!shouldIndex(D, /*IsRef=*/true))
    return true; // keep walking

  if (isa<AbstractFunctionDecl>(D)) {
    if (initFuncRefIndexSymbol(D, Loc, Info))
      return true;
  } else if (isa<AbstractStorageDecl>(D)) {
    if (initVarRefIndexSymbols(getCurrentExpr(), D, Loc, Info, AccKind))
      return true;
  } else {
    if (initIndexSymbol(D, Loc, /*IsRef=*/true, Info))
      return true;
  }

  if (isSystemModule && !hasUsefulRoleInSystemModule(Info.roles))
    return true;

  if (!startEntity(D, Info, /*IsRef=*/true))
    return true;

  // Report the accessors that were utilized.
  if (auto *ASD = dyn_cast<AbstractStorageDecl>(D)) {
    bool UsesGetter = Info.roles & (SymbolRoleSet)SymbolRole::Read;
    bool UsesSetter = Info.roles & (SymbolRoleSet)SymbolRole::Write;

    if (UsesGetter)
      if (!reportPseudoAccessor(ASD, AccessorKind::Get, /*IsRef=*/true,
                                Loc))
        return false;
    if (UsesSetter)
      if (!reportPseudoAccessor(ASD, AccessorKind::Set, /*IsRef=*/true,
                                Loc))
        return false;
  }

  return finishCurrentEntity();
}

bool IndexSwiftASTWalker::reportImplicitConformance(ValueDecl *witness, ValueDecl *requirement,
                                                    Decl *container) {
  if (!shouldIndex(witness, /*IsRef=*/true))
    return true; // keep walking

  SourceLoc loc;
  if (auto *extD = dyn_cast<ExtensionDecl>(container))
    loc = getLocForExtension(extD);
  else
    loc = container->getLoc(/*SerializedOK*/false);

  IndexSymbol info;
  if (initIndexSymbol(witness, loc, /*IsRef=*/true, info))
    return true;
  if (addRelation(info, (SymbolRoleSet) SymbolRole::RelationOverrideOf, requirement))
    return true;
  if (addRelation(info, (SymbolRoleSet) SymbolRole::RelationContainedBy, container))
    return true;
  // Remove the 'ref' role that \c initIndexSymbol introduces. This isn't
  // actually a 'reference', but an 'implicit' override.
  info.roles &= ~(SymbolRoleSet)SymbolRole::Reference;
  info.roles |= (SymbolRoleSet)SymbolRole::Implicit;

  if (!startEntity(witness, info, /*IsRef=*/true))
    return true;
  return finishCurrentEntity();
}

bool IndexSwiftASTWalker::initIndexSymbol(ValueDecl *D, SourceLoc Loc,
                                          bool IsRef, IndexSymbol &Info) {
  assert(D);
  if (Loc.isValid() && SrcMgr.findBufferContainingLoc(Loc) != BufferID)
    return true;
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    // Always base the symbol information on the canonical VarDecl
    D = VD->getCanonicalVarDecl();
  }

  Info.decl = D;
  Info.symInfo = getSymbolInfoForDecl(D);
  if (Info.symInfo.Kind == SymbolKind::Unknown)
    return true;

  // Cannot be extension, which is not a ValueDecl.

  if (IsRef) {
    Info.roles |= (unsigned)SymbolRole::Reference;
    if (auto Container = getContainingDecl())
      addRelation(Info, (unsigned)SymbolRole::RelationContainedBy, Container);
  } else {
    Info.roles |= (unsigned)SymbolRole::Definition;
    if (D->isImplicit())
      Info.roles |= (unsigned)SymbolRole::Implicit;
  }

  if (getNameAndUSR(D, /*ExtD=*/nullptr, Info.name, Info.USR))
    return true;

  std::tie(Info.line, Info.column, Info.offset) = getLineColAndOffset(Loc);
  if (!IsRef) {
    if (auto Group = D->getGroupName())
      Info.group = Group.getValue();
  }
  return false;
}

bool IndexSwiftASTWalker::initIndexSymbol(ExtensionDecl *ExtD, ValueDecl *ExtendedD,
                                          SourceLoc Loc, IndexSymbol &Info) {
  assert(ExtD && ExtendedD);
  Info.decl = ExtendedD;
  Info.symInfo = getSymbolInfoForDecl(ExtD);
  if (Info.symInfo.Kind == SymbolKind::Unknown)
    return true;

  Info.roles |= (unsigned)SymbolRole::Definition;

  if (getNameAndUSR(ExtendedD, ExtD, Info.name, Info.USR))
    return true;

  std::tie(Info.line, Info.column, Info.offset) = getLineColAndOffset(Loc);
  if (auto Group = ExtD->getGroupName())
    Info.group = Group.getValue();
  return false;
}

bool IndexSwiftASTWalker::initFuncDeclIndexSymbol(FuncDecl *D,
                                                  IndexSymbol &Info) {
  if (initIndexSymbol(D, D->getLoc(/*SerializedOK*/false), /*IsRef=*/false, Info))
    return true;

  if (isDynamicVarAccessorOrFunc(D, Info.symInfo)) {
    Info.roles |= (SymbolRoleSet)SymbolRole::Dynamic;
  }

  if (D->hasImplicitSelfDecl()) {
    // If this is an @IBAction or @IBSegueAction method, find the sender
    // parameter (if present) and relate the method to its type.
    ParamDecl *senderParam = nullptr;

    auto paramList = D->getParameters();
    if (D->getAttrs().hasAttribute<IBActionAttr>()) {
      if (paramList->size() > 0)
        senderParam = paramList->get(0);
    } else if (D->getAttrs().hasAttribute<IBSegueActionAttr>()) {
      if (paramList->size() > 1)
        senderParam = paramList->get(1);
    }

    if (senderParam)
      if (auto nominal = senderParam->getType()->getAnyNominal())
        addRelation(Info, (SymbolRoleSet) SymbolRole::RelationIBTypeOf,
                    nominal);
  }

  if (auto Group = D->getGroupName())
    Info.group = Group.getValue();
  return false;
}

bool IndexSwiftASTWalker::initFuncRefIndexSymbol(ValueDecl *D, SourceLoc Loc,
                                                 IndexSymbol &Info) {

  if (initIndexSymbol(D, Loc, /*IsRef=*/true, Info))
    return true;

  if (!isa<AbstractStorageDecl>(D) && !ide::isBeingCalled(ExprStack))
    return false;

  Info.roles |= (unsigned)SymbolRole::Call;
  if (auto *Caller = dyn_cast_or_null<AbstractFunctionDecl>(getParentDecl())) {
    if (addRelation(Info, (SymbolRoleSet) SymbolRole::RelationCalledBy, Caller))
      return true;
  }

  Expr *BaseE = ide::getBase(ExprStack);
  if (!BaseE)
    return false;

  if (ide::isDynamicCall(BaseE, D))
    Info.roles |= (unsigned)SymbolRole::Dynamic;

  SmallVector<NominalTypeDecl *, 1> Types;
  ide::getReceiverType(BaseE, Types);
  for (auto *ReceiverTy : Types) {
    if (addRelation(Info, (SymbolRoleSet) SymbolRole::RelationReceivedBy,
                    ReceiverTy))
      return true;
  }
  return false;
}

bool IndexSwiftASTWalker::initVarRefIndexSymbols(Expr *CurrentE, ValueDecl *D,
                                                 SourceLoc Loc, IndexSymbol &Info,
                                                 Optional<AccessKind> AccKind) {

  if (initIndexSymbol(D, Loc, /*IsRef=*/true, Info))
    return true;

  if (!CurrentE)
    return false;

  AccessKind Kind = AccKind.hasValue() ? *AccKind : AccessKind::Read;
  switch (Kind) {
  case swift::AccessKind::Read:
    Info.roles |= (unsigned)SymbolRole::Read;
    break;
  case swift::AccessKind::ReadWrite:
    Info.roles |= (unsigned)SymbolRole::Read;
    LLVM_FALLTHROUGH;
  case swift::AccessKind::Write:
    Info.roles |= (unsigned)SymbolRole::Write;
  }

  return false;
}

bool IndexSwiftASTWalker::indexComment(const Decl *D) {
  // FIXME: Workaround for getting tag locations. We should enhance cmark to
  // keep track of node offsets in the original comment text.
  struct TagLoc {
    StringRef Text;
    SourceLoc Loc;
  };
  SmallVector<TagLoc, 3> tagLocs;
  for (const auto &single : D->getRawComment().Comments) {
    size_t idx = single.RawText.find("- Tag:");
    if (idx != StringRef::npos) {
      tagLocs.push_back(TagLoc{single.RawText,
                               single.Range.getStart().getAdvancedLoc(idx)});
    }
  }
  if (tagLocs.empty())
    return true;

  swift::markup::MarkupContext MC;
  auto DC = getSingleDocComment(MC, D);
  if (!DC)
    return true;
  for (StringRef tagName : DC->getTags()) {
    tagName = tagName.trim();
    if (tagName.empty())
      continue;
    SourceLoc loc;
    for (const auto &tagLoc : tagLocs) {
      if (tagLoc.Text.contains(tagName)) {
        loc = tagLoc.Loc;
        break;
      }
    }
    if (loc.isInvalid())
      continue;
    IndexSymbol Info;
    Info.decl = nullptr;
    Info.symInfo = SymbolInfo{ SymbolKind::CommentTag, SymbolSubKind::None,
      SymbolLanguage::Swift, SymbolPropertySet() };
    Info.roles |= (unsigned)SymbolRole::Definition;
    Info.name = StringRef();
    SmallString<128> storage;
    {
      llvm::raw_svector_ostream OS(storage);
      OS << "t:" << tagName;
      Info.USR = stringStorage.copyString(OS.str());
    }
    std::tie(Info.line, Info.column, Info.offset) = getLineColAndOffset(loc);
    if (!IdxConsumer.startSourceEntity(Info) || !IdxConsumer.finishSourceEntity(Info.symInfo, Info.roles)) {
      Cancelled = true;
      break;
    }
  }
  return !Cancelled;
}

void IndexSwiftASTWalker::getRecursiveModuleImports(
    ModuleDecl &Mod, SmallVectorImpl<ModuleDecl *> &Imports) {
  auto It = ImportsMap.find(&Mod);
  if (It != ImportsMap.end()) {
    Imports.append(It->second.begin(), It->second.end());
    return;
  }

  llvm::SmallPtrSet<ModuleDecl *, 16> Visited;
  collectRecursiveModuleImports(Mod, Visited);
  Visited.erase(&Mod);

  warn([&Imports](llvm::raw_ostream &OS) {
    std::for_each(Imports.begin(), Imports.end(), [&OS](ModuleDecl *M) {
      if (M->getModuleFilename().empty()) {
        std::string Info = "swift::ModuleDecl with empty file name!! \nDetails: \n";
        Info += "  name: ";
        Info += M->getName().get();
        Info += "\n";

        auto Files = M->getFiles();
        std::for_each(Files.begin(), Files.end(), [&](FileUnit *FU) {
          Info += "  file unit: ";

          switch (FU->getKind()) {
          case FileUnitKind::Builtin:
            Info += "builtin";
            break;
          case FileUnitKind::Synthesized:
            Info += "synthesized";
            break;
          case FileUnitKind::Source:
            Info += "source, file=\"";
            Info += cast<SourceFile>(FU)->getFilename();
            Info += "\"";
            break;
          case FileUnitKind::SerializedAST:
            Info += "serialized ast, file=\"";
            Info += cast<LoadedFile>(FU)->getFilename();
            Info += "\"";
            break;
          case FileUnitKind::ClangModule:
          case FileUnitKind::DWARFModule:
            Info += "clang module, file=\"";
            Info += cast<LoadedFile>(FU)->getFilename();
            Info += "\"";
          }

          Info += "\n";
        });

        OS << "swift::ModuleDecl with empty file name! " << Info << "\n";
      }
    });
  });

  Imports.append(Visited.begin(), Visited.end());
  std::sort(Imports.begin(), Imports.end(), [](ModuleDecl *LHS, ModuleDecl *RHS) {
    return LHS->getModuleFilename() < RHS->getModuleFilename();
  });

  // Cache it.
  ImportsMap[&Mod].append(Imports.begin(), Imports.end());
}

void IndexSwiftASTWalker::collectRecursiveModuleImports(
    ModuleDecl &TopMod, llvm::SmallPtrSetImpl<ModuleDecl *> &Visited) {

  bool IsNew = Visited.insert(&TopMod).second;
  if (!IsNew)
    return;

  // Pure Clang modules are tied to their dependencies, no need to look into its
  // imports.
  // FIXME: What happens if the clang module imports a swift module ? So far
  // the assumption is that the path to the swift module will be fixed, so no
  // need to hash the clang module.
  // FIXME: This is a bit of a hack.
  if (TopMod.getFiles().size() == 1)
    if (TopMod.getFiles().front()->getKind() == FileUnitKind::ClangModule ||
        TopMod.getFiles().front()->getKind() == FileUnitKind::DWARFModule)
      return;

  auto It = ImportsMap.find(&TopMod);
  if (It != ImportsMap.end()) {
    Visited.insert(It->second.begin(), It->second.end());
    return;
  }

  ModuleDecl::ImportFilter ImportFilter;
  ImportFilter |= ModuleDecl::ImportFilterKind::Exported;
  ImportFilter |= ModuleDecl::ImportFilterKind::Default;
  // FIXME: ImportFilterKind::ShadowedByCrossImportOverlay?
  SmallVector<ImportedModule, 8> Imports;
  TopMod.getImportedModules(Imports);

  for (auto Import : Imports) {
    collectRecursiveModuleImports(*Import.importedModule, Visited);
  }
}

//===----------------------------------------------------------------------===//
// Indexing entry points
//===----------------------------------------------------------------------===//

void index::indexDeclContext(DeclContext *DC, IndexDataConsumer &consumer) {
  assert(DC);
  SourceFile *SF = DC->getParentSourceFile();
  IndexSwiftASTWalker walker(consumer, DC->getASTContext(), SF);
  walker.visitDeclContext(DC);
  consumer.finish();
}

void index::indexSourceFile(SourceFile *SF, IndexDataConsumer &consumer) {
  assert(SF);
  IndexSwiftASTWalker walker(consumer, SF->getASTContext(), SF);
  walker.visitModule(*SF->getParentModule());
  consumer.finish();
}

void index::indexModule(ModuleDecl *module, IndexDataConsumer &consumer) {
  assert(module);
  IndexSwiftASTWalker walker(consumer, module->getASTContext());
  walker.visitModule(*module);
  consumer.finish();
}
