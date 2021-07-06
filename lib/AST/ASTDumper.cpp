//===--- ASTDumper.cpp - Swift Language AST Dumper ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements dumping for the Swift ASTs.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/QuotedString.h"
#include "swift/Basic/STLExtras.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

struct TerminalColor {
  llvm::raw_ostream::Colors Color;
  bool Bold;
};

#define DEF_COLOR(NAME, COLOR, BOLD) \
static const TerminalColor NAME##Color = { llvm::raw_ostream::COLOR, BOLD };

DEF_COLOR(Func, YELLOW, false)
DEF_COLOR(Range, YELLOW, false)
DEF_COLOR(AccessLevel, YELLOW, false)
DEF_COLOR(ASTNode, YELLOW, true)
DEF_COLOR(Parameter, YELLOW, false)
DEF_COLOR(Extension, MAGENTA, false)
DEF_COLOR(Pattern, RED, true)
DEF_COLOR(Override, RED, false)
DEF_COLOR(Stmt, RED, true)
DEF_COLOR(Captures, RED, false)
DEF_COLOR(Arguments, RED, false)
DEF_COLOR(TypeRepr, GREEN, false)
DEF_COLOR(LiteralValue, GREEN, false)
DEF_COLOR(Decl, GREEN, true)
DEF_COLOR(Parenthesis, BLUE, false)
DEF_COLOR(Type, BLUE, false)
DEF_COLOR(Discriminator, BLUE, false)
DEF_COLOR(InterfaceType, GREEN, false)
DEF_COLOR(Identifier, GREEN, false)
DEF_COLOR(Expr, MAGENTA, true)
DEF_COLOR(ExprModifier, CYAN, false)
DEF_COLOR(DeclModifier, CYAN, false)
DEF_COLOR(ClosureModifier, CYAN, false)
DEF_COLOR(TypeField, CYAN, false)
DEF_COLOR(Location, CYAN, false)
DEF_COLOR(Label, WHITE, true)

#undef DEF_COLOR

namespace {
  /// RAII object that prints with the given color, if color is supported on the
  /// given stream.
  class PrintWithColorRAII {
    raw_ostream &OS;
    bool ShowColors;

  public:
    PrintWithColorRAII(raw_ostream &os, TerminalColor color)
    : OS(os), ShowColors(false)
    {
      ShowColors = os.has_colors();

      if (ShowColors)
        OS.changeColor(color.Color, color.Bold);
    }

    ~PrintWithColorRAII() {
      if (ShowColors) {
        OS.resetColor();
      }
    }

    raw_ostream &getOS() const { return OS; }

    template<typename T>
    friend raw_ostream &operator<<(PrintWithColorRAII &&printer,
                                   const T &value){
      printer.OS << value;
      return printer.OS;
    }

  };

/// Contains helpers for looking up types in expressions. Used by PrintExpr,
/// but threaded through the other printers too.
struct ExprTypeDelegate {
  llvm::function_ref<Type(Expr *)> GetTypeOfExpr;
  llvm::function_ref<Type(TypeRepr *)> GetTypeOfTypeRepr;
  llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
      GetTypeOfKeyPathComponent;

  ExprTypeDelegate(llvm::function_ref<Type(Expr *)> getTypeOfExpr,
                   llvm::function_ref<Type(TypeRepr *)> getTypeOfTypeRepr,
                   llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
                       getTypeOfKeyPathComponent)
    : GetTypeOfExpr(getTypeOfExpr), GetTypeOfTypeRepr(getTypeOfTypeRepr),
      GetTypeOfKeyPathComponent(getTypeOfKeyPathComponent) { }

  ExprTypeDelegate()
    : ExprTypeDelegate([](Expr *E) -> Type { return E->getType(); }, nullptr,
                       [](KeyPathExpr *E, unsigned index) -> Type {
                         return E->getComponents()[index].getComponentType();
                       }) { }
};

/// Helper class for dumping an AST node.
///
/// \c ASTNodeDumper is an RAII type which prints the surrounding structure of
/// an AST node and provides methods for printing information from it, too. It
/// handles printing the opening and closing parentheses, correctly formatting
/// and coloring things, etc.
class ASTNodeDumper {
  ASTContext *Ctx;
  raw_ostream *OS;
  ExprTypeDelegate *Delegate;

public:
  unsigned IndentChildren;

  ASTNodeDumper(ASTContext *ctx, raw_ostream &os, ExprTypeDelegate &delegate,
                unsigned indent,
                const char *Name, StringRef Label, TerminalColor Color)
    : Ctx(ctx), OS(&os), Delegate(&delegate), IndentChildren(indent + 2)
  {
    OS->indent(indent /* note this is parent indentation */);
    PrintWithColorRAII(*OS, ParenthesisColor) << '(';
    PrintWithColorRAII(*OS, LabelColor) << Label;
    if (!Label.empty())
      PrintWithColorRAII(*OS, LabelColor) << "=";
    PrintWithColorRAII(*OS, Color) << Name;
  }

  template<typename Creator>
  ASTNodeDumper(Creator *creator, const char *Name, StringRef Label,
                TerminalColor Color)
    : ASTNodeDumper(creator->Ctx, creator->OS, creator->Delegate,
                    creator->Indent, Name, Label, Color) { }

  // Type is uncopyable because its destructor has side effects.
  ASTNodeDumper(const ASTNodeDumper&) = delete;
  ASTNodeDumper& operator=(const ASTNodeDumper &) = delete;

  ASTNodeDumper(ASTNodeDumper &&Other)
    : Ctx(Other.Ctx), OS(Other.OS), Delegate(Other.Delegate),
      IndentChildren(Other.IndentChildren)
  {
    Other.OS = nullptr;
  }

  ASTNodeDumper& operator=(ASTNodeDumper&& Other) {
    Ctx = Other.Ctx;
    OS = Other.OS;
    Delegate = Other.Delegate;
    IndentChildren = Other.IndentChildren;

    Other.OS = nullptr;

    return *this;
  }

  ~ASTNodeDumper() {
    if (OS)
      PrintWithColorRAII(*OS, ParenthesisColor) << ')';
  }

  ASTNodeDumper child(const char *Name, StringRef Label, TerminalColor Color) {
    *OS << '\n';
    return ASTNodeDumper(Ctx, *OS, *Delegate, IndentChildren,
                         Name, Label, Color);
  }

  void printRec(Decl *D, StringRef Label = "");
  void printRec(Expr *E, StringRef Label = "");
  void printRec(Stmt *S, StringRef Label = "");
  void printRec(TypeRepr *T, StringRef Label = "");
  void printRec(const Pattern *P, StringRef Label = "");
  void printRec(Type Ty, StringRef Label = "");

  void printRec(ParameterList *params, StringRef Label = "");
  void printRec(StmtConditionElement C, StringRef Label = "");
  void printRec(ProtocolConformanceRef conf, StringRef Label = "");

  void printRec(const ASTNode N, StringRef Label = "") {
         if (auto n = N.dyn_cast<Expr *>()) printRec(n, Label);
    else if (auto n = N.dyn_cast<Stmt *>()) printRec(n, Label);
    else if (auto n = N.dyn_cast<Decl *>()) printRec(n, Label);
    else if (auto n = N.dyn_cast<Pattern *>()) printRec(n, Label);
    else if (auto n = N.dyn_cast<TypeRepr *>()) printRec(n, Label);
    else llvm_unreachable("unknown ASTNode");
  }

  template<typename T>
  void printRec(ArrayRef<T> elems, StringRef Label) {
    auto childDump = child("array", Label, ExprModifierColor);
    for (auto elt : elems) {
      childDump.printRec(elt, "");
    }
  }

  void printLabel(StringRef label, TerminalColor Color = DeclModifierColor) {
    *OS << " ";
    PrintWithColorRAII(*OS, Color) << label;
    if (!label.empty())
      PrintWithColorRAII(*OS, Color) << "=";
  }

  /// Print a field with a value and an optional label.
  template<typename T>
  void print(const T &value, StringRef label = "",
             TerminalColor Color = DeclModifierColor) {
    printLabel(label, Color);
    PrintWithColorRAII(*OS, Color) << value;
  }

  void print(SourceLoc value, StringRef label = "",
             TerminalColor Color = DeclModifierColor) {
    if (!Ctx) return;

    printLabel(label, Color);
    if (value.isValid())
      value.print(PrintWithColorRAII(*OS, Color).getOS(), Ctx->SourceMgr);
    else
      PrintWithColorRAII(*OS, Color) << "<<invalid>>";
  }

  /// Print a single flag.
  void printFlag(StringRef label, TerminalColor Color = DeclModifierColor) {
    PrintWithColorRAII(*OS, Color) << " " << label;
  }

  /// Print a single flag if it is set.
  void printFlag(bool isSet, StringRef label,
                 TerminalColor Color = DeclModifierColor) {
    if (isSet)
      printFlag(label, Color);
  }

  /// Print the name of a declaration being referenced.
  void printDeclRef(ConcreteDeclRef declRef, StringRef label,
                    TerminalColor Color = DeclColor) {
    printLabel(label, Color);
    declRef.dump(PrintWithColorRAII(*OS, Color).getOS());
  }

