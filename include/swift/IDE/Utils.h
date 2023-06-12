//===--- Utils.h - Misc utilities -------------------------------*- C++ -*-===//
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

#ifndef SWIFT_IDE_UTILS_H
#define SWIFT_IDE_UTILS_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/DeclNameLoc.h"
#include "swift/AST/Effects.h"
#include "swift/AST/Module.h"
#include "swift/AST/Expr.h"
#include "swift/Basic/LLVM.h"
#include "swift/IDE/SourceEntityWalker.h"
#include "swift/Parse/Token.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
  template<typename Fn> class function_ref;
  class MemoryBuffer;
}

namespace clang {
  class Module;
  class NamedDecl;
}

namespace swift {
  class ValueDecl;
  class ASTContext;
  class SourceFile;
  class TypeDecl;
  class SourceLoc;
  class Type;
  class Decl;
  class DeclContext;
  class CallExpr;
  class ClangNode;
  class ClangImporter;
  class Token;

namespace ide {
struct SourceCompleteResult {
  // Set to true if the input source is fully formed, false otherwise.
  bool IsComplete;
  // The text to use as the indent string when auto indenting the next line.
  // This will contain the exactly what the client typed (any whitespaces and
  // tabs) and can be used to indent subsequent lines. It does not include
  // the current indent level, IDE clients should insert the correct indentation
  // with spaces or tabs to account for the current indent level. The indent
  // prefix will contain the leading space characters of the line that
  // contained the '{', '(' or '[' character that was unbalanced.
  std::string IndentPrefix;
  // Returns the indent level as an indentation count (number of indentations
  // to apply). Clients can translate this into the standard indentation that
  // is being used by the IDE (3 spaces? 1 tab?) and should use the indent
  // prefix string followed by the correct indentation.
  uint32_t IndentLevel;

  SourceCompleteResult() :
      IsComplete(false),
      IndentPrefix(),
      IndentLevel(0) {}
};

SourceCompleteResult
isSourceInputComplete(std::unique_ptr<llvm::MemoryBuffer> MemBuf, SourceFileKind SFKind);
SourceCompleteResult isSourceInputComplete(StringRef Text, SourceFileKind SFKind);

/// Visits all overridden declarations exhaustively from VD, including protocol
/// conformances and clang declarations.
void walkOverriddenDecls(const ValueDecl *VD,
                         llvm::function_ref<void(llvm::PointerUnion<
                             const ValueDecl*, const clang::NamedDecl*>)> Fn);

void collectModuleNames(StringRef SDKPath, std::vector<std::string> &Modules);

struct PlaceholderOccurrence {
  /// The complete placeholder string.
  StringRef FullPlaceholder;
  /// The inner string of the placeholder.
  StringRef PlaceholderContent;
  /// The dollar identifier that was used to replace the placeholder.
  StringRef IdentifierReplacement;
};

/// Replaces Xcode editor placeholders (<#such as this#>) with dollar
/// identifiers and returns a new memory buffer.
///
/// The replacement identifier will be the same size as the placeholder so that
/// the new buffer will have the same size as the input buffer.
std::unique_ptr<llvm::MemoryBuffer>
  replacePlaceholders(std::unique_ptr<llvm::MemoryBuffer> InputBuf,
              llvm::function_ref<void(const PlaceholderOccurrence &)> Callback);

std::unique_ptr<llvm::MemoryBuffer>
  replacePlaceholders(std::unique_ptr<llvm::MemoryBuffer> InputBuf,
                      bool *HadPlaceholder = nullptr);

Optional<std::pair<unsigned, unsigned>> parseLineCol(StringRef LineCol);

class XMLEscapingPrinter : public StreamPrinter {
  public:
  XMLEscapingPrinter(raw_ostream &OS) : StreamPrinter(OS){};
  void printText(StringRef Text) override;
  void printXML(StringRef Text);
};

enum class CursorInfoKind {
  Invalid,
  ValueRef,
  ModuleRef,
  ExprStart,
  StmtStart,
};

/// Base class of more specialized \c ResolvedCursorInfos that also represents
/// and \c Invalid cursor info.
struct ResolvedCursorInfo : public llvm::RefCountedBase<ResolvedCursorInfo> {
protected:
  CursorInfoKind Kind = CursorInfoKind::Invalid;
  SourceFile *SF = nullptr;
  SourceLoc Loc;

protected:
  ResolvedCursorInfo(CursorInfoKind Kind, SourceFile *SF, SourceLoc Loc)
      : Kind(Kind), SF(SF), Loc(Loc) {}

public:
  ResolvedCursorInfo() = default;
  ResolvedCursorInfo(SourceFile *SF) : SF(SF) {}

