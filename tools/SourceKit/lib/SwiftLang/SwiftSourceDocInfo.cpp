//===--- SwiftSourceDocInfo.cpp -------------------------------------------===//
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

#include "SourceKit/Support/FileSystemProvider.h"
#include "SourceKit/Support/ImmutableTextBuffer.h"
#include "SourceKit/Support/Logging.h"
#include "SourceKit/Support/UIdent.h"
#include "SwiftASTManager.h"
#include "SwiftEditorDiagConsumer.h"
#include "SwiftLangSupport.h"

#include "swift/AST/ASTDemangler.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/LookupKinds.h"
#include "swift/AST/ModuleNameLookup.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SwiftNameTranslation.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CommentConversion.h"
#include "swift/IDE/IDERequests.h"
#include "swift/IDE/ModuleInterfacePrinting.h"
#include "swift/Refactoring/Refactoring.h"
#include "swift/IDE/SourceEntityWalker.h"
#include "swift/IDE/Utils.h"
#include "swift/Markup/XMLUtils.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/SymbolGraphGen/SymbolGraphGen.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"

#include "llvm/Support/MemoryBuffer.h"

#include <numeric>

using namespace SourceKit;
using namespace swift;
using namespace swift::ide;

namespace {
class AnnotatedDeclarationPrinter : public XMLEscapingPrinter {
public:
  AnnotatedDeclarationPrinter(raw_ostream &OS)
    :XMLEscapingPrinter(OS) { }

private:
  void printTypeRef(
      Type T, const TypeDecl *TD, Identifier Name,
      PrintNameContext NameContext = PrintNameContext::Normal) override {
    printXML("<Type usr=\"");
    SwiftLangSupport::printUSR(TD, OS);
    printXML("\">");
    StreamPrinter::printTypeRef(T, TD, Name, NameContext);
    printXML("</Type>");
  }
};
} // end anonymous namespace

static StringRef getTagForDecl(const Decl *D, bool isRef) {
  auto UID = SwiftLangSupport::getUIDForDecl(D, isRef);
  static const char *prefix = "source.lang.swift.";
  assert(UID.getName().startswith(prefix));
  return UID.getName().drop_front(strlen(prefix));
}

static StringRef ExternalParamNameTag = "decl.var.parameter.argument_label";
static StringRef LocalParamNameTag = "decl.var.parameter.name";
static StringRef GenericParamNameTag = "decl.generic_type_param.name";
static StringRef SyntaxKeywordTag = "syntaxtype.keyword";

static StringRef getTagForParameter(PrintStructureKind context) {
  switch (context) {
  case PrintStructureKind::FunctionParameter:
    return "decl.var.parameter";
  case PrintStructureKind::FunctionReturnType:
    return "decl.function.returntype";
  case PrintStructureKind::FunctionType:
    return "";
  case PrintStructureKind::TupleType:
    return "tuple";
  case PrintStructureKind::TupleElement:
    return "tuple.element";
  case PrintStructureKind::GenericParameter:
    return "decl.generic_type_param";
  case PrintStructureKind::GenericRequirement:
    return "decl.generic_type_requirement";
  case PrintStructureKind::BuiltinAttribute:
    return "syntaxtype.attribute.builtin";
  case PrintStructureKind::NumberLiteral:
    return "syntaxtype.number";
  case PrintStructureKind::StringLiteral:
    return "syntaxtype.string";
  case PrintStructureKind::DefaultArgumentClause:
  case PrintStructureKind::DeclGenericParameterClause:
  case PrintStructureKind::DeclGenericRequirementClause:
  case PrintStructureKind::EffectsSpecifiers:
  case PrintStructureKind::DeclResultTypeClause:
  case PrintStructureKind::FunctionParameterList:
  case PrintStructureKind::FunctionParameterType:
    // These kinds are ignored by 'isIgnoredPrintStructureKind()'
    llvm_unreachable("ignored structure kind");
  }
  llvm_unreachable("unexpected structure kind");
}

static StringRef getDeclNameTagForDecl(const Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Param:
    // When we're examining the parameter itself, it is the local name that is
    // the name of the variable.
    return LocalParamNameTag;
  case DeclKind::GenericTypeParam:
    return ""; // Handled by printName.
  case DeclKind::Constructor:
  case DeclKind::Destructor:
  case DeclKind::Subscript:
    // The names 'init'/'deinit'/'subscript' are actually keywords.
    return SyntaxKeywordTag;
  default:
    return "decl.name";
  }
}

namespace {
/// A typesafe union of contexts that the printer can be inside.
/// Currently: Decl, PrintStructureKind
class PrintContext {
  // Use the low bit to determine the type; store the enum value shifted left
  // to leave the low bit free.
  const uintptr_t value;
  static constexpr unsigned declTag = 0;
  static constexpr unsigned PrintStructureKindTag = 1;
  static constexpr unsigned typeTag = 2;
  static constexpr unsigned tagMask = 3;
  static constexpr unsigned tagShift = 2;
  bool hasTag(unsigned tag) const { return (value & tagMask) == tag; }

public:
  PrintContext(const Decl *D) : value(uintptr_t(D)) {
    static_assert(llvm::PointerLikeTypeTraits<Decl *>::NumLowBitsAvailable >=
                      tagShift,
                  "missing spare bit in Decl *");
  }
  PrintContext(PrintStructureKind K)
      : value((uintptr_t(K) << tagShift) | PrintStructureKindTag) {}
  PrintContext(TypeLoc unused) : value(typeTag) {}

  /// Get the context as a Decl, or nullptr.
  const Decl *getDecl() const {
    return hasTag(declTag) ? (const Decl *)value : nullptr;
  }
  /// Get the context as a PrintStructureKind, or None.
  Optional<PrintStructureKind> getPrintStructureKind() const {
    if (!hasTag(PrintStructureKindTag))
      return None;
    return PrintStructureKind(value >> tagShift);
  }
  /// Whether this is a PrintStructureKind context of the given \p kind.
  bool is(PrintStructureKind kind) const {
    auto storedKind = getPrintStructureKind();
    return storedKind && *storedKind == kind;
  }
  bool isType() const { return hasTag(typeTag); }
};

/// An ASTPrinter for annotating declarations with XML tags that describe the
/// key substructure of the declaration for CursorInfo/DocInfo.
///
/// Prints declarations with decl- and type-specific tags derived from the
/// UIDs used for decl/refs. For example (including newlines purely for ease of
/// reading):
///
/// \verbatim
///   <decl.function.free>
///     func <decl.name>foo</decl.name>
///     (
///     <decl.var.parameter>
///       <decl.var.parameter.name>x</decl.var.parameter.name>:
///       <ref.struct usr="Si">Int</ref.struct>
///     </decl.var.parameter>
///     ) -> <decl.function.returntype>
///            <ref.struct usr="Si">Int</ref.struct></decl.function.returntype>
///  </decl.function.free>
/// \endverbatim
class FullyAnnotatedDeclarationPrinter final : public XMLEscapingPrinter {
public:
  FullyAnnotatedDeclarationPrinter(raw_ostream &OS) : XMLEscapingPrinter(OS) {}

private:

  // MARK: The ASTPrinter callback interface.

  void printDeclPre(const Decl *D, Optional<BracketOptions> Bracket) override {
    contextStack.emplace_back(PrintContext(D));
    openTag(getTagForDecl(D, /*isRef=*/false));
  }
  void printDeclPost(const Decl *D, Optional<BracketOptions> Bracket) override {
    assert(contextStack.back().getDecl() == D && "unmatched printDeclPre");
    contextStack.pop_back();
    closeTag(getTagForDecl(D, /*isRef=*/false));
  }

  void printDeclLoc(const Decl *D) override {
    auto tag = getDeclNameTagForDecl(D);
    if (!tag.empty())
      openTag(tag);
  }
  void printDeclNameEndLoc(const Decl *D) override {
    auto tag = getDeclNameTagForDecl(D);
    if (!tag.empty())
      closeTag(tag);
  }

  void printTypePre(const TypeLoc &TL) override {
    auto tag = getTypeTagForCurrentContext();
    contextStack.emplace_back(PrintContext(TL));
    if (!tag.empty())
      openTag(tag);
  }
  void printTypePost(const TypeLoc &TL) override {
    assert(contextStack.back().isType());
    contextStack.pop_back();
    auto tag = getTypeTagForCurrentContext();
    if (!tag.empty())
      closeTag(tag);
  }

  bool isIgnoredPrintStructureKind(PrintStructureKind kind) {
    switch (kind) {
    case PrintStructureKind::DefaultArgumentClause:
    case PrintStructureKind::DeclGenericParameterClause:
    case PrintStructureKind::DeclGenericRequirementClause:
    case PrintStructureKind::EffectsSpecifiers:
    case PrintStructureKind::DeclResultTypeClause:
    case PrintStructureKind::FunctionParameterList:
    case PrintStructureKind::FunctionParameterType:
      return true;
    default:
      return false;
    }
  }

  void printStructurePre(PrintStructureKind kind, const Decl *D) override {
    if (isIgnoredPrintStructureKind(kind))
      return;
    if (kind == PrintStructureKind::TupleElement ||
        kind == PrintStructureKind::TupleType)
      fixupTuple(kind);

    contextStack.emplace_back(PrintContext(kind));
    auto tag = getTagForParameter(kind);
    if (tag.empty())
      return;

    if (D && kind == PrintStructureKind::GenericParameter) {
      assert(isa<ValueDecl>(D) && "unexpected non-value decl for param");
      openTagWithUSRForDecl(tag, cast<ValueDecl>(D));
    } else {
      openTag(tag);
    }
  }
  void printStructurePost(PrintStructureKind kind, const Decl *D) override {
    if (isIgnoredPrintStructureKind(kind))
      return;
    if (kind == PrintStructureKind::TupleElement ||
        kind == PrintStructureKind::TupleType) {
      auto prev = contextStack.pop_back_val();
      (void)prev;
      fixupTuple(kind);
      assert(prev.is(kind) && "unmatched printStructurePre");
    } else {
      assert(contextStack.back().is(kind) && "unmatched printStructurePre");
      contextStack.pop_back();
    }

    auto tag = getTagForParameter(kind);
    if (!tag.empty())
      closeTag(tag);
  }

  void printNamePre(PrintNameContext context) override {
    auto tag = getTagForPrintNameContext(context);
    if (!tag.empty())
      openTag(tag);
  }
  void printNamePost(PrintNameContext context) override {
    auto tag = getTagForPrintNameContext(context);
    if (!tag.empty())
      closeTag(tag);
  }

  void printTypeRef(
      Type T, const TypeDecl *TD, Identifier name,
      PrintNameContext NameContext = PrintNameContext::Normal) override {
    auto tag = getTagForDecl(TD, /*isRef=*/true);
    openTagWithUSRForDecl(tag, TD);
    insideRef = true;
    XMLEscapingPrinter::printTypeRef(T, TD, name, NameContext);
    insideRef = false;
    closeTag(tag);
  }

  // MARK: Convenience functions for printing.

  void openTag(StringRef tag) { OS << "<" << tag << ">"; }
  void closeTag(StringRef tag) { OS << "</" << tag << ">"; }

  void openTagWithUSRForDecl(StringRef tag, const ValueDecl *VD) {
    OS << "<" << tag << " usr=\"";
    SwiftLangSupport::printUSR(VD, OS);
    OS << "\">";
  }

  // MARK: Misc.

  StringRef getTypeTagForCurrentContext() const {
    if (contextStack.empty())
      return "";

    static StringRef parameterTypeTag = "decl.var.parameter.type";
    static StringRef genericParamTypeTag = "decl.generic_type_param.constraint";

    auto context = contextStack.back();
    if (context.is(PrintStructureKind::FunctionParameter))
      return parameterTypeTag;
    if (context.is(PrintStructureKind::GenericParameter))
      return genericParamTypeTag;
    if (context.is(PrintStructureKind::TupleElement))
      return "tuple.element.type";
    if (context.getPrintStructureKind().has_value() || context.isType())
      return "";

    assert(context.getDecl() && "unexpected context kind");
    switch (context.getDecl()->getKind()) {
    case DeclKind::Param:
      return parameterTypeTag;
    case DeclKind::GenericTypeParam:
      return genericParamTypeTag;
    case DeclKind::Var:
      return "decl.var.type";
    case DeclKind::Subscript:
    case DeclKind::Func:
    default:
      return "";
    }
  }

  StringRef getTagForPrintNameContext(PrintNameContext context) {
    if (insideRef)
      return "";

    bool insideParam =
        !contextStack.empty() &&
        contextStack.back().is(PrintStructureKind::FunctionParameter);

    switch (context) {
    case PrintNameContext::FunctionParameterExternal:
      return ExternalParamNameTag;
    case PrintNameContext::FunctionParameterLocal:
      return LocalParamNameTag;
    case PrintNameContext::TupleElement:
      if (insideParam)
        return ExternalParamNameTag;
      return "tuple.element.argument_label";
    case PrintNameContext::Keyword:
    case PrintNameContext::IntroducerKeyword:
      return SyntaxKeywordTag;
    case PrintNameContext::GenericParameter:
      return GenericParamNameTag;
    case PrintNameContext::Attribute:
      return "syntaxtype.attribute.name";
    default:
      return "";
    }
  }

  /// 'Fix' a tuple or tuple element structure kind to be a function parameter
  /// or function type if we are currently inside a function type. This
  /// simplifies functions that need to differentiate a tuple from the input
  /// part of a function type.
  void fixupTuple(PrintStructureKind &kind) {
    assert(kind == PrintStructureKind::TupleElement ||
           kind == PrintStructureKind::TupleType);
    // Skip over 'type's in the context stack.
    for (auto I = contextStack.rbegin(), E = contextStack.rend(); I != E; ++I) {
      if (I->is(PrintStructureKind::FunctionType)) {
        if (kind == PrintStructureKind::TupleElement)
          kind = PrintStructureKind::FunctionParameter;
        else
          kind = PrintStructureKind::FunctionType;
        break;
      } else if (!I->isType()) {
        break;
      }
    }
  }

private:
  /// A stack of contexts being printed, used to determine the context for
  /// subsequent ASTPrinter callbacks.
  llvm::SmallVector<PrintContext, 3> contextStack;
  bool insideRef = false;
};
} // end anonymous namespace

