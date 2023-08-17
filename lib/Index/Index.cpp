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
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
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
  case AccessorKind::Init:
    OS << "init:" << ASD->getName();
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
    return AFD->isMemberwiseInitializer();
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
    constexpr ModuleDecl::ImportFilter ImportFilter =
      ModuleDecl::getImportFilterLocal();

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

/// Identifies containers along with the types and expressions to which they
/// correspond.
///
/// The simplest form of a container is an function, it becomes the active
/// container immediately when it's visited. Pattern bindings however are more
/// complex, as their lexical structure does not translate directly into
/// container semantics.
///
/// Given the tuple binding 'let (a, b): (Int, String) = (intValue,
/// stringValue)' we can see that 'a' corresponds to 'Int' and 'intValue',
/// while 'b' corresponds to 'String' and 'stringValue'. By identifying these
/// relationships, we can pinpoint locations within the PatternBindingDecl
/// that the corresponding VarDecl should be marked as the current active
/// container. For example, when we walk to the TypeRepr for 'Int', we can
/// make 'a' the current active container, and likewise for 'String', 'b'. And
/// thus, when the walk reaches the point of creating the reference for 'Int'
/// it will be contained by 'a'.
///
/// These locations are also identified for single named pattern bindings; in
/// which case, the VarDecl is activated for all types and expressions with
/// the pattern.
///
/// Pattern bindings containing an AnyPattern (i.e let _) are a special case,
/// as they have no VarDecl. Their types and expressions are associated with
/// the current active container, if any. Therefore, given such a pattern
/// declared within a function, the type and expression references will be
/// contained by the function. If there is no active container, the references
/// are not contained.
class ContainerTracker {
  typedef llvm::PointerUnion<const VarDecl *, const TuplePattern *>
      PatternElement;
  typedef llvm::PointerUnion<const Decl *, const Pattern *> Container;
  typedef const void *ActivationKey;

  struct StackEntry {
    const Decl *TrackedDecl = nullptr;
    ActivationKey ActiveKey = nullptr;
    llvm::DenseMap<ActivationKey, Container> Containers{};
  };

  SmallVector<StackEntry, 4> Stack;

public:
  void beginTracking(Decl *D) {
    if (auto PBD = dyn_cast<PatternBindingDecl>(D)) {
      StackEntry Entry = identifyContainers(PBD);
      Stack.push_back(std::move(Entry));
    } else if (isa<AbstractFunctionDecl>(D)) {
      StackEntry Entry;
      Entry.TrackedDecl = D;
      Entry.ActiveKey = D;
      Entry.Containers[D] = D;
      Stack.push_back(std::move(Entry));
    }
  }

  void endTracking(Decl *D) {
    if (Stack.empty())
      return;
    if (Stack.back().TrackedDecl == D)
      Stack.pop_back();
  }

  bool empty() const { return Stack.empty(); }

  void forEachActiveContainer(llvm::function_ref<void(const Decl *)> f) const {
    if (Stack.empty())
      return;

    const StackEntry &Entry = Stack.back();

    if (!Entry.ActiveKey)
      return;

    auto MapEntry = Entry.Containers.find(Entry.ActiveKey);

    if (MapEntry == Entry.Containers.end())
      return;

    Container C = MapEntry->second;

    if (auto *D = C.dyn_cast<const Decl *>()) {
      f(D);
    } else if (auto *P = C.dyn_cast<const Pattern *>()) {
      P->forEachVariable([&](VarDecl *VD) { f(VD); });
    }
  }