  CursorInfoKind getKind() const { return Kind; }

  SourceFile *getSourceFile() const { return SF; }

  SourceLoc getLoc() const { return Loc; }
  void setLoc(SourceLoc Loc) { this->Loc = Loc; }

  friend bool operator==(const ResolvedCursorInfo &lhs,
                         const ResolvedCursorInfo &rhs) {
    return lhs.SF == rhs.SF &&
      lhs.Loc.getOpaquePointerValue() == rhs.Loc.getOpaquePointerValue();
  }

  bool isValid() const { return !isInvalid(); }
  bool isInvalid() const { return Kind == CursorInfoKind::Invalid; }
};

typedef llvm::IntrusiveRefCntPtr<ResolvedCursorInfo> ResolvedCursorInfoPtr;

struct ResolvedValueRefCursorInfo : public ResolvedCursorInfo {
private:
  ValueDecl *ValueD = nullptr;
  TypeDecl *CtorTyRef = nullptr;
  ExtensionDecl *ExtTyRef = nullptr;
  bool IsRef = true;
  Type Ty;
  Type ContainerType;
  Optional<std::pair<const CustomAttr *, Decl *>> CustomAttrRef = None;

  bool IsKeywordArgument = false;
  /// It this is a ref, whether it is "dynamic". See \c ide::isDynamicRef.
  bool IsDynamic = false;
  /// If this is a dynamic ref, the types of the base (multiple in the case of
  /// protocol composition).
  SmallVector<NominalTypeDecl *> ReceiverTypes;
  /// Declarations that were shadowed by \c ValueD using a shorthand syntax
  /// that names both the newly declared variable and the referenced variable
  /// by the same identifier in the source text. This includes shorthand
  /// closure captures (`[foo]`) and shorthand if captures
  /// (`if let foo {`). Ordered from innermost to outermost shadows.
  ///
  /// Decls that are shadowed using shorthand syntax should be reported as
  /// additional cursor info results.
  SmallVector<ValueDecl *> ShorthandShadowedDecls;

public:
  ResolvedValueRefCursorInfo() = default;
  explicit ResolvedValueRefCursorInfo(
      SourceFile *SF, SourceLoc Loc, ValueDecl *ValueD, TypeDecl *CtorTyRef,
      ExtensionDecl *ExtTyRef, bool IsRef, Type Ty, Type ContainerType,
      Optional<std::pair<const CustomAttr *, Decl *>> CustomAttrRef,
      bool IsKeywordArgument, bool IsDynamic,
      SmallVector<NominalTypeDecl *> ReceiverTypes,
      SmallVector<ValueDecl *> ShorthandShadowedDecls)
      : ResolvedCursorInfo(CursorInfoKind::ValueRef, SF, Loc), ValueD(ValueD),
        CtorTyRef(CtorTyRef), ExtTyRef(ExtTyRef), IsRef(IsRef), Ty(Ty),
        ContainerType(ContainerType), CustomAttrRef(CustomAttrRef),
        IsKeywordArgument(IsKeywordArgument), IsDynamic(IsDynamic),
        ReceiverTypes(ReceiverTypes),
        ShorthandShadowedDecls(ShorthandShadowedDecls) {}

  ValueDecl *getValueD() const { return ValueD; }

  ExtensionDecl *getExtTyRef() const { return ExtTyRef; }

  TypeDecl *getCtorTyRef() const { return CtorTyRef; }

  bool isRef() const { return IsRef; }

  Type getType() const { return Ty; }

  Type getContainerType() const { return ContainerType; }

  bool isKeywordArgument() const { return IsKeywordArgument; }
  void setIsKeywordArgument(bool IsKeywordArgument) {
    this->IsKeywordArgument = IsKeywordArgument;
  }