static Type findBaseTypeForReplacingArchetype(const ValueDecl *VD, const Type Ty) {
  if (Ty.isNull())
    return Type();

  // Find the nominal type decl related to VD.
  if (!VD->getDeclContext()->isTypeContext())
    return Type();

  return Ty->getRValueType()->getInOutObjectType()->getMetatypeInstanceType();
}

static void printAnnotatedDeclaration(const ValueDecl *VD,
                                      const Type BaseTy,
                                      raw_ostream &OS) {
  AnnotatedDeclarationPrinter Printer(OS);
  PrintOptions PO = PrintOptions::printQuickHelpDeclaration();
  if (BaseTy) {
    PO.setBaseType(BaseTy);
    PO.PrintAsMember = true;
  }

  // If it's implicit, try to find an overridden ValueDecl that's not implicit.
  // This will ensure we can properly annotate TypeRepr with a usr
  // in AnnotatedDeclarationPrinter.
  while (VD->isImplicit() && VD->getOverriddenDecl())
    VD = VD->getOverriddenDecl();

  // VD may be a compiler synthesized member, constructor, or shorthand argument
  // so always print it even if it's implicit.
  //
  // FIXME: Update PrintOptions::printQuickHelpDeclaration to print implicit
  // decls by default. That causes issues due to newlines being printed before
  // implicit OpaqueTypeDecls at time of writing.
  PO.TreatAsExplicitDeclList.push_back(VD);

  // Wrap this up in XML, as that's what we'll use for documentation comments.
  OS<<"<Declaration>";
  VD->print(Printer, PO);
  OS<<"</Declaration>";
}

void SwiftLangSupport::printFullyAnnotatedDeclaration(const ValueDecl *VD,
                                                      Type BaseTy,
                                                      raw_ostream &OS) {
  FullyAnnotatedDeclarationPrinter Printer(OS);
  PrintOptions PO = PrintOptions::printQuickHelpDeclaration();
  if (BaseTy) {
    PO.setBaseType(BaseTy);
    PO.PrintAsMember = true;
  }

  // If it's implicit, try to find an overridden ValueDecl that's not implicit.
  // This will ensure we can properly annotate TypeRepr with a usr
  // in AnnotatedDeclarationPrinter.
  while (VD->isImplicit() && VD->getOverriddenDecl())
    VD = VD->getOverriddenDecl();

  // VD may be a compiler synthesized member, constructor, or shorthand argument
  // so always print it even if it's implicit.
  //
  // FIXME: Update PrintOptions::printQuickHelpDeclaration to print implicit
  // decls by default. That causes issues due to newlines being printed before
  // implicit OpaqueTypeDecls at time of writing.
  PO.TreatAsExplicitDeclList.push_back(VD);

  VD->print(Printer, PO);
}

void SwiftLangSupport::printFullyAnnotatedDeclaration(const ExtensionDecl *ED,
                                                      raw_ostream &OS) {
  FullyAnnotatedDeclarationPrinter Printer(OS);
  PrintOptions PO = PrintOptions::printQuickHelpDeclaration();
  ED->print(Printer, PO);
}

void SwiftLangSupport::printFullyAnnotatedSynthesizedDeclaration(
    const swift::ValueDecl *VD, TypeOrExtensionDecl Target,
    llvm::raw_ostream &OS) {
  FullyAnnotatedDeclarationPrinter Printer(OS);
  PrintOptions PO = PrintOptions::printQuickHelpDeclaration();
  PO.initForSynthesizedExtension(Target);
  PO.PrintAsMember = true;
  VD->print(Printer, PO);
}

void SwiftLangSupport::printFullyAnnotatedSynthesizedDeclaration(
    const swift::ExtensionDecl *ED, TypeOrExtensionDecl Target,
    llvm::raw_ostream &OS) {
  FullyAnnotatedDeclarationPrinter Printer(OS);
  PrintOptions PO = PrintOptions::printQuickHelpDeclaration();
  PO.initForSynthesizedExtension(Target);
  ED->print(Printer, PO);
}

template <typename FnTy>
static void walkRelatedDecls(const ValueDecl *VD, const FnTy &Fn) {
  if (isa<ParamDecl>(VD))
    return; // Parameters don't have interesting related declarations.

  auto &ctx = VD->getASTContext();

  llvm::SmallDenseMap<DeclName, unsigned, 16> NamesSeen;
  ++NamesSeen[VD->getName()];


  auto *DC = VD->getDeclContext();
  bool typeLookup = DC->isTypeContext();

  SmallVector<ValueDecl *, 4> results;

  if (typeLookup) {
    auto type = DC->getDeclaredInterfaceType();
    if (!type->is<ErrorType>()) {
      DC->lookupQualified(type, DeclNameRef(VD->getBaseName()),
                          NL_QualifiedDefault, results);
    }
  } else {
    namelookup::lookupInModule(DC->getModuleScopeContext(),
                               VD->getBaseName(), results,
                               NLKind::UnqualifiedLookup,
                               namelookup::ResolutionKind::Overloadable,
                               DC->getModuleScopeContext(),
                               NL_UnqualifiedDefault);
  }

  SmallVector<ValueDecl *, 8> RelatedDecls;
  for (auto result : results) {
    if (result->getAttrs().isUnavailable(ctx))
      continue;

    if (result != VD) {
      ++NamesSeen[result->getName()];
      RelatedDecls.push_back(result);
    }
  }

  // Now provide the results along with whether the name is duplicate or not.
  for (auto result : RelatedDecls)
    Fn(result, typeLookup, NamesSeen[result->getName()] > 1);
}

//===----------------------------------------------------------------------===//
// SwiftLangSupport::getCursorInfo
//===----------------------------------------------------------------------===//

static StringRef getSourceToken(unsigned Offset,
                                ImmutableTextSnapshotRef Snap) {
  auto MemBuf = Snap->getBuffer()->getInternalBuffer();

  // FIXME: Invalid offset shouldn't reach here.
  if (Offset >= MemBuf->getBufferSize())
    return StringRef();

  SourceManager SM;
  auto MemBufRef = llvm::MemoryBuffer::getMemBuffer(MemBuf->getBuffer(),
                                                 MemBuf->getBufferIdentifier());
  auto BufId = SM.addNewSourceBuffer(std::move(MemBufRef));
  SourceLoc Loc = SM.getLocForOffset(BufId, Offset);
  return Lexer::getTokenAtLocation(SM, Loc).getText();
}

static llvm::Optional<unsigned>
mapOffsetToOlderSnapshot(unsigned Offset,
                         ImmutableTextSnapshotRef NewSnap,
                         ImmutableTextSnapshotRef OldSnap) {
  SmallVector<ReplaceImmutableTextUpdateRef, 16> Updates;
  OldSnap->foreachReplaceUntil(NewSnap,
    [&](ReplaceImmutableTextUpdateRef Upd)->bool {
      Updates.push_back(Upd);
      return true;
    });

  // Walk the updates backwards and "undo" them.
  for (auto I = Updates.rbegin(), E = Updates.rend(); I != E; ++I) {
    auto Upd = *I;
    if (Upd->getByteOffset() <= Offset &&
        Offset < Upd->getByteOffset() + Upd->getText().size())
      return None; // Offset is part of newly inserted text.

    if (Upd->getByteOffset() <= Offset) {
      Offset += Upd->getLength(); // "bring back" what was removed.
      Offset -= Upd->getText().size(); // "remove" what was added.
    }
  }
  return Offset;
}

static llvm::Optional<unsigned>
mapOffsetToNewerSnapshot(unsigned Offset,
                         ImmutableTextSnapshotRef OldSnap,
                         ImmutableTextSnapshotRef NewSnap) {
  bool Completed = OldSnap->foreachReplaceUntil(NewSnap,
    [&](ReplaceImmutableTextUpdateRef Upd)->bool {
      if (Upd->getByteOffset() <= Offset &&
          Offset < Upd->getByteOffset() + Upd->getLength())
        return false; // Offset is part of removed text.

      if (Upd->getByteOffset() <= Offset) {
        Offset += Upd->getText().size();
        Offset -= Upd->getLength();
      }
      return true;
    });

  if (Completed)
    return Offset;
  return None;
}

/// Tries to remap the location from a previous snapshot to the latest one and
/// then sets the location's line and column.
static void mapLocToLatestSnapshot(
    SwiftLangSupport &Lang, LocationInfo &Location,
    ArrayRef<ImmutableTextSnapshotRef> PreviousASTSnaps) {
  auto EditorDoc = Lang.getEditorDocuments()->findByPath(Location.Filename,
                                                         /*IsRealpath=*/true);
  if (!EditorDoc)
    return;

  ImmutableTextSnapshotRef LatestSnap = EditorDoc->getLatestSnapshot();
  if (!LatestSnap)
    return;

  for (auto &PrevSnap : PreviousASTSnaps) {
    if (PrevSnap->isFromSameBuffer(LatestSnap)) {
      if (PrevSnap->getStamp() == LatestSnap->getStamp())
        break;

      auto OptBegin = mapOffsetToNewerSnapshot(Location.Offset,
                                               PrevSnap, LatestSnap);
      if (!OptBegin.has_value()) {
        Location.Filename = StringRef();
        return;
      }

      auto OptEnd = mapOffsetToNewerSnapshot(Location.Offset +
                                             Location.Length,
                                             PrevSnap, LatestSnap);
      if (!OptEnd.has_value()) {
        Location.Filename = StringRef();
        return;
      }

      Location.Offset = *OptBegin;
      Location.Length = *OptEnd - *OptBegin;
    }
  }

  std::tie(Location.Line, Location.Column) =
      LatestSnap->getBuffer()->getLineAndColumn(Location.Offset);
}


/// Returns true for error.
static bool passCursorInfoForModule(ModuleEntity Mod,
                                    SwiftInterfaceGenMap &IFaceGenContexts,
                                    const CompilerInvocation &Invok,
                       std::function<void(const RequestResult<CursorInfoData> &)> Receiver) {
  std::string FullName = Mod.getFullName();
  SmallVector<CursorSymbolInfo, 1> Symbols;
  SmallVector<StringRef, 4> ModuleGroups;

  CursorSymbolInfo &Symbol = Symbols.emplace_back();
  Symbol.Kind = SwiftLangSupport::getUIDForModuleRef();
  Symbol.Name = Mod.getName();
  Symbol.ModuleName = FullName;
  if (auto IFaceGenRef = IFaceGenContexts.find(Symbol.ModuleName, Invok))
    Symbol.ModuleInterfaceName = IFaceGenRef->getDocumentName();
  Symbol.IsSystem = Mod.isNonUserModule();
  if (auto MD = Mod.getAsSwiftModule()) {
    ide::collectModuleGroups(const_cast<ModuleDecl *>(MD), ModuleGroups);
    Symbol.ModuleGroupArray = llvm::makeArrayRef(ModuleGroups);
  }

  CursorInfoData Data;
  Data.Symbols = Symbols;
  Receiver(RequestResult<CursorInfoData>::fromResult(Data));
  return false;
}

static void addRefactorings(
    SmallVectorImpl<RefactoringInfo> &intoLangInfos,
    const SmallVectorImpl<RefactorAvailabilityInfo> &availableInfos) {
  for (auto info : availableInfos) {
    auto uid = SwiftLangSupport::getUIDForRefactoringKind(info.Kind);
    bool hasRefactoringWithSameUID =
        llvm::any_of(intoLangInfos, [&](RefactoringInfo &existing) {
          return uid == existing.Kind;
        });
    if (hasRefactoringWithSameUID)
      continue;

    intoLangInfos.emplace_back(
        uid, ide::getDescriptiveRefactoringKindName(info.Kind),
        ide::getDescriptiveRenameUnavailableReason(info.AvailableKind));
  }
}

static Optional<unsigned>
getParamParentNameOffset(const ValueDecl *VD, SourceLoc Cursor) {
  if (Cursor.isInvalid())
    return None;
  SourceLoc Loc;
  if (auto PD = dyn_cast<ParamDecl>(VD)) {

    // Avoid returning parent loc for internal-only names.
    if (PD->getArgumentNameLoc().isValid() && PD->getArgumentNameLoc() != Cursor)
      return None;
    auto *DC = PD->getDeclContext();
    switch (DC->getContextKind()) {
      case DeclContextKind::SubscriptDecl:
        Loc = cast<SubscriptDecl>(DC)->getNameLoc();
        break;
      case DeclContextKind::AbstractFunctionDecl:
        Loc = cast<AbstractFunctionDecl>(DC)->getNameLoc();
        break;
      default:
        break;
    }
  }
  if (Loc.isInvalid())
    return None;
  auto &SM = VD->getASTContext().SourceMgr;
  return SM.getLocOffsetInBuffer(Loc, SM.findBufferContainingLoc(Loc));
}

static StringRef getModuleName(const ValueDecl *VD,
                               llvm::BumpPtrAllocator &Allocator) {
  ASTContext &Ctx = VD->getASTContext();
  ClangImporter *Importer =
      static_cast<ClangImporter *>(Ctx.getClangModuleLoader());
  if (auto ClangNode = VD->getClangNode()) {
    if (const auto *ClangMod = Importer->getClangOwningModule(ClangNode))
      return StringRef(ClangMod->getFullModuleName()).copy(Allocator);
    return "";
  }

  ModuleDecl *MD = VD->getModuleContext();
  // If the decl is from a cross-import overlay module, report the
  // overlay's declaring module as the owning module.
  if (ModuleDecl *Declaring = MD->getDeclaringModuleIfCrossImportOverlay())
    MD = Declaring;
  return MD->getNameStr();
}

struct DeclInfo {
  const ValueDecl *VD;
  Type ContainerType;
  bool IsRef;
  bool IsDynamic;
  ArrayRef<NominalTypeDecl *> ReceiverTypes;

  /// If VD is a synthesized property wrapper backing storage (_foo) or
  /// projected value ($foo) of a property (foo), the property instead.
  /// Otherwise, VD.
  const ValueDecl *OriginalProperty = nullptr;
  bool Unavailable = true;
  Type BaseType;
  /// Whether the \c VD is in a synthesized extension of \c BaseType
  bool InSynthesizedExtension = false;

