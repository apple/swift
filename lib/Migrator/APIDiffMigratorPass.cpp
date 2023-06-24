//===--- APIDiffMigratorPass.cpp ------------------------------------------===//
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

#include "swift/AST/USRGeneration.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Frontend/Frontend.h"
#include "swift/IDE/Utils.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Migrator/ASTMigratorPass.h"
#include "swift/Migrator/EditorAdapter.h"
#include "swift/Migrator/FixitApplyDiagnosticConsumer.h"
#include "swift/Migrator/Migrator.h"
#include "swift/Migrator/RewriteBufferEditsReceiver.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Edit/EditedSource.h"
#include "clang/Rewrite/Core/RewriteBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "swift/IDE/APIDigesterData.h"
#include "swift/Basic/Defer.h"

using namespace swift;
using namespace swift::migrator;
using namespace swift::ide;
using namespace swift::ide::api;

namespace {

struct FoundResult {
  SourceRange TokenRange;
  bool Optional; // Range includes a trailing ? or !, e.g. [SomeType!]
  bool Suffixable; // No need to wrap parens when adding optionality
  bool Suffixed; // Range is followed by a trailing ? or !, e.g. [SomeType]!
  bool isValid() const { return TokenRange.isValid(); }
};

class ChildIndexFinder : public TypeReprVisitor<ChildIndexFinder, FoundResult> {
  ArrayRef<uint8_t> ChildIndices;
  bool ParentIsOptional;
  bool IsFunctionTypeArgument;

public:
  ChildIndexFinder(ArrayRef<uint8_t> ChildIndices)
      : ChildIndices(ChildIndices), ParentIsOptional(false),
        IsFunctionTypeArgument(false) {}

  FoundResult findChild(AbstractFunctionDecl *Parent) {
    auto NextIndex = consumeNext();
    if (!NextIndex) {
      if (auto Func = dyn_cast<FuncDecl>(Parent)) {
        if (auto *const TyRepr = Func->getResultTypeRepr())
          return visit(TyRepr);
      } else if (auto Init = dyn_cast<ConstructorDecl>(Parent)) {
        SourceLoc End = Init->getFailabilityLoc();
        bool Optional = End.isValid();
        if (!Optional)
          End = Init->getNameLoc();
        return {SourceRange(Init->getNameLoc(), End), Optional,
          /*suffixable=*/true, /*suffixed=*/false};
      }
      return {SourceRange(), false, false, false};
    }

    for (auto *Param: *Parent->getParameters()) {
      if (!--NextIndex) {
        return visit(Param->getTypeRepr());
      }
    }
    llvm_unreachable("child index out of bounds");
  }

private:
  bool hasNextIndex() const {
    return !ChildIndices.empty();
  }

  unsigned consumeNext() {
    unsigned Next = ChildIndices.front();
    ChildIndices = ChildIndices.drop_front();
    return Next;
  }

  bool isUserTypeAlias(TypeRepr *T) const {
    if (auto Ident = dyn_cast<IdentTypeRepr>(T)) {
      if (auto Bound = Ident->getBoundDecl()) {
        return isa<TypeAliasDecl>(Bound) &&
          !Bound->getModuleContext()->isSystemModule();
      }
    }
    return false;
  }

public:

  template<typename T>
  FoundResult handleParent(TypeRepr *Parent, const ArrayRef<T> Children,
                           bool Optional = false, bool Suffixable = true) {
    if (!hasNextIndex())
      return {
        Parent->getSourceRange(),
        Optional, Suffixable, /*Suffixed=*/ParentIsOptional
      };

    auto NextIndex = consumeNext();
    if (isUserTypeAlias(Parent))
      return {SourceRange(), false, false, false};

    assert(NextIndex < Children.size());
    TypeRepr *Child = Children[NextIndex];
    ParentIsOptional = Optional;
    IsFunctionTypeArgument = NextIndex == 1 && isa<FunctionTypeRepr>(Parent);

    return visit(Child);
  }

  FoundResult handleParent(TypeRepr *Parent, TypeRepr *FirstChild,
                           TypeRepr *SecondChild, bool Optional = false,
                           bool Suffixable = true) {
    TypeRepr *Children[] = {FirstChild, SecondChild};
    return handleParent(Parent, llvm::makeArrayRef(Children), Optional,
                        Suffixable);
  }

  FoundResult handleParent(TypeRepr *Parent, TypeRepr *Base,
                           bool Optional = false, bool Suffixable = true) {
    return handleParent(Parent, llvm::makeArrayRef(Base), Optional, Suffixable);
  }

  FoundResult visitTypeRepr(TypeRepr *T) {
    llvm_unreachable("unexpected typerepr");
  }

  FoundResult visitErrorTypeRepr(ErrorTypeRepr *T) {
    return {SourceRange(), false, false, false};
  }

  FoundResult visitAttributedTypeRepr(AttributedTypeRepr *T) {
    return visit(T->getTypeRepr());
  }

  FoundResult visitOwnershipTypeRepr(OwnershipTypeRepr *T) {
    return visit(T->getBase());
  }

  FoundResult visitIsolatedTypeRepr(IsolatedTypeRepr *T) {
    return visit(T->getBase());
  }

  FoundResult visitArrayTypeRepr(ArrayTypeRepr *T) {
    return handleParent(T, T->getBase());
  }

  FoundResult visitDictionaryTypeRepr(DictionaryTypeRepr *T) {
    return handleParent(T, T->getKey(), T->getValue());
  }

  FoundResult visitTupleTypeRepr(TupleTypeRepr *T) {
    // Paren TupleTypeReprs may be arbitrarily nested so don't count as their
    // own index level except in the case of function type argument parens
    if (T->isParenType() && !IsFunctionTypeArgument) {
      ParentIsOptional = false;
      return visit(T->getElementType(0));
    }

    IsFunctionTypeArgument = false;
    llvm::SmallVector<TypeRepr *, 8> Children;
    T->getElementTypes(Children);
    return handleParent(T, ArrayRef<TypeRepr *>(Children));
  }

  FoundResult visitFunctionTypeRepr(FunctionTypeRepr *T) {
    return handleParent(T, T->getResultTypeRepr(), T->getArgsTypeRepr(),
                       /*Optional=*/false, /*Suffixable=*/false);
  }

  FoundResult visitCompositionTypeRepr(CompositionTypeRepr *T) {
    return handleParent(T, T->getTypes(), /*Optional=*/false,
                        /*Suffixable=*/false);
  }

  FoundResult visitSimpleIdentTypeRepr(SimpleIdentTypeRepr *T) {
    return handleParent(T, ArrayRef<TypeRepr*>());
  }

  FoundResult visitGenericIdentTypeRepr(GenericIdentTypeRepr *T) {
    return handleParent(T, T->getGenericArgs());
  }

  FoundResult visitMemberTypeRepr(MemberTypeRepr *T) {
    return visit(T->getLastComponent());
  }

  FoundResult visitOptionalTypeRepr(OptionalTypeRepr *T) {
    return handleParent(T, T->getBase(), /*Optional=*/true);
  }