  bool isDynamic() const { return this->IsDynamic; }

  ArrayRef<NominalTypeDecl *> getReceiverTypes() const {
    return this->ReceiverTypes;
  }

  ArrayRef<ValueDecl *> getShorthandShadowedDecls() const {
    return this->ShorthandShadowedDecls;
  };
  void setShorthandShadowedDecls(
      const SmallVector<ValueDecl *> &ShorthandShadowedDecls) {
    this->ShorthandShadowedDecls = ShorthandShadowedDecls;
  };

  ValueDecl *typeOrValue() { return CtorTyRef ? CtorTyRef : ValueD; }

  Optional<std::pair<const CustomAttr *, Decl *>> getCustomAttrRef() const {
    return CustomAttrRef;
  }

  static bool classof(const ResolvedCursorInfo *Info) {
    return Info->getKind() == CursorInfoKind::ValueRef;
  }
};

typedef llvm::IntrusiveRefCntPtr<ResolvedValueRefCursorInfo>
    ResolvedValueRefCursorInfoPtr;

struct ResolvedModuleRefCursorInfo : public ResolvedCursorInfo {
private:
  ModuleEntity Mod;

public:
  ResolvedModuleRefCursorInfo(SourceFile *SF, SourceLoc Loc, ModuleEntity Mod)
      : ResolvedCursorInfo(CursorInfoKind::ModuleRef, SF, Loc), Mod(Mod) {}

  ModuleEntity getMod() const { return Mod; }

  static bool classof(const ResolvedCursorInfo *Info) {
    return Info->getKind() == CursorInfoKind::ModuleRef;
  }
};

struct ResolvedExprStartCursorInfo : public ResolvedCursorInfo {
private:
  Expr *TrailingExpr = nullptr;

public:
  ResolvedExprStartCursorInfo(SourceFile *SF, SourceLoc Loc, Expr *TrailingExpr)
      : ResolvedCursorInfo(CursorInfoKind::ExprStart, SF, Loc),
        TrailingExpr(TrailingExpr) {}

  Expr *getTrailingExpr() const { return TrailingExpr; }

  static bool classof(const ResolvedCursorInfo *Info) {
    return Info->getKind() == CursorInfoKind::ExprStart;
  }
};

struct ResolvedStmtStartCursorInfo : public ResolvedCursorInfo {
  Stmt *TrailingStmt = nullptr;

  ResolvedStmtStartCursorInfo(SourceFile *SF, SourceLoc Loc, Stmt *TrailingStmt)
      : ResolvedCursorInfo(CursorInfoKind::StmtStart, SF, Loc),
        TrailingStmt(TrailingStmt) {}

  Stmt *getTrailingStmt() const { return TrailingStmt; }

  static bool classof(const ResolvedCursorInfo *Info) {
    return Info->getKind() == CursorInfoKind::StmtStart;
  }
};

void simple_display(llvm::raw_ostream &out, ResolvedCursorInfoPtr info);

struct UnresolvedLoc {
  SourceLoc Loc;
  bool ResolveArgLocs;
};

enum class LabelRangeType {
  None,
  CallArg,    // foo([a: ]2) or .foo([a: ]String)
  Param,  // func([a b]: Int)
  NoncollapsibleParam, // subscript([a a]: Int)
  Selector,   // #selector(foo.func([a]:))
};

struct ResolvedLoc {
  ASTWalker::ParentTy Node;
  CharSourceRange Range;
  std::vector<CharSourceRange> LabelRanges;
  Optional<unsigned> FirstTrailingLabel;
  LabelRangeType LabelType;
  bool IsActive;
  bool IsInSelector;
};

/// Used by NameMatcher to track parent CallExprs when walking a checked AST.
struct CallingParent {
  Expr *ApplicableTo;
  CallExpr *Call;
};


/// Finds the parse-only AST nodes and corresponding name and param/argument
/// label ranges for a given list of input name start locations
///
/// Resolved locations also indicate the nature of the matched occurrence (e.g.
/// whether it is within active/inactive code, or a selector or string literal).
class NameMatcher: public ASTWalker {
  SourceFile &SrcFile;
  std::vector<UnresolvedLoc> LocsToResolve;
  std::vector<ResolvedLoc> ResolvedLocs;
  ArrayRef<Token> TokensToCheck;