  DeclInfo(const ValueDecl *VD, Type ContainerType, bool IsRef, bool IsDynamic,
           ArrayRef<NominalTypeDecl *> ReceiverTypes,
           const CompilerInvocation &Invoc)
      : VD(VD), ContainerType(ContainerType), IsRef(IsRef),
        IsDynamic(IsDynamic), ReceiverTypes(ReceiverTypes) {
    if (VD == nullptr)
      return;

    // The synthesized properties $foo and _foo aren't unavailable even if
    // the original property foo is, so check them rather than the original
    // property.
    Unavailable = AvailableAttr::isUnavailable(VD);
    // No point computing the rest since they won't be used anyway.
    if (Unavailable)
      return;

    OriginalProperty = VD;
    if (auto *VarD = dyn_cast<VarDecl>(VD)) {
      if (auto *Wrapped = VarD->getOriginalWrappedProperty())
        OriginalProperty = Wrapped;
    }

    BaseType = findBaseTypeForReplacingArchetype(VD, ContainerType);
    if (BaseType) {
      if (auto *Target = BaseType->getAnyNominal()) {
        SynthesizedExtensionAnalyzer Analyzer(
            Target, PrintOptions::printModuleInterface(
                        Invoc.getFrontendOptions().PrintFullConvention));
        InSynthesizedExtension = Analyzer.isInSynthesizedExtension(VD);
      }
    }
  }
};

static StringRef copyAndClearString(llvm::BumpPtrAllocator &Allocator,
                                    SmallVectorImpl<char> &Str) {
  auto Ref = StringRef(Str.data(), Str.size()).copy(Allocator);
  Str.clear();
  return Ref;
}

template <typename T>
static ArrayRef<T> copyAndClearArray(llvm::BumpPtrAllocator &Allocator,
                                     SmallVectorImpl<T> &Array) {
  auto Ref = copyArray(Allocator, llvm::makeArrayRef(Array));
  Array.clear();
  return Ref;
}

static void setLocationInfoForClangNode(ClangNode ClangNode,
                                        ClangImporter *Importer,
                                        LocationInfo &Location) {
  clang::ASTContext &ClangCtx = Importer->getClangASTContext();
  clang::SourceManager &ClangSM = ClangCtx.getSourceManager();

  clang::SourceRange SR = ClangNode.getLocation();
  if (auto MD =
          dyn_cast_or_null<clang::ObjCMethodDecl>(ClangNode.getAsDecl())) {
    SR = clang::SourceRange(MD->getSelectorStartLoc(),
                            MD->getDeclaratorEndLoc());
  }

  clang::CharSourceRange CharRange =
      clang::Lexer::makeFileCharRange(clang::CharSourceRange::getTokenRange(SR),
                                      ClangSM, ClangCtx.getLangOpts());
  if (CharRange.isInvalid())
    return;

  std::pair<clang::FileID, unsigned> Decomp =
      ClangSM.getDecomposedLoc(CharRange.getBegin());
  if (!Decomp.first.isInvalid()) {
    if (auto FE = ClangSM.getFileEntryForID(Decomp.first)) {
      Location.Filename = FE->getName();

      std::pair<clang::FileID, unsigned> EndDecomp =
          ClangSM.getDecomposedLoc(CharRange.getEnd());

      Location.Offset = Decomp.second;
      Location.Length = EndDecomp.second - Decomp.second;
      Location.Line = ClangSM.getLineNumber(Decomp.first, Decomp.second);
      Location.Column = ClangSM.getColumnNumber(Decomp.first, Decomp.second);
    }
  }
}

static unsigned getCharLength(SourceManager &SM, SourceRange TokenRange) {
  SourceLoc CharEndLoc = Lexer::getLocForEndOfToken(SM, TokenRange.End);
  return SM.getByteDistance(TokenRange.Start, CharEndLoc);
}

static void setLocationInfo(const ValueDecl *VD,
                            LocationInfo &Location) {
  ASTContext &Ctx = VD->getASTContext();
  SourceManager &SM = Ctx.SourceMgr;

  auto ClangNode = VD->getClangNode();

  auto Loc = VD->getLoc(/*SerializedOK=*/true);
  if (Loc.isValid()) {
    auto getSignatureRange = [&](const ValueDecl *VD) -> Optional<unsigned> {
      if (auto FD = dyn_cast<AbstractFunctionDecl>(VD)) {
        SourceRange R = FD->getSignatureSourceRange();
        if (R.isValid())
          return getCharLength(SM, R);
      }
      return None;
    };
    unsigned NameLen;
    if (auto SigLen = getSignatureRange(VD)) {
      NameLen = SigLen.value();
    } else if (VD->hasName()) {
      NameLen = VD->getBaseName().userFacingName().size();
    } else {
      NameLen = getCharLength(SM, Loc);
    }

    auto [DeclBufID, DeclLoc] =
        VD->getModuleContext()->getOriginalLocation(Loc);
    Location.Filename = SM.getIdentifierForBuffer(DeclBufID);
    Location.Offset = SM.getLocOffsetInBuffer(DeclLoc, DeclBufID);
    Location.Length = NameLen;
    std::tie(Location.Line, Location.Column) =
        SM.getLineAndColumnInBuffer(DeclLoc, DeclBufID);
    if (auto GeneratedSourceInfo = SM.getGeneratedSourceInfo(DeclBufID)) {
      if (GeneratedSourceInfo->kind ==
          GeneratedSourceInfo::ReplacedFunctionBody) {
        // The location was in a temporary source buffer that just contains the
        // function body and which we created while reusing the ASTContext for
        // the rest of the file. Map the location back to the original file.
        unsigned OriginalBufID = SM.findBufferContainingLoc(
            GeneratedSourceInfo->originalSourceRange.getStart());
        auto OriginalStartOffset = SM.getLocOffsetInBuffer(
            GeneratedSourceInfo->originalSourceRange.getStart(), OriginalBufID);
        auto GeneratedStartOffset = SM.getLocOffsetInBuffer(
            GeneratedSourceInfo->generatedSourceRange.getStart(), DeclBufID);
        Location.Offset += OriginalStartOffset - GeneratedStartOffset;
        std::tie(Location.Line, Location.Column) =
            SM.getPresumedLineAndColumnForLoc(DeclLoc, DeclBufID);
      }
    }
  } else if (ClangNode) {
    ClangImporter *Importer =
        static_cast<ClangImporter*>(Ctx.getClangModuleLoader());
    setLocationInfoForClangNode(ClangNode, Importer, Location);
  }
}

static llvm::Error
fillSymbolInfo(CursorSymbolInfo &Symbol, const DeclInfo &DInfo,
               SourceLoc CursorLoc, bool AddSymbolGraph, SwiftLangSupport &Lang,
               const CompilerInvocation &Invoc,
               ArrayRef<ImmutableTextSnapshotRef> PreviousSnaps,
               llvm::BumpPtrAllocator &Allocator) {
  SmallString<256> Buffer;
  SmallVector<StringRef, 4> Strings;
  llvm::raw_svector_ostream OS(Buffer);

  Symbol.DeclarationLang = SwiftLangSupport::getUIDForDeclLanguage(DInfo.VD);
  Symbol.Kind = SwiftLangSupport::getUIDForDecl(DInfo.VD, DInfo.IsRef);

  SwiftLangSupport::printDisplayName(DInfo.VD, OS);
  Symbol.Name = copyAndClearString(Allocator, Buffer);

  SwiftLangSupport::printUSR(DInfo.OriginalProperty, OS);
  if (DInfo.InSynthesizedExtension) {
    OS << LangSupport::SynthesizedUSRSeparator;
    SwiftLangSupport::printUSR(DInfo.BaseType->getAnyNominal(), OS);
  }
  Symbol.USR = copyAndClearString(Allocator, Buffer);

  {
    PrintOptions Options;
    Options.PrintTypeAliasUnderlyingType = true;
    DInfo.VD->getInterfaceType().print(OS, Options);
  }
  Symbol.TypeName = copyAndClearString(Allocator, Buffer);

  // ParameterizedProtocolType should always be wrapped in ExistentialType and
  // cannot be mangled on its own.
  // But ParameterizedProtocolType can currently occur in 'typealias'
  // declarations. rdar://99176683
  // To avoid crashing in USR generation, return an error for now.
  if (auto Ty = DInfo.VD->getInterfaceType()) {
    while (auto MetaTy = Ty->getAs<MetatypeType>()) {
      Ty = MetaTy->getInstanceType();
    }
    if (Ty && Ty->getCanonicalType()->is<ParameterizedProtocolType>()) {
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "Cannot mangle USR for ParameterizedProtocolType without 'any'.");
    }
  }

  SwiftLangSupport::printDeclTypeUSR(DInfo.VD, OS);
  Symbol.TypeUSR = copyAndClearString(Allocator, Buffer);

  if (DInfo.ContainerType && !DInfo.ContainerType->hasArchetype()) {
    SwiftLangSupport::printTypeUSR(DInfo.ContainerType, OS);
  }
  Symbol.ContainerTypeUSR = copyAndClearString(Allocator, Buffer);

  ide::getDocumentationCommentAsXML(DInfo.OriginalProperty, OS);
  Symbol.DocComment = copyAndClearString(Allocator, Buffer);

  {
    auto *Group = DInfo.InSynthesizedExtension ? DInfo.BaseType->getAnyNominal()
                                               : DInfo.VD;
    if (auto Name = Group->getGroupName())
      Symbol.GroupName = Name.value();
  }

  ide::getLocalizationKey(DInfo.VD, OS);
  Symbol.LocalizationKey = copyAndClearString(Allocator, Buffer);

  printAnnotatedDeclaration(DInfo.VD, DInfo.BaseType, OS);
  Symbol.AnnotatedDeclaration = copyAndClearString(Allocator, Buffer);

  SwiftLangSupport::printFullyAnnotatedDeclaration(DInfo.VD, DInfo.BaseType,
                                                   OS);
  Symbol.FullyAnnotatedDeclaration = copyAndClearString(Allocator, Buffer);

  if (AddSymbolGraph) {
    SmallVector<symbolgraphgen::PathComponent, 4> PathComponents;
    SmallVector<symbolgraphgen::FragmentInfo, 8> FragmentInfos;

    symbolgraphgen::SymbolGraphOptions Options;
    Options.Target = Invoc.getLangOptions().Target;
    Options.MinimumAccessLevel = AccessLevel::Private;
    Options.IncludeSPISymbols = true;
    Options.IncludeClangDocs = true;
    Options.PrintPrivateStdlibSymbols = true;

    symbolgraphgen::printSymbolGraphForDecl(DInfo.VD, DInfo.BaseType,
                                            DInfo.InSynthesizedExtension,
                                            Options, OS, PathComponents,
                                            FragmentInfos);
    Symbol.SymbolGraph = copyAndClearString(Allocator, Buffer);

    SmallVector<ParentInfo, 4> Parents;
    for (auto &Component : PathComponents) {
      SwiftLangSupport::printUSR(Component.VD, OS);
      Parents.emplace_back(Component.Title.str().copy(Allocator),
                           Component.Kind,
                           copyAndClearString(Allocator, Buffer));
    };
    Symbol.ParentContexts = copyArray(Allocator, llvm::makeArrayRef(Parents));

    SmallVector<ReferencedDeclInfo, 8> ReferencedDecls;
    for (auto &FI: FragmentInfos) {
      SmallVector<ParentInfo, 4> FIParents;
      for (auto &Component: FI.ParentContexts) {
        SwiftLangSupport::printUSR(Component.VD, OS);
        FIParents.emplace_back(Component.Title.str().copy(Allocator),
                               Component.Kind,
                               copyAndClearString(Allocator, Buffer));
      }

      ASTContext &Ctx = FI.VD->getASTContext();
      StringRef Filename = "";
      if (auto Loc = FI.VD->getLoc(/*SerializedOK=*/true)) {
        Filename = Ctx.SourceMgr.getDisplayNameForLoc(Loc);
      } else if (auto ClangNode = FI.VD->getClangNode()) {
        auto Loc = ClangNode.getLocation();
        if (Loc.isValid()) {
          Filename = Ctx.getClangModuleLoader()->getClangASTContext()
              .getSourceManager()
              .getFilename(Loc);
        }
      }

      SwiftLangSupport::printUSR(FI.VD, OS);
      ReferencedDecls.emplace_back(
          copyAndClearString(Allocator, Buffer),
          SwiftLangSupport::getUIDForDeclLanguage(FI.VD),
          swift::getAccessLevelSpelling(FI.VD->getFormalAccess()), Filename,
          getModuleName(FI.VD, Allocator),
          FI.VD->getModuleContext()->isNonUserModule(), FI.VD->isSPI(),
          copyArray(Allocator, llvm::makeArrayRef(FIParents)));
    }
    Symbol.ReferencedSymbols = copyArray(Allocator,
                                         llvm::makeArrayRef(ReferencedDecls));
  }

  Symbol.ModuleName = getModuleName(DInfo.VD, Allocator);
  if (auto IFaceGenRef =
          Lang.getIFaceGenContexts().find(Symbol.ModuleName, Invoc))
    Symbol.ModuleInterfaceName = IFaceGenRef->getDocumentName();

  setLocationInfo(DInfo.OriginalProperty, Symbol.Location);
  if (!Symbol.Location.Filename.empty()) {
    mapLocToLatestSnapshot(Lang, Symbol.Location, PreviousSnaps);
    if (Symbol.Location.Filename.empty()) {
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "Failed to remap declaration to latest snapshot.");
    }
  }

  ide::walkOverriddenDecls(
      DInfo.VD,
      [&](llvm::PointerUnion<const ValueDecl *, const clang::NamedDecl *> D) {
        // Could have junk in from previous failing USR print
        Buffer.clear();
        if (auto VD = D.dyn_cast<const ValueDecl *>()) {
          if (SwiftLangSupport::printUSR(VD, OS))
            return;
        } else {
          if (clang::index::generateUSRForDecl(
                  D.get<const clang::NamedDecl *>(), Buffer))
            return;
        }
        Strings.push_back(copyAndClearString(Allocator, Buffer));
      });
  Symbol.OverrideUSRs = copyAndClearArray(Allocator, Strings);

  walkRelatedDecls(DInfo.VD, [&](const ValueDecl *RelatedDecl,
                                 bool UseOriginalBase, bool DuplicateName) {
    OS << "<RelatedName usr=\"";
    SwiftLangSupport::printUSR(RelatedDecl, OS);
    OS << "\">";
    if (isa<AbstractFunctionDecl>(RelatedDecl) && DuplicateName) {
      // Related decls are generally overloads, so print parameter types to
      // differentiate them.
      PrintOptions PO;
      PO.SkipAttributes = true;
      PO.PrintStaticKeyword = false;
      PO.PrintSelfAccessKindKeyword = false;
      PO.SkipIntroducerKeywords = true;
      PO.ArgAndParamPrinting =
          PrintOptions::ArgAndParamPrintingMode::ArgumentOnly;
      XMLEscapingPrinter Printer(OS);
      if (UseOriginalBase && DInfo.BaseType) {
        PO.setBaseType(DInfo.BaseType);
        PO.PrintAsMember = true;
      }
      RelatedDecl->print(Printer, PO);
    } else {
      SmallString<128> RelatedBuffer;
      llvm::raw_svector_ostream RelatedOS(RelatedBuffer);
      SwiftLangSupport::printDisplayName(RelatedDecl, RelatedOS);
      swift::markup::appendWithXMLEscaping(OS, RelatedBuffer);
    }
    OS << "</RelatedName>";

    Strings.push_back(copyAndClearString(Allocator, Buffer));
  });
  Symbol.AnnotatedRelatedDeclarations = copyAndClearArray(Allocator, Strings);

  for (auto *ReceiverTy : DInfo.ReceiverTypes) {
    if (!SwiftLangSupport::printUSR(ReceiverTy, OS))
      Strings.push_back(copyAndClearString(Allocator, Buffer));
  }
  Symbol.ReceiverUSRs = copyAndClearArray(Allocator, Strings);

  Symbol.IsSystem = DInfo.VD->getModuleContext()->isNonUserModule();
  Symbol.IsDynamic = DInfo.IsDynamic;
  Symbol.IsSynthesized = DInfo.VD->isImplicit();

  Symbol.ParentNameOffset = getParamParentNameOffset(DInfo.VD, CursorLoc);

  return llvm::Error::success();
}