  /// Pretty-print a TypeRepr.
  void printTypeRepr(TypeRepr *T, StringRef label) {
    printLabel(label, TypeReprColor);

    PrintWithColorRAII(*OS, TypeReprColor) << "'";
    if (T)
      T->print(PrintWithColorRAII(*OS, TypeReprColor).getOS());
    else
      PrintWithColorRAII(*OS, TypeReprColor) << "<<NULL>>";
    PrintWithColorRAII(*OS, TypeReprColor) << "'";

private:
  template <typename Name>
  void printNodeNameImpl(Name name, StringRef label) {
    printLabel(label, IdentifierColor);
    PrintWithColorRAII(*OS, IdentifierColor) << '"' << name << '"';
  }

public:
  /// Print the name of the node we are currently dumping. Do not use
  /// this to just print DeclNameRefs in general; only use it for a declaration
  /// name that the node "is".
  void printNodeName(StringRef name) {
    printNodeNameImpl(name, "");
  }

  /// Print the name of the node we are currently dumping. Do not use
  /// this to just print DeclNameRefs in general; only use it for a declaration
  /// name that the node "is".
  void printNodeName(DeclNameRef name) {
    printNodeNameImpl(name, "");
  }

  /// Print the name of the node we are currently dumping. Do not use
  /// this to just print DeclNames in general; only use it for a declaration
  /// name that the node "is".
  void printNodeName(DeclName name) {
    printNodeNameImpl(name, "");
  }

  /// Print the name of the node we are currently dumping. Do not use
  /// this to just print ValueDecls in general; only use it for a declaration
  /// that the node "is".
  void printNodeName(const ValueDecl *D) {
    SmallString<16> label;

    // Accessors don't have a usable name, so we instead prepend a field to the
    // name of the accessor's storage.
    if (auto *AD = dyn_cast<AccessorDecl>(D)) {
      llvm::raw_svector_ostream(label)
          << getAccessorKindString(AD->getAccessorKind()) << "_for";
      D = AD->getStorage();
    }

    if (D->getName()) {
      printNodeNameImpl(D->getName(), label);
      return;
    }

    SmallString<32> scratch;
    llvm::raw_svector_ostream(scratch) << "anonymous @ " << (const void*)D;
    printNodeNameImpl(scratch, label);
  }

  /// Print the source range of the node we are currently displaying. Do not use
  /// this to just print SourceRanges in general; only use it for the range
  /// of the current node.
  void printNodeRange(SourceRange R) {
    if (Ctx && R.isValid()) {
      printLabel("range", RangeColor);
      R.print(PrintWithColorRAII(*OS, RangeColor).getOS(),
              Ctx->SourceMgr, /*PrintText=*/false);
    }
  }

  /// Print the source location of the node we are currently displaying. Do not
  /// use this to just print SourceLocs in general; only use it for the location
  /// of the current node.
  void printNodeLocation(SourceLoc L) {
    if (!L.isValid()) return;
    print(L, "location", LocationColor);
  }

  // These exist for backwards compatibility. The goal is to turn all uses of
  // them into method calls instead.

  template <typename T>
  friend ASTNodeDumper &operator << (ASTNodeDumper &dumper, const T &value) {
    *dumper.OS << value;
    return dumper;
  }

  raw_ostream &getOS() {
    return *OS;
  }

  PrintWithColorRAII colored(TerminalColor color) {
    return PrintWithColorRAII(*OS, color);
  }
};
} // end anonymous namespace

static void printGenericParameters(raw_ostream &OS, GenericParamList *Params) {
  if (!Params)
    return;
  OS << ' ';
  Params->print(OS);
}


static StringRef
getSILFunctionTypeRepresentationString(SILFunctionType::Representation value) {
  switch (value) {
  case SILFunctionType::Representation::Thick: return "thick";
  case SILFunctionType::Representation::Block: return "block";
  case SILFunctionType::Representation::CFunctionPointer: return "c";
  case SILFunctionType::Representation::Thin: return "thin";
  case SILFunctionType::Representation::Method: return "method";
  case SILFunctionType::Representation::ObjCMethod: return "objc_method";
  case SILFunctionType::Representation::WitnessMethod: return "witness_method";
  case SILFunctionType::Representation::Closure: return "closure";
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

StringRef swift::getReadImplKindName(ReadImplKind kind) {
  switch (kind) {
  case ReadImplKind::Stored:
    return "stored";
  case ReadImplKind::Inherited:
    return "inherited";
  case ReadImplKind::Get:
    return "getter";
  case ReadImplKind::Address:
    return "addressor";
  case ReadImplKind::Read:
    return "read_coroutine";
  }
  llvm_unreachable("bad kind");
}

StringRef swift::getWriteImplKindName(WriteImplKind kind) {
  switch (kind) {
  case WriteImplKind::Immutable:
    return "immutable";
  case WriteImplKind::Stored:
    return "stored";
  case WriteImplKind::StoredWithObservers:
    return "stored_with_observers";
  case WriteImplKind::InheritedWithObservers:
    return "inherited_with_observers";
  case WriteImplKind::Set:
    return "setter";
  case WriteImplKind::MutableAddress:
    return "mutable_addressor";
  case WriteImplKind::Modify:
    return "modify_coroutine";
  }
  llvm_unreachable("bad kind");
}

StringRef swift::getReadWriteImplKindName(ReadWriteImplKind kind) {
  switch (kind) {
  case ReadWriteImplKind::Immutable:
    return "immutable";
  case ReadWriteImplKind::Stored:
    return "stored";
  case ReadWriteImplKind::MutableAddress:
    return "mutable_addressor";
  case ReadWriteImplKind::MaterializeToTemporary:
    return "materialize_to_temporary";
  case ReadWriteImplKind::Modify:
    return "modify_coroutine";
  case ReadWriteImplKind::StoredWithDidSet:
    return "stored_with_didset";
  case ReadWriteImplKind::InheritedWithDidSet:
    return "inherited_with_didset";
  }
  llvm_unreachable("bad kind");
}

static StringRef getImportKindString(ImportKind value) {
  switch (value) {
  case ImportKind::Module: return "module";
  case ImportKind::Type: return "type";
  case ImportKind::Struct: return "struct";
  case ImportKind::Class: return "class";
  case ImportKind::Enum: return "enum";
  case ImportKind::Protocol: return "protocol";
  case ImportKind::Var: return "var";
  case ImportKind::Func: return "func";
  }
  
  llvm_unreachable("Unhandled ImportKind in switch.");
}

static StringRef
getForeignErrorConventionKindString(ForeignErrorConvention::Kind value) {
  switch (value) {
  case ForeignErrorConvention::ZeroResult: return "ZeroResult";
  case ForeignErrorConvention::NonZeroResult: return "NonZeroResult";
  case ForeignErrorConvention::ZeroPreservedResult: return "ZeroPreservedResult";
  case ForeignErrorConvention::NilResult: return "NilResult";
  case ForeignErrorConvention::NonNilError: return "NonNilError";
  }

  llvm_unreachable("Unhandled ForeignErrorConvention in switch.");
}
static StringRef getDefaultArgumentKindString(DefaultArgumentKind value) {
  switch (value) {
    case DefaultArgumentKind::None: return "none";
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    case DefaultArgumentKind::NAME: return STRING;
#include "swift/AST/MagicIdentifierKinds.def"
    case DefaultArgumentKind::Inherited: return "inherited";
    case DefaultArgumentKind::NilLiteral: return "nil";
    case DefaultArgumentKind::EmptyArray: return "[]";
    case DefaultArgumentKind::EmptyDictionary: return "[:]";
    case DefaultArgumentKind::Normal: return "normal";
    case DefaultArgumentKind::StoredProperty: return "stored property";
  }

  llvm_unreachable("Unhandled DefaultArgumentKind in switch.");
}
static StringRef
getObjCSelectorExprKindString(ObjCSelectorExpr::ObjCSelectorKind value) {
  switch (value) {
    case ObjCSelectorExpr::Method: return "method";
    case ObjCSelectorExpr::Getter: return "getter";
    case ObjCSelectorExpr::Setter: return "setter";
  }

  llvm_unreachable("Unhandled ObjCSelectorExpr in switch.");
}
static StringRef getAccessSemanticsString(AccessSemantics value) {
  switch (value) {
    case AccessSemantics::Ordinary: return "ordinary";
    case AccessSemantics::DirectToStorage: return "direct_to_storage";
    case AccessSemantics::DirectToImplementation: return "direct_to_impl";
  }

  llvm_unreachable("Unhandled AccessSemantics in switch.");
}
static StringRef getMetatypeRepresentationString(MetatypeRepresentation value) {
  switch (value) {
    case MetatypeRepresentation::Thin: return "thin";
    case MetatypeRepresentation::Thick: return "thick";
    case MetatypeRepresentation::ObjC: return "@objc";
  }

  llvm_unreachable("Unhandled MetatypeRepresentation in switch.");
}
static StringRef
getStringLiteralExprEncodingString(StringLiteralExpr::Encoding value) {
  switch (value) {
    case StringLiteralExpr::UTF8: return "utf8";
    case StringLiteralExpr::OneUnicodeScalar: return "unicodeScalar";
  }

  llvm_unreachable("Unhandled StringLiteral in switch.");
}
static StringRef getCtorInitializerKindString(CtorInitializerKind value) {
  switch (value) {
    case CtorInitializerKind::Designated: return "designated";
    case CtorInitializerKind::Convenience: return "convenience";
    case CtorInitializerKind::ConvenienceFactory: return "convenience_factory";
    case CtorInitializerKind::Factory: return "factory";
  }

  llvm_unreachable("Unhandled CtorInitializerKind in switch.");
}
static StringRef getAssociativityString(Associativity value) {
  switch (value) {
    case Associativity::None: return "none";
    case Associativity::Left: return "left";
    case Associativity::Right: return "right";
  }

  llvm_unreachable("Unhandled Associativity in switch.");
}

//===----------------------------------------------------------------------===//
//  Decl printing.
//===----------------------------------------------------------------------===//

// Print a name.
static void printName(raw_ostream &os, DeclName name) {
  if (!name)
    os << "<anonymous>";
  else
    os << name;
}

static void dumpSubstitutionMapRec(
    SubstitutionMap map, llvm::raw_ostream &out,
    SubstitutionMap::DumpStyle style, unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited);

namespace {
  class PrintPattern : public PatternVisitor<PrintPattern, void, StringRef> {
  public:
    ASTContext *Ctx;
    raw_ostream &OS;
    ExprTypeDelegate &Delegate;
    unsigned Indent;

    explicit PrintPattern(ASTContext *Ctx, raw_ostream &os,
                          ExprTypeDelegate &delegate, unsigned indent = 0)
      : Ctx(Ctx), OS(os), Delegate(delegate), Indent(indent) { }

    LLVM_NODISCARD ASTNodeDumper printCommon(Pattern *P, const char *Name,
                                             StringRef Label) {
      ASTNodeDumper dump(this, Name, Label, PatternColor);

      if (P->isImplicit())
        dump.colored(ExprModifierColor) << " implicit";

      if (P->hasType()) {
        dump.colored(TypeColor) << " type='";
        P->getType().print(dump.colored(TypeColor).getOS());
        dump.colored(TypeColor) << "'";
      }

      return dump;
    }

    void visitParenPattern(ParenPattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_paren", Label);
      dump.printRec(P->getSubPattern());
    }

    void visitTuplePattern(TuplePattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_tuple", Label);

      dump << " names=";
      interleave(P->getElements(),
                 [&](const TuplePatternElt &elt) {
                   auto name = elt.getLabel();
                   dump << (name.empty() ? "''" : name.str());
                 },
                 [&] { dump << ","; });

      for (auto &elt : P->getElements())
        dump.printRec(elt.getPattern());
    }
    void visitNamedPattern(NamedPattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_named", Label);
      dump.printNodeName(P->getDecl());
    }
    void visitAnyPattern(AnyPattern *P, StringRef Label) {
      (void)printCommon(P, "pattern_any", Label);
    }
    void visitTypedPattern(TypedPattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_typed", Label);
      dump.printRec(P->getSubPattern());
      if (auto *repr = P->getTypeRepr()) {
        dump.printRec(repr);
      }
    }

    void visitIsPattern(IsPattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_is", Label);
      dump << ' ' << getCheckedCastKindName(P->getCastKind()) << ' ';
      P->getCastType().print(dump.getOS());
      if (auto sub = P->getSubPattern()) {
        dump.printRec(sub);
      }
    }
    void visitExprPattern(ExprPattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_expr", Label);
      dump << '\n';
      if (auto m = P->getMatchExpr())
        dump.printRec(m);
      else
        dump.printRec(P->getSubExpr());
    }
    void visitBindingPattern(BindingPattern *P, StringRef Label) {
      auto dump = printCommon(P, P->isLet() ? "pattern_let" : "pattern_var", Label);
      dump.printRec(P->getSubPattern());
    }
    void visitEnumElementPattern(EnumElementPattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_enum_element", Label);
      dump << ' ';
      P->getParentType().print(dump.colored(TypeColor).getOS());
      dump.colored(IdentifierColor) << '.' << P->getName();
      if (P->hasSubPattern()) {
        dump.printRec(P->getSubPattern());
      }
    }
    void visitOptionalSomePattern(OptionalSomePattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_optional_some", Label);
      dump.printRec(P->getSubPattern());
    }
    void visitBoolPattern(BoolPattern *P, StringRef Label) {
      auto dump = printCommon(P, "pattern_bool", Label);
      dump << (P->getValue() ? " true" : " false");
    }

  };

  /// PrintDecl - Visitor implementation of Decl::print.
  class PrintDecl : public DeclVisitor<PrintDecl, void, StringRef> {
  public:
    ASTContext *Ctx;
    raw_ostream &OS;
    ExprTypeDelegate &Delegate;
    unsigned Indent;

    explicit PrintDecl(ASTContext *Ctx, raw_ostream &os,
                       ExprTypeDelegate &delegate, unsigned indent = 0)
      : Ctx(Ctx), OS(os), Delegate(delegate), Indent(indent) { }

  private:
    void printWhereRequirements(ASTNodeDumper &dump,
        PointerUnion<const AssociatedTypeDecl *, const GenericContext *> Owner)
        const {
      const auto printWhere = [&](const TrailingWhereClause *Where) {
        if (Where) {
          dump << " where requirements: ";
          Where->print(dump.getOS(), /*printWhereKeyword*/ false);
        }
      };

      if (const auto GC = Owner.dyn_cast<const GenericContext *>()) {
        printWhere(GC->getTrailingWhereClause());
      } else {
        const auto ATD = Owner.get<const AssociatedTypeDecl *>();
        printWhere(ATD->getTrailingWhereClause());
      }
    }

    LLVM_NODISCARD ASTNodeDumper printCommon(Decl *D, const char *Name,
                                             StringRef Label,
                                             TerminalColor Color = DeclColor) {
      ASTNodeDumper dump(this, Name, Label, Color);

      if (D->isImplicit())
        dump.colored(DeclModifierColor) << " implicit";

      if (D->isHoisted())
        dump.colored(DeclModifierColor) << " hoisted";

      dump.printNodeRange(D->getSourceRange());

      if (D->TrailingSemiLoc.isValid())
        dump.colored(DeclModifierColor) << " trailing_semi";

      return dump;
    }

    void printInherited(ASTNodeDumper &dump, ArrayRef<TypeLoc> Inherited) {
      if (Inherited.empty())
        return;
      dump << " inherits: ";
      interleave(Inherited,
                 [&](TypeLoc Super) { Super.getType().print(dump.getOS()); },
                 [&] { dump << ", "; });
    }

  public:
    void visitImportDecl(ImportDecl *ID, StringRef Label) {
      auto dump = printCommon(ID, "import_decl", Label);

      dump.printFlag(ID->isExported(), "exported");

      if (ID->getImportKind() != ImportKind::Module)
        dump.print(getImportKindString(ID->getImportKind()), "kind");

      SmallString<64> scratch;
      ID->getImportPath().getString(scratch);
      dump.print(QuotedString(scratch));
    }

    void visitExtensionDecl(ExtensionDecl *ED, StringRef Label) {
      auto dump = printCommon(ED, "extension_decl", Label, ExtensionColor);
      dump << ' ';
      if (ED->hasBeenBound())
        ED->getExtendedType().print(dump.getOS());
      else
        ED->getExtendedTypeRepr()->print(dump.getOS());
      printCommonPost(dump, ED);
    }

    void visitTypeAliasDecl(TypeAliasDecl *TAD, StringRef Label) {
      auto dump = printCommon(TAD, "typealias", Label);
      dump.colored(TypeColor) << " type=";
      if (auto underlying = TAD->getCachedUnderlyingType()) {
        dump.colored(TypeColor) << "'" << underlying.getString() << "'";
      } else {
        dump.colored(TypeColor) << "<<<unresolved>>>";
      }
      printWhereRequirements(dump, TAD);
    }

    void visitOpaqueTypeDecl(OpaqueTypeDecl *OTD, StringRef Label) {
      auto dump = printCommon(OTD, "opaque_type", Label);
      dump.printNodeName(OTD->getNamingDecl());
      dump.colored(TypeColor) << " opaque_interface="
        << Type(OTD->getUnderlyingInterfaceType()).getString();
      dump << " in "
         << OTD->getOpaqueInterfaceGenericSignature()->getAsString();
      if (auto underlyingSubs = OTD->getUnderlyingTypeSubstitutions()) {
        dump << " underlying:\n";
        SmallPtrSet<const ProtocolConformance *, 4> Dumped;
        dumpSubstitutionMapRec(*underlyingSubs, dump.getOS(),
                               SubstitutionMap::DumpStyle::Full,
                               dump.IndentChildren, Dumped);
      }
    }

    LLVM_NODISCARD ASTNodeDumper
    printAbstractTypeParamCommon(AbstractTypeParamDecl *decl, const char *name,
                                 StringRef Label) {
      auto dump = printCommon(decl, name, Label);
      if (decl->getDeclContext()->getGenericEnvironmentOfContext()) {
        if (auto superclassTy = decl->getSuperclass()) {
          dump << " superclass='" << superclassTy->getString() << "'";
        }
      }
      return dump;
    }

    void visitGenericTypeParamDecl(GenericTypeParamDecl *decl, StringRef Label) {
      auto dump = printAbstractTypeParamCommon(decl, "generic_type_param",
                                               Label);
      dump << " depth=" << decl->getDepth() << " index=" << decl->getIndex();
    }

    void visitAssociatedTypeDecl(AssociatedTypeDecl *decl, StringRef Label) {
      auto dump = printAbstractTypeParamCommon(decl, "associated_type_decl",
                                               Label);
      if (auto defaultDef = decl->getDefaultDefinitionType()) {
        dump << " default=";
        defaultDef.print(dump.getOS());
      }
      printWhereRequirements(dump, decl);
      if (decl->overriddenDeclsComputed()) {
        dump << " overridden=";
        interleave(decl->getOverriddenDecls(),
                   [&](AssociatedTypeDecl *overridden) {
                     dump << overridden->getProtocol()->getName();
                   }, [&]() {
                     dump << ", ";
                   });
      }
    }

    void visitProtocolDecl(ProtocolDecl *PD, StringRef Label) {
      auto dump = printCommon(PD, "protocol", Label);

      dump << " requirement signature=";
      if (PD->isRequirementSignatureComputed()) {
        dump << GenericSignature::get({PD->getProtocolSelfType()} ,
                                      PD->getRequirementSignature())
                ->getAsString();
      } else {
        dump << "<null>";
      }
      printCommonPost(dump, PD);
    }

    LLVM_NODISCARD ASTNodeDumper
    printCommon(ValueDecl *VD, const char *Name, StringRef Label,
                TerminalColor Color = DeclColor) {
      auto dump = printCommon((Decl*)VD, Name, Label, Color);

      dump.printNodeName(VD);
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD))
        printGenericParameters(dump.getOS(), AFD->getParsedGenericParams());
      if (auto *GTD = dyn_cast<GenericTypeDecl>(VD))
        printGenericParameters(dump.getOS(), GTD->getParsedGenericParams());

      if (auto *var = dyn_cast<VarDecl>(VD)) {
        dump.colored(TypeColor) << " type='";
        if (var->hasInterfaceType())
          var->getType().print(dump.colored(TypeColor).getOS());
        else
          dump.colored(TypeColor) << "<null type>";
        dump.colored(TypeColor) << "'";
      }

      if (VD->hasInterfaceType()) {
        dump.colored(InterfaceTypeColor) << " interface type='";
        VD->getInterfaceType()->print(
            dump.colored(InterfaceTypeColor).getOS());
        dump.colored(InterfaceTypeColor) << "'";
      }

      if (VD->hasAccess()) {
        dump.colored(AccessLevelColor) << " access="
          << getAccessLevelSpelling(VD->getFormalAccess());
      }

      if (VD->overriddenDeclsComputed()) {
        auto overridden = VD->getOverriddenDecls();
        if (!overridden.empty()) {
          dump.colored(OverrideColor) << " override=";
          interleave(overridden,
                     [&](ValueDecl *overridden) {
                       overridden->dumpRef(
                                dump.colored(OverrideColor).getOS());
                     }, [&]() {
                       dump << ", ";
                     });
        }
      }

      auto VarD = dyn_cast<VarDecl>(VD);
      const auto &attrs = VD->getAttrs();
      dump.printFlag(attrs.hasAttribute<FinalAttr>() && !(VarD && VarD->isLet()),
                "final");
      dump.printFlag(attrs.hasAttribute<ObjCAttr>(), "@objc");
      dump.printFlag(attrs.hasAttribute<DynamicAttr>(), " dynamic");
      if (auto *attr = attrs.getAttribute<DynamicReplacementAttr>()) {
        dump << " @_dynamicReplacement(for: \"";
        dump << attr->getReplacedFunctionName();
        dump << "\")";
      }
      return dump;
    }

    LLVM_NODISCARD ASTNodeDumper
    printCommon(NominalTypeDecl *NTD, const char *Name, StringRef Label,
                TerminalColor Color = DeclColor) {
      auto dump = printCommon((ValueDecl *)NTD, Name, Label, Color);

      if (NTD->hasInterfaceType()) {
        if (NTD->isResilient())
          dump.printFlag("resilient");
        else
          dump.printFlag("non-resilient");
      }
      return dump;
    }

    void printCommonPost(ASTNodeDumper &dump, const IterableDeclContext *IDC) {
      switch (IDC->getIterableContextKind()) {
      case IterableDeclContextKind::NominalTypeDecl: {
        const auto NTD = cast<NominalTypeDecl>(IDC);
        printInherited(dump, NTD->getInherited());
        printWhereRequirements(dump, NTD);
        break;
      }
      case IterableDeclContextKind::ExtensionDecl:
        const auto ED = cast<ExtensionDecl>(IDC);
        printInherited(dump, ED->getInherited());
        printWhereRequirements(dump, ED);
        break;
      }

      for (Decl *D : IDC->getMembers())
        dump.printRec(D);
    }

    void visitSourceFile(const SourceFile &SF, StringRef Label) {
      ASTNodeDumper dump(this, "source_file", Label, ASTNodeColor);
      dump.colored(LocationColor) << " \"" << SF.getFilename() << '"';

      if (auto decls = SF.getCachedTopLevelDecls()) {
        for (Decl *D : *decls) {
          if (D->isImplicit())
            continue;

          dump.printRec(D);
        }
      }
    }

    void visitVarDecl(VarDecl *VD, StringRef Label) {
      auto dump = printCommon(VD, "var_decl", Label);
      if (VD->isLet())
        dump.colored(DeclModifierColor) << " let";
      if (VD->getAttrs().hasAttribute<LazyAttr>())
        dump.colored(DeclModifierColor) << " lazy";
      printStorageImpl(dump, VD);
      printAccessors(dump, VD);
    }

    void printStorageImpl(ASTNodeDumper &dump, AbstractStorageDecl *D) {
      dump.printFlag(D->isStatic(), "type");

      if (D->hasInterfaceType()) {
        auto impl = D->getImplInfo();
        dump.print(getReadImplKindName(impl.getReadImpl()), "readImpl");
        dump.printFlag(!impl.supportsMutation(), "immutable");

        if (impl.supportsMutation()) {
          dump.print(getWriteImplKindName(impl.getWriteImpl()), "writeImpl");
          dump.print(getReadWriteImplKindName(impl.getReadWriteImpl()),
                     "readWriteImpl");
        }
      }
    }

    void printAccessors(ASTNodeDumper &dump, AbstractStorageDecl *D) {
      for (auto accessor : D->getAllAccessors())
        dump.printRec(accessor);
    }

    void visitParamDecl(ParamDecl *PD, StringRef Label) {
      auto dump = printCommon(PD, "parameter", Label, ParameterColor);

      if (!PD->getArgumentName().empty())
        dump.colored(IdentifierColor) << " api_name=" << PD->getArgumentName();

      if (auto specifier = PD->getCachedSpecifier()) {
        switch (*specifier) {
        case ParamDecl::Specifier::Default:
          /* nothing */
          break;
        case ParamDecl::Specifier::InOut:
          dump.printFlag("inout");
          break;
        case ParamDecl::Specifier::Shared:
          dump.printFlag("shared");
          break;
        case ParamDecl::Specifier::Owned:
          dump.printFlag("owned");
          break;
        }
      }

      dump.printFlag(PD->isVariadic(), "variadic");
      dump.printFlag(PD->isAutoClosure(), "autoclosure");
      dump.printFlag(PD->getAttrs().hasAttribute<NonEphemeralAttr>(),
                     "nonEphemeral");

      if (PD->getDefaultArgumentKind() != DefaultArgumentKind::None) {
        dump.print(getDefaultArgumentKindString(PD->getDefaultArgumentKind()),
                   "default_arg");
      }

      if (PD->hasDefaultExpr() &&
          !PD->getDefaultArgumentCaptureInfo().isTrivial()) {
        dump << " ";
        PD->getDefaultArgumentCaptureInfo().print(
          dump.colored(CapturesColor).getOS());
      }

      if (auto init = PD->getStructuralDefaultExpr())
        dump.printRec(init, "expression");
    }

    void visitEnumCaseDecl(EnumCaseDecl *ECD, StringRef Label) {
      auto dump = printCommon(ECD, "enum_case_decl", Label);
      for (EnumElementDecl *D : ECD->getElements())
        dump.printRec(D);
    }

    void visitEnumDecl(EnumDecl *ED, StringRef Label) {
      auto dump = printCommon(ED, "enum_decl", Label);
      printCommonPost(dump, ED);
    }

    void visitEnumElementDecl(EnumElementDecl *EED, StringRef Label) {
      auto dump = printCommon(EED, "enum_element_decl", Label);
      if (auto *paramList = EED->getParameterList())
        dump.printRec(paramList);
    }

    void visitStructDecl(StructDecl *SD, StringRef Label) {
      auto dump = printCommon(SD, "struct_decl", Label);
      printCommonPost(dump, SD);
    }

    void visitClassDecl(ClassDecl *CD, StringRef Label) {
      auto dump = printCommon(CD, "class_decl", Label);
      if (CD->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>())
        dump << " @_staticInitializeObjCMetadata";
      printCommonPost(dump, CD);
    }

    void visitPatternBindingDecl(PatternBindingDecl *PBD, StringRef Label) {
      auto dump = printCommon(PBD, "pattern_binding_decl", Label);

      for (auto idx : range(PBD->getNumPatternEntries())) {
        auto childDump = dump.child("pattern_entry", "", PatternColor);

        childDump.printRec(PBD->getPattern(idx), "pattern");
        if (PBD->getOriginalInit(idx))
          childDump.printRec(PBD->getOriginalInit(idx), "original_init");
        if (PBD->getInit(idx))
          childDump.printRec(PBD->getInit(idx), "processed_init");
      }
    }

    void visitSubscriptDecl(SubscriptDecl *SD, StringRef Label) {
      auto dump = printCommon(SD, "subscript_decl", Label);
      printStorageImpl(dump, SD);
      printAccessors(dump, SD);
    }

    LLVM_NODISCARD ASTNodeDumper
    printCommonAFD(AbstractFunctionDecl *D, const char *Type, StringRef Label) {
      auto dump = printCommon(D, Type, Label, FuncColor);

      if (!D->getCaptureInfo().isTrivial()) {
        dump << " ";
        D->getCaptureInfo().print(dump.getOS());
      }

      return dump;
    }

    void printParameterList(const ParameterList *params, StringRef Label) {
      if (!Ctx && params->size() != 0 && params->get(0))
        Ctx = &params->get(0)->getASTContext();

      ASTNodeDumper dump(this, "parameter_list", Label, ParameterColor);

      dump.printNodeRange(params->getSourceRange());

      for (auto P : *params)
        dump.printRec(P);
    }

    void printAbstractFunctionDecl(ASTNodeDumper &dump,
                                   AbstractFunctionDecl *D) {
      if (auto fac = D->getForeignAsyncConvention()) {
        auto childDump = dump.child("foreign_async", "", DeclModifierColor);
        childDump.print(fac->completionHandlerParamIndex(),
                        "completion_handler_param_index");
        if (auto errorParamIndex = fac->completionHandlerErrorParamIndex())
          childDump.print(*errorParamIndex, "error_param_index");

        if (auto type = fac->completionHandlerType())
          childDump.print(type, "completion_handler_type");
      }

      if (auto fec = D->getForeignErrorConvention()) {
        auto childDump = dump.child("foreign_error", "", DeclModifierColor);
        childDump.print(getForeignErrorConventionKindString(fec->getKind()),
                        "kind");

        bool isOwned = (fec->isErrorOwned() == ForeignErrorConvention::IsOwned);
        childDump.printFlag(isOwned ? "owned" : "unowned");

        childDump.print(llvm::utostr(fec->getErrorParameterIndex()),
                        "param_index");
        childDump.print(fec->getErrorParameterType(), "param_type");

        bool wantResultType = (
          fec->getKind() == ForeignErrorConvention::ZeroResult ||
          fec->getKind() == ForeignErrorConvention::NonZeroResult);
        if (wantResultType)
          childDump.print(fec->getResultType(), "result_type");
      }

      if (auto *P = D->getImplicitSelfDecl())
        dump.printRec(P, "self");

      dump.printRec(D->getParameters());

      if (auto FD = dyn_cast<FuncDecl>(D)) {
        if (FD->getResultTypeRepr()) {
          dump.printRec(FD->getResultTypeRepr(), "result");
          if (auto opaque = FD->getOpaqueResultTypeDecl())
            dump.printRec(opaque, "opaque_result_decl");
        }
      }
      if (D->hasSingleExpressionBody())
        dump.printRec(D->getSingleExpressionBody(), "single_expression_body");
      else if (auto Body = D->getBody(/*canSynthesize=*/false))
        dump.printRec(Body, "body");
    }

    LLVM_NODISCARD ASTNodeDumper
    printCommonFD(FuncDecl *FD, const char *type, StringRef Label) {
      auto dump = printCommonAFD(FD, type, Label);
      dump.printFlag(FD->isStatic(), "type");
      return dump;
    }

    void visitFuncDecl(FuncDecl *FD, StringRef Label) {
      auto dump = printCommonFD(FD, "func_decl", Label);
      printAbstractFunctionDecl(dump, FD);
    }

    void visitAccessorDecl(AccessorDecl *AD, StringRef Label) {
      auto dump = printCommonFD(AD, "accessor_decl", Label);
      printAbstractFunctionDecl(dump, AD);
    }

    void visitConstructorDecl(ConstructorDecl *CD, StringRef Label) {
      auto dump = printCommonAFD(CD, "constructor_decl", Label);
      dump.printFlag(CD->isRequired(), "required");
      dump.printFlag(getCtorInitializerKindString(CD->getInitKind()));
      if (CD->isFailable())
        dump.print((CD->isImplicitlyUnwrappedOptional()
                    ? "ImplicitlyUnwrappedOptional"
                    : "Optional"), "failable");
      printAbstractFunctionDecl(dump, CD);
    }

    void visitDestructorDecl(DestructorDecl *DD, StringRef Label) {
      auto dump = printCommonAFD(DD, "destructor_decl", Label);
      printAbstractFunctionDecl(dump, DD);
    }

    void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD, StringRef Label) {
      auto dump = printCommon(TLCD, "top_level_code_decl", Label);
      if (TLCD->getBody())
        dump.printRec(TLCD->getBody());
    }
    
    void visitIfConfigDecl(IfConfigDecl *ICD, StringRef Label) {
      auto dump = printCommon(ICD, "if_config_decl", Label);
      for (auto &Clause : ICD->getClauses()) {
        auto childDump = dump.child((Clause.Cond ? "#if:" : "#else:"), "",
                                    StmtColor);
        childDump.printFlag(Clause.isActive, "active", DeclModifierColor);

        if (Clause.Cond)
          childDump.printRec(Clause.Cond);
        childDump.printRec(Clause.Elements, "elements");
      }
    }

    void visitPoundDiagnosticDecl(PoundDiagnosticDecl *PDD, StringRef Label) {
      auto dump = printCommon(PDD, "pound_diagnostic_decl", Label);
      dump.print(PDD->isError() ? "error" : "warning", "kind");
      dump.printRec(PDD->getMessage());
    }

    void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD, StringRef Label) {
      auto dump = printCommon(PGD, "precedence_group_decl", Label);

      dump.printNodeName(PGD->getName());
      dump.print(getAssociativityString(PGD->getAssociativity()),
                 "associativity");
      dump.printFlag(PGD->isAssignment(), "assignment");

      auto printRelations =
          [&](StringRef label, ArrayRef<PrecedenceGroupDecl::Relation> rels) {
        if (rels.empty()) return;

        auto childDump = dump.child("groups", label, DeclModifierColor);
        childDump << rels[0].Name;
        for (auto &rel : rels.slice(1))
          childDump << " " << rel.Name;
      };
      printRelations("higher_than", PGD->getHigherThan());
      printRelations("lower_than", PGD->getLowerThan());
    }