  /// The \c ArgumentList of a parent \c CustomAttr (if one exists) and
  /// the \c SourceLoc of the type name it applies to.
  llvm::Optional<Located<ArgumentList *>> CustomAttrArgList;
  unsigned InactiveConfigRegionNestings = 0;
  unsigned SelectorNestings = 0;

  /// The stack of parent CallExprs and the innermost expression they apply to.
  std::vector<CallingParent> ParentCalls;

  SourceManager &getSourceMgr() const;

  SourceLoc nextLoc() const;
  bool isDone() const { return LocsToResolve.empty(); };
  bool isActive() const { return !InactiveConfigRegionNestings; };
  bool isInSelector() const { return SelectorNestings; };
  bool checkComments();
  void skipLocsBefore(SourceLoc Start);
  bool shouldSkip(Expr *E);
  bool shouldSkip(SourceRange Range);
  bool shouldSkip(CharSourceRange Range);
  bool tryResolve(ASTWalker::ParentTy Node, SourceLoc NameLoc);
  bool tryResolve(ASTWalker::ParentTy Node, DeclNameLoc NameLoc,
                  ArgumentList *Args);
  bool tryResolve(ASTWalker::ParentTy Node, SourceLoc NameLoc, LabelRangeType RangeType,
                  ArrayRef<CharSourceRange> LabelLocs,
                  Optional<unsigned> FirstTrailingLabel);
  bool handleCustomAttrs(Decl *D);
  ArgumentList *getApplicableArgsFor(Expr* E);

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Arguments;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override;
  PostWalkResult<Expr *> walkToExprPost(Expr *E) override;
  PreWalkAction walkToDeclPre(Decl *D) override;
  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override;
  PreWalkAction walkToTypeReprPre(TypeRepr *T) override;
  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override;
  bool shouldWalkIntoGenericParams() override { return true; }
  bool shouldWalkIntoUncheckedMacroDefinitions() override { return true; }

  PreWalkResult<ArgumentList *>
  walkToArgumentListPre(ArgumentList *ArgList) override;

  // FIXME: Remove this
  bool shouldWalkAccessorsTheOldWay() override { return true; }

public:
  explicit NameMatcher(SourceFile &SrcFile) : SrcFile(SrcFile) { }
  std::vector<ResolvedLoc> resolve(ArrayRef<UnresolvedLoc> Locs, ArrayRef<Token> Tokens);
  ResolvedLoc resolve(UnresolvedLoc Loc);
};

enum class RangeKind : int8_t {
  Invalid = -1,
  SingleExpression,
  SingleStatement,
  SingleDecl,

  MultiStatement,
  PartOfExpression,

  MultiTypeMemberDecl,
};

struct DeclaredDecl {
  ValueDecl *VD;
  bool ReferredAfterRange;
  DeclaredDecl(ValueDecl* VD) : VD(VD), ReferredAfterRange(false) {}
  DeclaredDecl(): DeclaredDecl(nullptr) {}
  bool operator==(const DeclaredDecl& other);
};

struct ReferencedDecl {
  ValueDecl *VD;
  Type Ty;
  ReferencedDecl(ValueDecl* VD, Type Ty) : VD(VD), Ty(Ty) {}
  ReferencedDecl() : ReferencedDecl(nullptr, Type()) {}
};

enum class OrphanKind : int8_t {
  None,
  Break,
  Continue,
};

enum class ExitState: int8_t {
  Positive,
  Negative,
  Unsure,
};

struct ReturnInfo {
  TypeBase* ReturnType;
  ExitState Exit;
  ReturnInfo(): ReturnInfo(nullptr, ExitState::Unsure) {}
  ReturnInfo(TypeBase* ReturnType, ExitState Exit):
    ReturnType(ReturnType), Exit(Exit) {}
  ReturnInfo(ASTContext &Ctx, ArrayRef<ReturnInfo> Branches);
};

struct ResolvedRangeInfo {
  RangeKind Kind;
  ReturnInfo ExitInfo;
  ArrayRef<Token> TokensInRange;
  CharSourceRange ContentRange;
  bool HasSingleEntry;
  PossibleEffects UnhandledEffects;
  OrphanKind Orphan;