static bool
addCursorInfoForDecl(CursorInfoData &Data, ResolvedValueRefCursorInfoPtr Info,
                     bool AddRefactorings, bool AddSymbolGraph,
                     SwiftLangSupport &Lang, const CompilerInvocation &Invoc,
                     std::string &Diagnostic,
                     ArrayRef<ImmutableTextSnapshotRef> PreviousSnaps) {
  DeclInfo OrigInfo(Info->getValueD(), Info->getContainerType(), Info->isRef(),
                    Info->isDynamic(), Info->getReceiverTypes(), Invoc);
  DeclInfo CtorTypeInfo(Info->getCtorTyRef(), Type(), true, false,
                        ArrayRef<NominalTypeDecl *>(), Invoc);
  DeclInfo &MainInfo = CtorTypeInfo.VD ? CtorTypeInfo : OrigInfo;
  if (MainInfo.Unavailable) {
    Diagnostic = "Unavailable in the current compilation context.";
    return false;
  }

  CursorSymbolInfo &MainSymbol = Data.Symbols.emplace_back();
  // The primary result for constructor calls, eg. `MyType()` should be
  // the type itself, rather than the constructor. The constructor will be
  // added as a secondary result.
  if (auto Err =
          fillSymbolInfo(MainSymbol, MainInfo, Info->getLoc(), AddSymbolGraph,
                         Lang, Invoc, PreviousSnaps, Data.Allocator)) {
    llvm::handleAllErrors(std::move(Err), [&](const llvm::StringError &E) {
      Diagnostic = E.message();
    });
    return false;
  }

  if (MainInfo.VD != OrigInfo.VD && !OrigInfo.Unavailable) {
    CursorSymbolInfo &CtorSymbol = Data.Symbols.emplace_back();
    if (auto Err =
            fillSymbolInfo(CtorSymbol, OrigInfo, Info->getLoc(), AddSymbolGraph,
                           Lang, Invoc, PreviousSnaps, Data.Allocator)) {
      // Ignore but make sure to remove the partially-filled symbol
      llvm::handleAllErrors(std::move(Err), [](const llvm::StringError &E) {});
      Data.Symbols.pop_back();
    }
  }

  // Add in shadowed declarations if on a decl. For references just go to the
  // actual declaration.
  if (!Info->isRef()) {
    for (auto D : Info->getShorthandShadowedDecls()) {
      CursorSymbolInfo &SymbolInfo = Data.Symbols.emplace_back();
      DeclInfo DInfo(D, Type(), /*IsRef=*/true, /*IsDynamic=*/false,
                     ArrayRef<NominalTypeDecl *>(), Invoc);
      if (auto Err =
              fillSymbolInfo(SymbolInfo, DInfo, Info->getLoc(), AddSymbolGraph,
                             Lang, Invoc, PreviousSnaps, Data.Allocator)) {
        // Ignore but make sure to remove the partially-filled symbol
        llvm::handleAllErrors(std::move(Err), [](const llvm::StringError &E) {});
        Data.Symbols.pop_back();
      }
    }
  }

  if (AddRefactorings) {
    addRefactorings(Data.AvailableActions,
                    collectRefactorings(Info, /*ExcludeRename=*/false));
  }

  return true;
}

/// Returns true on success, false on error (and sets `Diagnostic` accordingly).
static bool passCursorInfoForDecl(
    ResolvedValueRefCursorInfoPtr Info, bool AddRefactorings,
    bool AddSymbolGraph, ArrayRef<RefactoringInfo> KnownRefactoringInfo,
    SwiftLangSupport &Lang, const CompilerInvocation &Invoc,
    std::string &Diagnostic, ArrayRef<ImmutableTextSnapshotRef> PreviousSnaps,
    bool DidReuseAST,
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver) {
  CursorInfoData Data;

  bool success =
      addCursorInfoForDecl(Data, Info, AddRefactorings, AddSymbolGraph, Lang,
                           Invoc, Diagnostic, PreviousSnaps);
  if (!success) {
    return false;
  }

  Data.AvailableActions.append(KnownRefactoringInfo.begin(),
                               KnownRefactoringInfo.end());

  Data.DidReuseAST = DidReuseAST;
  Receiver(RequestResult<CursorInfoData>::fromResult(Data));
  return true;
}

static clang::DeclarationName
getClangDeclarationName(const clang::NamedDecl *ND, NameTranslatingInfo &Info) {
  auto &Ctx = ND->getASTContext();
  auto OrigName = ND->getDeclName();
  assert(SwiftLangSupport::getNameKindForUID(Info.NameKind) == NameKind::ObjC);
  if (Info.BaseName.empty() == Info.ArgNames.empty()) {
    // cannot have both.
    return clang::DeclarationName();
  }
  if (!Info.BaseName.empty()) {
    return clang::DeclarationName(&Ctx.Idents.get(Info.BaseName));
  } else {
    switch (OrigName.getNameKind()) {
    case clang::DeclarationName::ObjCZeroArgSelector:
    case clang::DeclarationName::ObjCOneArgSelector:
    case clang::DeclarationName::ObjCMultiArgSelector:
      break;
    default:
      return clang::DeclarationName();
    }

    auto OrigSel = OrigName.getObjCSelector();
    unsigned NumPieces = OrigSel.isUnarySelector() ? 1 : OrigSel.getNumArgs();
    if (Info.ArgNames.size() > NumPieces)
      return clang::DeclarationName();

    ArrayRef<StringRef> Args = llvm::makeArrayRef(Info.ArgNames);
    std::vector<clang::IdentifierInfo *> Pieces;
    for (unsigned i = 0; i < NumPieces; ++i) {
      if (i >= Info.ArgNames.size() || Info.ArgNames[i].empty()) {
        Pieces.push_back(OrigSel.getIdentifierInfoForSlot(i));
      } else {
        StringRef T = Args[i];
        Pieces.push_back(&Ctx.Idents.get(T.endswith(":") ? T.drop_back() : T));
      }
    }
    return clang::DeclarationName(
        Ctx.Selectors.getSelector(OrigSel.getNumArgs(), Pieces.data()));
  }
}

static DeclName getSwiftDeclName(const ValueDecl *VD,
                                 NameTranslatingInfo &Info) {
  auto &Ctx = VD->getDeclContext()->getASTContext();
  assert(SwiftLangSupport::getNameKindForUID(Info.NameKind) == NameKind::Swift);
  const DeclName OrigName = VD->getName();
  DeclBaseName BaseName = Info.BaseName.empty()
                              ? OrigName.getBaseName()
                              : DeclBaseName(
                                Info.BaseName == "init"
                                  ? DeclBaseName::createConstructor()
                                  : Ctx.getIdentifier(Info.BaseName));
  auto OrigArgs = OrigName.getArgumentNames();
  SmallVector<Identifier, 8> Args(OrigArgs.begin(), OrigArgs.end());
  if (Info.ArgNames.size() > OrigArgs.size())
    return DeclName();
  for (unsigned i = 0; i < OrigArgs.size(); ++i) {
    if (i < Info.ArgNames.size() && !Info.ArgNames[i].empty()) {
      StringRef Arg = Info.ArgNames[i];
      Args[i] = Ctx.getIdentifier(Arg == "_" ? StringRef() : Arg);
    }
  }
  return DeclName(Ctx, BaseName, llvm::makeArrayRef(Args));
}

/// Returns true on success, false on error (and sets `Diagnostic` accordingly).
static bool passNameInfoForDecl(
    const ResolvedValueRefCursorInfo &CursorInfo, NameTranslatingInfo &Info,
    std::string &Diagnostic,
    std::function<void(const RequestResult<NameTranslatingInfo> &)> Receiver) {
  auto *VD = CursorInfo.getValueD();

  // If the given name is not a function name, and the cursor points to
  // a constructor call, we use the type declaration instead of the init
  // declaration to translate the name.
  if (Info.ArgNames.empty() && !Info.IsZeroArgSelector) {
    if (auto *TD = CursorInfo.getCtorTyRef()) {
      VD = TD;
    }
  }
  switch (SwiftLangSupport::getNameKindForUID(Info.NameKind)) {
  case NameKind::Swift: {
    NameTranslatingInfo Result;
    auto DeclName = getSwiftDeclName(VD, Info);
    if (!DeclName) {
      Diagnostic = "Unable to resolve Swift declaration name.";
      return false;
    }
    auto ResultPair =
        swift::objc_translation::getObjCNameForSwiftDecl(VD, DeclName);
    Identifier Name = ResultPair.first;
    if (!Name.empty()) {
      Result.NameKind = SwiftLangSupport::getUIDForNameKind(NameKind::ObjC);
      Result.BaseName = Name.str();
      Receiver(RequestResult<NameTranslatingInfo>::fromResult(Result));
    } else if (ObjCSelector Selector = ResultPair.second) {
      Result.NameKind = SwiftLangSupport::getUIDForNameKind(NameKind::ObjC);
      SmallString<64> Buffer;
      StringRef Total = Selector.getString(Buffer);
      SmallVector<StringRef, 4> Pieces;
      Total.split(Pieces, ":");
      if (Selector.getNumArgs()) {
        assert(Pieces.back().empty());
        Pieces.pop_back();
      } else {
        Result.IsZeroArgSelector = true;
      }
      Result.ArgNames.insert(Result.ArgNames.begin(), Pieces.begin(), Pieces.end());
      Receiver(RequestResult<NameTranslatingInfo>::fromResult(Result));
    } else {
      Diagnostic = "Unable to resolve name info.";
      return false;
    }
    return true;
  }
  case NameKind::ObjC: {
    ClangImporter *Importer = static_cast<ClangImporter *>(VD->getDeclContext()->
      getASTContext().getClangModuleLoader());

    const clang::NamedDecl *Named = nullptr;
    auto *BaseDecl = VD;

    while (!Named && BaseDecl) {
      Named = dyn_cast_or_null<clang::NamedDecl>(BaseDecl->getClangDecl());
      BaseDecl = BaseDecl->getOverriddenDecl();
    }
    if (!Named) {
      Diagnostic = "Unable to resolve a named declaration.";
      return false;
    }

    auto ObjCName = getClangDeclarationName(Named, Info);
    if (!ObjCName) {
      Diagnostic = "Unable to resolve ObjC declaration name.";
      return false;
    }

    DeclName Name = Importer->importName(Named, ObjCName);
    NameTranslatingInfo Result;
    Result.NameKind = SwiftLangSupport::getUIDForNameKind(NameKind::Swift);
    Result.BaseName = Name.getBaseName().userFacingName();
    llvm::transform(Name.getArgumentNames(),
                    std::back_inserter(Result.ArgNames),
                    [](Identifier Id) { return Id.str(); });
    Receiver(RequestResult<NameTranslatingInfo>::fromResult(Result));
    return true;
  }
  }
}

class CursorRangeInfoConsumer : public SwiftASTConsumer {
protected:
  SwiftLangSupport &Lang;
  SwiftInvocationRef ASTInvok;
  std::string InputFile;
  unsigned Offset;
  unsigned Length;

private:
  const bool TryExistingAST;
  SmallVector<ImmutableTextSnapshotRef, 4> PreviousASTSnaps;

protected:
  bool CancelOnSubsequentRequest;
protected:
  ArrayRef<ImmutableTextSnapshotRef> getPreviousASTSnaps() {
    return llvm::makeArrayRef(PreviousASTSnaps);
  }

public:
  CursorRangeInfoConsumer(StringRef InputFile, unsigned Offset, unsigned Length,
                          SwiftLangSupport &Lang, SwiftInvocationRef ASTInvok,
                          bool TryExistingAST, bool CancelOnSubsequentRequest)
      : Lang(Lang), ASTInvok(ASTInvok), InputFile(InputFile.str()),
        Offset(Offset), Length(Length), TryExistingAST(TryExistingAST),
        CancelOnSubsequentRequest(CancelOnSubsequentRequest) {}