    void printOperatorIdentifiers(ASTNodeDumper &dump, OperatorDecl *OD) {
      auto identifiers = OD->getIdentifiers();
      for (auto index : indices(identifiers)) {
        auto childDump = dump.child("identifier", llvm::utostr(index),
                                    DeclModifierColor);
        childDump.print(identifiers[index].Item);
      }
    }

    void visitInfixOperatorDecl(InfixOperatorDecl *IOD, StringRef Label) {
      auto dump = printCommon(IOD, "infix_operator_decl", Label);
      dump.printNodeName(IOD->getName());
      printOperatorIdentifiers(dump, IOD);
    }

    void visitPrefixOperatorDecl(PrefixOperatorDecl *POD, StringRef Label) {
      auto dump = printCommon(POD, "prefix_operator_decl", Label);
      dump.printNodeName(POD->getName());
      printOperatorIdentifiers(dump, POD);
    }

    void visitPostfixOperatorDecl(PostfixOperatorDecl *POD, StringRef Label) {
      auto dump = printCommon(POD, "postfix_operator_decl", Label);
      dump.printNodeName(POD->getName());
      printOperatorIdentifiers(dump, POD);
    }

    void visitModuleDecl(ModuleDecl *MD, StringRef Label) {
      auto dump = printCommon(MD, "module", Label);
      dump.printFlag(MD->isNonSwiftModule(), "non_swift");
    }

    void visitMissingMemberDecl(MissingMemberDecl *MMD, StringRef Label) {
      auto dump = printCommon(MMD, "missing_member_decl", Label);
      dump.printNodeName(MMD->getName());
    }
  };
} // end anonymous namespace

void ParameterList::dump() const {
  dump(llvm::errs(), 0);
}

void ParameterList::dump(raw_ostream &OS, unsigned Indent) const {
  ExprTypeDelegate delegate;
  PrintDecl(nullptr, OS, delegate, Indent).printParameterList(this, "");
  llvm::errs() << '\n';
}



void Decl::dump() const {
  dump(llvm::errs(), 0);
}

void Decl::dump(const char *filename) const {
  std::error_code ec;
  llvm::raw_fd_ostream stream(filename, ec, llvm::sys::fs::FA_Read |
                              llvm::sys::fs::FA_Write);
  // In assert builds, we blow up. Otherwise, we just return.
  assert(!ec && "Failed to open file for dumping?!");
  if (ec)
    return;
  dump(stream, 0);
}

void Decl::dump(raw_ostream &OS, unsigned Indent) const {
  ExprTypeDelegate delegate;
  PrintDecl(&getASTContext(), OS, delegate, Indent)
      .visit(const_cast<Decl *>(this), "");
  OS << '\n';
}

/// Print the given declaration context (with its parents).
void swift::printContext(raw_ostream &os, DeclContext *dc) {
  if (auto parent = dc->getParent()) {
    printContext(os, parent);
    os << '.';
  }

  switch (dc->getContextKind()) {
  case DeclContextKind::Module:
    printName(os, cast<ModuleDecl>(dc)->getName());
    break;

  case DeclContextKind::FileUnit:
    // FIXME: print the file's basename?
    os << "(file)";
    break;

  case DeclContextKind::SerializedLocal:
    os << "local context";
    break;

  case DeclContextKind::AbstractClosureExpr: {
    auto *ACE = cast<AbstractClosureExpr>(dc);
    if (isa<ClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "explicit closure discriminator=";
    }
    if (isa<AutoClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "autoclosure discriminator=";
    }
    PrintWithColorRAII(os, DiscriminatorColor) << ACE->getDiscriminator();
    break;
  }

  case DeclContextKind::GenericTypeDecl:
    printName(os, cast<GenericTypeDecl>(dc)->getName());
    break;

  case DeclContextKind::ExtensionDecl:
    if (auto extendedNominal = cast<ExtensionDecl>(dc)->getExtendedNominal()) {
      printName(os, extendedNominal->getName());
    }
    os << " extension";
    break;

  case DeclContextKind::Initializer:
    switch (cast<Initializer>(dc)->getInitializerKind()) {
    case InitializerKind::PatternBinding:
      os << "pattern binding initializer";
      break;
    case InitializerKind::DefaultArgument:
      os << "default argument initializer";
      break;
    case InitializerKind::PropertyWrapper:
      os << "property wrapper initializer";
      break;
    }
    break;

  case DeclContextKind::TopLevelCodeDecl:
    os << "top-level code";
    break;

  case DeclContextKind::AbstractFunctionDecl:
    printName(os, cast<AbstractFunctionDecl>(dc)->getName());
    break;

  case DeclContextKind::SubscriptDecl:
    printName(os, cast<SubscriptDecl>(dc)->getName());
    break;

  case DeclContextKind::EnumElementDecl:
    printName(os, cast<EnumElementDecl>(dc)->getName());
    break;
  }
}

std::string ValueDecl::printRef() const {
  std::string result;
  llvm::raw_string_ostream os(result);
  dumpRef(os);
  return os.str();
}

void ValueDecl::dumpRef(raw_ostream &os) const {
  // Print the context.
  printContext(os, getDeclContext());
  os << ".";

  // Print name.
  getName().printPretty(os);

  // Print location.
  auto &srcMgr = getASTContext().SourceMgr;
  if (getLoc().isValid()) {
    os << '@';
    getLoc().print(os, srcMgr);
  }
}

void LLVM_ATTRIBUTE_USED ValueDecl::dumpRef() const {
  dumpRef(llvm::errs());
}

void SourceFile::dump() const {
  dump(llvm::errs());
}

void SourceFile::dump(llvm::raw_ostream &OS, bool parseIfNeeded) const {
  // If we're allowed to parse the SourceFile, do so now. We need to force the
  // parsing request as by default the dumping logic tries not to kick any
  // requests.
  if (parseIfNeeded)
    (void)getTopLevelDecls();

  ExprTypeDelegate delegate;
  PrintDecl(&getASTContext(), OS, delegate).visitSourceFile(*this, "");
  llvm::errs() << '\n';
}