  FoundResult visitImplicitlyUnwrappedOptionalTypeRepr(ImplicitlyUnwrappedOptionalTypeRepr *T) {
    return handleParent(T, T->getBase(), /*Optional=*/true);
  }

  FoundResult visitProtocolTypeRepr(ProtocolTypeRepr *T) {
    return handleParent(T, T->getBase());
  }

  FoundResult visitMetatypeTypeRepr(MetatypeTypeRepr *T) {
    return handleParent(T, T->getBase());
  }

  FoundResult visitFixedTypeRepr(FixedTypeRepr *T) {
    return handleParent(T, ArrayRef<TypeRepr*>());
  }

  FoundResult visitSelfTypeRepr(SelfTypeRepr *T) {
    return handleParent(T, ArrayRef<TypeRepr*>());
  }
};

struct ConversionFunctionInfo {
  Expr *ExpressionToWrap;
  SmallString<256> Buffer;
  unsigned FuncNameStart;
  unsigned FuncNameEnd;
  ConversionFunctionInfo(Expr *ExpressionToWrap):
    ExpressionToWrap(ExpressionToWrap) {}
  StringRef getFuncDef() const { return Buffer.str(); }
  StringRef getFuncName() const {
    return Buffer.substr(FuncNameStart, FuncNameEnd - FuncNameStart);
  }
};

struct APIDiffMigratorPass : public ASTMigratorPass, public SourceEntityWalker {

  APIDiffItemStore DiffStore;

  bool isNilExpr(Expr *E) {
    auto Range = E->getSourceRange();
    return Range.isValid() && Lexer::getCharSourceRangeFromSourceRange(
      SF->getASTContext().SourceMgr, Range).str() == "nil";
  }

  bool isDotMember(CharSourceRange Range) {
    auto S = Range.str();
    return S.startswith(".") && S.substr(1).find(".") == StringRef::npos;
  }

  bool isDotMember(SourceRange Range) {
    return isDotMember(Lexer::getCharSourceRangeFromSourceRange(
      SF->getASTContext().SourceMgr, Range));
  }

  bool isDotMember(Expr *E) {
    auto Range = E->getSourceRange();
    return Range.isValid() && isDotMember(Range);
  }

  std::vector<APIDiffItem*> getRelatedDiffItems(ValueDecl *VD) {
    std::vector<APIDiffItem*> results;
    if (!VD)
      return results;
    auto addDiffItems = [&](ValueDecl *VD) {
      llvm::SmallString<64> Buffer;
      llvm::raw_svector_ostream OS(Buffer);
      if (swift::ide::printValueDeclUSR(VD, OS))
        return;
      auto Items = DiffStore.getDiffItems(Buffer.str());
      results.insert(results.end(), Items.begin(), Items.end());
    };

    addDiffItems(VD);
    for (auto *Overridden: collectAllOverriddenDecls(VD,
                                                     /*IncludeProtocolReqs=*/true,
                                                     /*Transitive=*/true)) {
      addDiffItems(Overridden);
    }
    return results;
  }

  DeclNameViewer getFuncRename(ValueDecl *VD, llvm::SmallString<32> &Buffer,
                               bool &IgnoreBase) {
    for (auto *Item: getRelatedDiffItems(VD)) {
      if (auto *CI = dyn_cast<CommonDiffItem>(Item)) {
        if (CI->isRename()) {
          IgnoreBase = true;
          switch(CI->NodeKind) {
          case SDKNodeKind::DeclFunction:
            IgnoreBase = false;
            LLVM_FALLTHROUGH;
          case SDKNodeKind::DeclConstructor:
            return DeclNameViewer(CI->getNewName());
          default:
            return DeclNameViewer();
          }
        }
      }
      if (auto *MI = dyn_cast<TypeMemberDiffItem>(Item)) {
        if (MI->Subkind == TypeMemberDiffItemSubKind::FuncRename) {
          llvm::raw_svector_ostream OS(Buffer);
          OS << MI->getNewTypeAndDot() << MI->newPrintedName;
          return DeclNameViewer(OS.str());
        }
      }
    }
    return DeclNameViewer();
  }


  bool isSimpleReplacement(APIDiffItem *Item, bool isDotMember, std::string &Text) {
    if (auto *MD = dyn_cast<TypeMemberDiffItem>(Item)) {
      if (MD->Subkind == TypeMemberDiffItemSubKind::SimpleReplacement) {
        bool NeedNoTypeName = isDotMember &&
          MD->oldPrintedName == MD->newPrintedName;
        if (NeedNoTypeName) {
          Text = (llvm::Twine(MD->isNewNameGlobal() ? "" : ".") +
            MD->getNewName().base()).str();
        } else {
          Text = (llvm::Twine(MD->getNewTypeAndDot()) +
            MD->getNewName().base()).str();
        }
        return true;
      }
    }

    // Simple rename.
    if (auto CI = dyn_cast<CommonDiffItem>(Item)) {
      if (CI->isRename() && (CI->NodeKind == SDKNodeKind::DeclVar ||
                             CI->NodeKind == SDKNodeKind::DeclType)) {
        Text = CI->getNewName().str();
        return true;
      }
    }
    return false;
  }

  std::vector<ConversionFunctionInfo> HelperFuncInfo;
  SourceLoc FileEndLoc;
  llvm::StringSet<> OverridingRemoveNames;

  /// For a given expression, check whether the type of this expression is
  /// type alias type, and the type alias type is known to change to raw
  /// representable type.
  bool isRecognizedTypeAliasChange(Expr *E) {
    if (auto Ty = E->getType()) {
      if (auto *NT = dyn_cast<TypeAliasType>(Ty.getPointer())) {
        for (auto Item: getRelatedDiffItems(NT->getDecl())) {
          if (auto CI = dyn_cast<CommonDiffItem>(Item)) {
            if (CI->DiffKind == NodeAnnotation::TypeAliasDeclToRawRepresentable) {
              return true;
            }
          }
        }
      }
    }
    return false;
  }

  APIDiffMigratorPass(EditorAdapter &Editor, SourceFile *SF,
                      const MigratorOptions &Opts):
    ASTMigratorPass(Editor, SF, Opts), DiffStore(Diags),
    FileEndLoc(SM.getRangeForBuffer(BufferID).getEnd()),
    OverridingRemoveNames(funcNamesForOverrideRemoval()) {}

  ~APIDiffMigratorPass() {
    Editor.disableCache();
    SWIFT_DEFER { Editor.enableCache(); };

    // Collect inserted functions to avoid re-insertion.
    std::set<std::string> InsertedFunctions;
    SmallVector<Decl*, 16> TopDecls;
    SF->getTopLevelDecls(TopDecls);
    for (auto *D: TopDecls) {
      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        InsertedFunctions.insert(
            std::string(FD->getBaseIdentifier()));
      }
    }

    // Handle helper functions without wrappees first.
    for (auto &Cur: HelperFuncInfo) {
      if (Cur.ExpressionToWrap)
        continue;
      auto FuncName = Cur.getFuncName().str();
      // Avoid inserting the helper function if it's already present.
      if (!InsertedFunctions.count(FuncName)) {
        Editor.insert(FileEndLoc, Cur.getFuncDef());
        InsertedFunctions.insert(FuncName);
      }
    }