  bool canUseASTWithSnapshots(ArrayRef<ImmutableTextSnapshotRef> Snapshots) override {
    if (!TryExistingAST) {
      LOG_INFO_FUNC(High, "will resolve using up-to-date AST");
      return false;
    }

    // If there is an existing AST and the offset can be mapped back to the
    // document snapshot that was used to create it, then use that AST.
    // The downside is that we may return stale information, but we get the
    // benefit of increased responsiveness, since the request will not be
    // blocked waiting on the AST to be fully typechecked.

    ImmutableTextSnapshotRef InputSnap;
    if (auto EditorDoc = Lang.getEditorDocuments()->findByPath(
            InputFile, /*IsRealpath=*/true))
      InputSnap = EditorDoc->getLatestSnapshot();
    if (!InputSnap)
      return false;

    auto mappedBackOffset = [&]()->llvm::Optional<unsigned> {
      for (auto &Snap : Snapshots) {
        if (Snap->isFromSameBuffer(InputSnap)) {
          if (Snap->getStamp() == InputSnap->getStamp())
            return Offset;

          auto OptOffset = mapOffsetToOlderSnapshot(Offset, InputSnap, Snap);
          if (!OptOffset.has_value())
            return None;

          // Check that the new and old offset still point to the same token.
          StringRef NewTok = getSourceToken(Offset, InputSnap);
          if (NewTok.empty())
            return None;
          if (NewTok == getSourceToken(OptOffset.value(), Snap))
            return OptOffset;

          return None;
        }
      }
      return None;
    };

    auto OldOffsetOpt = mappedBackOffset();
    if (OldOffsetOpt.has_value()) {
      Offset = *OldOffsetOpt;
      PreviousASTSnaps.append(Snapshots.begin(), Snapshots.end());
      LOG_INFO_FUNC(High, "will try existing AST");
      return true;
    }

    LOG_INFO_FUNC(High, "will resolve using up-to-date AST");
    return false;
  }
};

static SourceFile *retrieveInputFile(StringRef inputBufferName,
                                     const CompilerInstance &CI,
                                     bool haveRealPath = false) {
  // Don't bother looking up if we have the same file as the primary file or
  // we weren't given a separate input file
  if (inputBufferName.empty() ||
      CI.getPrimarySourceFile()->getFilename() == inputBufferName)
    return CI.getPrimarySourceFile();

  // Otherwise, try to find the given buffer identifier
  const SourceManager &SM = CI.getSourceMgr();
  Optional<unsigned> inputBufferID =
      SM.getIDForBufferIdentifier(inputBufferName);
  if (!inputBufferID) {
    // If that failed, try again with symlinks resolved (unless we've already
    // done that)
    if (haveRealPath)
      return nullptr;
    std::string realPath =
        SwiftLangSupport::resolvePathSymlinks(inputBufferName);
    return retrieveInputFile(realPath, CI, /*haveRealPath=*/true);
  }

  return CI.getMainModule()->getSourceFileContainingLocation(
      SM.getRangeForBuffer(*inputBufferID).getStart());
}

static void resolveCursor(
    SwiftLangSupport &Lang, StringRef InputFile, unsigned Offset,
    unsigned Length, bool Actionables, bool SymbolGraph,
    SwiftInvocationRef Invok, bool TryExistingAST,
    bool CancelOnSubsequentRequest,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver) {
  assert(Invok);
  assert(fileSystem);

  class CursorInfoConsumer : public CursorRangeInfoConsumer {
    bool Actionables;
    bool SymbolGraph;
    SourceKitCancellationToken CancellationToken;
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver;

  public:
    CursorInfoConsumer(
        StringRef InputFile, unsigned Offset, unsigned Length, bool Actionables,
        bool SymbolGraph, SwiftLangSupport &Lang, SwiftInvocationRef ASTInvok,
        bool TryExistingAST, bool CancelOnSubsequentRequest,
        SourceKitCancellationToken CancellationToken,
        std::function<void(const RequestResult<CursorInfoData> &)> Receiver)
        : CursorRangeInfoConsumer(InputFile, Offset, Length, Lang, ASTInvok,
                                  TryExistingAST, CancelOnSubsequentRequest),
          Actionables(Actionables), SymbolGraph(SymbolGraph),
          CancellationToken(CancellationToken), Receiver(std::move(Receiver)) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      auto &CompIns = AstUnit->getCompilerInstance();

      SourceFile *SF = retrieveInputFile(InputFile, CompIns);
      if (!SF) {
        Receiver(RequestResult<CursorInfoData>::fromError(
            "Unable to find input file"));
        return;
      }

      SourceManager &SM = CompIns.getSourceMgr();
      unsigned BufferID = SF->getBufferID().value();
      SourceLoc Loc =
        Lexer::getLocForStartOfToken(SM, BufferID, Offset);
      if (Loc.isInvalid()) {
        Receiver(RequestResult<CursorInfoData>::fromError(
            "Unable to find initial lookup location"));
        return;
      }

      // Sanitize length.
      if (Length) {
        SourceLoc TokEnd = Lexer::getLocForEndOfToken(SM, Loc);
        SourceLoc EndLoc = SM.getLocForOffset(BufferID, Offset + Length);

        // If TokEnd is not before the given EndLoc, the EndLoc contains no
        // more stuff than this token, so set the length to 0.
        if (SM.isBeforeInBuffer(EndLoc, TokEnd) || TokEnd == EndLoc)
          Length = 0;
      }

      // Retrieve relevant actions on the code under selection.
      llvm::SmallVector<RefactoringInfo, 8> Actions;
      if (Actionables && Length) {
        RangeConfig Range;
        Range.BufferID = BufferID;
        auto Pair = SM.getLineAndColumnInBuffer(Loc);
        Range.Line = Pair.first;
        Range.Column = Pair.second;
        Range.Length = Length;
        bool CollectRangeStartRefactorings = false;
        addRefactorings(
            Actions,
            collectRefactorings(SF, Range, CollectRangeStartRefactorings, {}));
        if (!CollectRangeStartRefactorings) {
          // If Length is given then this request is only for refactorings,
          // return straight away unless we need cursor based refactorings as
          // well.
          CursorInfoData Data;
          Data.AvailableActions = Actions;
          Receiver(RequestResult<CursorInfoData>::fromResult(Data));
          return;
        }
        // Fall through to collect cursor based refactorings
      }

      ResolvedCursorInfoPtr CursorInfo =
          evaluateOrDefault(CompIns.getASTContext().evaluator,
                            CursorInfoRequest{CursorInfoOwner(SF, Loc)},
                            new ResolvedCursorInfo());

      CompilerInvocation CompInvok;
      ASTInvok->applyTo(CompInvok);

      switch (CursorInfo->getKind()) {
      case CursorInfoKind::ModuleRef: {
        auto ModuleRefInfo = cast<ResolvedModuleRefCursorInfo>(CursorInfo);
        passCursorInfoForModule(ModuleRefInfo->getMod(),
                                Lang.getIFaceGenContexts(), CompInvok,
                                Receiver);
        return;
      }
      case CursorInfoKind::ValueRef: {
        std::string Diagnostic;
        bool Success = passCursorInfoForDecl(
            cast<ResolvedValueRefCursorInfo>(CursorInfo), Actionables,
            SymbolGraph, Actions, Lang, CompInvok, Diagnostic,
            getPreviousASTSnaps(),
            /*DidReuseAST=*/false, Receiver);
        if (!Success) {
          if (!getPreviousASTSnaps().empty()) {
            // Attempt again using the up-to-date AST.
            resolveCursor(Lang, InputFile, Offset, Length, Actionables,
                          SymbolGraph, ASTInvok, /*TryExistingAST=*/false,
                          CancelOnSubsequentRequest, SM.getFileSystem(),
                          CancellationToken, Receiver);
          } else {
            CursorInfoData Info;
            Info.InternalDiagnostic = Diagnostic;
            Receiver(RequestResult<CursorInfoData>::fromResult(Info));
          }
        }
        return;
      }
      case CursorInfoKind::ExprStart:
      case CursorInfoKind::StmtStart: {
        if (Actionables) {
          addRefactorings(
              Actions, collectRefactorings(CursorInfo, /*ExcludeRename=*/true));
          if (!Actions.empty()) {
            CursorInfoData Data;
            Data.AvailableActions = Actions;
            Receiver(RequestResult<CursorInfoData>::fromResult(Data));
            return;
          }
        }

        CursorInfoData Info;
        Info.InternalDiagnostic =
            "Resolved to incomplete expression or statement.";
        Receiver(RequestResult<CursorInfoData>::fromResult(Info));
        return;
      }
      case CursorInfoKind::Invalid:
        CursorInfoData Data;
        if (Actionables) {
          Data.AvailableActions = Actions;
        } else {
          Data.InternalDiagnostic = "Unable to resolve cursor info.";
        }
        Receiver(RequestResult<CursorInfoData>::fromResult(Data));
        return;
      }
    }

    void cancelled() override {
      Receiver(RequestResult<CursorInfoData>::cancelled());
    }

    void failed(StringRef Error) override {
      LOG_WARN_FUNC("cursor info failed: " << Error);
      Receiver(RequestResult<CursorInfoData>::fromError(Error));
    }
  };

  auto Consumer = std::make_shared<CursorInfoConsumer>(
      InputFile, Offset, Length, Actionables, SymbolGraph, Lang, Invok,
      TryExistingAST, CancelOnSubsequentRequest, CancellationToken, Receiver);

  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  static const char OncePerASTTokenWithActionables = 0;
  const void *Once = nullptr;
  if (CancelOnSubsequentRequest)
    Once = Actionables ? &OncePerASTTokenWithActionables : &OncePerASTToken;
  Lang.getASTManager()->processASTAsync(Invok, std::move(Consumer), Once,
                                        CancellationToken, fileSystem);
}

static void computeDiagnostics(
    SwiftLangSupport &Lang, SwiftInvocationRef Invok,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<DiagnosticsResult> &)> Receiver) {

  class DiagnosticsConsumer : public SwiftASTConsumer {
    std::function<void(const RequestResult<DiagnosticsResult> &)> Receiver;

  public:
    DiagnosticsConsumer(
        std::function<void(const RequestResult<DiagnosticsResult> &)> Receiver)
        : Receiver(Receiver) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      unsigned BufferID = *AstUnit->getPrimarySourceFile().getBufferID();
      auto &DiagConsumer = AstUnit->getEditorDiagConsumer();
      auto Diagnostics = DiagConsumer.getDiagnosticsForBuffer(BufferID);
      Receiver(RequestResult<DiagnosticsResult>::fromResult(Diagnostics));
    }

    void cancelled() override {
      Receiver(RequestResult<DiagnosticsResult>::cancelled());
    }
  };

  auto Consumer = std::make_shared<DiagnosticsConsumer>(std::move(Receiver));

  Lang.getASTManager()->processASTAsync(Invok, std::move(Consumer),
                                        /*OncePerASTToken=*/nullptr,
                                        CancellationToken, FileSystem);
}

static void resolveName(
    SwiftLangSupport &Lang, StringRef InputFile, unsigned Offset,
    SwiftInvocationRef Invok, bool TryExistingAST, NameTranslatingInfo &Input,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<NameTranslatingInfo> &)> Receiver) {
  assert(Invok);

  class NameInfoConsumer : public CursorRangeInfoConsumer {
    NameTranslatingInfo Input;
    SourceKitCancellationToken CancellationToken;
    std::function<void(const RequestResult<NameTranslatingInfo> &)> Receiver;

  public:
    NameInfoConsumer(
        StringRef InputFile, unsigned Offset, SwiftLangSupport &Lang,
        SwiftInvocationRef ASTInvok, bool TryExistingAST,
        NameTranslatingInfo Input, SourceKitCancellationToken CancellationToken,
        std::function<void(const RequestResult<NameTranslatingInfo> &)>
            Receiver)
        : CursorRangeInfoConsumer(InputFile, Offset, 0, Lang, ASTInvok,
                                  TryExistingAST,
                                  /*CancelOnSubsequentRequest=*/false),
          Input(std::move(Input)), CancellationToken(CancellationToken),
          Receiver(std::move(Receiver)) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      auto &CompIns = AstUnit->getCompilerInstance();

      SourceFile *SF = retrieveInputFile(InputFile, CompIns);
      if (!SF) {
        Receiver(RequestResult<NameTranslatingInfo>::fromError(
            "Unable to find input file"));
        return;
      }

      SourceLoc Loc = Lexer::getLocForStartOfToken(CompIns.getSourceMgr(),
                                                   *SF->getBufferID(), Offset);
      if (Loc.isInvalid()) {
        Receiver(RequestResult<NameTranslatingInfo>::fromError(
          "Unable to resolve the start of the token."));
        return;
      }

      ResolvedCursorInfoPtr CursorInfo =
          evaluateOrDefault(CompIns.getASTContext().evaluator,
                            CursorInfoRequest{CursorInfoOwner(SF, Loc)},
                            new ResolvedCursorInfo());
      if (CursorInfo->isInvalid()) {
        NameTranslatingInfo Info;
        Info.InternalDiagnostic = "Unable to resolve cursor info.";
        Receiver(RequestResult<NameTranslatingInfo>::fromResult(Info));
        return;
      }

      CompilerInvocation CompInvok;
      ASTInvok->applyTo(CompInvok);

      switch (CursorInfo->getKind()) {
      case CursorInfoKind::ModuleRef:
        return;

      case CursorInfoKind::ValueRef: {
        std::string Diagnostic;
        bool Success =
            passNameInfoForDecl(*cast<ResolvedValueRefCursorInfo>(CursorInfo),
                                Input, Diagnostic, Receiver);
        if (!Success) {
          if (!getPreviousASTSnaps().empty()) {
            // Attempt again using the up-to-date AST.
            resolveName(Lang, InputFile, Offset, ASTInvok,
                        /*TryExistingAST=*/false, Input, CancellationToken,
                        Receiver);
          } else {
            NameTranslatingInfo Info;
            Info.InternalDiagnostic = Diagnostic;
            Receiver(RequestResult<NameTranslatingInfo>::fromResult(Info));
          }
        }
        return;
      }
      case CursorInfoKind::ExprStart:
      case CursorInfoKind::StmtStart: {
        NameTranslatingInfo Info;
        Info.InternalDiagnostic =
            "Resolved to incomplete expression or statement.";
        Receiver(RequestResult<NameTranslatingInfo>::fromResult(Info));
        return;
      }
      case CursorInfoKind::Invalid:
        llvm_unreachable("bad sema token kind.");
      }
    }

    void cancelled() override {
      Receiver(RequestResult<NameTranslatingInfo>::cancelled());
    }

    void failed(StringRef Error) override {
      LOG_WARN_FUNC("name info failed: " << Error);
      Receiver(RequestResult<NameTranslatingInfo>::fromError(Error));
    }
  };

  auto Consumer = std::make_shared<NameInfoConsumer>(
      InputFile, Offset, Lang, Invok, TryExistingAST, Input, CancellationToken,
      Receiver);

  Lang.getASTManager()->processASTAsync(
      Invok, std::move(Consumer), /*OncePerASTToken=*/nullptr,
      CancellationToken, llvm::vfs::getRealFileSystem());
}