  // The topmost ast nodes contained in the given range.
  ArrayRef<ASTNode> ContainedNodes;
  ArrayRef<DeclaredDecl> DeclaredDecls;
  ArrayRef<ReferencedDecl> ReferencedDecls;
  DeclContext* RangeContext;
  Expr* CommonExprParent;

  ResolvedRangeInfo(RangeKind Kind, ReturnInfo ExitInfo,
                    ArrayRef<Token> TokensInRange,
                    DeclContext* RangeContext,
                    Expr *CommonExprParent, bool HasSingleEntry,
                    PossibleEffects UnhandledEffects,
                    OrphanKind Orphan, ArrayRef<ASTNode> ContainedNodes,
                    ArrayRef<DeclaredDecl> DeclaredDecls,
                    ArrayRef<ReferencedDecl> ReferencedDecls): Kind(Kind),
                      ExitInfo(ExitInfo),
                      TokensInRange(TokensInRange),
                      ContentRange(calculateContentRange(TokensInRange)),
                      HasSingleEntry(HasSingleEntry),
                      UnhandledEffects(UnhandledEffects),
                      Orphan(Orphan), ContainedNodes(ContainedNodes),
                      DeclaredDecls(DeclaredDecls),
                      ReferencedDecls(ReferencedDecls),
                      RangeContext(RangeContext),
                      CommonExprParent(CommonExprParent) {}
  ResolvedRangeInfo(ArrayRef<Token> TokensInRange) :
  ResolvedRangeInfo(RangeKind::Invalid, {nullptr, ExitState::Unsure},
                    TokensInRange, nullptr, /*Commom Expr Parent*/nullptr,
                    /*Single entry*/true, /*UnhandledEffects*/{},
                    OrphanKind::None, {}, {}, {}) {}
  ResolvedRangeInfo(): ResolvedRangeInfo(ArrayRef<Token>()) {}
  void print(llvm::raw_ostream &OS) const;
  ExitState exit() const { return ExitInfo.Exit; }
  Type getType() const { return ExitInfo.ReturnType; }

  friend bool operator==(const ResolvedRangeInfo &lhs,
                         const ResolvedRangeInfo &rhs) {
    if (lhs.TokensInRange.size() != rhs.TokensInRange.size())
      return false;
    if (lhs.TokensInRange.empty())
      return true;
    return lhs.TokensInRange.front().getLoc() ==
      rhs.TokensInRange.front().getLoc();
  }

private:
  static CharSourceRange calculateContentRange(ArrayRef<Token> Tokens);
};

void simple_display(llvm::raw_ostream &out, const ResolvedRangeInfo &info);

/// This provides a utility to view a printed name by parsing the components
/// of that name. The components include a base name and an array of argument
/// labels.
class DeclNameViewer {
  StringRef BaseName;
  SmallVector<StringRef, 4> Labels;
  bool IsValid;
  bool HasParen;
public:
  DeclNameViewer(StringRef Text);
  DeclNameViewer() : DeclNameViewer(StringRef()) {}
  operator bool() const { return !BaseName.empty(); }
  StringRef base() const { return BaseName; }
  llvm::ArrayRef<StringRef> args() const { return llvm::makeArrayRef(Labels); }
  unsigned argSize() const { return Labels.size(); }
  unsigned partsCount() const { return 1 + Labels.size(); }
  unsigned commonPartsCount(DeclNameViewer &Other) const;
  bool isValid() const { return IsValid; }
  bool isFunction() const { return HasParen; }
};

enum class RegionType {
  Unmatched,
  Mismatch,
  ActiveCode,
  InactiveCode,
  String,
  Selector,
  Comment,
};

enum class RefactoringRangeKind {
  BaseName,                    // func [foo](a b: Int)
  KeywordBaseName,             // [init](a: Int)
  ParameterName,               // func foo(a[ b]: Int)
  NoncollapsibleParameterName, // subscript(a[ a]: Int)
  DeclArgumentLabel,           // func foo([a] b: Int)
  CallArgumentLabel,           // foo([a]: 1)
  CallArgumentColon,           // foo(a[: ]1)
  CallArgumentCombined,        // foo([]1) could expand to foo([a: ]1)
  SelectorArgumentLabel,       // foo([a]:)
};

struct NoteRegion {
  RefactoringRangeKind Kind;