  void activateContainersFor(ActivationKey K) {
    if (Stack.empty())
      return;

    StackEntry &Entry = Stack.back();

    // Only activate the ActivationKey if there's an entry in the Containers.
    if (Entry.Containers.count(K))
      Entry.ActiveKey = K;
  }

private:
  StackEntry identifyContainers(const PatternBindingDecl *PBD) const {
    StackEntry Entry;
    Entry.TrackedDecl = PBD;

    if (auto *VD = PBD->getSingleVar()) {
      // This is a single var binding; therefore, it may also have custom
      // attributes. Immediately activate the VarDecl so that it can be the
      // container for the attribute references.
      Entry.ActiveKey = PBD;
      Entry.Containers[PBD] = VD;
    }

    for (auto Index : range(PBD->getNumPatternEntries())) {
      Pattern *P = PBD->getPattern(Index);
      if (!P)
        continue;

      TypeRepr *PatternTypeRepr = nullptr;
      Expr *PatternInitExpr = nullptr;

      if (auto *TP = dyn_cast<TypedPattern>(P)) {
        if (auto *TR = TP->getTypeRepr()) {
          if (auto *TTR = dyn_cast<TupleTypeRepr>(TR)) {
            PatternTypeRepr = TTR;
          } else {
            // Non-tuple type, associate all elements in this pattern with the
            // type.
            associateAllPatternElements(P, TR, Entry);
          }
        }
      }

      if (auto *InitExpr = PBD->getInit(Index)) {
        if (auto *TE = dyn_cast<TupleExpr>(InitExpr)) {
          PatternInitExpr = TE;
        } else {
          // Non-tuple initializer, associate all elements in this pattern with
          // the initializer.
          associateAllPatternElements(P, InitExpr, Entry);
        }
      }

      if (PatternTypeRepr || PatternInitExpr) {
        forEachPatternElementPreservingIndex(
            P, [&](PatternElement Element, size_t Index) {
              associatePatternElement(Element, Index, PatternTypeRepr,
                                      PatternInitExpr, Entry);
            });
      }
    }

    return Entry;
  }

  void associatePatternElement(PatternElement Element, size_t Index,
                               TypeRepr *TR, Expr *E, StackEntry &Entry) const {
    if (auto *TTR = dyn_cast_or_null<TupleTypeRepr>(TR)) {
      if (Index < TTR->getNumElements())
        TR = TTR->getElementType(Index);
    }

    if (auto *TE = dyn_cast_or_null<TupleExpr>(E)) {
      if (Index < TE->getNumElements())
        E = TE->getElement(Index);
    }

    if (!Element) {
      // This element is represents an AnyPattern (i.e let _).
      if (TR)
        associateAnyPattern(TR, Entry);

      if (E)
        associateAnyPattern(E, Entry);

      return;
    }

    if (auto *VD = Element.dyn_cast<const VarDecl *>()) {
      if (TR)
        Entry.Containers[TR] = VD;

      if (E)
        Entry.Containers[E] = VD;
    } else if (auto *TP = Element.dyn_cast<const TuplePattern *>()) {
      forEachPatternElementPreservingIndex(
          TP, [&](PatternElement Element, size_t Index) {
            associatePatternElement(Element, Index, TR, E, Entry);
          });
    }
  }

  // AnyPatterns behave differently to other patterns as they've no associated
  // VarDecl. The given ActivationKey is therefore associated with the current
  // active container, if any.
  void associateAnyPattern(ActivationKey K, StackEntry &Entry) const {
    Entry.Containers[K] = activeContainer();
  }

  Container activeContainer() const {
    if (Stack.empty())
      return nullptr;

    const StackEntry &Entry = Stack.back();

    if (Entry.ActiveKey) {
      auto ActiveContainer = Entry.Containers.find(Entry.ActiveKey);

      if (ActiveContainer != Entry.Containers.end())
        return ActiveContainer->second;
    }

    return nullptr;
  }

  void associateAllPatternElements(const Pattern *P, ActivationKey K,
                                   StackEntry &Entry) const {
    if (isAnyPattern(P)) {
      // This pattern consists of a single AnyPattern (i.e let _).
      associateAnyPattern(K, Entry);
    } else {
      Entry.Containers[K] = P;
    }
  }