static void
resolveRange(SwiftLangSupport &Lang, StringRef InputFile, unsigned Offset,
             unsigned Length, SwiftInvocationRef Invok, bool TryExistingAST,
             bool CancelOnSubsequentRequest,
             SourceKitCancellationToken CancellationToken,
             std::function<void(const RequestResult<RangeInfo> &)> Receiver) {
  assert(Invok);

  class RangeInfoConsumer : public CursorRangeInfoConsumer {
    SourceKitCancellationToken CancellationToken;
    std::function<void(const RequestResult<RangeInfo> &)> Receiver;

  public:
    RangeInfoConsumer(
        StringRef InputFile, unsigned Offset, unsigned Length,
        SwiftLangSupport &Lang, SwiftInvocationRef ASTInvok,
        bool TryExistingAST, bool CancelOnSubsequentRequest,
        SourceKitCancellationToken CancellationToken,
        std::function<void(const RequestResult<RangeInfo> &)> Receiver)
        : CursorRangeInfoConsumer(InputFile, Offset, Length, Lang, ASTInvok,
                                  TryExistingAST, CancelOnSubsequentRequest),
          CancellationToken(CancellationToken), Receiver(std::move(Receiver)) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      // FIXME: Implement tracing
      auto &CompIns = AstUnit->getCompilerInstance();

      SourceFile *SF = retrieveInputFile(InputFile, CompIns);
      if (!SF) {
        Receiver(
            RequestResult<RangeInfo>::fromError("Unable to find input file"));
        return;
      }

      ResolvedRangeInfo Info = evaluateOrDefault(
          CompIns.getASTContext().evaluator,
          RangeInfoRequest(RangeInfoOwner({SF, Offset, Length})),
          ResolvedRangeInfo());

      CompilerInvocation CompInvok;
      ASTInvok->applyTo(CompInvok);

      RangeInfo Result;
      Result.RangeKind = Lang.getUIDForRangeKind(Info.Kind);
      if (Info.Kind == RangeKind::Invalid) {
        Result.RangeContent = "";
      } else {
        assert(Info.ContentRange.isValid());
        Result.RangeContent = Info.ContentRange.str();
      }

      switch (Info.Kind) {
      case RangeKind::SingleExpression: {
        SmallString<64> SS;
        llvm::raw_svector_ostream OS(SS);
        Info.ExitInfo.ReturnType->print(OS);
        Result.ExprType = OS.str();
        Receiver(RequestResult<RangeInfo>::fromResult(Result));
        return;
      }
      case RangeKind::SingleDecl:
      case RangeKind::MultiTypeMemberDecl:
      case RangeKind::MultiStatement:
      case RangeKind::SingleStatement: {
        Receiver(RequestResult<RangeInfo>::fromResult(Result));
        return;
      }
      case RangeKind::PartOfExpression:
      case RangeKind::Invalid:
        if (!getPreviousASTSnaps().empty()) {
          // Attempt again using the up-to-date AST.
          resolveRange(Lang, InputFile, Offset, Length, ASTInvok,
                       /*TryExistingAST=*/false, CancelOnSubsequentRequest,
                       CancellationToken, Receiver);
        } else {
          Receiver(RequestResult<RangeInfo>::fromResult(Result));
        }
        return;
      }
    }

    void cancelled() override {
      Receiver(RequestResult<RangeInfo>::cancelled());
    }

    void failed(StringRef Error) override {
      LOG_WARN_FUNC("range info failed: " << Error);
      Receiver(RequestResult<RangeInfo>::fromError(Error));
    }
  };

  auto Consumer = std::make_shared<RangeInfoConsumer>(
      InputFile, Offset, Length, Lang, Invok, TryExistingAST,
      CancelOnSubsequentRequest, CancellationToken, Receiver);
  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  const void *Once = CancelOnSubsequentRequest ? &OncePerASTToken : nullptr;
  Lang.getASTManager()->processASTAsync(Invok, std::move(Consumer), Once,
                                        CancellationToken,
                                        llvm::vfs::getRealFileSystem());
}

static void deliverCursorInfoResults(
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver,
    CancellableResult<CursorInfoResults> Results, SwiftLangSupport &Lang,
    const CompilerInvocation &Invoc, bool AddRefactorings,
    bool AddSymbolGraph) {
  switch (Results.getKind()) {
  case CancellableResultKind::Success: {
    // TODO: Implement delivery of other result types as more cursor info kinds
    // are migrated to be completion-like.
    CursorInfoData Data;
    for (auto ResolvedCursorInfo : Results->ResolvedCursorInfos) {
      if (auto Result = dyn_cast_or_null<ResolvedValueRefCursorInfo>(
              ResolvedCursorInfo)) {
        std::string Diagnostic; // Unused
        addCursorInfoForDecl(Data, Result, AddRefactorings, AddSymbolGraph,
                             Lang, Invoc, Diagnostic, /*PreviousSnaps=*/{});
      }
    }
    Data.DidReuseAST = Results->DidReuseAST;
    if (!Data.Symbols.empty()) {
      Receiver(RequestResult<CursorInfoData>::fromResult(Data));
    }
    break;
  }
  case CancellableResultKind::Failure:
    Receiver(RequestResult<CursorInfoData>::fromError(Results.getError()));
    break;
  case CancellableResultKind::Cancelled:
    Receiver(RequestResult<CursorInfoData>::cancelled());
    break;
  }
}

void SwiftLangSupport::getCursorInfo(
    StringRef PrimaryFilePath, StringRef InputBufferName, unsigned Offset,
    unsigned Length, bool Actionables, bool SymbolGraph,
    bool CancelOnSubsequentRequest, ArrayRef<const char *> Args,
    Optional<VFSOptions> vfsOptions,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver) {
  std::string error;
  auto fileSystem = getFileSystem(vfsOptions, PrimaryFilePath, error);
  if (!fileSystem)
    return Receiver(RequestResult<CursorInfoData>::fromError(error));

  if (auto IFaceGenRef = IFaceGenContexts.get(PrimaryFilePath)) {
    IFaceGenRef->accessASTAsync([this, IFaceGenRef, Offset, Actionables,
                                 SymbolGraph, Receiver] {
      SwiftInterfaceGenContext::ResolvedEntity Entity;
      Entity = IFaceGenRef->resolveEntityForOffset(Offset);
      if (Entity.isResolved()) {
        CompilerInvocation Invok;
        IFaceGenRef->applyTo(Invok);
        if (Entity.Mod) {
          passCursorInfoForModule(Entity.Mod, IFaceGenContexts, Invok,
                                  Receiver);
        } else {
          std::string Diagnostic; // Unused.
          ResolvedValueRefCursorInfoPtr Info = new ResolvedValueRefCursorInfo(
              /*SourcFile=*/nullptr, SourceLoc(),
              const_cast<ValueDecl *>(Entity.Dcl),
              /*CtorTyRef=*/nullptr,
              /*ExtTyRef=*/nullptr, Entity.IsRef,
              /*Ty=*/Type(),
              /*ContainerType=*/Type(),
              /*CustomAttrRef=*/None,
              /*IsKeywordArgument=*/false,
              /*IsDynamic=*/false,
              /*ReceiverTypes=*/{},
              /*ShorthandShadowedDecls=*/{});
          passCursorInfoForDecl(
              Info, Actionables, SymbolGraph, {}, *this, Invok, Diagnostic,
              /*PreviousSnaps=*/{},
              /*DidReuseAST=*/false, Receiver);
        }
      } else {
        CursorInfoData Info;
        Info.InternalDiagnostic =
            "Unable to resolve entity from generated interface.";
        Receiver(RequestResult<CursorInfoData>::fromResult(Info));
      }
    });
    return;
  }

  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, fileSystem, Error);
  if (!Error.empty()) {
    LOG_WARN_FUNC("error creating ASTInvocation: " << Error);
  }
  if (!Invok) {
    Receiver(RequestResult<CursorInfoData>::fromError(Error));
    return;
  }

  // Solver based cursor info cannot handle generated buffers or range based
  // cursor info.
  std::shared_ptr<llvm::MemoryBuffer> InputBuffer;
  if (InputBufferName.empty() && Length == 0) {
    std::string InputFileError;
    llvm::SmallString<128> RealInputFilePath;
    fileSystem->getRealPath(PrimaryFilePath, RealInputFilePath);
    InputBuffer =
        std::shared_ptr<llvm::MemoryBuffer>(getASTManager()->getMemoryBuffer(
            RealInputFilePath, fileSystem, InputFileError));
  }

  // Receiver is async, so be careful about captured values. This is all
  // fairly hacky in order to run AST based cursor info before solver based.
  // ie. we shouldn't need to apply the compiler invocation again or copy any
  // arguments. We could possibly sink this down into `resolveCursor`. Or
  // improve the solver based so we don't need to run the old.
  auto ASTBasedReceiver = [this, CancellationToken, Invok, InputBuffer,
                           fileSystem, Receiver, Offset, Actionables,
                           SymbolGraph](
                              const RequestResult<CursorInfoData> &Res) {
    // AST based completion *always* produces a result
    bool NoResults = Res.isError() || Res.isCancelled();
    if (Res.isValue()) {
      NoResults = Res.value().Symbols.empty();
    }
    if (!NoResults || !InputBuffer) {
      Receiver(Res);
      return;
    }

    CompilerInvocation CompInvok;
    Invok->applyTo(CompInvok);

    SmallVector<const char *, 0> Args;
    Args.reserve(Invok->getArgs().size());
    for (const std::string &Arg : Invok->getArgs()) {
      Args.push_back(Arg.c_str());
    }

    bool SolverProducedResults = false;
    auto SolverBasedReceiver = [&](const RequestResult<CursorInfoData> &Res) {
      SolverProducedResults = true;
      Receiver(Res);
    };

    performWithParamsToCompletionLikeOperation(
        InputBuffer.get(), Offset,
        /*InsertCodeCompletionToken=*/false, Args, fileSystem,
        CancellationToken,
        [&](CancellableResult<CompletionLikeOperationParams> ParmsResult) {
          ParmsResult.mapAsync<CursorInfoResults>(
              [&](auto &Params, auto DeliverTransformed) {
                getIDEInspectionInstance()->cursorInfo(
                    Params.Invocation, Args, fileSystem,
                    Params.completionBuffer, Offset, Params.DiagC,
                    Params.CancellationFlag, DeliverTransformed);
              },
              [&](auto Result) {
                deliverCursorInfoResults(SolverBasedReceiver, Result, *this,
                                         CompInvok, Actionables, SymbolGraph);
              });
        });

    // If the solver based cursor info produced no results, fallback to the
    // original AST based.
    if (!SolverProducedResults) {
      Receiver(Res);
    }
  };

  resolveCursor(*this, InputBufferName, Offset, Length, Actionables,
                SymbolGraph, Invok, /*TryExistingAST=*/true,
                CancelOnSubsequentRequest, fileSystem, CancellationToken,
                ASTBasedReceiver);
}

void SwiftLangSupport::getDiagnostics(
    StringRef PrimaryFilePath, ArrayRef<const char *> Args,
    Optional<VFSOptions> VfsOptions,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<DiagnosticsResult> &)> Receiver) {
  std::string FileSystemError;
  auto FileSystem = getFileSystem(VfsOptions, PrimaryFilePath, FileSystemError);
  if (!FileSystem) {
    Receiver(RequestResult<DiagnosticsResult>::fromError(FileSystemError));
    return;
  }

  std::string InvocationError;
  SwiftInvocationRef Invok = ASTMgr->getTypecheckInvocation(
      Args, PrimaryFilePath, FileSystem, InvocationError);
  if (!InvocationError.empty()) {
    LOG_WARN_FUNC("error creating ASTInvocation: " << InvocationError);
  }
  if (!Invok) {
    Receiver(RequestResult<DiagnosticsResult>::fromError(InvocationError));
    return;
  }

  computeDiagnostics(*this, Invok, FileSystem, CancellationToken, Receiver);
}

void SwiftLangSupport::getRangeInfo(
    StringRef PrimaryFilePath, StringRef InputBufferName, unsigned Offset,
    unsigned Length, bool CancelOnSubsequentRequest,
    ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<RangeInfo> &)> Receiver) {
  if (IFaceGenContexts.get(InputBufferName)) {
    // FIXME: return range info for generated interfaces.
    Receiver(RequestResult<RangeInfo>::fromError(
        "Range info for generated interfaces is not implemented."));
    return;
  }
  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<RangeInfo>::fromError(Error));
    return;
  }
  if (Length == 0) {
    Receiver(RequestResult<RangeInfo>::fromError("Invalid range length."));
    return;
  }
  resolveRange(*this, InputBufferName, Offset, Length, Invok,
               /*TryExistingAST=*/true, CancelOnSubsequentRequest,
               CancellationToken, Receiver);
}

void SwiftLangSupport::getNameInfo(
    StringRef InputFile, unsigned Offset, NameTranslatingInfo &Input,
    ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<NameTranslatingInfo> &)> Receiver) {

  if (auto IFaceGenRef = IFaceGenContexts.get(InputFile)) {
    IFaceGenRef->accessASTAsync([IFaceGenRef, Offset, Input, Receiver] {
      SwiftInterfaceGenContext::ResolvedEntity Entity;
      Entity = IFaceGenRef->resolveEntityForOffset(Offset);
      if (Entity.isResolved()) {
        CompilerInvocation Invok;
        IFaceGenRef->applyTo(Invok);
        if (Entity.Mod) {
          // Module is ignored
        } else {
          // FIXME: Should pass the main module for the interface but currently
          // it's not necessary.
        }
      } else {
        NameTranslatingInfo Info;
        Info.InternalDiagnostic =
            "Unable to resolve entity from generated interface.";
        Receiver(RequestResult<NameTranslatingInfo>::fromResult(Info));
      }
    });
    return;
  }

  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, InputFile, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<NameTranslatingInfo>::fromError(Error));
    return;
  }

  resolveName(*this, InputFile, Offset, Invok, /*TryExistingAST=*/true, Input,
              CancellationToken, Receiver);
}