  // The below are relative to the containing Replacement's Text
  unsigned StartLine;
  unsigned StartColumn;
  unsigned EndLine;
  unsigned EndColumn;
  Optional<unsigned> ArgIndex;
};

struct Replacement {
  /// If the edit is outside of the originally request source file, the path
  /// to the file it is editing.
  StringRef Path;
  /// Range to apply the replacement to, zero-width if making an addition.
  CharSourceRange Range;
  /// If the edit is actually a file (which could be generated/from an
  /// expansion), the name (or path) of that buffer.
  StringRef BufferName;
  /// The text to replace \c Range with.
  StringRef Text;
  ArrayRef<NoteRegion> RegionsWorthNote;
};

class SourceEditConsumer {
public:
  virtual void accept(SourceManager &SM, RegionType RegionType, ArrayRef<Replacement> Replacements) = 0;
  virtual ~SourceEditConsumer() = default;
  void accept(SourceManager &SM, CharSourceRange Range, StringRef Text, ArrayRef<NoteRegion> SubRegions = {});
  void accept(SourceManager &SM, SourceLoc Loc, StringRef Text, ArrayRef<NoteRegion> SubRegions = {});
  void insertAfter(SourceManager &SM, SourceLoc Loc, StringRef Text, ArrayRef<NoteRegion> SubRegions = {});
  void accept(SourceManager &SM, Replacement Replacement) { accept(SM, RegionType::ActiveCode, {Replacement}); }
  void remove(SourceManager &SM, CharSourceRange Range);
  void acceptMacroExpansionBuffer(SourceManager &SM, unsigned bufferID,
                                  SourceFile *containingSF,
                                  bool adjustExpansion, bool includeBufferName);
};

/// This helper stream inserts text into a SourceLoc by calling functions in
/// SourceEditorConsumer when it is destroyed.
class EditorConsumerInsertStream: public raw_ostream {
  SourceEditConsumer &Consumer;
  SourceManager &SM;
  CharSourceRange Range;
  llvm::SmallString<64> Buffer;
  llvm::raw_svector_ostream OS;

public:
  explicit EditorConsumerInsertStream(SourceEditConsumer &Consumer,
                                      SourceManager &SM,
                                      CharSourceRange Range):
    Consumer(Consumer), SM(SM), Range(Range), Buffer(), OS(Buffer) {}

  explicit EditorConsumerInsertStream(SourceEditConsumer &Consumer,
                                      SourceManager &SM,
                                      SourceLoc Loc):
    EditorConsumerInsertStream(Consumer, SM, CharSourceRange(Loc, 0)) {}

  ~EditorConsumerInsertStream() {
    Consumer.accept(SM, Range, OS.str());
  }