void Pattern::dump() const {
  dump(llvm::errs());
}

static ASTContext *getASTContextFromType(Type ty) {
  if (ty)
    return &ty->getASTContext();
  return nullptr;
}

void Pattern::dump(raw_ostream &OS, unsigned Indent) const {
  ExprTypeDelegate delegate;
  PrintPattern(getASTContextFromType(getType()), OS, delegate, Indent)
    .visit(const_cast<Pattern*>(this), "");
  OS << '\n';
}

//===----------------------------------------------------------------------===//
// Printing for Stmt and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintStmt - Visitor implementation of Stmt::dump.
class PrintStmt : public StmtVisitor<PrintStmt, void, StringRef> {
public:
  ASTContext *Ctx;
  raw_ostream &OS;
  ExprTypeDelegate &Delegate;
  unsigned Indent;

  explicit PrintStmt(ASTContext *ctx, raw_ostream &os,
                     ExprTypeDelegate &delegate, unsigned indent)
    : Ctx(ctx), OS(os), Delegate(delegate), Indent(indent) { }

  void visit(Stmt *S, StringRef Label) {
    if (S)
      StmtVisitor<PrintStmt, void, StringRef>::visit(S, Label);
    else {
      (void)ASTNodeDumper(this, "<<null>>", Label, StmtColor);
    }
  }

  LLVM_NODISCARD ASTNodeDumper
  printCommon(Stmt *S, const char *Name, StringRef Label) {
    ASTNodeDumper dump(this, Name, Label, StmtColor);

    dump.printFlag(S->isImplicit(), "implicit");
    dump.printNodeRange(S->getSourceRange());
    dump.printFlag(S->TrailingSemiLoc.isValid(), "trailing_semi");

    return dump;
  }

  void visitBraceStmt(BraceStmt *S, StringRef Label) {
    auto dump = printCommon(S, "brace_stmt", Label);

    for (auto Elt : S->getElements())
      dump.printRec(Elt);
  }

  void visitReturnStmt(ReturnStmt *S, StringRef Label) {
    auto dump = printCommon(S, "return_stmt", Label);
    if (S->hasResult())
      dump.printRec(S->getResult());
  }

  void visitYieldStmt(YieldStmt *S, StringRef Label) {
    auto dump = printCommon(S, "yield_stmt", Label);
    for (auto yield : S->getYields())
      dump.printRec(yield);
  }

  void visitDeferStmt(DeferStmt *S, StringRef Label) {
    auto dump = printCommon(S, "defer_stmt", Label);
    dump.printRec(S->getTempDecl());
    dump.printRec(S->getCallExpr());
  }

  void visitIfStmt(IfStmt *S, StringRef Label) {
    auto dump = printCommon(S, "if_stmt", Label);

    dump.printRec(S->getCond(), "conditions");
    dump.printRec(S->getThenStmt(), "then_body");
    if (S->getElseStmt())
      dump.printRec(S->getElseStmt(), "else_body");
  }

  void visitGuardStmt(GuardStmt *S, StringRef Label) {
    auto dump = printCommon(S, "guard_stmt", Label);
    dump.printRec(S->getCond(), "conditions");
    dump.printRec(S->getBody(), "else_body");
  }

  void visitDoStmt(DoStmt *S, StringRef Label) {
    auto dump = printCommon(S, "do_stmt", Label);
    dump.printRec(S->getBody(), "body");
  }

  void visitWhileStmt(WhileStmt *S, StringRef Label) {
    auto dump = printCommon(S, "while_stmt", Label);
    dump.printRec(S->getCond(), "conditions");
    dump.printRec(S->getBody(), "body");
  }

  void visitRepeatWhileStmt(RepeatWhileStmt *S, StringRef Label) {
    auto dump = printCommon(S, "repeat_while_stmt", Label);
    dump.printRec(S->getBody(), "body");
    dump.printRec(S->getCond(), "condition");
  }

  void visitForEachStmt(ForEachStmt *S, StringRef Label) {
    auto dump = printCommon(S, "for_each_stmt", Label);

    dump.printRec(S->getPattern(), "pattern");
    if (S->getWhere())
      dump.printRec(S->getWhere(), "where_clause");
    dump.printRec(S->getSequence(), "sequence");

    if (S->getIteratorVar())
      dump.printRec(S->getIteratorVar(), "iterator_var");
    if (S->getIteratorVarRef())
      dump.printRec(S->getIteratorVarRef(), "iterator_var_ref");

    if (S->getConvertElementExpr())
      dump.printRec(S->getConvertElementExpr(), "convert_element_expr");
    if (S->getElementExpr())
      dump.printRec(S->getElementExpr(), "element_expr");

    dump.printRec(S->getBody(), "body");
  }

  void visitBreakStmt(BreakStmt *S, StringRef Label) {
    (void)printCommon(S, "break_stmt", Label);
  }

  void visitContinueStmt(ContinueStmt *S, StringRef Label) {
    (void)printCommon(S, "continue_stmt", Label);
  }

  void visitFallthroughStmt(FallthroughStmt *S, StringRef Label) {
    (void)printCommon(S, "fallthrough_stmt", Label);
  }

  void visitSwitchStmt(SwitchStmt *S, StringRef Label) {
    auto dump = printCommon(S, "switch_stmt", Label);

    dump.printRec(S->getSubjectExpr(), "subject");
    dump.printRec(S->getRawCases(), "cases");
  }

  void visitCaseStmt(CaseStmt *S, StringRef Label) {
    auto dump = printCommon(S, "case_stmt", Label);

    dump.printFlag(S->hasUnknownAttr(), "@unknown");

    if (S->hasCaseBodyVariables())
      dump.printRec(S->getCaseBodyVariables(), "case_body_variables");

    for (const auto &LabelItem : S->getCaseLabelItems()) {
      auto childDump = dump.child("case_label_item", "", StmtColor);
      childDump.printFlag(LabelItem.isDefault(), "default");

      if (auto *CasePattern = LabelItem.getPattern())
        childDump.printRec(CasePattern, "pattern");
      if (auto *Guard = LabelItem.getGuardExpr())
        childDump.printRec(const_cast<Expr *>(Guard), "where_clause");
    }

    dump.printRec(S->getBody(), "body");
  }

  void visitFailStmt(FailStmt *S, StringRef Label) {
    (void)printCommon(S, "fail_stmt", Label);
  }

  void visitThrowStmt(ThrowStmt *S, StringRef Label) {
    auto dump = printCommon(S, "throw_stmt", Label);
    dump.printRec(S->getSubExpr());
  }

  void visitPoundAssertStmt(PoundAssertStmt *S, StringRef Label) {
    auto dump = printCommon(S, "pound_assert", Label);
    dump.print(QuotedString(S->getMessage()), "message");

    dump.printRec(S->getCondition());
  }

  void visitDoCatchStmt(DoCatchStmt *S, StringRef Label) {
    auto dump = printCommon(S, "do_catch_stmt", Label);

    dump.printRec(S->getBody(), "do_body");
    dump.printRec(S->getCatches(), "catch_clauses");
  }
};

} // end anonymous namespace

void ASTNodeDumper::printRec(StmtConditionElement C, StringRef Label) {
  switch (C.getKind()) {
  case StmtConditionElement::CK_Boolean:
    return printRec(C.getBoolean(), Label);

  case StmtConditionElement::CK_PatternBinding: {
    auto dump = child("pattern", Label, PatternColor);
    dump.printRec(C.getPattern());
    dump.printRec(C.getInitializer());
    break;
  }

  case StmtConditionElement::CK_Availability: {
    auto dump = child("#available", Label, ASTNodeColor);
    for (auto *Query : C.getAvailability()->getQueries()) {
      dump << '\n';
      switch (Query->getKind()) {
      case AvailabilitySpecKind::PlatformVersionConstraint:
        cast<PlatformVersionConstraintAvailabilitySpec>(Query)
            ->print(dump.getOS(), dump.IndentChildren);
        break;
      case AvailabilitySpecKind::LanguageVersionConstraint:
      case AvailabilitySpecKind::PackageDescriptionVersionConstraint:
        cast<PlatformVersionConstraintAvailabilitySpec>(Query)
            ->print(dump.getOS(), dump.IndentChildren);
        break;
      case AvailabilitySpecKind::OtherPlatform:
        cast<OtherPlatformAvailabilitySpec>(Query)
            ->print(dump.getOS(), dump.IndentChildren);
        break;
      }
    }
    break;
  }
  }
}

void Stmt::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}

void Stmt::dump(raw_ostream &OS, const ASTContext *Ctx, unsigned Indent) const {
  ExprTypeDelegate delegate;
  PrintStmt(const_cast<ASTContext *>(Ctx), OS, delegate, Indent)
      .visit(const_cast<Stmt*>(this), "");
}

//===----------------------------------------------------------------------===//
// Printing for Expr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintExpr - Visitor implementation of Expr::dump.
class PrintExpr : public ExprVisitor<PrintExpr, void, StringRef> {
public:
  ASTContext *Ctx;
  raw_ostream &OS;
  ExprTypeDelegate &Delegate;
  unsigned Indent;

  Type getTypeOfExpr(Expr *E) {
    return Delegate.GetTypeOfExpr(E);
  }
  bool canGetTypeOfTypeRepr() {
    return (bool)Delegate.GetTypeOfTypeRepr;
  }
  Type getTypeOfTypeRepr(TypeRepr *T) {
    return Delegate.GetTypeOfTypeRepr(T);
  }
  Type getTypeOfKeyPathComponent(KeyPathExpr *E, unsigned index) {
    return Delegate.GetTypeOfKeyPathComponent(E, index);
  }

  PrintExpr(ASTContext *ctx, raw_ostream &os, ExprTypeDelegate &delegate,
            unsigned indent)
      : Ctx(ctx), OS(os), Delegate(delegate), Indent(indent) {}

  void visit(Expr *E, StringRef Label) {
    if (E)
      ExprVisitor<PrintExpr, void, StringRef>::visit(E, Label);
    else {
      (void)ASTNodeDumper(this, "<<null>>", Label, ExprColor);
    }
  }

  /// FIXME: This should use ExprWalker to print children.

  LLVM_NODISCARD ASTNodeDumper
  printCommon(Expr *E, const char *C, StringRef Label) {
    if (!Ctx)
      if (auto Ty = getTypeOfExpr(E))
        Ctx = &Ty->getASTContext();

    ASTNodeDumper dump(this, C, Label, ExprColor);

    dump.printFlag(E->isImplicit(), "implicit", ExprModifierColor);

    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    dump.colored(TypeColor)
        << " type='" << getTypeOfExpr(E).getString(PO) << '\'';

    dump.printNodeLocation(E->getLoc());
    dump.printNodeRange(E->getSourceRange());
    dump.printFlag(E->TrailingSemiLoc.isValid(), "trailing_semi",
                   ExprModifierColor);

    return dump;
  }

  void printSemanticExpr(ASTNodeDumper &dump, Expr *semanticExpr) {
    if (semanticExpr)
      dump.printRec(semanticExpr, "semantic_expr");
  }

  void visitErrorExpr(ErrorExpr *E, StringRef Label) {
    (void)printCommon(E, "error_expr", Label);
  }