static void resolveCursorFromUSR(
    SwiftLangSupport &Lang, StringRef InputFile, StringRef USR,
    SwiftInvocationRef Invok, bool TryExistingAST,
    bool CancelOnSubsequentRequest,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver) {
  assert(Invok);

  class CursorInfoConsumer : public SwiftASTConsumer {
    std::string InputFile;
    StringRef USR;
    SwiftLangSupport &Lang;
    SwiftInvocationRef ASTInvok;
    const bool TryExistingAST;
    bool CancelOnSubsequentRequest;
    SourceKitCancellationToken CancellationToken;
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver;
    SmallVector<ImmutableTextSnapshotRef, 4> PreviousASTSnaps;

  public:
    CursorInfoConsumer(
        StringRef InputFile, StringRef USR, SwiftLangSupport &Lang,
        SwiftInvocationRef ASTInvok, bool TryExistingAST,
        bool CancelOnSubsequentRequest,
        SourceKitCancellationToken CancellationToken,
        std::function<void(const RequestResult<CursorInfoData> &)> Receiver)
        : InputFile(InputFile), USR(USR), Lang(Lang),
          ASTInvok(std::move(ASTInvok)), TryExistingAST(TryExistingAST),
          CancelOnSubsequentRequest(CancelOnSubsequentRequest),
          CancellationToken(CancellationToken), Receiver(std::move(Receiver)) {}

    bool canUseASTWithSnapshots(
        ArrayRef<ImmutableTextSnapshotRef> Snapshots) override {
      if (!TryExistingAST) {
        LOG_INFO_FUNC(High, "will resolve using up-to-date AST");
        return false;
      }

      if (!Snapshots.empty()) {
        PreviousASTSnaps.append(Snapshots.begin(), Snapshots.end());
        LOG_INFO_FUNC(High, "will try existing AST");
        return true;
      }

      LOG_INFO_FUNC(High, "will resolve using up-to-date AST");
      return false;
    }

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      auto &CompIns = AstUnit->getCompilerInstance();

      if (USR.startswith("c:")) {
        LOG_WARN_FUNC("lookup for C/C++/ObjC USRs not implemented");
        CursorInfoData Info;
        Info.InternalDiagnostic = "Lookup for C/C++/ObjC USRs not implemented.";
        Receiver(RequestResult<CursorInfoData>::fromResult(Info));
        return;
      }

      auto &context = CompIns.getASTContext();
      TypeDecl *D = Demangle::getTypeDeclForUSR(context, USR);

      if (!D) {
        CursorInfoData Info;
        Info.InternalDiagnostic = "Unable to resolve type from USR.";
        Receiver(RequestResult<CursorInfoData>::fromResult(Info));
        return;
      }

      CompilerInvocation CompInvok;
      ASTInvok->applyTo(CompInvok);

      if (auto *M = dyn_cast<ModuleDecl>(D)) {
        passCursorInfoForModule(M, Lang.getIFaceGenContexts(), CompInvok,
                                Receiver);
      } else {
        auto *DC = D->getDeclContext();

        Type ContainerType;
        if (DC->isTypeContext()) {
          auto ContainerType = DC->getSelfInterfaceType();
          ContainerType =
              D->getInnermostDeclContext()->mapTypeIntoContext(ContainerType);
        }

        ResolvedValueRefCursorInfoPtr Info = new ResolvedValueRefCursorInfo(
            /*SourceFile=*/nullptr, SourceLoc(), D,
            /*CtorTyRef=*/nullptr,
            /*ExtTyRef=*/nullptr,
            /*IsRef=*/false,
            /*Ty=*/Type(), ContainerType,
            /*CustomAttrRef=*/None,
            /*IsKeywordArgument=*/false,
            /*IsDynamic=*/false,
            /*ReceiverTypes=*/{},
            /*ShorthandShadowedDecls=*/{});

        Type selfTy;

        std::string Diagnostic;
        bool Success = passCursorInfoForDecl(
            Info, /*AddRefactorings*/ false,
            /*AddSymbolGraph*/ false, {}, Lang, CompInvok, Diagnostic,
            PreviousASTSnaps,
            /*DidReuseAST=*/false, Receiver);
        if (!Success) {
          if (!PreviousASTSnaps.empty()) {
            // Attempt again using the up-to-date AST.
            resolveCursorFromUSR(Lang, InputFile, USR, ASTInvok,
                                 /*TryExistingAST=*/false,
                                 CancelOnSubsequentRequest,
                                 CompIns.getSourceMgr().getFileSystem(),
                                 CancellationToken, Receiver);
          } else {
            CursorInfoData Info;
            Info.InternalDiagnostic = Diagnostic;
            Receiver(RequestResult<CursorInfoData>::fromResult(Info));
          }
        }
      }
    }

    void cancelled() override {
      Receiver(RequestResult<CursorInfoData>::cancelled());
    }

    void failed(StringRef Error) override {
      LOG_WARN_FUNC("cursor info failed: " << Error);
      Receiver(RequestResult<CursorInfoData>::fromError(Error));
    }
  };

  auto Consumer = std::make_shared<CursorInfoConsumer>(
      InputFile, USR, Lang, Invok, TryExistingAST, CancelOnSubsequentRequest,
      CancellationToken, Receiver);
  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  const void *Once = CancelOnSubsequentRequest ? &OncePerASTToken : nullptr;
  Lang.getASTManager()->processASTAsync(Invok, std::move(Consumer), Once,
                                        CancellationToken, fileSystem);
}

void SwiftLangSupport::getCursorInfoFromUSR(
    StringRef PrimaryFilePath, StringRef InputBufferName, StringRef USR,
    bool CancelOnSubsequentRequest, ArrayRef<const char *> Args,
    Optional<VFSOptions> vfsOptions,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<CursorInfoData> &)> Receiver) {
  std::string error;

  auto fileSystem = getFileSystem(vfsOptions, PrimaryFilePath, error);
  if (!fileSystem)
    return Receiver(RequestResult<CursorInfoData>::fromError(error));

  if (auto IFaceGenRef = IFaceGenContexts.get(PrimaryFilePath)) {
    LOG_WARN_FUNC("Info from usr for generated interface not implemented yet.");
    CursorInfoData Info;
    Info.InternalDiagnostic = "Info for generated interfaces not implemented.";
    Receiver(RequestResult<CursorInfoData>::fromResult(Info));
    return;
  }

  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, fileSystem, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<CursorInfoData>::fromError(Error));
    return;
  }

  resolveCursorFromUSR(*this, InputBufferName, USR, Invok,
                       /*TryExistingAST=*/true, CancelOnSubsequentRequest,
                       fileSystem, CancellationToken, Receiver);
}

//===----------------------------------------------------------------------===//
// SwiftLangSupport::findUSRRange
//===----------------------------------------------------------------------===//

llvm::Optional<std::pair<unsigned, unsigned>>
SwiftLangSupport::findUSRRange(StringRef DocumentName, StringRef USR) {
  if (auto IFaceGenRef = IFaceGenContexts.get(DocumentName))
    return IFaceGenRef->findUSRRange(USR);

  // Only works for a module interface document currently.
  // FIXME: Report it as failed request.
  return None;
}

//===----------------------------------------------------------------------===//
// SwiftLangSupport::findRelatedIdentifiersInFile
//===----------------------------------------------------------------------===//

namespace {
class RelatedIdScanner : public SourceEntityWalker {
  ValueDecl *Dcl;
  llvm::SmallDenseSet<std::pair<unsigned, unsigned>, 8> &Ranges;
  /// Declarations that are tied to the same name as \c Dcl and should thus also
  /// be renamed if \c Dcl is renamed. Most notabliy this contains closure
  /// captures like `[foo]`.
  llvm::SmallVectorImpl<ValueDecl *> &RelatedDecls;
  SourceManager &SourceMgr;
  unsigned BufferID = -1;
  bool Cancelled = false;

public:
  explicit RelatedIdScanner(
      SourceFile &SrcFile, unsigned BufferID, ValueDecl *D,
      llvm::SmallDenseSet<std::pair<unsigned, unsigned>, 8> &Ranges,
      llvm::SmallVectorImpl<ValueDecl *> &RelatedDecls)
      : Ranges(Ranges), RelatedDecls(RelatedDecls),
        SourceMgr(SrcFile.getASTContext().SourceMgr), BufferID(BufferID) {
    if (auto *V = dyn_cast<VarDecl>(D)) {
      // Always use the canonical var decl for comparison. This is so we
      // pick up all occurrences of x in case statements like the below:
      //   case .first(let x), .second(let x)
      //     fallthrough
      //   case .third(let x)
      //     print(x)
      Dcl = V->getCanonicalVarDecl();

      // If we have a property wrapper backing property or projected value, use
      // the wrapped property instead (i.e. if this is _foo or $foo, pretend
      // it's foo).
      if (auto *Wrapped = V->getOriginalWrappedProperty()) {
        Dcl = Wrapped;
      }
    } else {
      Dcl = D;
    }
  }

private:
  bool walkToExprPre(Expr *E) override {
    if (Cancelled)
      return false;

    // Check if there are closure captures like `[foo]` where the caputred
    // variable should also be renamed
    if (auto CaptureList = dyn_cast<CaptureListExpr>(E)) {
      for (auto ShorthandShadow : getShorthandShadows(CaptureList)) {
        if (ShorthandShadow.first == Dcl) {
          RelatedDecls.push_back(ShorthandShadow.second);
        } else if (ShorthandShadow.second == Dcl) {
          RelatedDecls.push_back(ShorthandShadow.first);
        }
      }
    }
    return true;
  }

  bool walkToStmtPre(Stmt *S) override {
    if (Cancelled)
      return false;

    if (auto CondStmt = dyn_cast<LabeledConditionalStmt>(S)) {
      for (auto ShorthandShadow : getShorthandShadows(CondStmt)) {
        if (ShorthandShadow.first == Dcl) {
          RelatedDecls.push_back(ShorthandShadow.second);
        } else if (ShorthandShadow.second == Dcl) {
          RelatedDecls.push_back(ShorthandShadow.first);
        }
      }
    }
    return true;
  }

  bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
    if (Cancelled)
      return false;
    if (auto *V = dyn_cast<VarDecl>(D)) {
      // Handle references to the implicitly generated vars in case statements
      // matching multiple patterns
      D = V->getCanonicalVarDecl();
    }
    if (D == Dcl)
      return passId(Range);
    return true;
  }

  bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                          TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef, Type T,
                          ReferenceMetaData Data) override {
    if (Cancelled)
      return false;

    if (auto *V = dyn_cast<VarDecl>(D)) {
      D = V->getCanonicalVarDecl();

      // If we have a property wrapper backing property or projected value, use
      // the wrapped property for comparison instead (i.e. if this is _foo or
      // $foo, pretend it's foo).
      if (auto *Wrapped = V->getOriginalWrappedProperty()) {
        assert(Range.getByteLength() > 1 &&
               (Range.str().front() == '_' || Range.str().front() == '$'));
        D = Wrapped;
        Range = CharSourceRange(Range.getStart().getAdvancedLoc(1), Range.getByteLength() - 1);
      }
    } else if (CtorTyRef) {
      D = CtorTyRef;
    }

    if (D == Dcl)
      return passId(Range);
    return true;
  }

  bool passId(CharSourceRange Range) {
    unsigned Offset = SourceMgr.getLocOffsetInBuffer(Range.getStart(),BufferID);
    Ranges.insert({Offset, Range.getByteLength()});
    return !Cancelled;
  }
};

} // end anonymous namespace

void SwiftLangSupport::findRelatedIdentifiersInFile(
    StringRef PrimaryFilePath, StringRef InputBufferName, unsigned Offset,
    bool CancelOnSubsequentRequest, ArrayRef<const char *> Args,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<RelatedIdentsInfo> &)> Receiver) {

  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<RelatedIdentsInfo>::fromError(Error));
    return;
  }

  class RelatedIdConsumer : public SwiftASTConsumer {
    std::string InputFile;
    unsigned Offset;
    std::function<void(const RequestResult<RelatedIdentsInfo> &)> Receiver;
    SwiftInvocationRef Invok;

  public:
    RelatedIdConsumer(
        StringRef InputFile, unsigned Offset,
        std::function<void(const RequestResult<RelatedIdentsInfo> &)> Receiver,
        SwiftInvocationRef Invok)
        : InputFile(InputFile.str()), Offset(Offset),
          Receiver(std::move(Receiver)), Invok(Invok) {}

    // FIXME: Don't silently eat errors here.
    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      auto &CompInst = AstUnit->getCompilerInstance();

      auto *SrcFile = retrieveInputFile(InputFile, CompInst);
      if (!SrcFile) {
        Receiver(RequestResult<RelatedIdentsInfo>::fromError(
            "Unable to find input file"));
        return;
      }

      SmallVector<std::pair<unsigned, unsigned>, 8> Ranges;

      auto Action = [&]() {
        unsigned BufferID = SrcFile->getBufferID().value();
        SourceLoc Loc =
          Lexer::getLocForStartOfToken(CompInst.getSourceMgr(), BufferID, Offset);
        if (Loc.isInvalid())
          return;

        ResolvedCursorInfoPtr CursorInfo =
            evaluateOrDefault(CompInst.getASTContext().evaluator,
                              CursorInfoRequest{CursorInfoOwner(SrcFile, Loc)},
                              new ResolvedCursorInfo());
        auto ValueRefCursorInfo =
            dyn_cast<ResolvedValueRefCursorInfo>(CursorInfo);
        if (!ValueRefCursorInfo)
          return;
        if (ValueRefCursorInfo->isKeywordArgument())
          return;

        ValueDecl *VD = ValueRefCursorInfo->typeOrValue();
        if (!VD)
          return; // This was a module reference.

        // Only accept pointing to an identifier.
        if (!ValueRefCursorInfo->isRef() &&
            (isa<ConstructorDecl>(VD) || isa<DestructorDecl>(VD) ||
             isa<SubscriptDecl>(VD)))
          return;
        if (VD->isOperator())
          return;

        // Record ranges in a set first so we don't record some ranges twice.
        // This could happen in capture lists where e.g. `[foo]` is both the
        // reference of the captured variable and the declaration of the
        // variable usable in the closure.
        llvm::SmallDenseSet<std::pair<unsigned, unsigned>, 8> RangesSet;

        // List of decls whose ranges should be reported as related identifiers.
        SmallVector<ValueDecl *, 2> Worklist;
        Worklist.push_back(VD);

        // Decls that we have already visited, so we don't walk circles.
        SmallPtrSet<ValueDecl *, 2> VisitedDecls;
        while (!Worklist.empty()) {
          ValueDecl *Dcl = Worklist.back();
          Worklist.pop_back();
          if (!VisitedDecls.insert(Dcl).second) {
            // We have already visited this decl. Don't visit it again.
            continue;
          }

          RelatedIdScanner Scanner(*SrcFile, BufferID, Dcl, RangesSet,
                                   Worklist);

          if (auto *Case = getCaseStmtOfCanonicalVar(Dcl)) {
            Scanner.walk(Case);
            while ((Case = Case->getFallthroughDest().getPtrOrNull())) {
              Scanner.walk(Case);
            }
          } else if (DeclContext *LocalDC =
                         Dcl->getDeclContext()->getLocalContext()) {
            Scanner.walk(LocalDC);
          } else {
            Scanner.walk(*SrcFile);
          }
        }

        // Sort ranges so we get deterministic output.
        Ranges.insert(Ranges.end(), RangesSet.begin(), RangesSet.end());
        llvm::sort(Ranges,
                   [](const std::pair<unsigned, unsigned> &LHS,
                      const std::pair<unsigned, unsigned> &RHS) -> bool {
                     if (LHS.first == RHS.first) {
                       return LHS.second < RHS.second;
                     } else {
                       return LHS.first < RHS.first;
                     }
                   });
      };
      Action();
      RelatedIdentsInfo Info;
      Info.Ranges = Ranges;
      Receiver(RequestResult<RelatedIdentsInfo>::fromResult(Info));
    }

    void cancelled() override {
      Receiver(RequestResult<RelatedIdentsInfo>::cancelled());
    }

    void failed(StringRef Error) override {
      LOG_WARN_FUNC("related idents failed: " << Error);
      Receiver(RequestResult<RelatedIdentsInfo>::fromError(Error));
    }

    static CaseStmt *getCaseStmtOfCanonicalVar(Decl *D) {
      assert(D);
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        if (auto *Canonical = VD->getCanonicalVarDecl()) {
          return dyn_cast_or_null<CaseStmt>(Canonical->getRecursiveParentPatternStmt());
        }
      }
      return nullptr;
    }
  };

  auto Consumer = std::make_shared<RelatedIdConsumer>(InputBufferName, Offset,
                                                      Receiver, Invok);
  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  const void *Once = CancelOnSubsequentRequest ? &OncePerASTToken : nullptr;
  ASTMgr->processASTAsync(Invok, std::move(Consumer), Once, CancellationToken,
                          llvm::vfs::getRealFileSystem());
}