  void write_impl(const char *ptr, size_t size) override {
    OS.write(ptr, size);
  }
  uint64_t current_pos() const override {
    return OS.tell();
  }
  size_t preferred_buffer_size() const override {
    return 0;
  }
};

/// Outputs replacements as JSON, see `writeEditsInJson`
class SourceEditJsonConsumer : public SourceEditConsumer {
  struct Implementation;
  Implementation &Impl;
public:
  SourceEditJsonConsumer(llvm::raw_ostream &OS);
  ~SourceEditJsonConsumer();
  void accept(SourceManager &SM, RegionType RegionType, ArrayRef<Replacement> Replacements) override;
};

/// Outputs replacements to `OS` in the form
/// ```
/// // </path/to/file> startLine:startCol -> endLine:endCol
/// replacement
/// text
///
/// ```
class SourceEditTextConsumer : public SourceEditConsumer {
  llvm::raw_ostream &OS;

public:
  SourceEditTextConsumer(llvm::raw_ostream &OS) : OS(OS) {}
  void accept(SourceManager &SM, RegionType RegionType,
              ArrayRef<Replacement> Replacements) override;
};

/// Outputs the rewritten buffer to `OS` with RUN and CHECK lines removed
class SourceEditOutputConsumer : public SourceEditConsumer {
  struct Implementation;
  Implementation &Impl;

public:
  SourceEditOutputConsumer(SourceManager &SM, unsigned BufferId, llvm::raw_ostream &OS);
  ~SourceEditOutputConsumer();
  void accept(SourceManager &SM, RegionType RegionType, ArrayRef<Replacement> Replacements) override;
};

/// Broadcasts `accept` to all `Consumers`
class BroadcastingSourceEditConsumer : public SourceEditConsumer {
  ArrayRef<std::unique_ptr<SourceEditConsumer>> Consumers;

public:
  BroadcastingSourceEditConsumer(
      ArrayRef<std::unique_ptr<SourceEditConsumer>> Consumers)
      : Consumers(Consumers) {}
  void accept(SourceManager &SM, RegionType RegionType,
              ArrayRef<Replacement> Replacements) override;
};

enum class LabelRangeEndAt: int8_t {
  BeforeElemStart,
  LabelNameOnly,
};

struct CallArgInfo {
  Expr *ArgExp;
  CharSourceRange LabelRange;
  bool IsTrailingClosure;
  CharSourceRange getEntireCharRange(const SourceManager &SM) const;
};

std::vector<CallArgInfo>
getCallArgInfo(SourceManager &SM, ArgumentList *Args, LabelRangeEndAt EndKind);

// Get the ranges of argument labels from an Arg, either tuple or paren, and
// the index of the first trailing closure argument, if any. This includes empty
// ranges for any unlabelled arguments, including the first trailing closure.
std::pair<std::vector<CharSourceRange>, Optional<unsigned>>
getCallArgLabelRanges(SourceManager &SM, ArgumentList *Args,
                      LabelRangeEndAt EndKind);

/// Whether a decl is defined from clang source.
bool isFromClang(const Decl *D);

/// Retrieve the effective Clang node for the given declaration, which
/// copes with the odd case of imported Error enums.
ClangNode getEffectiveClangNode(const Decl *decl);

/// Retrieve the Clang node for the given extension, if it has one.
ClangNode extensionGetClangNode(const ExtensionDecl *ext);

/// Utility for finding the referenced declaration from a call, which might
/// include a second level of function application for a 'self.' expression,
/// or a curry thunk, etc. If \p semantic is true then the underlying semantic
/// expression of \p expr is used.
std::pair<Type, ConcreteDeclRef> getReferencedDecl(Expr *expr,
                                                   bool semantic = true);

/// Whether the last expression in \p ExprStack is being called.
bool isBeingCalled(ArrayRef<Expr*> ExprStack);

/// The base of the last expression in \p ExprStack (which may look up the
/// stack in eg. the case of a `DotSyntaxCallExpr`).
Expr *getBase(ArrayRef<Expr *> ExprStack);

/// Returns whether or not \p D could be overridden, eg. it's a member of a
/// protocol, a non-final method in a class, etc.
bool isDeclOverridable(ValueDecl *D);

/// Given a reference to a member \p D and its \p Base expression, return
/// whether that declaration could be dynamic, ie. may resolve to some other
/// declaration. Note that while the decl itself itself may be overridable, a
/// reference to it is not necessarily "dynamic". Furthermore,  is *not* the
/// `dynamic` keyword.
///
/// A simple example is `SomeType.classMethod()`. `classMethod`
/// is itself overridable, but that particular reference to it *has* to be the
/// one in `SomeType`. Contrast that to `type(of: foo).classMethod()` where
/// `classMethod` could be any `classMethod` up or down the hierarchy from the
/// type of the \p Base expression.
bool isDynamicRef(Expr *Base, ValueDecl *D, llvm::function_ref<Type(Expr *)> getType = [](Expr *E) { return E->getType(); });

/// Adds the resolved nominal types of \p Base to \p Types.
void getReceiverType(Expr *Base,
                     SmallVectorImpl<NominalTypeDecl *> &Types);

} // namespace ide
} // namespace swift

#endif // SWIFT_IDE_UTILS_H