  /// Enumerates elements within a Pattern while preserving their source
  /// location. Given the pattern binding 'let (a, _, (b, c)) = ...' the given
  /// function is called with the following values:
  ///
  /// VarDecl(a), 0
  /// nullptr, 1
  /// TuplePattern(b, c), 2
  ///
  /// Here nullptr represents the location of an AnyPattern, for which there
  /// is no associated VarDecl.
  void forEachPatternElementPreservingIndex(
      const Pattern *P,
      llvm::function_ref<void(PatternElement, size_t)> f) const {
    auto *SP = P->getSemanticsProvidingPattern();

    if (auto *TP = dyn_cast<TuplePattern>(SP)) {
      for (size_t Index = 0; Index < TP->getNumElements(); ++Index) {
        f(getPatternElement(TP->getElement(Index).getPattern()), Index);
      }
    } else {
      f(getPatternElement(SP), 0);
    }
  }

  PatternElement getPatternElement(const Pattern *P) const {
    auto *SP = P->getSemanticsProvidingPattern();

    if (auto *NP = dyn_cast<NamedPattern>(SP)) {
      return NP->getDecl();
    } else if (auto *TP = dyn_cast<TuplePattern>(SP)) {
      return TP;
    }

    return nullptr;
  }

  bool isAnyPattern(const Pattern *P) const {
    auto *SP = P->getSemanticsProvidingPattern();

    if (isa<NamedPattern>(SP) || isa<TuplePattern>(SP)) {
      return false;
    }

    return true;
  }
};

struct MappedLoc {
  unsigned Line;
  unsigned Column;
  bool IsGenerated;
};

class IndexSwiftASTWalker : public SourceEntityWalker {
  IndexDataConsumer &IdxConsumer;
  SourceManager &SrcMgr;
  unsigned BufferID;
  bool enableWarnings;

  ModuleDecl *CurrentModule = nullptr;
  bool IsModuleFile = false;
  bool isSystemModule = false;

  struct Entity {
    Decl *D;
    SymbolInfo SymInfo;
    SymbolRoleSet Roles;
    SmallVector<IndexedWitness, 6> ExplicitWitnesses;
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
  ContainerTracker Containers;

  // Already handled references that should be suppressed if found later.
  llvm::DenseSet<SourceLoc> RefsToSuppress;

  // Contains a mapping for captures of the form [x], from the declared "x"
  // to the captured "x" in the enclosing scope. Also includes shorthand if
  // let bindings.
  llvm::DenseMap<ValueDecl *, ValueDecl *> sameNamedCaptures;

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
        OS << Mod.getFullName(/*useRealNameIfAliased=*/true);
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

  ValueDecl *firstDecl(ValueDecl *D) {
    while (true) {
      auto captured = sameNamedCaptures.find(D);
      if (captured == sameNamedCaptures.end())
        break;
      D = captured->second;
    }
    return D;
  }

public:
  IndexSwiftASTWalker(IndexDataConsumer &IdxConsumer, ASTContext &Ctx,
                      SourceFile *SF = nullptr)
      : IdxConsumer(IdxConsumer), SrcMgr(Ctx.SourceMgr),
        BufferID(SF ? SF->getBufferID().value_or(-1) : -1),
        enableWarnings(IdxConsumer.enableWarnings()) {}

  ~IndexSwiftASTWalker() override {
    assert(Cancelled || EntitiesStack.empty());
    assert(Cancelled || ManuallyVisitedAccessorStack.empty());
    assert(Cancelled || Containers.empty());
  }

  /// Walk both the arguments and expansion of the macro, so we index both.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
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

  /// Report calls to the initializers of property wrapper types on wrapped
  /// properties.
  ///
  /// These may be either explicit:
  ///     `\@Wrapper(initialValue: 42) var x: Int`
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

    auto *MemberwiseInit = DeclRef->getDecl();
    auto NameLoc = DeclRef->getNameLoc();
    auto ArgNames = MemberwiseInit->getName().getArgumentNames();

    // Get label locations.
    llvm::SmallVector<Argument, 4> Args;
    if (NameLoc.isCompound()) {
      size_t LabelIndex = 0;
      while (auto ArgLoc = NameLoc.getArgumentLabelLoc(LabelIndex)) {
        Args.emplace_back(ArgLoc, ArgNames[LabelIndex], /*expr*/ nullptr);
        LabelIndex++;
      }
    } else if (auto *CallParent = dyn_cast_or_null<CallExpr>(getParentExpr())) {
      auto *args = CallParent->getArgs()->getOriginalArgs();
      Args.append(args->begin(), args->end());
    }