//===----------------------------------------------------------------------===//
// SwiftLangSupport::findActiveRegionsInFile
//===----------------------------------------------------------------------===//

namespace {
class IfConfigScanner : public SourceEntityWalker {
  unsigned BufferID = -1;
  SmallVectorImpl<IfConfigInfo> &Infos;
  bool Cancelled = false;

public:
  explicit IfConfigScanner(unsigned BufferID,
                           SmallVectorImpl<IfConfigInfo> &Infos)
      : BufferID(BufferID), Infos(Infos) {}

private:
  bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
    if (Cancelled)
      return false;

    if (auto *IfDecl = dyn_cast<IfConfigDecl>(D)) {
      for (auto &Clause : IfDecl->getClauses()) {
        unsigned Offset = D->getASTContext().SourceMgr.getLocOffsetInBuffer(
            Clause.Loc, BufferID);
        Infos.emplace_back(Offset, Clause.isActive);
      }
    }

    return true;
  }
};

} // end anonymous namespace

void SwiftLangSupport::findActiveRegionsInFile(
    StringRef PrimaryFilePath, StringRef InputBufferName,
    ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<ActiveRegionsInfo> &)> Receiver) {

  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<ActiveRegionsInfo>::fromError(Error));
    return;
  }

  class IfConfigConsumer : public SwiftASTConsumer {
    std::string InputFile;
    std::function<void(const RequestResult<ActiveRegionsInfo> &)> Receiver;
    SwiftInvocationRef Invok;

  public:
    IfConfigConsumer(
        StringRef InputFile,
        std::function<void(const RequestResult<ActiveRegionsInfo> &)> Receiver,
        SwiftInvocationRef Invok)
        : InputFile(InputFile.str()), Receiver(std::move(Receiver)),
          Invok(Invok) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      auto *SF = retrieveInputFile(InputFile, AstUnit->getCompilerInstance());
      if (!SF) {
        Receiver(RequestResult<ActiveRegionsInfo>::fromError(
            "Unable to find input file"));
        return;
      }

      SmallVector<IfConfigInfo> Configs;
      IfConfigScanner Scanner(*SF->getBufferID(), Configs);
      Scanner.walk(SF);

      // Sort by offset so nested decls are reported
      // in source order (not tree order).
      llvm::sort(Configs,
                 [](const IfConfigInfo &LHS, const IfConfigInfo &RHS) -> bool {
                   return LHS.Offset < RHS.Offset;
                 });
      ActiveRegionsInfo Info;
      Info.Configs = Configs;
      Receiver(RequestResult<ActiveRegionsInfo>::fromResult(Info));
    }

    void cancelled() override {
      Receiver(RequestResult<ActiveRegionsInfo>::cancelled());
    }

    void failed(StringRef Error) override {
      LOG_WARN_FUNC("inactive ranges failed: " << Error);
      Receiver(RequestResult<ActiveRegionsInfo>::fromError(Error));
    }
  };

  auto Consumer =
      std::make_shared<IfConfigConsumer>(InputBufferName, Receiver, Invok);
  ASTMgr->processASTAsync(Invok, std::move(Consumer),
                          /*OncePerASTToken=*/nullptr, CancellationToken,
                          llvm::vfs::getRealFileSystem());
}

//===----------------------------------------------------------------------===//
// SwiftLangSupport::semanticRefactoring
//===----------------------------------------------------------------------===//

static RefactoringKind getIDERefactoringKind(SemanticRefactoringInfo Info) {
  switch(Info.Kind) {
    case SemanticRefactoringKind::None: return RefactoringKind::None;
#define SEMANTIC_REFACTORING(KIND, NAME, ID)                                   \
    case SemanticRefactoringKind::KIND: return RefactoringKind::KIND;
#include "swift/Refactoring/RefactoringKinds.def"
  }
}

void SwiftLangSupport::semanticRefactoring(
    StringRef PrimaryFilePath, SemanticRefactoringInfo Info,
    ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
    CategorizedEditsReceiver Receiver) {
  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<ArrayRef<CategorizedEdits>>::fromError(Error));
    return;
  }
  assert(Invok);

  class SemaRefactoringConsumer : public SwiftASTConsumer {
    SemanticRefactoringInfo Info;
    CategorizedEditsReceiver Receiver;

  public:
    SemaRefactoringConsumer(SemanticRefactoringInfo Info,
                            CategorizedEditsReceiver Receiver) : Info(Info),
                                                Receiver(std::move(Receiver)) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      RequestRefactoringEditConsumer EditConsumer(Receiver);

      auto &CompIns = AstUnit->getCompilerInstance();

      RefactoringOptions Opts(getIDERefactoringKind(Info));

      SourceFile *SF = retrieveInputFile(Info.InputBufferName, CompIns);
      if (!SF) {
        Receiver(RequestResult<ArrayRef<CategorizedEdits>>::fromError(
            "Unable to find input file"));
        return;
      }

      Opts.Range.BufferID = *SF->getBufferID();
      Opts.Range.Line = Info.Line;
      Opts.Range.Column = Info.Column;
      Opts.Range.Length = Info.Length;
      Opts.PreferredName = Info.PreferredName.str();

      refactorSwiftModule(CompIns.getMainModule(), Opts, EditConsumer,
                          EditConsumer);
    }

    void cancelled() override {
      Receiver(RequestResult<ArrayRef<CategorizedEdits>>::cancelled());
    }

    void failed(StringRef Error) override {
      Receiver(RequestResult<ArrayRef<CategorizedEdits>>::fromError(Error));
    }
  };

  auto Consumer = std::make_shared<SemaRefactoringConsumer>(Info, Receiver);
  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  getASTManager()->processASTAsync(Invok, std::move(Consumer), &OncePerASTToken,
                                   CancellationToken,
                                   llvm::vfs::getRealFileSystem());
}

void SwiftLangSupport::collectExpressionTypes(
    StringRef PrimaryFilePath, StringRef InputBufferName,
    ArrayRef<const char *> Args, ArrayRef<const char *> ExpectedProtocols,
    bool FullyQualified, bool CanonicalType,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<ExpressionTypesInFile> &)>
        Receiver) {
  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<ExpressionTypesInFile>::fromError(Error));
    return;
  }
  assert(Invok);
  class ExpressionTypeCollector: public SwiftASTConsumer {
    std::function<void(const RequestResult<ExpressionTypesInFile> &)> Receiver;
    std::string InputFile;
    std::vector<const char *> ExpectedProtocols;
    bool FullyQualified;
    bool CanonicalType;
  public:
    ExpressionTypeCollector(
        std::function<void(const RequestResult<ExpressionTypesInFile> &)>
            Receiver,
        StringRef InputFile, ArrayRef<const char *> ExpectedProtocols,
        bool FullyQualified, bool CanonicalType)
        : Receiver(std::move(Receiver)), InputFile(InputFile.str()),
          ExpectedProtocols(ExpectedProtocols.vec()),
          FullyQualified(FullyQualified), CanonicalType(CanonicalType) {}
    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      SourceFile *SF =
          retrieveInputFile(InputFile, AstUnit->getCompilerInstance());
      if (!SF) {
        Receiver(RequestResult<ExpressionTypesInFile>::fromError(
            "Unable to find input file"));
        return;
      }

      std::vector<ExpressionTypeInfo> Scratch;
      llvm::SmallString<256> TypeBuffer;
      llvm::raw_svector_ostream OS(TypeBuffer);
      ExpressionTypesInFile Result;
      for (auto Item :
           collectExpressionType(*SF, ExpectedProtocols, Scratch,
                                 FullyQualified, CanonicalType, OS)) {
        Result.Results.push_back({Item.offset, Item.length, Item.typeOffset, {}});
        for (auto P: Item.protocols) {
          Result.Results.back().ProtocolOffsets.push_back(P.first);
        }
      }
      Result.TypeBuffer = OS.str();
      Receiver(RequestResult<ExpressionTypesInFile>::fromResult(Result));
    }

    void cancelled() override {
      Receiver(RequestResult<ExpressionTypesInFile>::cancelled());
    }

    void failed(StringRef Error) override {
      Receiver(RequestResult<ExpressionTypesInFile>::fromError(Error));
    }
  };
  auto Collector = std::make_shared<ExpressionTypeCollector>(
      Receiver, InputBufferName, ExpectedProtocols, FullyQualified,
      CanonicalType);
  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  getASTManager()->processASTAsync(Invok, std::move(Collector),
                                   &OncePerASTToken, CancellationToken,
                                   llvm::vfs::getRealFileSystem());
}

void SwiftLangSupport::collectVariableTypes(
    StringRef PrimaryFilePath, StringRef InputBufferName,
    ArrayRef<const char *> Args, Optional<unsigned> Offset,
    Optional<unsigned> Length, bool FullyQualified,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<VariableTypesInFile> &)> Receiver) {
  std::string Error;
  SwiftInvocationRef Invok =
      ASTMgr->getTypecheckInvocation(Args, PrimaryFilePath, Error);
  if (!Invok) {
    LOG_WARN_FUNC("failed to create an ASTInvocation: " << Error);
    Receiver(RequestResult<VariableTypesInFile>::fromError(Error));
    return;
  }
  assert(Invok);

  class VariableTypeCollectorASTConsumer : public SwiftASTConsumer {
  private:
    std::function<void(const RequestResult<VariableTypesInFile> &)> Receiver;
    std::string InputFile;
    Optional<unsigned> Offset;
    Optional<unsigned> Length;
    bool FullyQualified;

  public:
    VariableTypeCollectorASTConsumer(
        std::function<void(const RequestResult<VariableTypesInFile> &)>
            Receiver,
        StringRef InputFile, Optional<unsigned> Offset,
        Optional<unsigned> Length, bool FullyQualified)
        : Receiver(std::move(Receiver)), InputFile(InputFile), Offset(Offset),
          Length(Length), FullyQualified(FullyQualified) {}

    void handlePrimaryAST(ASTUnitRef AstUnit) override {
      auto &CompInst = AstUnit->getCompilerInstance();
      SourceFile *SF = retrieveInputFile(InputFile, CompInst);
      if (!SF) {
        Receiver(RequestResult<VariableTypesInFile>::fromError(
            "Unable to find input file"));
        return;
      }

      // Construct the range for which variable types are to be queried. If
      // offset/length are unset, the (default) range will be used, which
      // corresponds to the entire document.
      SourceRange Range;
      if (Offset.has_value() && Length.has_value()) {
        auto &SM = CompInst.getSourceMgr();
        unsigned BufferID = SF->getBufferID().value();
        SourceLoc Start = Lexer::getLocForStartOfToken(SM, BufferID, *Offset);
        SourceLoc End =
            Lexer::getLocForStartOfToken(SM, BufferID, *Offset + *Length);
        Range = SourceRange(Start, End);
      }

      std::vector<VariableTypeInfo> Infos;
      std::string TypeBuffer;
      llvm::raw_string_ostream OS(TypeBuffer);
      VariableTypesInFile Result;

      collectVariableType(*SF, Range, FullyQualified, Infos, OS);

      for (auto Info : Infos) {
        Result.Results.push_back({Info.Offset, Info.Length, Info.TypeOffset, Info.HasExplicitType});
      }
      Result.TypeBuffer = OS.str();
      Receiver(RequestResult<VariableTypesInFile>::fromResult(Result));
    }

    void cancelled() override {
      Receiver(RequestResult<VariableTypesInFile>::cancelled());
    }

    void failed(StringRef Error) override {
      Receiver(RequestResult<VariableTypesInFile>::fromError(Error));
    }
  };

  auto Collector = std::make_shared<VariableTypeCollectorASTConsumer>(
      Receiver, InputBufferName, Offset, Length, FullyQualified);
  /// FIXME: When request cancellation is implemented and Xcode adopts it,
  /// don't use 'OncePerASTToken'.
  static const char OncePerASTToken = 0;
  getASTManager()->processASTAsync(Invok, std::move(Collector),
                                   &OncePerASTToken, CancellationToken,
                                   llvm::vfs::getRealFileSystem());
}