  void visitCodeCompletionExpr(CodeCompletionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "code_completion_expr", Label);
    if (E->getBase())
      dump.printRec(E->getBase());
  }

  void visitNilLiteralExpr(NilLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "nil_literal_expr", Label);
    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "integer_literal_expr", Label);

    dump.printFlag(E->isNegative(), "negative", LiteralValueColor);

    Type T = getTypeOfExpr(E);
    if (T.isNull() || !T->is<BuiltinIntegerType>())
      dump.print(E->getDigitsText(), "value", LiteralValueColor);
    else
      dump.print(E->getValue(), "value", LiteralValueColor);

    dump.printDeclRef(E->getBuiltinInitializer(), "builtin_initializer",
                      LiteralValueColor);
    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);
  }

  void visitFloatLiteralExpr(FloatLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "float_literal_expr", Label);

    dump.printFlag(E->isNegative(), "negative", LiteralValueColor);
    dump.print(E->getDigitsText(), "value", LiteralValueColor);

    dump.printDeclRef(E->getBuiltinInitializer(), "builtin_initializer",
                      LiteralValueColor);
    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);

    if (!E->getBuiltinType().isNull())
      dump.print(E->getBuiltinType(), "builtin_type", TypeColor);
  }

  void visitBooleanLiteralExpr(BooleanLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "boolean_literal_expr", Label);

    dump.print(E->getValue() ? "true" : "false", "value", LiteralValueColor);

    dump.printDeclRef(E->getBuiltinInitializer(), "builtin_initializer",
                      LiteralValueColor);
    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);
  }

  void visitStringLiteralExpr(StringLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "string_literal_expr", Label);

    dump.print(getStringLiteralExprEncodingString(E->getEncoding()), "encoding",
               LiteralValueColor);
    dump.print(QuotedString(E->getValue()), "value", LiteralValueColor);

    dump.printDeclRef(E->getBuiltinInitializer(), "builtin_initializer",
                      LiteralValueColor);
    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);
  }

  void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "interpolated_string_literal_expr", Label);
    
    dump.print(E->getTrailingQuoteLoc(), "trailing_quote_loc", LocationColor);

    dump.print(E->getLiteralCapacity(), "literal_capacity", LiteralValueColor);
    dump.print(E->getInterpolationCount(), "interpolation_count",
               LiteralValueColor);

    dump.printDeclRef(E->getBuilderInit(), "builder_init", LiteralValueColor);
    dump.printDeclRef(E->getInitializer(), "result_init", LiteralValueColor);

    dump.printRec(E->getAppendingExpr());
  }
  void visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "magic_identifier_literal_expr", Label);

    dump.print(MagicIdentifierLiteralExpr::getKindString(E->getKind()), "kind");

    if (E->isString())
      dump.print(getStringLiteralExprEncodingString(E->getStringEncoding()),
                 "encoding", LiteralValueColor);

    dump.printDeclRef(E->getBuiltinInitializer(), "builtin_initializer",
                      LiteralValueColor);
    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);
  }

  void visitObjectLiteralExpr(ObjectLiteralExpr *E, StringRef Label) {
    auto dump = printCommon(E, "object_literal", Label);

    dump.print(QuotedString(E->getLiteralKindPlainName()), "kind");
    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);
    printArgumentLabels(dump, E->getArgumentLabels());

    dump.printRec(E->getArg());
  }

  void visitDiscardAssignmentExpr(DiscardAssignmentExpr *E, StringRef Label) {
    (void)printCommon(E, "discard_assignment_expr", Label);
  }

  void visitDeclRefExpr(DeclRefExpr *E, StringRef Label) {
    auto dump = printCommon(E, "declref_expr", Label);

    dump.printDeclRef(E->getDeclRef(), "decl", DeclColor);

    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      dump.print(getAccessSemanticsString(E->getAccessSemantics()), "",
                 AccessLevelColor);

    dump.print(getFunctionRefKindStr(E->getFunctionRefKind()), "function_ref",
               ExprModifierColor);
  }

  void visitSuperRefExpr(SuperRefExpr *E, StringRef Label) {
    (void)printCommon(E, "super_ref_expr", Label);
  }

  void visitTypeExpr(TypeExpr *E, StringRef Label) {
    auto dump = printCommon(E, "type_expr", Label);

    dump.printTypeRepr(E->getTypeRepr(), "typerepr");
  }

  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E, StringRef Label) {
    auto dump = printCommon(E, "other_constructor_ref_expr", Label);
    dump.printDeclRef(E->getDeclRef(), "decl");
  }

  void visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *E, StringRef Label) {
    auto dump = printCommon(E, "overloaded_decl_ref_expr", Label);

    dump.printNodeName(E->getDecls()[0]->getBaseName());
    dump.print(E->getDecls().size(), "number_of_decls", ExprModifierColor);
    dump.print(getFunctionRefKindStr(E->getFunctionRefKind()), "function_ref",
               ExprModifierColor);

    for (auto D : E->getDecls()) {
      auto childDump = dump.child("candidate", "", DeclModifierColor);
      childDump.printDeclRef(D, "decl");
    }
  }

  void visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E, StringRef Label) {
    auto dump = printCommon(E, "unresolved_decl_ref_expr", Label);

    dump.printNodeName(E->getName());
    dump.print(getFunctionRefKindStr(E->getFunctionRefKind()), "function_ref",
               ExprModifierColor);
  }

  void visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *E, StringRef Label) {
    auto dump = printCommon(E, "unresolved_specialize_expr", Label);

    dump.printRec(E->getSubExpr());

    for (TypeLoc T : E->getUnresolvedParams()) {
      dump.printRec(T.getTypeRepr());
    }
  }

  void visitMemberRefExpr(MemberRefExpr *E, StringRef Label) {
    auto dump = printCommon(E, "member_ref_expr", Label);

    dump.printDeclRef(E->getMember(), "decl");

    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      dump.print(getAccessSemanticsString(E->getAccessSemantics()), "",
                 AccessLevelColor);

    dump.printFlag(E->isSuper(), "super");

    dump.printRec(E->getBase());
  }

  void visitDynamicMemberRefExpr(DynamicMemberRefExpr *E, StringRef Label) {
    auto dump = printCommon(E, "dynamic_member_ref_expr", Label);

    dump.printDeclRef(E->getMember(), "decl");
    dump.printRec(E->getBase());
  }

  void visitUnresolvedMemberExpr(UnresolvedMemberExpr *E, StringRef Label) {
    auto dump = printCommon(E, "unresolved_member_expr", Label);

    dump.printNodeName(E->getName());
    dump.print(getFunctionRefKindStr(E->getFunctionRefKind()), "function_ref",
               ExprModifierColor);
  }

  void visitDotSelfExpr(DotSelfExpr *E, StringRef Label) {
    auto dump = printCommon(E, "dot_self_expr", Label);
    dump.printRec(E->getSubExpr());
  }
  void visitParenExpr(ParenExpr *E, StringRef Label) {
    auto dump = printCommon(E, "paren_expr", Label);

    dump.printFlag(E->hasTrailingClosure(), "trailing-closure");
    dump.printRec(E->getSubExpr());
  }

  void visitAwaitExpr(AwaitExpr *E, StringRef Label) {
    auto dump = printCommon(E, "await_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitUnresolvedMemberChainResultExpr(UnresolvedMemberChainResultExpr *E,
                                            StringRef Label){
    auto dump = printCommon(E, "unresolved_member_chain_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitTupleExpr(TupleExpr *E, StringRef Label) {
    auto dump = printCommon(E, "tuple_expr", Label);

    dump.printFlag(E->hasTrailingClosure(), "trailing-closure");

    if (E->hasElementNames()) {
      dump.printLabel("names", IdentifierColor);
      interleave(E->getElementNames(),
                 [&](Identifier name) {
                   dump.colored(IdentifierColor) << QuotedString(name.str());
                 },
                 [&] { dump.colored(IdentifierColor) << ","; });
    }

    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      if (E->getElement(i))
        dump.printRec(E->getElement(i));
      else
        (void)dump.child("<<default value>>", "", ExprColor);
    }
  }

  void visitArrayExpr(ArrayExpr *E, StringRef Label) {
    auto dump = printCommon(E, "array_expr", Label);

    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);

    for (auto elt : E->getElements())
      dump.printRec(elt);
  }

  void visitDictionaryExpr(DictionaryExpr *E, StringRef Label) {
    auto dump = printCommon(E, "dictionary_expr", Label);

    dump.printDeclRef(E->getInitializer(), "initializer", LiteralValueColor);

    for (auto elt : E->getElements())
      dump.printRec(elt);
  }

  void visitSubscriptExpr(SubscriptExpr *E, StringRef Label) {
    auto dump = printCommon(E, "subscript_expr", Label);

    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      dump.print(getAccessSemanticsString(E->getAccessSemantics()), "",
                 AccessLevelColor);
    dump.printFlag(E->isSuper(), "super");
    if (E->hasDecl())
      dump.printDeclRef(E->getDecl(), "decl");
    printArgumentLabels(dump, E->getArgumentLabels());

    dump.printRec(E->getBase(), "base");
    dump.printRec(E->getIndex(), "index");
  }

  void visitKeyPathApplicationExpr(KeyPathApplicationExpr *E, StringRef Label) {
    auto dump = printCommon(E, "keypath_application_expr", Label);

    dump.printRec(E->getBase(), "base");
    dump.printRec(E->getKeyPath(), "key_path");
  }

  void visitDynamicSubscriptExpr(DynamicSubscriptExpr *E, StringRef Label) {
    auto dump = printCommon(E, "dynamic_subscript_expr", Label);

    dump.printDeclRef(E->getMember(), "decl");
    printArgumentLabels(dump, E->getArgumentLabels());

    dump.printRec(E->getBase(), "base");
    dump.printRec(E->getIndex(), "index");
  }

  void visitUnresolvedDotExpr(UnresolvedDotExpr *E, StringRef Label) {
    auto dump = printCommon(E, "unresolved_dot_expr", Label);

    dump.printNodeName(E->getName());
    dump.print(getFunctionRefKindStr(E->getFunctionRefKind()), "function_ref",
               ExprModifierColor);

    if (E->getBase())
      dump.printRec(E->getBase(), "base");
  }

  void visitTupleElementExpr(TupleElementExpr *E, StringRef Label) {
    auto dump = printCommon(E, "tuple_element_expr", Label);

    dump.print(E->getFieldNumber(), "index", IdentifierColor);

    dump.printRec(E->getBase(), "base");
  }

  void visitDestructureTupleExpr(DestructureTupleExpr *E, StringRef Label) {
    auto dump = printCommon(E, "destructure_tuple_expr", Label);

    dump.printRec(E->getDestructuredElements(), "destructured");
    dump.printRec(E->getSubExpr());
    dump.printRec(E->getResultExpr());
  }

  void visitUnresolvedTypeConversionExpr(UnresolvedTypeConversionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "unresolvedtype_conversion_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitFunctionConversionExpr(FunctionConversionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "function_conversion_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitCovariantFunctionConversionExpr(CovariantFunctionConversionExpr *E,
                                            StringRef Label) {
    auto dump = printCommon(E, "covariant_function_conversion_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitCovariantReturnConversionExpr(CovariantReturnConversionExpr *E,
                                          StringRef Label) {
    auto dump = printCommon(E, "covariant_return_conversion_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitImplicitlyUnwrappedFunctionConversionExpr(
      ImplicitlyUnwrappedFunctionConversionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "implicitly_unwrapped_function_conversion_expr",
                            Label);
    dump.printRec(E->getSubExpr());
  }

  void visitUnderlyingToOpaqueExpr(UnderlyingToOpaqueExpr *E, StringRef Label) {
    auto dump = printCommon(E, "underlying_to_opaque_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitErasureExpr(ErasureExpr *E, StringRef Label) {
    auto dump = printCommon(E, "erasure_expr", Label);

    dump.printRec(E->getConformances(), "conformances");
    dump.printRec(E->getSubExpr());
  }

  void visitAnyHashableErasureExpr(AnyHashableErasureExpr *E, StringRef Label) {
    auto dump = printCommon(E, "any_hashable_erasure_expr", Label);
    dump.printRec(E->getConformance());
    dump.printRec(E->getSubExpr());
  }

  void visitConditionalBridgeFromObjCExpr(ConditionalBridgeFromObjCExpr *E,
                                          StringRef Label) {
    auto dump = printCommon(E, "conditional_bridge_from_objc_expr", Label);

    dump.printDeclRef(E->getConversion(), "conversion");
    dump.printRec(E->getSubExpr());
  }

  void visitBridgeFromObjCExpr(BridgeFromObjCExpr *E, StringRef Label) {
    auto dump = printCommon(E, "bridge_from_objc_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitBridgeToObjCExpr(BridgeToObjCExpr *E, StringRef Label) {
    auto dump = printCommon(E, "bridge_to_objc_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitLoadExpr(LoadExpr *E, StringRef Label) {
    auto dump = printCommon(E, "load_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitMetatypeConversionExpr(MetatypeConversionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "metatype_conversion_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitCollectionUpcastConversionExpr(CollectionUpcastConversionExpr *E,
                                           StringRef Label) {
    auto dump = printCommon(E, "collection_upcast_expr", Label);

    dump.printRec(E->getSubExpr());
    if (auto keyConversion = E->getKeyConversion())
      dump.printRec(keyConversion.Conversion, "key_conversion");
    if (auto valueConversion = E->getValueConversion())
      dump.printRec(valueConversion.Conversion, "value_conversion");
  }

  void visitDerivedToBaseExpr(DerivedToBaseExpr *E, StringRef Label) {
    auto dump = printCommon(E, "derived_to_base_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitArchetypeToSuperExpr(ArchetypeToSuperExpr *E, StringRef Label) {
    auto dump = printCommon(E, "archetype_to_super_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitInjectIntoOptionalExpr(InjectIntoOptionalExpr *E, StringRef Label) {
    auto dump = printCommon(E, "inject_into_optional", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitClassMetatypeToObjectExpr(ClassMetatypeToObjectExpr *E,
                                      StringRef Label) {
    auto dump = printCommon(E, "class_metatype_to_object", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitExistentialMetatypeToObjectExpr(ExistentialMetatypeToObjectExpr *E,
                                            StringRef Label) {
    auto dump = printCommon(E, "existential_metatype_to_object", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitProtocolMetatypeToObjectExpr(ProtocolMetatypeToObjectExpr *E,
                                         StringRef Label) {
    auto dump = printCommon(E, "protocol_metatype_to_object", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitInOutToPointerExpr(InOutToPointerExpr *E, StringRef Label) {
    auto dump = printCommon(E, "inout_to_pointer", Label);
    dump.printFlag(E->isNonAccessing(), "nonaccessing");
    dump.printRec(E->getSubExpr());
  }

  void visitArrayToPointerExpr(ArrayToPointerExpr *E, StringRef Label) {
    auto dump = printCommon(E, "array_to_pointer", Label);
    dump.printFlag(E->isNonAccessing(), "nonaccessing");
    dump.printRec(E->getSubExpr());
  }

  void visitStringToPointerExpr(StringToPointerExpr *E, StringRef Label) {
    auto dump = printCommon(E, "string_to_pointer", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitPointerToPointerExpr(PointerToPointerExpr *E, StringRef Label) {
    auto dump = printCommon(E, "pointer_to_pointer", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitForeignObjectConversionExpr(ForeignObjectConversionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "foreign_object_conversion", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitUnevaluatedInstanceExpr(UnevaluatedInstanceExpr *E, StringRef Label) {
    auto dump = printCommon(E, "unevaluated_instance", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitDifferentiableFunctionExpr(DifferentiableFunctionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "differentiable_function", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitLinearFunctionExpr(LinearFunctionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "linear_function", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitDifferentiableFunctionExtractOriginalExpr(
      DifferentiableFunctionExtractOriginalExpr *E, StringRef Label) {
    auto dump = printCommon(E, "differentiable_function_extract_original", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitLinearFunctionExtractOriginalExpr(
      LinearFunctionExtractOriginalExpr *E, StringRef Label) {
    auto dump = printCommon(E, "linear_function_extract_original", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitLinearToDifferentiableFunctionExpr(
      LinearToDifferentiableFunctionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "linear_to_differentiable_function", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitInOutExpr(InOutExpr *E, StringRef Label) {
    auto dump = printCommon(E, "inout_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitVarargExpansionExpr(VarargExpansionExpr *E, StringRef Label) {
    auto dump = printCommon(E, "vararg_expansion_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitForceTryExpr(ForceTryExpr *E, StringRef Label) {
    auto dump = printCommon(E, "force_try_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitOptionalTryExpr(OptionalTryExpr *E, StringRef Label) {
    auto dump = printCommon(E, "optional_try_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitTryExpr(TryExpr *E, StringRef Label) {
    auto dump = printCommon(E, "try_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitSequenceExpr(SequenceExpr *E, StringRef Label) {
    auto dump = printCommon(E, "sequence_expr", Label);
    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i)
      dump.printRec(E->getElement(i));
  }

  void visitCaptureListExpr(CaptureListExpr *E, StringRef Label) {
    auto dump = printCommon(E, "capture_list", Label);
    {
      auto childDump = dump.child("captures", "", CapturesColor);
      for (auto capture : E->getCaptureList())
        childDump.printRec(capture.PBD);
    }
    dump.printRec(E->getClosureBody(), "body");
  }

  LLVM_NODISCARD ASTNodeDumper
  printClosure(AbstractClosureExpr *E, char const *name, StringRef Label) {
    auto dump = printCommon(E, name, Label);

    dump.print(E->getDiscriminator(), "discriminator", DiscriminatorColor);

    switch (auto isolation = E->getActorIsolation()) {
    case ClosureActorIsolation::Independent:
      break;

    case ClosureActorIsolation::ActorInstance:
      dump.printDeclRef(isolation.getActorInstance(), "actor_isolated",
                        CapturesColor);
      break;

    case ClosureActorIsolation::GlobalActor:
      dump.print(isolation.getGlobalActor(), "global_actor_isolated",
                 CapturesColor);
      break;
    }

    if (!E->getCaptureInfo().isTrivial()) {
      dump << " ";
      E->getCaptureInfo().print(dump.colored(CapturesColor).getOS());
    }
    // Printing a function type doesn't indicate whether it's escaping because it doesn't 
    // matter in 99% of contexts. AbstractClosureExpr nodes are one of the only exceptions.
    if (auto Ty = getTypeOfExpr(E)) {
      if (auto fType = Ty->getAs<AnyFunctionType>()) {
        dump.printFlag(!fType->getExtInfo().isNoEscape(), "escaping",
                       ClosureModifierColor);
        dump.printFlag(fType->getExtInfo().isSendable(), "concurrent",
                       ClosureModifierColor);
      }
    }

    return dump;
  }

  void visitClosureExpr(ClosureExpr *E, StringRef Label) {
    auto dump = printClosure(E, "closure_expr", Label);

    dump.printFlag(E->hasSingleExpressionBody(), "single_expr",
                   ClosureModifierColor);
    dump.printFlag(E->allowsImplicitSelfCapture(), "implicit_self",
                   ClosureModifierColor);
    dump.printFlag(E->inheritsActorContext(), "inherits_actor_context",
                   ClosureModifierColor);

    if (E->getParameters())
      dump.printRec(E->getParameters());
    dump.printRec(E->getBody(), "body");
  }

  void visitAutoClosureExpr(AutoClosureExpr *E, StringRef Label) {
    auto dump = printClosure(E, "autoclosure_expr", Label);

    if (E->getParameters())
      dump.printRec(E->getParameters());
    dump.printRec(E->getSingleExpressionBody(), "single_expr_body");
  }

  void visitDynamicTypeExpr(DynamicTypeExpr *E, StringRef Label) {
    auto dump = printCommon(E, "metatype_expr", Label);
    dump.printRec(E->getBase(), "base");
  }

  void visitOpaqueValueExpr(OpaqueValueExpr *E, StringRef Label) {
    auto dump = printCommon(E, "opaque_value_expr", Label);
    dump.print((void*)E);
  }

  void visitPropertyWrapperValuePlaceholderExpr(
      PropertyWrapperValuePlaceholderExpr *E, StringRef Label) {
    auto dump = printCommon(E, "property_wrapper_value_placeholder_expr",
                            Label);

    dump.printRec(E->getOpaqueValuePlaceholder(), "opaque_value_placeholder");
    if (auto *value = E->getOriginalWrappedValue())
      dump.printRec(value, "original_wrapped_value");
  }

  void visitAppliedPropertyWrapperExpr(AppliedPropertyWrapperExpr *E,
                                       StringRef Label) {
    auto dump = printCommon(E, "applied_property_wrapper_expr", Label);
    dump.printRec(E->getValue());
  }

  void visitDefaultArgumentExpr(DefaultArgumentExpr *E, StringRef Label) {
    auto dump = printCommon(E, "default_argument_expr", Label);

    dump.printLabel("default_args_owner");
    E->getDefaultArgsOwner().dump(dump.getOS());

    dump.print(E->getParamIndex(), "param_index");
  }

  void printArgumentLabels(ASTNodeDumper & dump,
                           ArrayRef<Identifier> argLabels) {
    dump.printLabel("arg_labels", ArgumentsColor);
    for (auto label : argLabels) {
      dump.colored(ArgumentsColor)
        << (label.empty() ? "_" : label.str()) << ":";
    }
  }

  void printApplyExpr(ApplyExpr *E, const char *NodeName, StringRef Label) {
    auto dump = printCommon(E, NodeName, Label);

    if (E->isThrowsSet())
      dump.printFlag(E->throws() ? " throws" : " nothrow");

    if (auto call = dyn_cast<CallExpr>(E))
      printArgumentLabels(dump, call->getArgumentLabels());

    dump.printRec(E->getFn(), "fn");
    dump.printRec(E->getArg(), "arg");
  }

  void visitCallExpr(CallExpr *E, StringRef Label) {
    printApplyExpr(E, "call_expr", Label);
  }
  void visitPrefixUnaryExpr(PrefixUnaryExpr *E, StringRef Label) {
    printApplyExpr(E, "prefix_unary_expr", Label);
  }
  void visitPostfixUnaryExpr(PostfixUnaryExpr *E, StringRef Label) {
    printApplyExpr(E, "postfix_unary_expr", Label);
  }
  void visitBinaryExpr(BinaryExpr *E, StringRef Label) {
    printApplyExpr(E, "binary_expr", Label);
  }
  void visitDotSyntaxCallExpr(DotSyntaxCallExpr *E, StringRef Label) {
    printApplyExpr(E, "dot_syntax_call_expr", Label);
  }
  void visitConstructorRefCallExpr(ConstructorRefCallExpr *E, StringRef Label) {
    printApplyExpr(E, "constructor_ref_call_expr", Label);
  }

  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *E, StringRef Label) {
    auto dump = printCommon(E, "dot_syntax_base_ignored", Label);
    dump.printRec(E->getLHS());
    dump.printRec(E->getRHS());
  }

  void printExplicitCastExpr(ExplicitCastExpr *E, const char *name, StringRef Label) {
    auto dump = printCommon(E, name, Label);

    if (auto checkedCast = dyn_cast<CheckedCastExpr>(E))
      dump.printFlag(getCheckedCastKindName(checkedCast->getCastKind()));

    dump.printLabel("writtenType");
    dump << "'";
    if (canGetTypeOfTypeRepr())
      getTypeOfTypeRepr(E->getCastTypeRepr()).print(dump.getOS());
    else
      E->getCastType().print(dump.getOS());
    dump << "'";

    dump.printRec(E->getSubExpr());
  }

  void visitForcedCheckedCastExpr(ForcedCheckedCastExpr *E, StringRef Label) {
    printExplicitCastExpr(E, "forced_checked_cast_expr", Label);
  }

  void visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *E,
                                       StringRef Label) {
    printExplicitCastExpr(E, "conditional_checked_cast_expr", Label);
  }

  void visitIsExpr(IsExpr *E, StringRef Label) {
    printExplicitCastExpr(E, "is_subtype_expr", Label);
  }

  void visitCoerceExpr(CoerceExpr *E, StringRef Label) {
    printExplicitCastExpr(E, "coerce_expr", Label);
  }

  void visitArrowExpr(ArrowExpr *E, StringRef Label) {
    auto dump = printCommon(E, "arrow_expr", Label);
    dump.printFlag(E->getAsyncLoc().isValid(), "async");
    dump.printFlag(E->getThrowsLoc().isValid(), "throws");

    dump.printRec(E->getArgsExpr(), "args");
    dump.printRec(E->getResultExpr(), "result");
  }

  void visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *E, StringRef Label) {
    auto dump = printCommon(E, "rebind_self_in_constructor_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitIfExpr(IfExpr *E, StringRef Label) {
    auto dump = printCommon(E, "if_expr", Label);
    dump.printRec(E->getCondExpr(), "condition");
    dump.printRec(E->getThenExpr(), "then_expr");
    dump.printRec(E->getElseExpr(), "else_expr");
  }

  void visitAssignExpr(AssignExpr *E, StringRef Label) {
    auto dump = printCommon(E, "assign_expr", Label);
    dump.printRec(E->getDest(), "dest");
    dump.printRec(E->getSrc(), "src");
  }

  void visitEnumIsCaseExpr(EnumIsCaseExpr *E, StringRef Label) {
    auto dump = printCommon(E, "enum_is_case_expr", Label);
    dump.printNodeName(E->getEnumElement());

    dump.printRec(E->getSubExpr());
  }

  void visitUnresolvedPatternExpr(UnresolvedPatternExpr *E, StringRef Label) {
    auto dump = printCommon(E, "unresolved_pattern_expr", Label);
    dump.printRec(E->getSubPattern());
  }

  void visitBindOptionalExpr(BindOptionalExpr *E, StringRef Label) {
    auto dump = printCommon(E, "bind_optional_expr", Label);
    dump.print(E->getDepth(), "depth");

    dump.printRec(E->getSubExpr());
  }

  void visitOptionalEvaluationExpr(OptionalEvaluationExpr *E, StringRef Label) {
    auto dump = printCommon(E, "optional_evaluation_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitForceValueExpr(ForceValueExpr *E, StringRef Label) {
    auto dump = printCommon(E, "force_value_expr", Label);
    dump.printFlag(E->isForceOfImplicitlyUnwrappedOptional(),
                   "implicit_iuo_unwrap", ExprModifierColor);

    dump.printRec(E->getSubExpr());
  }

  void visitOpenExistentialExpr(OpenExistentialExpr *E, StringRef Label) {
    auto dump = printCommon(E, "open_existential_expr", Label);

    dump.printRec(E->getOpaqueValue(), "opaque_value");
    dump.printRec(E->getExistentialValue(), "existential_value");
    dump.printRec(E->getSubExpr());
  }

  void visitMakeTemporarilyEscapableExpr(MakeTemporarilyEscapableExpr *E,
                                         StringRef Label) {
    auto dump = printCommon(E, "make_temporarily_escapable_expr", Label);

    dump.printRec(E->getOpaqueValue(), "opaque_value");
    dump.printRec(E->getNonescapingClosureValue(), "nonescaping_closure_value");
    dump.printRec(E->getSubExpr());
  }

  void visitEditorPlaceholderExpr(EditorPlaceholderExpr *E, StringRef Label) {
    auto dump = printCommon(E, "editor_placeholder_expr", Label);

    dump.print(E->getTrailingAngleBracketLoc(), "trailing_angle_bracket_loc",
               LocationColor);

    auto *TyR = E->getPlaceholderTypeRepr();
    auto *ExpTyR = E->getTypeForExpansion();
    if (TyR)
      dump.printRec(TyR, "placeholder_type_repr");
    if (ExpTyR && ExpTyR != TyR)
      dump.printRec(ExpTyR, "type_for_expansion");

    printSemanticExpr(dump, E->getSemanticExpr());
  }

  void visitLazyInitializerExpr(LazyInitializerExpr *E, StringRef Label) {
    auto dump = printCommon(E, "lazy_initializer_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitObjCSelectorExpr(ObjCSelectorExpr *E, StringRef Label) {
    auto dump = printCommon(E, "objc_selector_expr", Label);
    dump.print(getObjCSelectorExprKindString(E->getSelectorKind()), "kind");
    dump.printDeclRef(E->getMethod(), "decl");

    dump.printRec(E->getSubExpr());
  }

  void visitKeyPathExpr(KeyPathExpr *E, StringRef Label) {
    auto dump = printCommon(E, "keypath_expr", Label);
    dump.printFlag(E->isObjC(), "objc");

    {
      auto childDump = dump.child("", "components", ExprColor);
      for (unsigned i : indices(E->getComponents())) {
        Optional<ASTNodeDumper> grandchildDump;
        auto &component = E->getComponents()[i];
        switch (component.getKind()) {
        case KeyPathExpr::Component::Kind::Invalid:
          grandchildDump = childDump.child("", "invalid", ASTNodeColor);
          break;

        case KeyPathExpr::Component::Kind::OptionalChain:
          grandchildDump = childDump.child("", "optional_chain", ASTNodeColor);
          break;

        case KeyPathExpr::Component::Kind::OptionalForce:
          grandchildDump = childDump.child("", "optional_force", ASTNodeColor);
          break;

        case KeyPathExpr::Component::Kind::OptionalWrap:
          grandchildDump = childDump.child("", "optional_wrap", ASTNodeColor);
          break;

        case KeyPathExpr::Component::Kind::Property:
          grandchildDump = childDump.child("", "property", ASTNodeColor);
          grandchildDump->printDeclRef(component.getDeclRef(), "decl");
          break;

        case KeyPathExpr::Component::Kind::Subscript:
          grandchildDump = childDump.child("", "subscript", ASTNodeColor);
          grandchildDump->printDeclRef(component.getDeclRef(), "decl");
          break;

        case KeyPathExpr::Component::Kind::UnresolvedProperty:
          grandchildDump = childDump.child("", "unresolved_property",
                                           ASTNodeColor);
          grandchildDump->printNodeName(component.getUnresolvedDeclName());
          break;

        case KeyPathExpr::Component::Kind::UnresolvedSubscript:
          grandchildDump = childDump.child("", "unresolved_subscript",
                                           ASTNodeColor);
          printArgumentLabels(*grandchildDump, component.getSubscriptLabels());
          break;

        case KeyPathExpr::Component::Kind::Identity:
          grandchildDump = childDump.child("", "identity", ASTNodeColor);
          break;

        case KeyPathExpr::Component::Kind::TupleElement:
          grandchildDump = childDump.child("", "tuple_element", ASTNodeColor);
          grandchildDump->print(component.getTupleIndex(), "index",
                                DiscriminatorColor);
          break;

        case KeyPathExpr::Component::Kind::DictionaryKey:
          grandchildDump = childDump.child("", "dict_key", ASTNodeColor);
          grandchildDump->print(component.getUnresolvedDeclName(), "key",
                                IdentifierColor);
          break;

        case KeyPathExpr::Component::Kind::CodeCompletion:
          grandchildDump = childDump.child("completion", "", ASTNodeColor);
          break;
        }

        grandchildDump->print(getTypeOfKeyPathComponent(E, i), "type",
                              TypeColor);
        if (auto indexExpr = component.getIndexExpr()) {
          grandchildDump->printRec(indexExpr);
        }
      }
    }

    if (auto stringLiteral = E->getObjCStringLiteralExpr())
      dump.printRec(stringLiteral, "objc_string_literal");

    if (!E->isObjC()) {
      if (auto root = E->getParsedRoot())
        dump.printRec(root, "parsed_root");
      if (auto path = E->getParsedPath())
        dump.printRec(path, "parsed_path");
    }
  }

  void visitKeyPathDotExpr(KeyPathDotExpr *E, StringRef Label) {
    (void)printCommon(E, "key_path_dot_expr", Label);
  }

  void visitOneWayExpr(OneWayExpr *E, StringRef Label) {
    auto dump = printCommon(E, "one_way_expr", Label);
    dump.printRec(E->getSubExpr());
  }

  void visitTapExpr(TapExpr *E, StringRef Label) {
    if (!Ctx)
      Ctx = &E->getVar()->getDeclContext()->getASTContext();

    auto dump = printCommon(E, "tap_expr", Label);
    dump.printDeclRef(E->getVar(), "var");

    dump.printRec(E->getSubExpr(), "initializer");
    dump.printRec(E->getBody(), "body");
  }
};

} // end anonymous namespace

void Expr::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

void Expr::dump(raw_ostream &OS, llvm::function_ref<Type(Expr *)> getTypeOfExpr,
                llvm::function_ref<Type(TypeRepr *)> getTypeOfTypeRepr,
                llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
                    getTypeOfKeyPathComponent,
                unsigned Indent) const {
  ExprTypeDelegate delegate(getTypeOfExpr, getTypeOfTypeRepr,
                            getTypeOfKeyPathComponent);
  PrintExpr(getASTContextFromType(getTypeOfExpr(const_cast<Expr *>(this))),
            OS, delegate, Indent)
      .visit(const_cast<Expr *>(this), "");
}

void Expr::dump(raw_ostream &OS, unsigned Indent) const {
  ExprTypeDelegate delegate;
  PrintExpr(getASTContextFromType(getType()), OS, delegate, Indent)
      .visit(const_cast<Expr *>(this), "");
}

void Expr::print(ASTPrinter &Printer, const PrintOptions &Opts) const {
  // FIXME: Fully use the ASTPrinter.
  llvm::SmallString<128> Str;
  llvm::raw_svector_ostream OS(Str);
  dump(OS);
  Printer << OS.str();
}

//===----------------------------------------------------------------------===//
// Printing for TypeRepr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
class PrintTypeRepr : public TypeReprVisitor<PrintTypeRepr, void, StringRef> {
public:
  ASTContext *Ctx;
  raw_ostream &OS;
  ExprTypeDelegate &Delegate;
  unsigned Indent;

  explicit PrintTypeRepr(ASTContext *ctx, raw_ostream &os,
                         ExprTypeDelegate &delegate, unsigned indent)
    : Ctx(ctx), OS(os), Delegate(delegate), Indent(indent) { }

  LLVM_NODISCARD ASTNodeDumper
  printCommon(const char *Name, StringRef Label) {
    return ASTNodeDumper(this, Name, Label, TypeReprColor);
  }

  void visitErrorTypeRepr(ErrorTypeRepr *T, StringRef Label) {
    (void)printCommon("type_error", Label);
  }

  void visitAttributedTypeRepr(AttributedTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_attributed", Label);

    dump.printLabel("attrs");
    T->printAttrs(dump.getOS());

    dump.printRec(T->getTypeRepr());
  }

  void visitIdentTypeRepr(IdentTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_ident", Label);
    for (auto comp : T->getComponentRange()) {
      auto childDump = dump.child("component", "", TypeReprColor);
      childDump.printNodeName(comp->getNameRef());

      if (comp->isBound())
        childDump.printDeclRef(comp->getBoundDecl(), "bind", DeclColor);
      else
        childDump.print("none", "bind", DeclColor);

      if (auto GenIdT = dyn_cast<GenericIdentTypeRepr>(comp))
        for (auto genArg : GenIdT->getGenericArgs())
          childDump.printRec(genArg);
    }
  }

  void visitFunctionTypeRepr(FunctionTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_function", Label);
    dump.printFlag(T->isAsync(), "async");
    dump.printFlag(T->isThrowing(), "throws");

    dump.printRec(T->getArgsTypeRepr(), "arg_type");
    dump.printRec(T->getResultTypeRepr(), "result_type");
  }

  void visitArrayTypeRepr(ArrayTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_array", Label);
    dump.printRec(T->getBase(), "element");
  }

  void visitDictionaryTypeRepr(DictionaryTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_dictionary", Label);
    dump.printRec(T->getKey(), "key_type");
    dump.printRec(T->getValue(), "value_type");
  }

  void visitTupleTypeRepr(TupleTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_tuple", Label);

    for (auto elem : T->getElements()) {
      if (elem.Name.empty())
        dump.printRec(elem.Type);
      else
        dump.printRec(elem.Type, elem.Name.str());
    }
  }

  void visitCompositionTypeRepr(CompositionTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_composite", Label);
    for (auto elem : T->getTypes())
      dump.printRec(elem);
  }

  void visitMetatypeTypeRepr(MetatypeTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_metatype", Label);
    dump.printRec(T->getBase(), "instance_type");
  }

  void visitProtocolTypeRepr(ProtocolTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_protocol", Label);
    dump.printRec(T->getBase());
  }

  void visitInOutTypeRepr(InOutTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_inout", Label);
    dump.printRec(T->getBase());
  }
  
  void visitSharedTypeRepr(SharedTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_shared", Label);
    dump.printRec(T->getBase());
  }

  void visitOwnedTypeRepr(OwnedTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_owned", Label);
    dump.printRec(T->getBase());
  }

  void visitIsolatedTypeRepr(IsolatedTypeRepr *T, StringRef Label) {
    auto dump = printCommon("isolated", Label);
    dump.printRec(T->getBase());
  }

  void visitOptionalTypeRepr(OptionalTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_optional", Label);
    dump.printRec(T->getBase());
  }

  void visitImplicitlyUnwrappedOptionalTypeRepr(
      ImplicitlyUnwrappedOptionalTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_implicitly_unwrapped_optional", Label);
    dump.printRec(T->getBase());
  }

  void visitOpaqueReturnTypeRepr(OpaqueReturnTypeRepr *T, StringRef Label) {
    auto dump = printCommon("type_opaque_return", Label);
    dump.printRec(T->getConstraint());
  }

  void visitNamedOpaqueReturnTypeRepr(NamedOpaqueReturnTypeRepr *T, StringRef Label) {
    printCommon("type_named_opaque_return", Label);
    OS << '\n';
    printRec(T->getBase());
    printCommonPost();
  }

  void visitPlaceholderTypeRepr(PlaceholderTypeRepr *T, StringRef Label) {
    (void)printCommon("type_placeholder", Label);
  }

  void visitFixedTypeRepr(FixedTypeRepr *T, StringRef Label) {
    auto Ty = T->getType();
    if (!Ctx && Ty)
      Ctx = &Ty->getASTContext();

    auto dump = printCommon("type_fixed", Label);
    dump.printNodeLocation(T->getLoc());
    dump.printRec(Ty, "fixed_type");
  }

  void visitSILBoxTypeRepr(SILBoxTypeRepr *T, StringRef Label) {
    auto dump = printCommon("sil_box", Label);

    ArrayRef<SILBoxTypeReprField> Fields = T->getFields();
    for (unsigned i = 0, end = Fields.size(); i != end; ++i) {
      auto childDump = dump.child("sil_box_field", "", TypeReprColor);
      childDump.printFlag(Fields[i].isMutable(), "mutable");

      childDump.printRec(Fields[i].getFieldType());
    }

    for (auto genArg : T->getGenericArguments())
      dump.printRec(genArg);
  }
};

} // end anonymous namespace

void TypeRepr::dump() const {
  ExprTypeDelegate delegate;
  PrintTypeRepr(nullptr, llvm::errs(), delegate, 0)
      .visit(const_cast<TypeRepr*>(this), "");
  llvm::errs() << '\n';
}

// Recursive helpers to avoid infinite recursion for recursive protocol
// conformances.
static void dumpProtocolConformanceRec(
    const ProtocolConformance *conformance, llvm::raw_ostream &out,
    unsigned indent, StringRef Label,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited);

static void dumpProtocolConformanceRefRec(
    const ProtocolConformanceRef conformance, llvm::raw_ostream &out,
    unsigned indent, StringRef label,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  if (conformance.isInvalid()) {
    out.indent(indent) << "(";
    if (!label.empty())
      out << label << "=";
    out << "invalid_conformance)";
  } else if (conformance.isConcrete()) {
    dumpProtocolConformanceRec(conformance.getConcrete(), out, indent, label,
                               visited);
  } else {
    out.indent(indent) << "(";
    if (!label.empty())
      out << label << "=";
    out <<"abstract_conformance protocol="
        << conformance.getAbstract()->getName();
    PrintWithColorRAII(out, ParenthesisColor) << ')';
  }
}

static void dumpProtocolConformanceRec(
    const ProtocolConformance *conformance, llvm::raw_ostream &out,
    unsigned indent, StringRef label,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  // A recursive conformance shouldn't have its contents printed, or there's
  // infinite recursion. (This also avoids printing things that occur multiple
  // times in a conformance hierarchy.)
  auto shouldPrintDetails = visited.insert(conformance).second;

  auto printCommon = [&](StringRef kind) {
    out.indent(indent);
    PrintWithColorRAII(out, ParenthesisColor) << '(';
    if (!label.empty())
      out << label << "=";
    out << kind << "_conformance type=" << conformance->getType()
        << " protocol=" << conformance->getProtocol()->getName();

    if (!shouldPrintDetails)
      out << " (details printed above)";
  };

  switch (conformance->getKind()) {
  case ProtocolConformanceKind::Normal: {
    auto normal = cast<NormalProtocolConformance>(conformance);

    printCommon("normal");
    if (!shouldPrintDetails)
      break;

    // Maybe print information about the conforming context?
    if (normal->isLazilyLoaded()) {
      out << " lazy";
    } else {
      normal->forEachTypeWitness(
          [&](const AssociatedTypeDecl *req, Type ty,
              const TypeDecl *) -> bool {
            out << '\n';
            out.indent(indent + 2);
            PrintWithColorRAII(out, ParenthesisColor) << '(';
            out << "assoc_type req=" << req->getName() << " type=";
            PrintWithColorRAII(out, TypeColor) << Type(ty->getDesugaredType());
            PrintWithColorRAII(out, ParenthesisColor) << ')';
            return false;
          });
      normal->forEachValueWitness([&](const ValueDecl *req,
                                      Witness witness) {
        out << '\n';
        out.indent(indent + 2);
        PrintWithColorRAII(out, ParenthesisColor) << '(';
        out << "value req=" << req->getName() << " witness=";
        if (!witness) {
          out << "(none)";
        } else if (witness.getDecl() == req) {
          out << "(dynamic)";
        } else {
          witness.getDecl()->dumpRef(out);
        }
        PrintWithColorRAII(out, ParenthesisColor) << ')';
      });

      for (auto sigConf : normal->getSignatureConformances()) {
        out << '\n';
        dumpProtocolConformanceRefRec(sigConf, out, indent + 2, "", visited);
      }
    }

    if (auto condReqs = normal->getConditionalRequirementsIfAvailable()) {
      for (auto requirement : *condReqs) {
        out << '\n';
        out.indent(indent + 2);
        requirement.dump(out);
      }
    } else {
      out << '\n';
      out.indent(indent + 2);
      out << "(conditional requirements unable to be computed)";
    }
    break;
  }

  case ProtocolConformanceKind::Self: {
    printCommon("self");
    break;
  }

  case ProtocolConformanceKind::Inherited: {
    auto conf = cast<InheritedProtocolConformance>(conformance);
    printCommon("inherited");
    if (!shouldPrintDetails)
      break;

    out << '\n';
    dumpProtocolConformanceRec(conf->getInheritedConformance(), out, indent + 2,
                               "", visited);
    break;
  }

  case ProtocolConformanceKind::Specialized: {
    auto conf = cast<SpecializedProtocolConformance>(conformance);
    printCommon("specialized");
    if (!shouldPrintDetails)
      break;

    out << '\n';
    dumpSubstitutionMapRec(conf->getSubstitutionMap(), out,
                           SubstitutionMap::DumpStyle::Full, indent + 2,
                           visited);
    out << '\n';
    if (auto condReqs = conf->getConditionalRequirementsIfAvailableOrCached(
            /*computeIfPossible=*/false)) {
      for (auto subReq : *condReqs) {
        out.indent(indent + 2);
        subReq.dump(out);
        out << '\n';
      }
    } else {
      out.indent(indent + 2);
      out << "(conditional requirements unable to be computed)\n";
    }
    dumpProtocolConformanceRec(conf->getGenericConformance(), out, indent + 2,
                               "", visited);
    break;
  }
  }

  PrintWithColorRAII(out, ParenthesisColor) << ')';
}

static void dumpSubstitutionMapRec(
    SubstitutionMap map, llvm::raw_ostream &out,
    SubstitutionMap::DumpStyle style, unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  auto genericSig = map.getGenericSignature();
  out.indent(indent);

  auto printParen = [&](char p) {
    PrintWithColorRAII(out, ParenthesisColor) << p;
  };
  printParen('(');
  SWIFT_DEFER { printParen(')'); };
  out << "substitution_map generic_signature=";
  if (genericSig.isNull()) {
    out << "<nullptr>";
    return;
  }

  genericSig->print(out);
  auto genericParams = genericSig->getGenericParams();
  auto replacementTypes =
      static_cast<const SubstitutionMap &>(map).getReplacementTypesBuffer();
  for (unsigned i : indices(genericParams)) {
    if (style == SubstitutionMap::DumpStyle::Minimal) {
      out << " ";
    } else {
      out << "\n";
      out.indent(indent + 2);
    }
    printParen('(');
    out << "substitution ";
    genericParams[i]->print(out);
    out << " -> ";
    if (replacementTypes[i]) {
      PrintOptions opts;
      opts.PrintForSIL = true;
      replacementTypes[i]->print(out, opts);
    }
    else
      out << "<<unresolved concrete type>>";
    printParen(')');
  }
  // A minimal dump doesn't need the details about the conformances, a lot of
  // that info can be inferred from the signature.
  if (style == SubstitutionMap::DumpStyle::Minimal)
    return;

  auto conformances = map.getConformances();
  for (const auto &req : genericSig->getRequirements()) {
    if (req.getKind() != RequirementKind::Conformance)
      continue;

    out << "\n";
    out.indent(indent + 2);
    printParen('(');
    out << "conformance type=";
    req.getFirstType()->print(out);
    out << "\n";
    dumpProtocolConformanceRefRec(conformances.front(), out, indent + 4, "",
                                  visited);

    printParen(')');
    conformances = conformances.slice(1);
  }
}

void ProtocolConformanceRef::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}

void ProtocolConformanceRef::dump(llvm::raw_ostream &out, unsigned indent,
                                  bool details) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  if (!details && isConcrete())
    visited.insert(getConcrete());

  dumpProtocolConformanceRefRec(*this, out, indent, "", visited);
}

void ProtocolConformanceRef::print(llvm::raw_ostream &out) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpProtocolConformanceRefRec(*this, out, 0, "", visited);

}

void ProtocolConformance::dump() const {
  auto &out = llvm::errs();
  dump(out);
  out << '\n';
}

void ProtocolConformance::dump(llvm::raw_ostream &out, unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpProtocolConformanceRec(this, out, indent, "", visited);
}

void SubstitutionMap::dump(llvm::raw_ostream &out, DumpStyle style,
                           unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpSubstitutionMapRec(*this, out, style, indent, visited);
}

void SubstitutionMap::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

//===----------------------------------------------------------------------===//
// Dumping for Types.
//===----------------------------------------------------------------------===//

namespace {
  class PrintType : public TypeVisitor<PrintType, void, StringRef> {
  public:
    ASTContext *Ctx;
    raw_ostream &OS;
    ExprTypeDelegate &Delegate;
    unsigned Indent;

    explicit PrintType(ASTContext *ctx, raw_ostream &os,
                       ExprTypeDelegate &delegate, unsigned indent)
      : Ctx(ctx), OS(os), Delegate(delegate), Indent(indent) { }

    void visit(Type T, StringRef Label) {
      if (!T.isNull())
        TypeVisitor<PrintType, void, StringRef>::visit(T, Label);
      else {
        (void)printCommon("<<null>>", Label);
      }
    }

  private:
    LLVM_NODISCARD ASTNodeDumper
    printCommon(const char *name, StringRef label) {
      return ASTNodeDumper(this, name, label, TypeColor);
    }

    void dumpParameterFlags(ASTNodeDumper &dump,
                            ParameterTypeFlags paramFlags) {
      dump.printFlag(paramFlags.isVariadic(), "vararg");
      dump.printFlag(paramFlags.isAutoClosure(), "autoclosure");
      dump.printFlag(paramFlags.isNonEphemeral(), "nonEphemeral");
      switch (paramFlags.getValueOwnership()) {
      case ValueOwnership::Default: break;
      case ValueOwnership::Owned: dump.printFlag("owned"); break;
      case ValueOwnership::Shared: dump.printFlag("shared"); break;
      case ValueOwnership::InOut: dump.printFlag("inout"); break;
      }
    }

  public:

#define TRIVIAL_TYPE_PRINTER(Class,Name)                                       \
    void visit##Class##Type(Class##Type *T, StringRef label) {                 \
      (void)printCommon(#Name "_type", label);                                 \
    }

    void visitErrorType(ErrorType *T, StringRef label) {
      auto dump = printCommon("error_type", label);
      if (auto originalType = T->getOriginalType())
        dump.printRec(originalType, "original_type");
    }

    TRIVIAL_TYPE_PRINTER(Unresolved, unresolved)

    void visitPlaceholderType(PlaceholderType *T, StringRef label) {
      auto dump = printCommon("placeholder_type", label);
      auto originator = T->getOriginator();
      if (auto *typeVar = originator.dyn_cast<TypeVariableType *>()) {
        dump.print(typeVar, "type_variable");
      } else if (auto *VD = originator.dyn_cast<VarDecl *>()) {
        dump.printDeclRef(VD, "decl");
      } else if (auto *EE = originator.dyn_cast<ErrorExpr *>()) {
        dump.printFlag("error_expr");
      } else if (auto *DMT = originator.dyn_cast<DependentMemberType *>()) {
        dump.printRec(DMT, "dependent_member_type");
      } else if (auto *PTR = originator.dyn_cast<PlaceholderTypeRepr *>()) {
        dump.printRec(PTR, "placeholder_type_repr");
      } else {
        llvm_unreachable("Unknown originator type???");
      }
    }

    void visitBuiltinIntegerType(BuiltinIntegerType *T, StringRef label) {
      auto dump = printCommon("builtin_integer_type", label);
      if (T->isFixedWidth())
        dump.print(T->getFixedWidth(), "bit_width");
      else
        dump.printFlag("word_sized");
    }

    void visitBuiltinFloatType(BuiltinFloatType *T, StringRef label) {
      auto dump = printCommon("builtin_float_type", label);
      dump.print(T->getBitWidth(), "bit_width");
    }

    TRIVIAL_TYPE_PRINTER(BuiltinIntegerLiteral, builtin_integer_literal)
    TRIVIAL_TYPE_PRINTER(BuiltinJob, builtin_job)
    TRIVIAL_TYPE_PRINTER(BuiltinExecutor, builtin_executor_ref)
    TRIVIAL_TYPE_PRINTER(BuiltinDefaultActorStorage, builtin_default_actor_storage)
    TRIVIAL_TYPE_PRINTER(BuiltinRawPointer, builtin_raw_pointer)
    TRIVIAL_TYPE_PRINTER(BuiltinRawUnsafeContinuation, builtin_raw_unsafe_continuation)
    TRIVIAL_TYPE_PRINTER(BuiltinNativeObject, builtin_native_object)
    TRIVIAL_TYPE_PRINTER(BuiltinBridgeObject, builtin_bridge_object)
    TRIVIAL_TYPE_PRINTER(BuiltinUnsafeValueBuffer, builtin_unsafe_value_buffer)
    TRIVIAL_TYPE_PRINTER(SILToken, sil_token)

    void visitBuiltinVectorType(BuiltinVectorType *T, StringRef label) {
      auto dump = printCommon("builtin_vector_type", label);
      dump.print(T->getNumElements(), "num_elements");
      dump.printRec(T->getElementType());
    }

    void visitTypeAliasType(TypeAliasType *T, StringRef label) {
      auto dump = printCommon("type_alias_type", label);
      dump.printDeclRef(T->getDecl(), "decl");

      if (auto underlying = T->getSinglyDesugaredType())
        dump.print(underlying, "underlying", TypeColor);
      else
        dump.print("<<unresolved>>", "underlying", TypeColor);

      if (T->getParent())
        dump.printRec(T->getParent(), "parent");

      dump.printRec(T->getDirectGenericArgs(), "generic_args");
    }

    void visitParenType(ParenType *T, StringRef label) {
      auto dump = printCommon("paren_type", label);
      dumpParameterFlags(dump, T->getParameterFlags());
      dump.printRec(T->getUnderlyingType());
    }

    void visitTupleType(TupleType *T, StringRef label) {
      auto dump = printCommon("tuple_type", label);
      dump.print(T->getNumElements(), "num_elements");
      
      for (const auto &elt : T->getElements()) {
        auto childDump = dump.child("", "tuple_type_elt", TypeFieldColor);
        if (elt.hasName())
          childDump.print(elt.getName().str(), "name");
        dumpParameterFlags(childDump, elt.getParameterFlags());
        childDump.printRec(elt.getType());
      }
    }

#define REF_STORAGE(Name, name, ...) \
    void visit##Name##StorageType(Name##StorageType *T, StringRef label) { \
      auto dump = printCommon(#name "_storage_type", label); \
      dump.printRec(T->getReferentType(), "referent_type"); \
    }
#include "swift/AST/ReferenceStorage.def"

    void visitEnumType(EnumType *T, StringRef label) {
      auto dump = printCommon("enum_type", label);
      dump.printDeclRef(T->getDecl(), "decl");
      if (T->getParent())
        dump.printRec(T->getParent(), "parent");
    }

    void visitStructType(StructType *T, StringRef label) {
      auto dump = printCommon("struct_type", label);
      dump.printDeclRef(T->getDecl(), "decl");
      if (T->getParent())
        dump.printRec(T->getParent(), "parent");
    }

    void visitClassType(ClassType *T, StringRef label) {
      auto dump = printCommon("class_type", label);
      dump.printDeclRef(T->getDecl(), "decl");
      if (T->getParent())
        dump.printRec(T->getParent(), "parent");
    }

    void visitProtocolType(ProtocolType *T, StringRef label) {
      auto dump = printCommon("protocol_type", label);
      dump.print(T->getDecl(), "decl");
      if (T->getParent())
        dump.printRec(T->getParent(), "parent");
    }

    void visitMetatypeType(MetatypeType *T, StringRef label) {
      auto dump = printCommon("metatype_type", label);
      if (T->hasRepresentation())
        dump.printFlag(getMetatypeRepresentationString(T->getRepresentation()));
      dump.printRec(T->getInstanceType());
    }

    void visitExistentialMetatypeType(ExistentialMetatypeType *T,
                                      StringRef label) {
      auto dump = printCommon("existential_metatype_type", label);
      if (T->hasRepresentation())
        dump.printFlag(getMetatypeRepresentationString(T->getRepresentation()));
      dump.printRec(T->getInstanceType());
    }

    void visitModuleType(ModuleType *T, StringRef label) {
      auto dump = printCommon("module_type", label);
      dump.printDeclRef(T->getModule(), "module");
    }

    void visitDynamicSelfType(DynamicSelfType *T, StringRef label) {
      auto dump = printCommon("dynamic_self_type", label);
      dump.printRec(T->getSelfType());
    }
    
    LLVM_NODISCARD ASTNodeDumper printArchetypeCommon(ArchetypeType *T,
                                                      const char *className,
                                                      StringRef label) {
      auto dump = printCommon(className, label);
      dump.print(static_cast<void *>(T), "address");
      dump.printFlag(T->requiresClass(), "class");

      if (auto layout = T->getLayoutConstraint()) {
        dump.printLabel("layout");
        layout->print(dump.getOS());
      }

      if (auto superclass = T->getSuperclass())
        dump.printRec(superclass, "superclass");

      for (auto proto : T->getConformsTo()) {
        auto childDump = dump.child("protocol_conformance", "", TypeFieldColor);
        childDump.printDeclRef(proto, "decl");
      }

      return dump;
    }
    
    void printArchetypeNestedTypes(ASTNodeDumper &dump, ArchetypeType *T) {
      for (auto nestedType : T->getKnownNestedTypes()) {
        auto childDump = dump.child("nested_type", "", TypeFieldColor);

        auto label = nestedType.first.str();
        if (!nestedType.second)
          childDump.print("<<unresolved>>", label, TypeColor);
        else
          childDump.print(nestedType.second.getString(), label, TypeColor);
      }
    }

    void visitPrimaryArchetypeType(PrimaryArchetypeType *T, StringRef label) {
      auto dump = printArchetypeCommon(T, "primary_archetype_type", label);
      dump.printNodeName(T->getFullName());
      printArchetypeNestedTypes(dump, T);
    }
    void visitNestedArchetypeType(NestedArchetypeType *T, StringRef label) {
      auto dump = printArchetypeCommon(T, "nested_archetype_type", label);
      dump.printNodeName(T->getFullName());
      dump.print(T->getParent(), "parent");
      dump.printDeclRef(T->getAssocType(), "assoc_type");
      printArchetypeNestedTypes(dump, T);
    }
    void visitOpenedArchetypeType(OpenedArchetypeType *T, StringRef label) {
      auto dump = printArchetypeCommon(T, "opened_archetype_type", label);
      dump.print(T->getOpenedExistentialID(), "opened_existential_id");
      dump.printRec(T->getOpenedExistentialType(), "opened_existential");
      printArchetypeNestedTypes(dump, T);
    }
    void visitOpaqueTypeArchetypeType(OpaqueTypeArchetypeType *T,
                                      StringRef label) {
      auto dump = printArchetypeCommon(T, "opaque_type", label);
      dump.printDeclRef(T->getDecl()->getNamingDecl(), "decl");

      if (!T->getSubstitutions().empty()) {
        dump << '\n';
        SmallPtrSet<const ProtocolConformance *, 4> Dumped;
        dumpSubstitutionMapRec(T->getSubstitutions(), dump.getOS(),
                               SubstitutionMap::DumpStyle::Full,
                               dump.IndentChildren, Dumped);
      }

      printArchetypeNestedTypes(dump, T);
    }

    void visitGenericTypeParamType(GenericTypeParamType *T, StringRef label) {
      auto dump = printCommon("generic_type_param_type", label);
      dump.print(T->getDepth(), "depth");
      dump.print(T->getIndex(), "index");
      if (auto decl = T->getDecl())
        dump.printDeclRef(decl, "decl");
    }

    void visitDependentMemberType(DependentMemberType *T, StringRef label) {
      auto dump = printCommon("dependent_member_type", label);
      if (auto assocType = T->getAssocType()) {
        dump.printDeclRef(assocType, "assoc_type");
      } else {
        dump.print(T->getName(), "unresolved_name", IdentifierColor);
      }
      dump.printRec(T->getBase(), "base");
    }

    void printAnyFunctionParams(ArrayRef<AnyFunctionType::Param> params,
                                StringRef label) {
      auto dump = printCommon("function_params", label);
      dump.print(params.size(), "num_params");

      for (const auto &param : params) {
        auto childDump = dump.child("", "param", TypeFieldColor);
        if (param.hasLabel())
          childDump.print(param.getLabel().str(), "api_name");
        if (param.hasInternalLabel())
          childDump.print(param.getInternalLabel().str(), "internal_name");
        dumpParameterFlags(childDump, param.getParameterFlags());
        childDump.printRec(param.getPlainType());
      }
    }

    void printClangTypeRec(ASTNodeDumper &dump, const ClangTypeInfo &clangTy,
                           StringRef label = "") {
      // [TODO: Improve-Clang-type-printing]
      if (clangTy.empty()) return;

      std::string s;
      llvm::raw_string_ostream os(s);
      auto &clangCtx = Ctx->getClangModuleLoader()->getClangASTContext();
      clangTy.dump(os, clangCtx);

      auto childDump = dump.child("clang_type", label, TypeFieldColor);
      childDump.print(os.str());
    }

    LLVM_NODISCARD ASTNodeDumper
    printAnyFunctionTypeCommon(AnyFunctionType *T, const char *name,
                               StringRef label) {
      auto dump = printCommon(name, label);

      SILFunctionType::Representation representation =
        T->getExtInfo().getSILRepresentation();
      if (representation != SILFunctionType::Representation::Thick)
        dump.print(getSILFunctionTypeRepresentationString(representation),
                   "representation");

      dump.printFlag(!T->isNoEscape(), "escaping");
      dump.printFlag(T->isSendable(), "Sendable");
      dump.printFlag(T->isAsync(), "async");
      dump.printFlag(T->isThrowing(), "throws");

      if (Type globalActor = T->getGlobalActor())
        dump.print(globalActor, "global_actor");

      printClangTypeRec(dump, T->getClangTypeInfo());
      printAnyFunctionParams(T->getParams(), "input");
      dump.printRec(T->getResult(), "output");

      return dump;
    }

    void visitFunctionType(FunctionType *T, StringRef label) {
      (void)printAnyFunctionTypeCommon(T, "function_type", label);
    }

    void visitGenericFunctionType(GenericFunctionType *T, StringRef label) {
      auto dump = printAnyFunctionTypeCommon(T, "generic_function_type", label);

      // FIXME: generic signature dumping needs improvement
      auto childDump = dump.child("generic_sig", "", ASTNodeColor);
      childDump.print(T->getGenericSignature()->getAsString());
    }

    template <typename Elem>
    void printInterfaceTypesRec(ASTNodeDumper &dump, ArrayRef<Elem> elems,
                                StringRef label) {
      auto childDump = dump.child("array", label, TypeFieldColor);
      for (auto elem : elems)
        childDump.printRec(elem.getInterfaceType());
    }

    void visitSILFunctionType(SILFunctionType *T, StringRef label) {
      auto dump = printCommon("sil_function_type", label);
      dump.print(T->getString(), "type", TypeColor);

      printClangTypeRec(dump, T->getClangTypeInfo());

      printInterfaceTypesRec(dump, T->getParameters(), "inputs");
      printInterfaceTypesRec(dump, T->getYields(), "yields");
      printInterfaceTypesRec(dump, T->getResults(), "results");

      if (auto error  = T->getOptionalErrorResult())
        dump.printRec(error->getInterfaceType(), "error");

      dump << '\n';
      T->getPatternSubstitutions().dump(dump.getOS(),
                                        SubstitutionMap::DumpStyle::Full,
                                        dump.IndentChildren);
      dump << '\n';
      T->getInvocationSubstitutions().dump(dump.getOS(),
                                           SubstitutionMap::DumpStyle::Full,
                                           dump.IndentChildren);
    }

    void visitSILBlockStorageType(SILBlockStorageType *T, StringRef label) {
      auto dump = printCommon("sil_block_storage_type", label);
      dump.printRec(T->getCaptureType(), "capture_type");
    }

    void visitSILBoxType(SILBoxType *T, StringRef label) {
      auto dump = printCommon("sil_box_type", label);
      // FIXME: Print the structure of the type.
      dump.print(T->getString(), "type");
    }

    void visitArraySliceType(ArraySliceType *T, StringRef label) {
      auto dump = printCommon("array_slice_type", label);
      dump.printRec(T->getBaseType(), "base_type");
    }

    void visitOptionalType(OptionalType *T, StringRef label) {
      auto dump = printCommon("optional_type", label);
      dump.printRec(T->getBaseType(), "base_type");
    }

    void visitDictionaryType(DictionaryType *T, StringRef label) {
      auto dump = printCommon("dictionary_type", label);
      dump.printRec(T->getKeyType(), "key_type");
      dump.printRec(T->getValueType(), "value_type");
    }

    void visitProtocolCompositionType(ProtocolCompositionType *T,
                                      StringRef label) {
      auto dump = printCommon("protocol_composition_type", label);
      dump.printFlag(T->hasExplicitAnyObject(), "any_object");
      dump.printRec(T->getMembers(), "members");
    }

    void visitLValueType(LValueType *T, StringRef label) {
      auto dump = printCommon("lvalue_type", label);
      dump.printRec(T->getObjectType(), "object_type");
    }

    void visitInOutType(InOutType *T, StringRef label) {
      auto dump = printCommon("inout_type", label);
      dump.printRec(T->getObjectType(), "object_type");
    }

    void visitUnboundGenericType(UnboundGenericType *T, StringRef label) {
      auto dump = printCommon("unbound_generic_type", label);
      dump.printDeclRef(T->getDecl(), "decl");
      if (T->getParent())
        dump.printRec(T->getParent(), "parent");
    }

    void printBoundGenericType(BoundGenericType *T, const char *name,
                               StringRef label) {
      auto dump = printCommon(name, label);
      dump.printDeclRef(T->getDecl(), "decl");
      if (T->getParent())
        dump.printRec(T->getParent(), "parent");
      dump.printRec(T->getGenericArgs(), "generic_args");
    }

    void visitBoundGenericClassType(BoundGenericClassType *T, StringRef label) {
      printBoundGenericType(T, "bound_generic_class_type", label);
    }
    void visitBoundGenericStructType(BoundGenericStructType *T,
                                     StringRef label) {
      printBoundGenericType(T, "bound_generic_struct_type", label);
    }
    void visitBoundGenericEnumType(BoundGenericEnumType *T, StringRef label) {
      printBoundGenericType(T, "bound_generic_enum_type", label);
    }

    void visitTypeVariableType(TypeVariableType *T, StringRef label) {
      auto dump = printCommon("type_variable_type", label);
      dump.print(T->getID(), "id");
    }

#undef TRIVIAL_TYPE_PRINTER
  };
} // end anonymous namespace

void Type::dump() const {
  dump(llvm::errs());
}

void Type::dump(raw_ostream &os, unsigned indent) const {
  #if SWIFT_BUILD_ONLY_SYNTAXPARSERLIB
    return; // not needed for the parser library.
  #endif

  ExprTypeDelegate delegate;
  PrintType(getASTContextFromType(*this), os, delegate, indent)
      .visit(*this, "");
  os << "\n";
}

void TypeBase::dump() const {
  // Make sure to print type variables.
  Type(const_cast<TypeBase *>(this)).dump();
}

void TypeBase::dump(raw_ostream &os, unsigned indent) const {
  Type(const_cast<TypeBase *>(this)).dump(os, indent);
}

void GenericSignatureImpl::dump() const {
  GenericSignature(const_cast<GenericSignatureImpl *>(this)).dump();
}

void GenericEnvironment::dump(raw_ostream &os) const {
  os << "Generic environment:\n";
  for (auto gp : getGenericParams()) {
    gp->dump(os);
    mapTypeIntoContext(gp)->dump(os);
  }
  os << "Generic parameters:\n";
  for (auto paramTy : getGenericParams())
    paramTy->dump(os);
}

void GenericEnvironment::dump() const {
  dump(llvm::errs());
}

StringRef swift::getAccessorKindString(AccessorKind value) {
  switch (value) {
#define ACCESSOR(ID)
#define SINGLETON_ACCESSOR(ID, KEYWORD) \
  case AccessorKind::ID: return #KEYWORD;
#include "swift/AST/AccessorKinds.def"
  }

  llvm_unreachable("Unhandled AccessorKind in switch.");
}

void StableSerializationPath::dump() const {
  dump(llvm::errs());
}

static StringRef getExternalPathComponentKindString(
                  StableSerializationPath::ExternalPath::ComponentKind kind) {
  switch (kind) {
#define CASE(ID, STRING) \
  case StableSerializationPath::ExternalPath::ID: return STRING;
  CASE(Record, "record")
  CASE(Enum, "enum")
  CASE(Namespace, "namespace")
  CASE(Typedef, "typedef")
  CASE(TypedefAnonDecl, "anonymous tag")
  CASE(ObjCInterface, "@interface")
  CASE(ObjCProtocol, "@protocol")
#undef CASE
  }
  llvm_unreachable("bad kind");
}

void StableSerializationPath::dump(llvm::raw_ostream &os) const {
  if (isSwiftDecl()) {
    os << "clang decl of:\n";
    getSwiftDecl()->dump(os, 2);
  } else {
    auto &path = getExternalPath();
    using ExternalPath = StableSerializationPath::ExternalPath;
    os << "external path: ";
    size_t index = 0;
    for (auto &entry : path.Path) {
      if (index++) os << " -> ";
      os << getExternalPathComponentKindString(entry.first);
      if (ExternalPath::requiresIdentifier(entry.first))  {
        os << "(" << entry.second << ")";
      }
    }
    os << "\n";
  }
}

void ASTNodeDumper::printRec(Decl *D, StringRef Label) {
  *OS << '\n';
  PrintDecl(Ctx, *OS, *Delegate, IndentChildren).visit(D, Label);
}
void ASTNodeDumper::printRec(Expr *E, StringRef Label) {
  *OS << '\n';
  PrintExpr(Ctx, *OS, *Delegate, IndentChildren).visit(E, Label);
}
void ASTNodeDumper::printRec(Stmt *S, StringRef Label) {
  *OS << '\n';
  PrintStmt(Ctx, *OS, *Delegate, IndentChildren).visit(S, Label);
}
void ASTNodeDumper::printRec(TypeRepr *T, StringRef Label) {
  *OS << '\n';
  PrintTypeRepr(Ctx, *OS, *Delegate, IndentChildren).visit(T, Label);
}
void ASTNodeDumper::printRec(const Pattern *P, StringRef Label) {
  *OS << '\n';
  PrintPattern(Ctx, *OS, *Delegate, IndentChildren)
      .visit(const_cast<Pattern *>(P), Label);
}
void ASTNodeDumper::printRec(Type Ty, StringRef Label) {
  *OS << '\n';
  PrintType(Ctx, *OS, *Delegate, IndentChildren)
      .visit(Ty, Label);
}
void ASTNodeDumper::printRec(ParameterList *PL, StringRef Label) {
  *OS << '\n';
  PrintDecl(Ctx, *OS, *Delegate, IndentChildren).printParameterList(PL, Label);
}
void ASTNodeDumper::printRec(ProtocolConformanceRef conf, StringRef Label) {
  *OS << '\n';
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpProtocolConformanceRefRec(conf, *OS, IndentChildren, Label, visited);
}