    if (Args.empty())
      return;

    // match labels to properties
    auto *TypeContext =
        MemberwiseInit->getDeclContext()->getSelfNominalTypeDecl();
    if (!TypeContext || !shouldIndex(TypeContext, false))
      return;

    unsigned CurLabel = 0;
    for (auto Prop : TypeContext->getMemberwiseInitProperties()) {
      if (CurLabel == Args.size())
        break;

      if (Args[CurLabel].getLabel() != Prop->getName())
        continue;

      IndexSymbol Info;
      auto LabelLoc = Args[CurLabel++].getLabelLoc();
      if (initIndexSymbol(Prop, LabelLoc, /*IsRef=*/true, Info))
        continue;
      if (startEntity(Prop, Info, /*IsRef=*/true))
        finishCurrentEntity();
    }
  }

  bool walkToExprPre(Expr *E) override {
    if (Cancelled)
      return false;

    // Record any same named captures/shorthand if let bindings so we can
    // treat their references as references to the original decl.
    if (auto *captureList = dyn_cast<CaptureListExpr>(E)) {
      for (auto shadows : getShorthandShadows(captureList)) {
        sameNamedCaptures[shadows.first] = shadows.second;
      }
    }

    ExprStack.push_back(E);
    Containers.activateContainersFor(E);
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

  bool walkToStmtPre(Stmt *stmt) override {
    if (Cancelled)
      return false;

    // Record any same named captures/shorthand if let bindings so we can
    // treat their references as references to the original decl.
    if (auto *condition = dyn_cast<LabeledConditionalStmt>(stmt)) {
      for (auto shadows : getShorthandShadows(condition)) {
        sameNamedCaptures[shadows.first] = shadows.second;
      }
    }
    return true;
  }

  bool walkToTypeReprPre(TypeRepr *T) override {
    if (Cancelled)
      return false;
    Containers.activateContainersFor(T);
    return true;
  }

  bool walkToPatternPre(Pattern *P) override {
    if (Cancelled)
      return false;
    Containers.activateContainersFor(P);
    return true;
  }

  void beginBalancedASTOrderDeclVisit(Decl *D) override {
    Containers.beginTracking(D);
  }

  void endBalancedASTOrderDeclVisit(Decl *D) override {
    Containers.endTracking(D);
  }

  /// Extensions redeclare all generic parameters of their extended type to add
  /// their additional restrictions. There are two issues with this model for
  /// indexing:
  ///  - The generic parameter declarations of the extension are implicit so we
  ///    wouldn't report them in the index. Any usage of the generic param in
  ///    the extension references this implicit declaration so we don't include
  ///    it in the index either.
  ///  - The implicit re-declarations have their own USRs so any usage of a
  ///    generic parameter inside an extension would use a different USR than
  ///    declaration of the param in the extended type.
  ///
  /// To fix these issues, we replace the reference to the implicit generic
  /// parameter defined in the extension by a reference to the generic parameter
  /// defined in the extended type.
  ///
  /// \returns the canonicalized replaced generic param decl if it can be found
  ///          or \p GenParam otherwise.
  ValueDecl *
  canonicalizeGenericTypeParamDeclForIndex(GenericTypeParamDecl *GenParam) {
    auto Extension = dyn_cast_or_null<ExtensionDecl>(
        GenParam->getDeclContext()->getAsDecl());
    if (!Extension) {
      // We are not referencing a generic parameter defined in an extension.
      // Nothing to do.
      return GenParam;
    }
    assert(GenParam->isImplicit() &&
           "Generic param decls in extension should always be implicit and "
           "shadow a generic param in the extended type.");
    assert(Extension->getExtendedNominal() &&
           "The implicit generic types on the extension should only be created "
           "if the extended type was found");

    auto ExtendedTypeGenSig =
        Extension->getExtendedNominal()->getGenericSignature();
    assert(ExtendedTypeGenSig && "Extension is generic but extended type not?");

    // The generic parameter in the extension has the same depths and index
    // as the one in the extended type.
    for (auto ExtendedTypeGenParam : ExtendedTypeGenSig.getGenericParams()) {
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

    if (Loc.isInvalid() || isSuppressed(Loc))
      return true;

    // Dig back to the original captured variable
    if (auto *VD = dyn_cast<VarDecl>(D)) {
      D = firstDecl(D);
    }

    IndexSymbol Info;

    if (Data.isImplicit)
      Info.roles |= (unsigned)SymbolRole::Implicit;

    if (CtorTyRef) {
      IndexSymbol CtorInfo(Info);
      if (Data.isImplicitCtorType)
        CtorInfo.roles |= (unsigned)SymbolRole::Implicit;
      if (!reportRef(CtorTyRef, Loc, CtorInfo, Data.AccKind))
        return false;
    }

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
        reportRef(Wrapped, AfterDollar, Info, llvm::None);
      }
    }

    return true;
  }