    // Remove all helper functions that're without expressions to wrap.
    HelperFuncInfo.erase(std::remove_if(HelperFuncInfo.begin(),
      HelperFuncInfo.end(), [](const ConversionFunctionInfo& Info) {
        return !Info.ExpressionToWrap;
    }), HelperFuncInfo.end());

    for (auto &Cur: HelperFuncInfo) {
      assert(Cur.ExpressionToWrap);
      // Avoid wrapping nil expression.
      if (isNilExpr(Cur.ExpressionToWrap))
        continue;

      // Avoid wrapping a single expression with multiple conversion functions.
      auto count = std::count_if(HelperFuncInfo.begin(), HelperFuncInfo.end(),
        [&] (ConversionFunctionInfo &Info) {
          return Info.ExpressionToWrap->getSourceRange() ==
            Cur.ExpressionToWrap->getSourceRange();
        });
      if (count > 1)
        continue;
      assert(count == 1);

      // A conversion function will be redundant if the expression will change
      // from type alias to raw-value representable.
      if (isRecognizedTypeAliasChange(Cur.ExpressionToWrap))
        continue;
      auto FuncName = Cur.getFuncName().str();

      // Avoid inserting the helper function if it's already present.
      if (!InsertedFunctions.count(FuncName)) {
        Editor.insert(FileEndLoc, Cur.getFuncDef());
        InsertedFunctions.insert(FuncName);
      }
      Editor.insertBefore(Cur.ExpressionToWrap->getStartLoc(),
        (Twine(FuncName) + "(").str());
      Editor.insertAfterToken(Cur.ExpressionToWrap->getEndLoc(), ")");
    }
  }

  void run() {
    if (Opts.APIDigesterDataStorePaths.empty())
      return;
    for (auto Path : Opts.APIDigesterDataStorePaths)
      DiffStore.addStorePath(Path);
    DiffStore.printIncomingUsr(Opts.DumpUsr);
    walk(SF);
  }

  bool updateStringRepresentableDeclRef(APIDiffItem *Diff,
      CharSourceRange Range) {
    auto *CD = dyn_cast<CommonDiffItem>(Diff);
    if (!CD)
      return false;
    if (CD->NodeKind != SDKNodeKind::DeclVar)
      return false;
    if (!CD->isStringRepresentableChange())
      return false;
    switch(CD->DiffKind) {
    case NodeAnnotation::SimpleStringRepresentableUpdate:
      Editor.insert(Range.getEnd(), ".rawValue");
      return true;
    default:
      return false;
    }
  }

  bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                          TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef,
                          Type T, ReferenceMetaData Data) override {
    if (Data.isImplicit)
      return true;

    for (auto *Item: getRelatedDiffItems(CtorTyRef ? CtorTyRef: D)) {
      std::string RepText;
      if (isSimpleReplacement(Item, isDotMember(Range), RepText)) {
        Editor.replace(Range, RepText);
        return true;
      }
    }
    return true;
  }

  struct ReferenceCollector : public SourceEntityWalker {
    ValueDecl *Target;
    CharSourceRange Result;
    ReferenceCollector(ValueDecl* Target) : Target(Target) {}
    bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                            TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef,
                            Type T, ReferenceMetaData Data) override {
      if (D == Target && !Data.isImplicit && Range.isValid()) {
        Result = Range;
        return false;
      }
      return true;
    }
  };

  void emitRenameLabelChanges(ArgumentList *Args, DeclNameViewer NewName,
                              llvm::ArrayRef<unsigned> IgnoreArgIndex) {
    unsigned Idx = 0;
    auto Ranges = getCallArgLabelRanges(SM, Args,
                                        LabelRangeEndAt::LabelNameOnly);
    llvm::SmallVector<uint8_t, 2> ToRemoveIndices;
    for (unsigned I = 0; I < Ranges.first.size(); I ++) {
      if (std::any_of(IgnoreArgIndex.begin(), IgnoreArgIndex.end(),
                      [I](unsigned Ig) { return Ig == I; }))
        continue;

      // Ignore the first trailing closure label
      if (Ranges.second && I == Ranges.second)
        continue;

      auto LR = Ranges.first[I];
      if (Idx < NewName.argSize()) {
        auto Label = NewName.args()[Idx++];

        if (!Label.empty()) {
          if (LR.getByteLength())
            Editor.replace(LR, Label);
          else
            Editor.insert(LR.getStart(), (llvm::Twine(Label) + ": ").str());
        } else if (LR.getByteLength()){
          // New label is "_" however the old label is explicit.
          ToRemoveIndices.push_back(I);
        }
      }
    }
    if (!ToRemoveIndices.empty()) {
      auto Ranges = getCallArgLabelRanges(SM, Args,
                                         LabelRangeEndAt::BeforeElemStart);
      for (auto I : ToRemoveIndices) {
        Editor.remove(Ranges.first[I]);
      }
    }
  }

  void handleFuncRename(ValueDecl *FD, Expr* FuncRefContainer,
                        ArgumentList *Args) {
    bool IgnoreBase = false;
    llvm::SmallString<32> Buffer;
    if (auto View = getFuncRename(FD, Buffer, IgnoreBase)) {
      if (!IgnoreBase) {
        ReferenceCollector Walker(FD);
        Walker.walk(FuncRefContainer);
        Editor.replace(Walker.Result, View.base());
      }
      emitRenameLabelChanges(Args, View, {});
    }
  }

  bool handleQualifiedReplacement(Expr* Call) {
    auto handleDecl = [&](ValueDecl *VD, SourceRange ToReplace) {
      for (auto *I: getRelatedDiffItems(VD)) {
        if (auto *Item = dyn_cast<TypeMemberDiffItem>(I)) {
          if (Item->Subkind == TypeMemberDiffItemSubKind::QualifiedReplacement) {
            bool NeedNoTypeName = isDotMember(ToReplace) &&
              Item->oldPrintedName == Item->newPrintedName;
            if (NeedNoTypeName) {
              Editor.replace(ToReplace,
                (llvm::Twine(Item->isNewNameGlobal() ? "" : ".") +
                Item->getNewName().base()).str());
            } else {
              Editor.replace(ToReplace,
                (llvm::Twine(Item->getNewTypeAndDot()) +
                  Item->getNewName().base()).str());
            }
            return true;
          }
        }
      }
      return false;
    };
    if (auto *VD = getReferencedDecl(Call, /*semantic=*/false).second.getDecl())
      if (handleDecl(VD, Call->getSourceRange()))
        return true;

    return false;
  }

  bool handleSpecialCases(ValueDecl *FD, CallExpr* Call, ArgumentList *Args) {
    SpecialCaseDiffItem *Item = nullptr;
    for (auto *I: getRelatedDiffItems(FD)) {
      Item = dyn_cast<SpecialCaseDiffItem>(I);
      if (Item)
        break;
    }
    if (!Item)
      return false;
    std::vector<CallArgInfo> AllArgs =
      getCallArgInfo(SM, Args, LabelRangeEndAt::LabelNameOnly);
    switch(Item->caseId) {
    case SpecialCaseId::NSOpenGLSetOption: {
      // swift 3.2:
      // NSOpenGLSetOption(NSOpenGLGOFormatCacheSize, 5)
      // swift 4:
      // NSOpenGLGOFormatCacheSize.globalValue = 5
      CallArgInfo &FirstArg = AllArgs[0];
      CallArgInfo &SecondArg = AllArgs[1];
      Editor.remove(CharSourceRange(SM, Call->getStartLoc(),
                                    FirstArg.ArgExp->getStartLoc()));
      Editor.replace(CharSourceRange(SM, Lexer::getLocForEndOfToken(SM,
        FirstArg.ArgExp->getEndLoc()), SecondArg.LabelRange.getStart()),
                     ".globalValue = ");
      Editor.remove(Call->getEndLoc());
      return true;
    }
    case SpecialCaseId::NSOpenGLGetOption: {
      // swift 3.2:
      // NSOpenGLGetOption(NSOpenGLGOFormatCacheSize, &cacheSize)
      // swift 4:
      // cacheSize = NSOpenGLGOFormatCacheSize.globalValue
      CallArgInfo &FirstArg = AllArgs[0];
      CallArgInfo &SecondArg = AllArgs[1];

      StringRef SecondArgContent = SecondArg.getEntireCharRange(SM).str();
      if (SecondArgContent[0] == '&')
        SecondArgContent = SecondArgContent.substr(1);

      Editor.replace(CharSourceRange(SM, Call->getStartLoc(),
                                     FirstArg.ArgExp->getStartLoc()),
                     (llvm::Twine(SecondArgContent) + " = ").str());
      Editor.replace(CharSourceRange(SM, FirstArg.getEntireCharRange(SM).getEnd(),
                                     Lexer::getLocForEndOfToken(SM, Call->getEndLoc())),
                     ".globalValue");
      return true;
    }
    case SpecialCaseId::StaticAbsToSwiftAbs:
      // swift 3:
      // CGFloat.abs(1.0)
      // Float.abs(1.0)
      // Double.abs(1.0)
      // Float80.abs(1.0)
      //
      // swift 4:
      // Swift.abs(1.0)
      // Swift.abs(1.0)
      // Swift.abs(1.0)
      // Swift.abs(1.0)
      Editor.replace(Call->getFn()->getSourceRange(), "Swift.abs");
      return true;
    case SpecialCaseId::ToUIntMax:
      if (const auto *DotCall = dyn_cast<DotSyntaxCallExpr>(Call->getFn())) {
        Editor.insert(DotCall->getStartLoc(), "UInt64(");
        Editor.replace({ DotCall->getDotLoc(), Call->getEndLoc() }, ")");
        return true;
      }
      return false;
    case SpecialCaseId::ToIntMax:
      if (const auto *DotCall = dyn_cast<DotSyntaxCallExpr>(Call->getFn())) {
        Editor.insert(DotCall->getStartLoc(), "Int64(");
        Editor.replace({ DotCall->getDotLoc(), Call->getEndLoc() }, ")");
        return true;
      }
      return false;
    case SpecialCaseId::NSOpenGLGetVersion: {
      if (Args->size() != 2)
        return false;

      auto extractArg = [](const Expr *Arg) -> const DeclRefExpr * {
        while (const auto *ICE = dyn_cast<ImplicitConversionExpr>(Arg)) {
          Arg = ICE->getSubExpr();
        }
        if (const auto *IOE = dyn_cast<InOutExpr>(Arg)) {
          return dyn_cast<DeclRefExpr>(IOE->getSubExpr());
        }
        return nullptr;
      };

      const auto *Arg0 = extractArg(Args->getExpr(0));
      const auto *Arg1 = extractArg(Args->getExpr(1));

      if (!(Arg0 && Arg1)) {
        return false;
      }
      SmallString<256> Scratch;
      llvm::raw_svector_ostream OS(Scratch);
      auto StartLoc = Call->getStartLoc();
      Editor.insert(StartLoc, "(");
      Editor.insert(StartLoc,
        SM.extractText(Lexer::getCharSourceRangeFromSourceRange(SM,
          Arg0->getSourceRange())));
      Editor.insert(StartLoc, ", ");
      Editor.insert(StartLoc,
        SM.extractText(Lexer::getCharSourceRangeFromSourceRange(SM,
          Arg1->getSourceRange())));
      Editor.insert(StartLoc, ") = ");
      Editor.replace(Call->getSourceRange(), "NSOpenGLContext.openGLVersion");
      return true;
    }
    case SpecialCaseId::UIApplicationMain: {
      // If the first argument is CommandLine.argc, replace the second argument
      // with CommandLine.unsafeArgv
      CallArgInfo &FirstArg = AllArgs[0];
      // handle whitespace/line splits around the first arg when matching
      auto FirstArgSplit =
        SM.extractText(FirstArg.getEntireCharRange(SM)).rsplit('.');
      if (!FirstArgSplit.second.empty() &&
          FirstArgSplit.first.trim() == "CommandLine" &&
          FirstArgSplit.second.trim() == "argc") {
        CallArgInfo &SecondArg = AllArgs[1];
        Editor.replace(SecondArg.getEntireCharRange(SM),
                       "CommandLine.unsafeArgv");
        return true;
      }
      return false;
    }
    }
    llvm_unreachable("unhandled case");
  }

  bool handleTypeHoist(ValueDecl *FD, CallExpr* Call, ArgumentList *Args) {
    TypeMemberDiffItem *Item = nullptr;
    for (auto *I: getRelatedDiffItems(FD)) {
      Item = dyn_cast<TypeMemberDiffItem>(I);
      if (Item)
        break;
    }
    if (!Item)
      return false;
    if (Item->Subkind == TypeMemberDiffItemSubKind::SimpleReplacement ||
        Item->Subkind == TypeMemberDiffItemSubKind::QualifiedReplacement ||
        Item->Subkind == TypeMemberDiffItemSubKind::FuncRename)
      return false;

    if (Item->Subkind == TypeMemberDiffItemSubKind::GlobalFuncToStaticProperty) {
      Editor.replace(Call->getSourceRange(),
        (llvm::Twine(Item->getNewTypeAndDot()) + Item->getNewName().base()).str());
      return true;
    }
    if (*Item->selfIndex)
      return false;
    std::vector<CallArgInfo> AllArgs =
      getCallArgInfo(SM, Args, LabelRangeEndAt::LabelNameOnly);
    if (!AllArgs.size())
      return false;
    assert(*Item->selfIndex == 0 && "we cannot handle otherwise");
    DeclNameViewer NewName = Item->getNewName();
    llvm::SmallVector<unsigned, 2> IgnoredArgIndices;
    IgnoredArgIndices.push_back(*Item->selfIndex);
    if (auto RI = Item->removedIndex)
      IgnoredArgIndices.push_back(*RI);
    emitRenameLabelChanges(Args, NewName, IgnoredArgIndices);
    auto *SelfExpr = AllArgs[0].ArgExp;
    if (auto *IOE = dyn_cast<InOutExpr>(SelfExpr))
      SelfExpr = IOE->getSubExpr();
    const bool NeedParen = !SelfExpr->canAppendPostfixExpression();

    // Remove the global function name: "Foo(a, b..." to "a, b..."
    Editor.remove(CharSourceRange(SM, Call->getStartLoc(),
                                  SelfExpr->getStartLoc()));
    if (NeedParen)
      Editor.insert(SelfExpr->getStartLoc(), "(");
    std::string MemberFuncBase;
    if (Item->Subkind == TypeMemberDiffItemSubKind::HoistSelfAndUseProperty)
      MemberFuncBase = (llvm::Twine(NeedParen ? ")." : ".") + Item->getNewName().
        base()).str();
    else
      MemberFuncBase = (llvm::Twine(NeedParen ? ")." : ".") + Item->getNewName().
        base() + "(").str();

    if (AllArgs.size() > 1) {
      Editor.replace(CharSourceRange(SM, Lexer::getLocForEndOfToken(SM,
        SelfExpr->getEndLoc()), AllArgs[1].LabelRange.getStart()),
                     MemberFuncBase);
    } else {
      Editor.insert(Lexer::getLocForEndOfToken(SM, SelfExpr->getEndLoc()),
                    MemberFuncBase);
    }

    switch (Item->Subkind) {
    case TypeMemberDiffItemSubKind::FuncRename:
    case TypeMemberDiffItemSubKind::GlobalFuncToStaticProperty:
    case TypeMemberDiffItemSubKind::SimpleReplacement:
    case TypeMemberDiffItemSubKind::QualifiedReplacement:
      llvm_unreachable("should be handled elsewhere");
    case TypeMemberDiffItemSubKind::HoistSelfOnly:
      // we are done here.
      return true;
    case TypeMemberDiffItemSubKind::HoistSelfAndRemoveParam: {
      unsigned RI = *Item->removedIndex;
      CallArgInfo &ToRemove = AllArgs[RI];
      if (AllArgs.size() == RI + 1) {
        Editor.remove(ToRemove.getEntireCharRange(SM));
      } else {
        CallArgInfo &AfterToRemove = AllArgs[RI + 1];
        Editor.remove(CharSourceRange(SM, ToRemove.LabelRange.getStart(),
                                      AfterToRemove.LabelRange.getStart()));
      }
      return true;
    }
    case TypeMemberDiffItemSubKind::HoistSelfAndUseProperty:
      // Remove ).
      Editor.remove(Args->getEndLoc());
      return true;
    }
    llvm_unreachable("unhandled subkind");
  }

  void handleFunctionCallToPropertyChange(ValueDecl *FD, Expr* FuncRefContainer,
                                          ArgumentList *Args) {
    for (auto *Item : getRelatedDiffItems(FD)) {
      if (auto *CD = dyn_cast<CommonDiffItem>(Item)) {
        switch (CD->DiffKind) {
        case NodeAnnotation::GetterToProperty: {
          // Remove "()"
          Editor.remove(Lexer::getCharSourceRangeFromSourceRange(SM,
                                                       Args->getSourceRange()));
          return;
        }
        case NodeAnnotation::SetterToProperty: {
          ReferenceCollector Walker(FD);
          Walker.walk(FuncRefContainer);
          auto ReplaceRange = CharSourceRange(SM, Walker.Result.getStart(),
                                         Args->getStartLoc().getAdvancedLoc(1));

          // Replace "x.getY(" with "x.Y =".
          auto Replacement = (llvm::Twine(Walker.Result.str()
                                          .substr(3)) + " = ").str();
          SmallString<64> Scratch;
          Editor.replace(ReplaceRange,
                         camel_case::toLowercaseInitialisms(Replacement, Scratch));

          // Remove ")"
          Editor.remove(CharSourceRange(SM, Args->getEndLoc(),
                                        Args->getEndLoc().getAdvancedLoc(1)));
          return;
        }
        default:
          break;
        }
      }
    }
  }

  void replaceExpr(Expr* E, StringRef Text) {
    Editor.replace(CharSourceRange(SM, E->getStartLoc(),
      Lexer::getLocForEndOfToken(SM, E->getEndLoc())), Text);
  }

  bool wrapAttributeReference(Expr *Reference, Expr *WrapperTarget,
                              bool FromString) {
    auto *RD = Reference->getReferencedDecl().getDecl();
    if (!RD)
      return false;

    std::string Rename;
    Optional<NodeAnnotation> Kind;
    StringRef LeftComment;
    StringRef RightComment;
    for (auto *Item: getRelatedDiffItems(RD)) {
      if (isSimpleReplacement(Item, isDotMember(Reference), Rename)) {
      } else if (auto *CI = dyn_cast<CommonDiffItem>(Item)) {
        if (CI->isStringRepresentableChange() &&
            CI->NodeKind == SDKNodeKind::DeclVar) {
          Kind = CI->DiffKind;
          LeftComment = CI->LeftComment;
          RightComment = CI->RightComment;
        }
      }
    }
    if (!Kind.has_value())
      return false;
    if (Kind) {
      insertHelperFunction(*Kind, LeftComment, RightComment, FromString,
        WrapperTarget);
    }
    if (!Rename.empty()) {
      replaceExpr(Reference, Rename);
    }
    return true;
  }

  bool handleAssignDestMigration(Expr *E) {
    auto *ASE = dyn_cast<AssignExpr>(E);
    if (!ASE || !ASE->getDest() || !ASE->getSrc())
      return false;
    auto *Dest = ASE->getDest();
    auto Src = ASE->getSrc();
    if (wrapAttributeReference(Dest, Src, true)) {
      // We should handle the assignment source here since we won't visit
      // the children with present changes.
      handleAttributeReference(Src);
      return true;
    }
    return false;
  }

  bool handleAttributeReference(Expr *E) {
    return wrapAttributeReference(E, E, false);
  }

  ConversionFunctionInfo &insertHelperFunction(NodeAnnotation Anno,
                                               StringRef RawType,
                                               StringRef NewType,
                                               bool FromString,
                                               Expr *Wrappee) {
    HelperFuncInfo.emplace_back(Wrappee);
    ConversionFunctionInfo &Info = HelperFuncInfo.back();
    llvm::raw_svector_ostream OS(Info.Buffer);
    OS << "\n";
    OS << "// Helper function inserted by Swift 4.2 migrator.\n";
    OS << "fileprivate func ";
    Info.FuncNameStart = Info.Buffer.size();
    OS << (FromString ? "convertTo" : "convertFrom");
    SmallVector<std::string, 8> Segs;
    StringRef guard = "\tguard let input = input else { return nil }\n";
    switch(Anno) {
    case NodeAnnotation::OptionalArrayMemberUpdate:
      Segs = {"Optional", "Array", (Twine("[") + RawType + "]?").str()};
      Segs.push_back((Twine("[") + NewType +"]?").str());
      Segs.push_back((Twine(guard) + "\treturn input.map { key in " + NewType +"(key) }").str());
      Segs.push_back((Twine(guard) + "\treturn input.map { key in key.rawValue }").str());
      break;
    case NodeAnnotation::OptionalDictionaryKeyUpdate:
      Segs = {"Optional", "Dictionary", (Twine("[") + RawType + ": Any]?").str()};
      Segs.push_back((Twine("[") + NewType +": Any]?").str());
      Segs.push_back((Twine(guard) +
                      "\treturn Dictionary(uniqueKeysWithValues: input.map"
                      " { key, value in (" + NewType + "(rawValue: key), value)})").str());
      Segs.push_back((Twine(guard) +
                      "\treturn Dictionary(uniqueKeysWithValues: input.map"
                      " {key, value in (key.rawValue, value)})").str());
      break;
    case NodeAnnotation::ArrayMemberUpdate:
      Segs = {"", "Array", (Twine("[") + RawType + "]").str()};
      Segs.push_back((Twine("[") + NewType +"]").str());
      Segs.push_back((Twine("\treturn input.map { key in ") + NewType +"(key) }").str());
      Segs.push_back("\treturn input.map { key in key.rawValue }");
      break;
    case NodeAnnotation::DictionaryKeyUpdate:
      Segs = {"", "Dictionary", (Twine("[") + RawType + ": Any]").str()};
      Segs.push_back((Twine("[") + NewType +": Any]").str());
      Segs.push_back((Twine("\treturn Dictionary(uniqueKeysWithValues: input.map"
        " { key, value in (") + NewType + "(rawValue: key), value)})").str());
      Segs.push_back("\treturn Dictionary(uniqueKeysWithValues: input.map"
                     " {key, value in (key.rawValue, value)})");
      break;
    case NodeAnnotation::SimpleStringRepresentableUpdate:
      Segs = {"", "", RawType.str()};
      Segs.push_back(NewType.str());
      Segs.push_back((Twine("\treturn ") + NewType + "(rawValue: input)").str());
      Segs.push_back("\treturn input.rawValue");
      break;
    case NodeAnnotation::SimpleOptionalStringRepresentableUpdate:
      Segs = {"Optional", "", (Twine(RawType) +"?").str()};
      Segs.push_back((Twine(NewType) +"?").str());
      Segs.push_back((Twine(guard) + "\treturn " + NewType + "(rawValue: input)").str());
      Segs.push_back((Twine(guard) + "\treturn input.rawValue").str());
      break;
    default:
      llvm_unreachable("shouldn't handle this key.");
    }
    assert(Segs.size() == 6);
    OS << Segs[0];
    SmallVector<StringRef, 4> Parts;
    NewType.split(Parts, '.');
    for (auto P: Parts)
      OS << P;
    OS << Segs[1];
    Info.FuncNameEnd = Info.Buffer.size();
    if (FromString) {
      OS << "(_ input: " << Segs[2] << ") -> " << Segs[3] << " {\n";
      OS << Segs[4] << "\n}\n";
    } else {
      OS << "(_ input: " << Segs[3] << ") -> " << Segs[2] << " {\n";
      OS << Segs[5] << "\n}\n";
    }
    return Info;
  }

  void handleStringRepresentableArg(ValueDecl *FD, ArgumentList *Args,
                                    Expr *Call) {
    NodeAnnotation Kind;
    StringRef RawType;
    StringRef NewAttributeType;
    uint8_t ArgIdx;
    for (auto Item: getRelatedDiffItems(FD)) {
      if (auto *CI = dyn_cast<CommonDiffItem>(Item)) {
        if (CI->isStringRepresentableChange()) {
          Kind = CI->DiffKind;
          RawType = CI->LeftComment;
          NewAttributeType = CI->RightComment;
          assert(CI->getChildIndices().size() == 1);
          ArgIdx = CI->getChildIndices().front();
          break;
        }
      }
    }
    if (NewAttributeType.empty())
      return;
    Expr *WrapTarget = Call;
    bool FromString = false;
    if (ArgIdx) {
      ArgIdx --;
      FromString = true;
      auto AllArgs = getCallArgInfo(SM, Args, LabelRangeEndAt::LabelNameOnly);
      if (AllArgs.size() <= ArgIdx)
        return;
      WrapTarget = AllArgs[ArgIdx].ArgExp;
    }
    assert(WrapTarget);
    insertHelperFunction(Kind, RawType, NewAttributeType, FromString, WrapTarget);
  }

  bool hasRevertRawRepresentableChange(ValueDecl *VD) {
    for (auto Item: getRelatedDiffItems(VD)) {
      if (auto *CI = dyn_cast<CommonDiffItem>(Item)) {
        if (CI->DiffKind ==
              NodeAnnotation::RevertTypeAliasDeclToRawRepresentable)
          return true;
      }
    }
    return false;
  }

  bool handleRevertRawRepresentable(Expr *E) {
    // Change attribute.rawValue to attribute
    if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
      auto Found = false;
      if (auto *Base = MRE->getBase()) {
        if (hasRevertRawRepresentableChange(Base->getType()->getAnyNominal())) {
          Found = true;
        }
      }
      if (!Found)
        return false;
      auto NL = MRE->getNameLoc().getStartLoc();
      auto DL = MRE->getDotLoc();
      if (NL.isInvalid() || DL.isInvalid())
        return false;
      CharSourceRange Range = Lexer::getCharSourceRangeFromSourceRange(SM, {DL, NL});
      if (Range.str() == ".rawValue") {
        Editor.remove(Range);
        return true;
      }
    }

    // Change attribute(rawValue: "value") to "value"
    // Change attribute("value") to "value"
    if (auto *CE = dyn_cast<CallExpr>(E)) {
      auto Found = false;
      if (auto *CRC = dyn_cast<ConstructorRefCallExpr>(CE->getFn())) {
        if (auto *TE = dyn_cast<TypeExpr>(CRC->getBase())) {
          if (hasRevertRawRepresentableChange(TE->getInstanceType()->getAnyNominal()))
            Found = true;
        }
      }
      if (!Found)
        return false;
      std::vector<CallArgInfo> AllArgs =
        getCallArgInfo(SM, CE->getArgs(), LabelRangeEndAt::LabelNameOnly);
      if (AllArgs.size() == 1) {
        auto Label = AllArgs.front().LabelRange.str();
        if (Label == "rawValue" || Label.empty()) {
          Editor.replace(CE->getSourceRange(),
                         Lexer::getCharSourceRangeFromSourceRange(SM,
                           AllArgs.front().ArgExp->getSourceRange()).str());
          return true;
        }
      }
    }
    return false;
  }

  void handleResultTypeChange(ValueDecl *FD, Expr *Call) {
    Optional<NodeAnnotation> ChangeKind;

    // look for related change item for the function decl.
    for (auto Item: getRelatedDiffItems(FD)) {
      if (auto *CI = dyn_cast<CommonDiffItem>(Item)) {
        // check if the function's return type has been changed from nonnull
        // to nullable.
        if (CI->DiffKind == NodeAnnotation::WrapOptional &&
            CI->getChildIndices().size() == 1 &&
            CI->getChildIndices().front() == 0) {
          ChangeKind = NodeAnnotation::WrapOptional;
          break;
        }
      }
    }
    if (!ChangeKind.has_value())
      return;
    // If a function's return type has been changed from nonnull to nullable,
    // append ! to the original call expression.
    if (*ChangeKind == NodeAnnotation::WrapOptional) {
      Editor.insertAfterToken(Call->getSourceRange().End, "!");
    }
  }

  // If a property has changed from nonnull to nullable, we should add ! to the
  // reference of the property.
  bool handlePropertyTypeChange(Expr *E) {
    if (auto MRE = dyn_cast<MemberRefExpr>(E)) {
      if (auto *VD = MRE->getMember().getDecl()) {
        for (auto *I: getRelatedDiffItems(VD)) {
          if (auto *Item = dyn_cast<CommonDiffItem>(I)) {
            if (Item->DiffKind == NodeAnnotation::WrapOptional &&
                Item->NodeKind == SDKNodeKind::DeclVar) {
              Editor.insertAfterToken(E->getEndLoc(), "!");
              return true;
            }
          }
        }
      }
    }
    return false;
  }

  bool walkToExprPre(Expr *E) override {
    if (E->getSourceRange().isInvalid())
      return false;
    if (handleRevertRawRepresentable(E)) {
      // The name may also change, so we should keep visiting.
      return true;
    }
    if (handleQualifiedReplacement(E))
      return false;
    if (handleAssignDestMigration(E))
      return false;
    if (handleAttributeReference(E))
      return false;
    if (handlePropertyTypeChange(E))
      return false;
    if (auto *CE = dyn_cast<CallExpr>(E)) {
      auto Fn = CE->getFn();
      auto Args = CE->getArgs();

      if (auto *DRE = dyn_cast<DeclRefExpr>(Fn)) {
        if (auto *VD = DRE->getDecl()) {
          if (VD->getNumCurryLevels() == 1) {
            handleFuncRename(VD, Fn, Args);
            handleTypeHoist(VD, CE, Args);
            handleSpecialCases(VD, CE, Args);
            handleStringRepresentableArg(VD, Args, CE);
            handleResultTypeChange(VD, CE);
          }
        }
      }

      if (auto *SelfApply = dyn_cast<ApplyExpr>(Fn)) {
        if (auto VD = SelfApply->getFn()->getReferencedDecl().getDecl()) {
          if (VD->getNumCurryLevels() == 2) {
            handleFuncRename(VD, SelfApply->getFn(), Args);
            handleFunctionCallToPropertyChange(VD, SelfApply->getFn(), Args);
            handleSpecialCases(VD, CE, Args);
            handleStringRepresentableArg(VD, Args, CE);
            handleResultTypeChange(VD, CE);
          }
        }
      }
    }
    return true;
  }

  static void collectParameters(AbstractFunctionDecl *AFD,
                                SmallVectorImpl<ParamDecl*> &Results) {
    for (auto PD : *AFD->getParameters()) {
      Results.push_back(PD);
    }
  }

  void handleFuncDeclRename(AbstractFunctionDecl *AFD,
                            CharSourceRange NameRange) {
    bool IgnoreBase = false;
    llvm::SmallString<32> Buffer;
    if (auto View = getFuncRename(AFD, Buffer, IgnoreBase)) {
      if (!IgnoreBase)
        Editor.replace(NameRange, View.base());
      unsigned Index = 0;
      SmallVector<ParamDecl*, 4> Params;
      collectParameters(AFD, Params);
      for (auto *PD: Params) {
        if (Index == View.argSize())
          break;
        StringRef NewArg = View.args()[Index++];
        auto ArgLoc = PD->getArgumentNameLoc();

        // Represent empty label with underscore.
        if (NewArg.empty())
          NewArg = "_";

        // If the argument name is not specified, add the argument name before
        // the parameter name.
        if (ArgLoc.isInvalid())
          Editor.insertBefore(PD->getNameLoc(),
                              (llvm::Twine(NewArg) + " ").str());
        else {
          // Otherwise, replace the argument name directly.
          Editor.replaceToken(ArgLoc, NewArg);
        }
      }
    }
  }

  bool typeReplacementMayNeedParens(StringRef Replacement) const {
    return Replacement.contains('&') || Replacement.contains("->");
  }

  void handleOverridingTypeChange(AbstractFunctionDecl *AFD,
                                  CommonDiffItem *DiffItem) {
    assert(AFD);
    assert(DiffItem->isTypeChange());
    ChildIndexFinder Finder(DiffItem->getChildIndices());
    auto Result = Finder.findChild(AFD);
    if (!Result.isValid())
      return;

    switch (DiffItem->DiffKind) {
      case ide::api::NodeAnnotation::WrapOptional:
        if (Result.Suffixable) {
          Editor.insertAfterToken(Result.TokenRange.End, "?");
        } else {
          Editor.insertWrap("(", Result.TokenRange, ")?");
        }
        break;
      case ide::api::NodeAnnotation::WrapImplicitOptional:
        if (Result.Suffixable) {
          Editor.insertAfterToken(Result.TokenRange.End, "!");
        } else {
          Editor.insertWrap("(", Result.TokenRange, (")!"));
        }
        break;
      case ide::api::NodeAnnotation::UnwrapOptional:
        if (Result.Optional)
          Editor.remove(Result.TokenRange.End);
        break;
      case ide::api::NodeAnnotation::ImplicitOptionalToOptional:
        if (Result.Optional)
          Editor.replace(Result.TokenRange.End, "?");
        break;
      case ide::api::NodeAnnotation::TypeRewritten:
        Editor.replace(Result.TokenRange, DiffItem->RightComment);
        if (Result.Suffixed && typeReplacementMayNeedParens(DiffItem->RightComment)) {
          Editor.insertBefore(Result.TokenRange.Start, "(");
          Editor.insertAfterToken(Result.TokenRange.End, ")");
        }
        break;
      default:
        break;
    }
  }

  void handleOverridingPropertyChange(AbstractFunctionDecl *AFD,
                                      CommonDiffItem *DiffItem) {
    assert(AFD);
    assert(DiffItem->isToPropertyChange());
    auto FD = dyn_cast<FuncDecl>(AFD);
    if (!FD)
      return;

    switch (DiffItem->DiffKind) {
    case NodeAnnotation::GetterToProperty: {
      auto FuncLoc = FD->getFuncLoc();
      auto ReturnTyLoc = FD->getResultTypeSourceRange().Start;
      auto NameLoc = FD->getNameLoc();
      if (FuncLoc.isInvalid() || ReturnTyLoc.isInvalid() || NameLoc.isInvalid())
        break;

      // Replace "func" with "var"
      Editor.replaceToken(FuncLoc, "var");

      // Replace "() -> " with ": "
      Editor.replace(CharSourceRange(SM, Lexer::getLocForEndOfToken(SM, NameLoc),
        ReturnTyLoc), ": ");

      break;
    }
    case NodeAnnotation::SetterToProperty: {
      // FIXME: we should migrate this case too.
      break;
    }
    default:
      llvm_unreachable("should not be handled here.");
    }
  }

  // When users override a SDK function whose parameter types have been changed,
  // we should introduce a local variable in the body of the function definition
  // to shadow the changed parameter. Also, a proper conversion function should
  // be defined to bridge the parameter to the local variable.
  void handleLocalParameterBridge(AbstractFunctionDecl *AFD,
                                  CommonDiffItem *DiffItem) {
    assert(AFD);
    assert(DiffItem->isStringRepresentableChange());

    // We only handle top-level parameter type change.
    if (DiffItem->getChildIndices().size() != 1)
      return;
    auto Idx = DiffItem->getChildIndices().front();

    // We don't handle return type change.
    if (Idx == 0)
      return;
    Idx --;
    SmallVector<ParamDecl*, 4> Params;
    collectParameters(AFD, Params);
    if (Params.size() <= Idx)
      return;

    // Get the internal name of the changed parameter.
    auto VariableName = Params[Idx]->getParameterName().str();

    // Insert the helper function to convert the type back to raw types.
    auto &Info = insertHelperFunction(DiffItem->DiffKind, DiffItem->LeftComment,
      DiffItem->RightComment, /*From String*/false,
      /*No expression to wrap*/nullptr);

    auto BL = AFD->getBodySourceRange().Start;
    if (BL.isValid()) {
      // Insert the local variable declaration after the opening brace.
      Editor.insertAfterToken(BL,
        (llvm::Twine("\n// Local variable inserted by Swift 4.2 migrator.") +
         "\nlet " + VariableName + " = " + Info.getFuncName() + "(" +
         VariableName + ")\n").str());
    }
  }

  llvm::StringSet<> funcNamesForOverrideRemoval() {
    llvm::StringSet<> Results;
    Results.insert("c:objc(cs)NSObject(im)application:delegateHandlesKey:");
    Results.insert("c:objc(cs)NSObject(im)changeColor:");
    Results.insert("c:objc(cs)NSObject(im)controlTextDidBeginEditing:");
    Results.insert("c:objc(cs)NSObject(im)controlTextDidEndEditing:");
    Results.insert("c:objc(cs)NSObject(im)controlTextDidChange:");
    Results.insert("c:objc(cs)NSObject(im)changeFont:");
    Results.insert("c:objc(cs)NSObject(im)validModesForFontPanel:");
    Results.insert("c:objc(cs)NSObject(im)discardEditing");
    Results.insert("c:objc(cs)NSObject(im)commitEditing");
    Results.insert("c:objc(cs)NSObject(im)commitEditingWithDelegate:didCommitSelector:contextInfo:");
    Results.insert("c:objc(cs)NSObject(im)commitEditingAndReturnError:");
    Results.insert("c:objc(cs)NSObject(im)objectDidBeginEditing:");
    Results.insert("c:objc(cs)NSObject(im)objectDidEndEditing:");
    Results.insert("c:objc(cs)NSObject(im)validateMenuItem:");
    Results.insert("c:objc(cs)NSObject(im)pasteboard:provideDataForType:");
    Results.insert("c:objc(cs)NSObject(im)pasteboardChangedOwner:");
    Results.insert("c:objc(cs)NSObject(im)validateToolbarItem:");
    Results.insert("c:objc(cs)NSObject(im)layer:shouldInheritContentsScale:fromWindow:");
    Results.insert("c:objc(cs)NSObject(im)view:stringForToolTip:point:userData:");
    return Results;
  }

  SourceLoc shouldRemoveOverride(AbstractFunctionDecl *AFD) {
    if (AFD->getKind() != DeclKind::Func)
      return SourceLoc();
    SourceLoc OverrideLoc;

    // Get the location of override keyword.
    if (auto *Override = AFD->getAttrs().getAttribute<OverrideAttr>()) {
      if (Override->getRange().isValid()) {
        OverrideLoc = Override->getLocation();
      }
    }
    if (OverrideLoc.isInvalid())
      return SourceLoc();
    auto *OD = AFD->getOverriddenDecl();
    llvm::SmallString<64> Buffer;
    llvm::raw_svector_ostream OS(Buffer);
    if (swift::ide::printValueDeclUSR(OD, OS))
      return SourceLoc();
    return OverridingRemoveNames.contains(OS.str()) ? OverrideLoc : SourceLoc();
  }

  struct SuperRemoval: public ASTWalker {
    EditorAdapter &Editor;
    llvm::StringSet<> &USRs;
    SuperRemoval(EditorAdapter &Editor, llvm::StringSet<> &USRs):
      Editor(Editor), USRs(USRs) {}

    /// Walk everything in a macro.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    bool isSuperExpr(Expr *E) {
      if (E->isImplicit())
        return false;
      // Check if the expression is super.foo().
      if (auto *CE = dyn_cast<CallExpr>(E)) {
        if (auto *DSC = dyn_cast<DotSyntaxCallExpr>(CE->getFn())) {
          if (!isa<SuperRefExpr>(DSC->getBase()))
            return false;
          llvm::SmallString<64> Buffer;
          llvm::raw_svector_ostream OS(Buffer);
          auto *RD = DSC->getFn()->getReferencedDecl().getDecl();
          if (swift::ide::printValueDeclUSR(RD, OS))
            return false;
          return USRs.contains(OS.str());
        }
      }
      // We should handle try super.foo() too.
      if (auto *TE = dyn_cast<AnyTryExpr>(E)) {
        return isSuperExpr(TE->getSubExpr());
      }
      return false;
    }
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      if (auto *BS = dyn_cast<BraceStmt>(S)) {
        for(auto Ele: BS->getElements()) {
          if (Ele.is<Expr*>() && isSuperExpr(Ele.get<Expr*>())) {
            Editor.remove(Ele.getSourceRange());
          }
	}
      }
      // We only handle top-level expressions, so avoid visiting further.
      return Action::SkipChildren(S);
    }
  };

  bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
    if (D->isImplicit())
      return true;
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      handleFuncDeclRename(AFD, Range);
      for (auto *Item: getRelatedDiffItems(AFD)) {
        if (auto *DiffItem = dyn_cast<CommonDiffItem>(Item)) {
          if (DiffItem->isTypeChange())
            handleOverridingTypeChange(AFD, DiffItem);
          else if (DiffItem->isToPropertyChange())
            handleOverridingPropertyChange(AFD, DiffItem);
          else if (DiffItem->isStringRepresentableChange())
            handleLocalParameterBridge(AFD, DiffItem);
        }
      }
      auto OverrideLoc = shouldRemoveOverride(AFD);
      if (OverrideLoc.isValid()) {
        // Remove override keyword.
        Editor.remove(OverrideLoc);
        // Remove super-dot call.
        SuperRemoval Removal(Editor, OverridingRemoveNames);
        D->walk(Removal);
      }
    }

    // Handle property overriding migration.
    if (auto *VD = dyn_cast<VarDecl>(D)) {
      for (auto *Item: getRelatedDiffItems(VD)) {
        if (auto *CD = dyn_cast<CommonDiffItem>(Item)) {
          // If the overridden property has been renamed, we should rename
          // this property decl as well.
          if (CD->isRename() && VD->getNameLoc().isValid()) {
            Editor.replaceToken(VD->getNameLoc(), CD->getNewName());
          }
        }
      }
    }
    return true;
  }
};

} // end anonymous namespace

void migrator::runAPIDiffMigratorPass(EditorAdapter &Editor,
                                      SourceFile *SF,
                                      const MigratorOptions &Opts) {
  APIDiffMigratorPass { Editor, SF, Opts }.run();
}