  bool visitModuleReference(ModuleEntity Mod, CharSourceRange Range) override {
    auto MappedLoc = getMappedLocation(Range.getStart());
    if (!MappedLoc)
      return true;

    IndexSymbol Info;
    Info.line = MappedLoc->Line;
    Info.column = MappedLoc->Column;
    Info.roles |= (unsigned)SymbolRole::Reference;
    if (MappedLoc->IsGenerated) {
      Info.roles |= (unsigned)SymbolRole::Implicit;
    }
    Info.symInfo = getSymbolInfoForModule(Mod);
    getModuleNameAndUSR(Mod, Info.name, Info.USR);
    addContainedByRelationIfContained(Info);

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

  void addContainedByRelationIfContained(IndexSymbol &Info) {
    Containers.forEachActiveContainer([&](const Decl *D) {
      addRelation(Info, (unsigned)SymbolRole::RelationContainedBy,
                  const_cast<Decl *>(D));
    });
  }

  void suppressRefAtLoc(SourceLoc Loc) {
    if (Loc.isInvalid()) return;
    RefsToSuppress.insert(Loc);
  }

  bool isSuppressed(SourceLoc Loc) const {
    return Loc.isValid() && RefsToSuppress.contains(Loc);
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
                 llvm::Optional<AccessKind> AccKind);
  bool reportImplicitConformance(ValueDecl *witness, ValueDecl *requirement,
                                 Decl *container);

  bool startEntity(Decl *D, IndexSymbol &Info, bool IsRef);
  bool startEntityDecl(ValueDecl *D);

  bool reportRelatedRef(ValueDecl *D, SourceLoc Loc, bool isImplicit, SymbolRoleSet Relations, Decl *Related);

  /// Report references for dependent types
  ///
  /// NOTE: If the dependent type is a typealias, report the underlying types as well.
  ///
  /// \param Ty The type being referenced.
  /// \param Relations The relationship between the referenced type and the passed Decl.
  /// \param Related The Decl that is referencing the type.
  /// \param isImplicit Whether the reference is implicit, such as for a typealias' underlying type.
  /// \param Loc The location of the reference, otherwise the location of the TypeLoc is used.
  bool reportRelatedTypeRef(const TypeLoc &Ty, SymbolRoleSet Relations, Decl *Related,
                            bool isImplicit=false, SourceLoc Loc={});
  bool reportInheritedTypeRefs(
      ArrayRef<InheritedEntry> Inherited, Decl *Inheritee);

  bool reportPseudoGetterDecl(VarDecl *D) {
    return reportPseudoAccessor(D, AccessorKind::Get, /*IsRef=*/false,
                                D->getLoc(/*SerializedOK=*/false));
  }
  bool reportPseudoSetterDecl(VarDecl *D) {
    return reportPseudoAccessor(D, AccessorKind::Set, /*IsRef=*/false,
                                D->getLoc(/*SerializedOK=*/false));
  }
  bool reportPseudoAccessor(AbstractStorageDecl *D, AccessorKind AccKind,
                            bool IsRef, SourceLoc Loc);
  bool reportIsDynamicRef(ValueDecl *D, IndexSymbol &Info);

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
                              IndexSymbol &Info,
                              llvm::Optional<AccessKind> AccKind);

  bool indexComment(const Decl *D);

  // Return the line and column of \p loc, as well as whether it was from a
  // generated buffer or not. 0:0 if indexing a module and \p loc is invalid.
  // \c None if \p loc is otherwise invalid or its original location isn't
  // contained within the current buffer.
  llvm::Optional<MappedLoc> getMappedLocation(SourceLoc loc) {
    if (loc.isInvalid()) {
      if (IsModuleFile)
        return {{0, 0, false}};
      return llvm::None;
    }

    bool inGeneratedBuffer =
        !SrcMgr.rangeContainsTokenLoc(SrcMgr.getRangeForBuffer(BufferID), loc);

    auto bufferID = BufferID;
    if (inGeneratedBuffer) {
      std::tie(bufferID, loc) = CurrentModule->getOriginalLocation(loc);
      if (BufferID != bufferID) {
        assert(false && "Location is not within file being indexed");
        return llvm::None;
      }
    }

    auto [line, col] = SrcMgr.getLineAndColumnInBuffer(loc, bufferID);
    return {{line, col, inGeneratedBuffer}};
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
    if (IsModuleFile && D->getFormalAccess() < AccessLevel::Public)
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
  CurrentModule = Context->getParentModule();
  IsModuleFile = false;
  isSystemModule = Context->getParentModule()->isNonUserModule();
  auto accessor = dyn_cast<AccessorDecl>(Context);
  if (accessor)
    ManuallyVisitedAccessorStack.push_back(accessor);
  walk(Context);
  if (accessor)
    ManuallyVisitedAccessorStack.pop_back();
}

void IndexSwiftASTWalker::visitModule(ModuleDecl &Mod) {
  CurrentModule = &Mod;

  SourceFile *SrcFile = nullptr;
  for (auto File : Mod.getFiles()) {
    if (auto SF = dyn_cast<SourceFile>(File)) {
      auto BufID = SF->getBufferID();
      if (BufID.has_value() && *BufID == BufferID) {
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
    if (auto *synthesizedSF = SrcFile->getSynthesizedFile()) {
      walk(synthesizedSF);
    }
  } else {
    IsModuleFile = true;
    isSystemModule = Mod.isNonUserModule();
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

    llvm::Optional<bool> IsClangModuleOpt;
    for (auto File : Mod->getFiles()) {
      switch (File->getKind()) {
      case FileUnitKind::Source:
      case FileUnitKind::Builtin:
      case FileUnitKind::Synthesized:
        break;
      case FileUnitKind::SerializedAST:
        assert(!IsClangModuleOpt.has_value() &&
               "cannot handle multi-file modules");
        IsClangModuleOpt = false;
        break;
      case FileUnitKind::ClangModule:
      case FileUnitKind::DWARFModule:
        assert(!IsClangModuleOpt.has_value() &&
               "cannot handle multi-file modules");
        IsClangModuleOpt = true;
        break;
      }
    }
    if (!IsClangModuleOpt.has_value())
      continue;
    bool IsClangModule = *IsClangModuleOpt;
    // Use module real name in case module aliasing is used.
    // For example, if a file being indexed has `import Foo`
    // and `-module-alias Foo=Bar` is passed, treat Foo as an
    // alias and Bar as the real module name as its dependency.
    StringRef ModuleName = Mod->getRealName().str();

    // If this module is an underscored cross-import overlay, use the name
    // of the underlying module that declared it instead.
    if (ModuleDecl *Declaring = Mod->getDeclaringModuleIfCrossImportOverlay())
      ModuleName = Declaring->getNameStr();

    if (!IdxConsumer.startDependency(ModuleName, Path, IsClangModule,
                                     Mod->isNonUserModule()))
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
    if (!normal || !shouldIndex(normal->getProtocol(), /*IsRef=*/true))
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
      EntitiesStack.push_back({D, Info.symInfo, Info.roles, std::move(explicitWitnesses)});
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

  for (auto Overridden: collectAllOverriddenDecls(D, /*IncludeProtocolReqs=*/false)) {
    addRelation(Info, (SymbolRoleSet) SymbolRole::RelationOverrideOf, Overridden);
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
  suppressRefAtLoc(Loc);

  if (!reportRef(D, Loc, Info, llvm::None)) {
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

bool IndexSwiftASTWalker::reportRelatedTypeRef(const TypeLoc &Ty, SymbolRoleSet Relations,
                                               Decl *Related, bool Implicit, SourceLoc Loc) {
  if (auto *composite = llvm::dyn_cast_or_null<CompositionTypeRepr>(Ty.getTypeRepr())) {
    SourceLoc IdLoc = Loc.isValid() ? Loc : composite->getSourceLoc();
    for (auto *Type : composite->getTypes()) {
      if (!reportRelatedTypeRef(Type, Relations, Related, /*isImplicit=*/Implicit, IdLoc))
        return false;
    }

    return true;
  } else if (auto *declRefTR = dyn_cast_or_null<DeclRefTypeRepr>(Ty.getTypeRepr())) {
    SourceLoc IdLoc = Loc.isValid() ? Loc : declRefTR->getLoc();
    NominalTypeDecl *NTD = nullptr;
    bool isImplicit = Implicit;
    if (auto *VD = declRefTR->getBoundDecl()) {
      if (auto *TAD = dyn_cast<TypeAliasDecl>(VD)) {
        IndexSymbol Info;
        if (isImplicit)
          Info.roles |= (unsigned)SymbolRole::Implicit;
        if (!reportRef(TAD, IdLoc, Info, llvm::None))
          return false;
        if (auto Ty = TAD->getUnderlyingType()) {
          NTD = Ty->getAnyNominal();
          isImplicit = true;
        }

        if (isa_and_nonnull<CompositionTypeRepr>(TAD->getUnderlyingTypeRepr())) {
          TypeLoc TL(TAD->getUnderlyingTypeRepr(), TAD->getUnderlyingType());
          if (!reportRelatedTypeRef(TL, Relations, Related, /*isImplicit=*/true, IdLoc))
            return false;
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
    if (ide::isDeclOverridable(D)) {
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

  TypeLoc TL(D->getExtendedTypeRepr(), D->getExtendedType());
  if (!reportRelatedTypeRef(TL, (SymbolRoleSet)SymbolRole::RelationExtendedBy, D))
      return false;
  if (!reportInheritedTypeRefs(D->getInherited(), D))
      return false;

  return true;
}

bool IndexSwiftASTWalker::report(ValueDecl *D) {
  auto *shadowedDecl = firstDecl(D);
  if (D != shadowedDecl) {
    // Report a reference to the shadowed decl
    SourceLoc loc = D->getNameLoc();

    IndexSymbol info;
    if (!reportRef(shadowedDecl, loc, info, AccessKind::Read))
      return false;

    // Suppress the reference if there is any (it is implicit and hence
    // already skipped in the shorthand if let case, but explicit in the
    // captured case).
    suppressRefAtLoc(loc);

    // Skip the definition of a shadowed decl
    return true;
  }

  if (startEntityDecl(D)) {
    // Pass accessors.
    if (auto StoreD = dyn_cast<AbstractStorageDecl>(D)) {
      bool usedPseudoAccessors = false;
      if (isa<VarDecl>(D) && !isa<ParamDecl>(D) &&
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
                                    llvm::Optional<AccessKind> AccKind) {
  if (!shouldIndex(D, /*IsRef=*/true))
    return true; // keep walking

  if (isa<AbstractFunctionDecl>(D) || isa<EnumElementDecl>(D)) {
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
    if (!isa<ParamDecl>(D)) {
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
  }

  return finishCurrentEntity();
}

bool IndexSwiftASTWalker::reportIsDynamicRef(ValueDecl *D, IndexSymbol &Info) {
  Expr *BaseE = ide::getBase(ExprStack);
  if (!BaseE)
    return false;

  if (!ide::isDynamicRef(BaseE, D))
    return false;

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

  auto MappedLoc = getMappedLocation(Loc);
  if (!MappedLoc)
    return true;

  if (auto *VD = dyn_cast<VarDecl>(D)) {
    // Always base the symbol information on the canonical VarDecl
    D = VD->getCanonicalVarDecl();
  }

  Info.decl = D;
  Info.line = MappedLoc->Line;
  Info.column = MappedLoc->Column;
  Info.symInfo = getSymbolInfoForDecl(D);

  if (Info.symInfo.Kind == SymbolKind::Unknown)
    return true;

  // Cannot be extension, which is not a ValueDecl.

  if (getNameAndUSR(D, /*ExtD=*/nullptr, Info.name, Info.USR))
    return true;

  if (IsRef) {
    Info.roles |= (unsigned)SymbolRole::Reference;
    addContainedByRelationIfContained(Info);
  } else {
    Info.roles |= (unsigned)SymbolRole::Definition;
    if (D->isImplicit())
      Info.roles |= (unsigned)SymbolRole::Implicit;
    if (auto Group = D->getGroupName())
      Info.group = Group.value();
  }

  if (MappedLoc->IsGenerated) {
    Info.roles |= (unsigned)SymbolRole::Implicit;
  }

  return false;
}

bool IndexSwiftASTWalker::initIndexSymbol(ExtensionDecl *ExtD, ValueDecl *ExtendedD,
                                          SourceLoc Loc, IndexSymbol &Info) {
  assert(ExtD && ExtendedD);

  auto MappedLoc = getMappedLocation(Loc);
  if (!MappedLoc)
    return true;

  Info.decl = ExtendedD;
  Info.line = MappedLoc->Line;
  Info.column = MappedLoc->Column;
  Info.symInfo = getSymbolInfoForDecl(ExtD);

  if (Info.symInfo.Kind == SymbolKind::Unknown)
    return true;

  Info.roles |= (unsigned)SymbolRole::Definition;
  if (MappedLoc->IsGenerated) {
    Info.roles |= (unsigned)SymbolRole::Implicit;
  }

  if (getNameAndUSR(ExtendedD, ExtD, Info.name, Info.USR))
    return true;

  if (auto Group = ExtD->getGroupName())
    Info.group = Group.value();
  return false;
}

bool IndexSwiftASTWalker::initFuncDeclIndexSymbol(FuncDecl *D,
                                                  IndexSymbol &Info) {
  if (initIndexSymbol(D, D->getLoc(/*SerializedOK*/false), /*IsRef=*/false, Info))
    return true;

  if (ide::isDeclOverridable(D)) {
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
      if (auto nominal = senderParam->getInterfaceType()->getAnyNominal())
        addRelation(Info, (SymbolRoleSet) SymbolRole::RelationIBTypeOf,
                    nominal);
  }

  if (auto Group = D->getGroupName())
    Info.group = Group.value();
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

  if (reportIsDynamicRef(D, Info))
    return true;

  return false;
}

bool IndexSwiftASTWalker::initVarRefIndexSymbols(
    Expr *CurrentE, ValueDecl *D, SourceLoc Loc, IndexSymbol &Info,
    llvm::Optional<AccessKind> AccKind) {
  if (initIndexSymbol(D, Loc, /*IsRef=*/true, Info))
    return true;

  if (!CurrentE)
    return false;

  AccessKind Kind = AccKind.has_value() ? *AccKind : AccessKind::Read;
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

  if (reportIsDynamicRef(D, Info))
    return true;

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

    auto mappedLoc = getMappedLocation(loc);
    if (!mappedLoc)
      continue;

    IndexSymbol Info;

    Info.decl = nullptr;

    Info.line = mappedLoc->Line;
    Info.column = mappedLoc->Column;

    Info.symInfo = SymbolInfo{ SymbolKind::CommentTag, SymbolSubKind::None,
      SymbolLanguage::Swift, SymbolPropertySet() };

    Info.roles |= (unsigned)SymbolRole::Definition;
    if (mappedLoc->IsGenerated) {
      Info.roles |= (unsigned)SymbolRole::Implicit;
    }

    Info.name = StringRef();
    SmallString<128> storage;
    {
      llvm::raw_svector_ostream OS(storage);
      OS << "t:" << tagName;
      Info.USR = stringStorage.copyString(OS.str());
    }

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
