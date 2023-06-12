//===--- ASTDumper.cpp - Swift Language AST Dumper ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
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
#include "swift/AST/Attr.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/PackConformance.h"
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
DEF_COLOR(ArgModifier, CYAN, false)
DEF_COLOR(ClosureModifier, CYAN, false)
DEF_COLOR(TypeField, CYAN, false)
DEF_COLOR(Location, CYAN, false)

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
} // end anonymous namespace

static void printGenericParameters(raw_ostream &OS, GenericParamList *Params) {
  if (!Params)
    return;
  OS << ' ';
  Params->print(OS);
}

static void printSourceRange(raw_ostream &OS, const SourceRange R,
                             const ASTContext &Ctx) {
  if (!R.isValid())
    return;

  PrintWithColorRAII(OS, RangeColor) << " range=";
  R.print(PrintWithColorRAII(OS, RangeColor).getOS(), Ctx.SourceMgr,
          /*PrintText=*/false);
}

static StringRef
getSILFunctionTypeRepresentationString(SILFunctionType::Representation value) {
  switch (value) {
  case SILFunctionType::Representation::Thick: return "thick";
  case SILFunctionType::Representation::Block: return "block";
  case SILFunctionType::Representation::CFunctionPointer: return "c";
  case SILFunctionType::Representation::CXXMethod:
    return "cxx_method";
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
    case AccessSemantics::DistributedThunk: return "distributed_thunk";
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

static void printArgument(raw_ostream &OS, const Argument &arg,
                          unsigned indentLevel,
                          std::function<void(Expr *)> printRec) {
  OS.indent(indentLevel);
  PrintWithColorRAII(OS, ParenthesisColor) << '(';
  PrintWithColorRAII(OS, ExprColor) << "argument";

  auto label = arg.getLabel();
  if (!label.empty()) {
    PrintWithColorRAII(OS, ArgumentsColor) << " label=";
    PrintWithColorRAII(OS, ArgumentsColor) << label.str();
  }
  if (arg.isInOut())
    PrintWithColorRAII(OS, ArgModifierColor) << " inout";

  OS << '\n';
  printRec(arg.getExpr());
  PrintWithColorRAII(OS, ParenthesisColor) << ')';
}

static void printArgumentList(raw_ostream &OS,
                              const ArgumentList *argList,
                              unsigned &indentLevel,
                              std::function<void(Expr *)> printRec,
                              bool indent = true) {
  if (indent)
    indentLevel += 2;

  OS.indent(indentLevel);
  PrintWithColorRAII(OS, ParenthesisColor) << '(';
  PrintWithColorRAII(OS, ExprColor) << "argument_list";

  if (argList->isImplicit())
    PrintWithColorRAII(OS, ArgModifierColor) << " implicit";

  if (argList->hasAnyArgumentLabels()) {
    PrintWithColorRAII(OS, ArgumentsColor) << " labels=";
    for (auto arg : *argList) {
      auto label = arg.getLabel();
      PrintWithColorRAII(OS, ArgumentsColor)
      << (label.empty() ? "_" : label.str()) << ":";
    }
  }

  indentLevel += 2;
  for (auto arg : *argList) {
    OS << '\n';
    printArgument(OS, arg, indentLevel, printRec);
  }
  indentLevel -= 2;

  PrintWithColorRAII(OS, ParenthesisColor) << ')';

  if (indent)
    indentLevel -= 2;
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
  class PrintPattern : public PatternVisitor<PrintPattern> {
  public:
    raw_ostream &OS;
    unsigned Indent;

    explicit PrintPattern(raw_ostream &os, unsigned indent = 0)
      : OS(os), Indent(indent) { }

    void printRec(Decl *D) { D->dump(OS, Indent + 2); }
    void printRec(Expr *E) { E->dump(OS, Indent + 2); }
    void printRec(Stmt *S, const ASTContext &Ctx) { S->dump(OS, &Ctx, Indent + 2); }
    void printRec(TypeRepr *T);
    void printRec(const Pattern *P) {
      PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
    }

    raw_ostream &printCommon(Pattern *P, const char *Name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, PatternColor) << Name;

      if (P->isImplicit())
        PrintWithColorRAII(OS, ExprModifierColor) << " implicit";

      if (P->hasType()) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        P->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        PrintWithColorRAII(OS, TypeColor) << "'";
      }
      return OS;
    }

    void visitParenPattern(ParenPattern *P) {
      printCommon(P, "pattern_paren") << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitTuplePattern(TuplePattern *P) {
      printCommon(P, "pattern_tuple");

      OS << " names=";
      interleave(P->getElements(),
                 [&](const TuplePatternElt &elt) {
                   auto name = elt.getLabel();
                   OS << (name.empty() ? "''" : name.str());
                 },
                 [&] { OS << ","; });

      for (auto &elt : P->getElements()) {
        OS << '\n';
        printRec(elt.getPattern());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitNamedPattern(NamedPattern *P) {
      printCommon(P, "pattern_named");
      PrintWithColorRAII(OS, IdentifierColor) << " '" << P->getNameStr() << "'";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitAnyPattern(AnyPattern *P) {
      if (P->isAsyncLet()) {
        printCommon(P, "async_let ");
      }
      printCommon(P, "pattern_any");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitTypedPattern(TypedPattern *P) {
      printCommon(P, "pattern_typed") << '\n';
      printRec(P->getSubPattern());
      if (auto *repr = P->getTypeRepr()) {
        OS << '\n';
        printRec(repr);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitIsPattern(IsPattern *P) {
      printCommon(P, "pattern_is")
        << ' ' << getCheckedCastKindName(P->getCastKind()) << ' ';
      P->getCastType().print(OS);
      if (auto sub = P->getSubPattern()) {
        OS << '\n';
        printRec(sub);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitExprPattern(ExprPattern *P) {
      printCommon(P, "pattern_expr");
      OS << '\n';
      if (auto m = P->getCachedMatchExpr())
        printRec(m);
      else
        printRec(P->getSubExpr());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitBindingPattern(BindingPattern *P) {
      printCommon(P, P->isLet() ? "pattern_let" : "pattern_var");
      OS << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitEnumElementPattern(EnumElementPattern *P) {
      printCommon(P, "pattern_enum_element");
      OS << ' ';
      P->getParentType().print(PrintWithColorRAII(OS, TypeColor).getOS());
      PrintWithColorRAII(OS, IdentifierColor) << '.' << P->getName();
      if (P->hasSubPattern()) {
        OS << '\n';
        printRec(P->getSubPattern());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitOptionalSomePattern(OptionalSomePattern *P) {
      printCommon(P, "pattern_optional_some");
      OS << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitBoolPattern(BoolPattern *P) {
      printCommon(P, "pattern_bool");
      OS << (P->getValue() ? " true)" : " false)");
    }

  };

  /// PrintDecl - Visitor implementation of Decl::print.
  class PrintDecl : public DeclVisitor<PrintDecl> {
  public:
    raw_ostream &OS;
    unsigned Indent;

    explicit PrintDecl(raw_ostream &os, unsigned indent = 0)
      : OS(os), Indent(indent) { }

  private:
    void printRec(Decl *D) { PrintDecl(OS, Indent + 2).visit(D); }
    void printRec(Expr *E) { E->dump(OS, Indent+2); }
    void printRec(Stmt *S, const ASTContext &Ctx) { S->dump(OS, &Ctx, Indent+2); }
    void printRec(Pattern *P) { PrintPattern(OS, Indent+2).visit(P); }
    void printRec(TypeRepr *T);

    void printWhereRequirements(
        PointerUnion<const AssociatedTypeDecl *, const GenericContext *> Owner)
        const {
      const auto printWhere = [&](const TrailingWhereClause *Where) {
        if (Where) {
          OS << " where requirements: ";
          Where->print(OS, /*printWhereKeyword*/ false);
        }
      };

      if (const auto GC = Owner.dyn_cast<const GenericContext *>()) {
        printWhere(GC->getTrailingWhereClause());
      } else {
        const auto ATD = Owner.get<const AssociatedTypeDecl *>();
        printWhere(ATD->getTrailingWhereClause());
      }
    }

    // Print a field with a value.
    template<typename T>
    raw_ostream &printField(StringRef name, const T &value) {
      OS << " ";
      PrintWithColorRAII(OS, TypeFieldColor) << name;
      OS << "=" << value;
      return OS;
    }

    void printCommon(Decl *D, const char *Name,
                     TerminalColor Color = DeclColor) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, Color) << Name;

      if (D->isImplicit())
        PrintWithColorRAII(OS, DeclModifierColor) << " implicit";

      if (D->isHoisted())
        PrintWithColorRAII(OS, DeclModifierColor) << " hoisted";

      printSourceRange(OS, D->getSourceRange(), D->getASTContext());

      if (D->TrailingSemiLoc.isValid())
        PrintWithColorRAII(OS, DeclModifierColor) << " trailing_semi";
    }

    void printInherited(ArrayRef<InheritedEntry> Inherited) {
      if (Inherited.empty())
        return;
      OS << " inherits: ";
      interleave(Inherited,
                 [&](InheritedEntry Super) { Super.getType().print(OS); },
                 [&] { OS << ", "; });
    }

  public:
    void visitImportDecl(ImportDecl *ID) {
      printCommon(ID, "import_decl");

      if (ID->isExported())
        OS << " exported";

      if (ID->getImportKind() != ImportKind::Module)
        OS << " kind=" << getImportKindString(ID->getImportKind());

      OS << " '";
      // Check if module aliasing was used for the given imported module; for
      // example, if '-module-alias Foo=Bar' was passed and this module has
      // 'import Foo', its corresponding real module name 'Bar' should be printed.
      ImportPath::Builder scratch;
      ID->getRealImportPath(scratch).print(OS);
      OS << "')";
    }

    void visitExtensionDecl(ExtensionDecl *ED) {
      printCommon(ED, "extension_decl", ExtensionColor);
      OS << ' ';
      if (ED->hasBeenBound())
        ED->getExtendedType().print(OS);
      else
        ED->getExtendedTypeRepr()->print(OS);
      printCommonPost(ED);
    }

    void printDeclName(const ValueDecl *D) {
      if (D->getName()) {
        PrintWithColorRAII(OS, IdentifierColor)
          << '\"' << D->getName() << '\"';
      } else {
        PrintWithColorRAII(OS, IdentifierColor)
          << "'anonname=" << (const void*)D << '\'';
      }
    }

    void visitTypeAliasDecl(TypeAliasDecl *TAD) {
      printCommon(TAD, "typealias");
      PrintWithColorRAII(OS, TypeColor) << " type=";
      if (auto underlying = TAD->getCachedUnderlyingType()) {
        PrintWithColorRAII(OS, TypeColor)
          << "'" << underlying.getString() << "'";
      } else {
        PrintWithColorRAII(OS, TypeColor) << "<<<unresolved>>>";
      }
      printWhereRequirements(TAD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitOpaqueTypeDecl(OpaqueTypeDecl *OTD) {
      printCommon(OTD, "opaque_type");
      OS << " naming_decl=";
      printDeclName(OTD->getNamingDecl());
      PrintWithColorRAII(OS, TypeColor) << " opaque_interface="
        << OTD->getDeclaredInterfaceType().getString();
      OS << " in "
         << OTD->getOpaqueInterfaceGenericSignature()->getAsString();
      if (auto underlyingSubs = OTD->getUniqueUnderlyingTypeSubstitutions()) {
        OS << " underlying:\n";
        SmallPtrSet<const ProtocolConformance *, 4> Dumped;
        dumpSubstitutionMapRec(*underlyingSubs, OS,
                               SubstitutionMap::DumpStyle::Full,
                               Indent + 2, Dumped);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitGenericTypeParamDecl(GenericTypeParamDecl *decl) {
      printCommon(decl, "generic_type_param");
      OS << " depth=" << decl->getDepth() << " index=" << decl->getIndex();
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitAssociatedTypeDecl(AssociatedTypeDecl *decl) {
      printCommon(decl, "associated_type_decl");
      if (auto defaultDef = decl->getDefaultDefinitionType()) {
        OS << " default=";
        defaultDef.print(OS);
      }
      printWhereRequirements(decl);
      if (decl->overriddenDeclsComputed()) {
        OS << " overridden=";
        interleave(decl->getOverriddenDecls(),
                   [&](AssociatedTypeDecl *overridden) {
                     OS << overridden->getProtocol()->getName();
                   }, [&]() {
                     OS << ", ";
                   });
      }

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitProtocolDecl(ProtocolDecl *PD) {
      printCommon(PD, "protocol");

      OS << " requirement signature=";
      if (PD->isRequirementSignatureComputed()) {
        auto requirements = PD->getRequirementSignature().getRequirements();
        OS << GenericSignature::get({PD->getProtocolSelfType()}, requirements)
                ->getAsString();
      } else {
        OS << "<null>";
      }
      printCommonPost(PD);
    }

    void printCommon(ValueDecl *VD, const char *Name,
                     TerminalColor Color = DeclColor) {
      printCommon((Decl*)VD, Name, Color);

      OS << ' ';
      printDeclName(VD);
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD))
        printGenericParameters(OS, AFD->getParsedGenericParams());
      if (auto *GTD = dyn_cast<GenericTypeDecl>(VD))
        printGenericParameters(OS, GTD->getParsedGenericParams());
      if (auto *MD = dyn_cast<MacroDecl>(VD))
        printGenericParameters(OS, MD->getParsedGenericParams());

      if (auto *var = dyn_cast<VarDecl>(VD)) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        if (var->hasInterfaceType())
          var->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        else
          PrintWithColorRAII(OS, TypeColor) << "<null type>";
        PrintWithColorRAII(OS, TypeColor) << "'";
      }

      if (VD->hasInterfaceType()) {
        PrintWithColorRAII(OS, InterfaceTypeColor) << " interface type='";
        VD->getInterfaceType()->print(
            PrintWithColorRAII(OS, InterfaceTypeColor).getOS());
        PrintWithColorRAII(OS, InterfaceTypeColor) << "'";
      }

      if (VD->hasAccess()) {
        PrintWithColorRAII(OS, AccessLevelColor) << " access="
          << getAccessLevelSpelling(VD->getFormalAccess());
      }

      if (VD->overriddenDeclsComputed()) {
        auto overridden = VD->getOverriddenDecls();
        if (!overridden.empty()) {
          PrintWithColorRAII(OS, OverrideColor) << " override=";
          interleave(overridden,
                     [&](ValueDecl *overridden) {
                       overridden->dumpRef(
                                PrintWithColorRAII(OS, OverrideColor).getOS());
                     }, [&]() {
                       OS << ", ";
                     });
        }
      }

      auto VarD = dyn_cast<VarDecl>(VD);
      const auto &attrs = VD->getAttrs();
      if (attrs.hasAttribute<FinalAttr>() && !(VarD && VarD->isLet()))
        OS << " final";
      if (attrs.hasAttribute<ObjCAttr>())
        OS << " @objc";
      if (attrs.hasAttribute<DynamicAttr>())
        OS << " dynamic";
      if (auto *attr = attrs.getAttribute<DynamicReplacementAttr>()) {
        OS << " @_dynamicReplacement(for: \"";
        OS << attr->getReplacedFunctionName();
        OS << "\")";
      }
      switch (VD->getLifetimeAnnotation()) {
      case LifetimeAnnotation::EagerMove:
        OS << " _eagerMove";
        break;
      case LifetimeAnnotation::Lexical:
        OS << " _lexical";
        break;
      case LifetimeAnnotation::None:
        break;
      }
    }

    void printCommon(NominalTypeDecl *NTD, const char *Name,
                     TerminalColor Color = DeclColor) {
      printCommon((ValueDecl *)NTD, Name, Color);

      if (NTD->hasInterfaceType()) {
        if (NTD->isResilient())
          OS << " resilient";
        else
          OS << " non-resilient";
      }
    }

    void printCommonPost(const IterableDeclContext *IDC) {
      switch (IDC->getIterableContextKind()) {
      case IterableDeclContextKind::NominalTypeDecl: {
        const auto NTD = cast<NominalTypeDecl>(IDC);
        printInherited(NTD->getInherited());
        printWhereRequirements(NTD);
        break;
      }
      case IterableDeclContextKind::ExtensionDecl:
        const auto ED = cast<ExtensionDecl>(IDC);
        printInherited(ED->getInherited());
        printWhereRequirements(ED);
        break;
      }

      for (Decl *D : IDC->getMembers()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSourceFile(const SourceFile &SF) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ASTNodeColor) << "source_file ";
      PrintWithColorRAII(OS, LocationColor) << '\"' << SF.getFilename() << '\"';

      if (auto items = SF.getCachedTopLevelItems()) {
        for (auto item : *items) {
          if (item.isImplicit())
            continue;

          OS << '\n';

          if (auto decl = item.dyn_cast<Decl *>()) {
            printRec(decl);
          } else if (auto stmt = item.dyn_cast<Stmt *>()) {
            stmt->dump(OS, &SF.getASTContext(), Indent + 2);
          } else {
            auto expr = item.get<Expr *>();
            expr->dump(OS, Indent + 2);
          }
        }
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitVarDecl(VarDecl *VD) {
      printCommon(VD, "var_decl");
      if (VD->isDistributed())
        PrintWithColorRAII(OS, DeclModifierColor) << " distributed";
      if (VD->isLet())
        PrintWithColorRAII(OS, DeclModifierColor) << " let";
      if (VD->getAttrs().hasAttribute<LazyAttr>())
        PrintWithColorRAII(OS, DeclModifierColor) << " lazy";
      printStorageImpl(VD);
      printAccessors(VD);
      if (VD->getAttrs().hasAttribute<KnownToBeLocalAttr>()) {
        OS << " known-to-be-local";
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printStorageImpl(AbstractStorageDecl *D) {
      if (D->isStatic())
        PrintWithColorRAII(OS, DeclModifierColor) << " type";

      if (D->hasInterfaceType()) {
        auto impl = D->getImplInfo();
        PrintWithColorRAII(OS, DeclModifierColor)
          << " readImpl="
          << getReadImplKindName(impl.getReadImpl());
        if (!impl.supportsMutation()) {
          PrintWithColorRAII(OS, DeclModifierColor)
            << " immutable";
        } else {
          PrintWithColorRAII(OS, DeclModifierColor)
            << " writeImpl="
            << getWriteImplKindName(impl.getWriteImpl());
          PrintWithColorRAII(OS, DeclModifierColor)
            << " readWriteImpl="
            << getReadWriteImplKindName(impl.getReadWriteImpl());
        }
      }
    }

    void printAccessors(AbstractStorageDecl *D) {
      for (auto accessor : D->getAllAccessors()) {
        OS << "\n";
        printRec(accessor);
      }
    }

    void visitParamDecl(ParamDecl *PD) {
      printParameter(PD);
    }

    void visitEnumCaseDecl(EnumCaseDecl *ECD) {
      printCommon(ECD, "enum_case_decl");
      for (EnumElementDecl *D : ECD->getElements()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitEnumDecl(EnumDecl *ED) {
      printCommon(ED, "enum_decl");
      printCommonPost(ED);
    }

    void visitEnumElementDecl(EnumElementDecl *EED) {
      printCommon(EED, "enum_element_decl");
      if (auto *paramList = EED->getParameterList()) {
        Indent += 2;
        OS << "\n";
        printParameterList(paramList);
        Indent -= 2;
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitStructDecl(StructDecl *SD) {
      printCommon(SD, "struct_decl");
      printCommonPost(SD);
    }

    void visitClassDecl(ClassDecl *CD) {
      printCommon(CD, "class_decl");
      if (CD->isExplicitActor()) {
        OS << " actor";
      } else if (CD->isExplicitDistributedActor()) {
        OS << " distributed actor";
      }
      if (CD->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>())
        OS << " @_staticInitializeObjCMetadata";
      printCommonPost(CD);
    }

    void visitBuiltinTupleDecl(BuiltinTupleDecl *BTD) {
      printCommon(BTD, "builtin_tuple_decl");
      printCommonPost(BTD);
    }

    void visitPatternBindingDecl(PatternBindingDecl *PBD) {
      printCommon(PBD, "pattern_binding_decl");

      for (auto idx : range(PBD->getNumPatternEntries())) {
        OS << '\n';
        printRec(PBD->getPattern(idx));
        if (PBD->getOriginalInit(idx)) {
          OS << '\n';
          OS.indent(Indent + 2);
          OS << "Original init:\n";
          printRec(PBD->getOriginalInit(idx));
        }
        if (PBD->getInit(idx)) {
          OS << '\n';
          OS.indent(Indent + 2);
          OS << "Processed init:\n";
          printRec(PBD->getInit(idx));
        }
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSubscriptDecl(SubscriptDecl *SD) {
      printCommon(SD, "subscript_decl");
      printStorageImpl(SD);
      printAccessors(SD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printCommonAFD(AbstractFunctionDecl *D, const char *Type) {
      printCommon(D, Type, FuncColor);
      if (!D->getCaptureInfo().isTrivial()) {
        OS << " ";
        D->getCaptureInfo().print(OS);
      }

      if (D->getAttrs().hasAttribute<NonisolatedAttr>()) {
        PrintWithColorRAII(OS, ExprModifierColor) << " nonisolated";
      }
      if (D->isDistributed()) {
        PrintWithColorRAII(OS, ExprModifierColor) << " distributed";
      }
      if (D->isDistributedThunk()) {
        PrintWithColorRAII(OS, ExprModifierColor) << " distributed-thunk";
      }

      if (auto fac = D->getForeignAsyncConvention()) {
        OS << " foreign_async=";
        if (auto type = fac->completionHandlerType())
          type.print(OS);
        OS << ",completion_handler_param="
           << fac->completionHandlerParamIndex();
        if (auto errorParamIndex = fac->completionHandlerErrorParamIndex())
          OS << ",error_param=" << *errorParamIndex;
      }

      if (auto fec = D->getForeignErrorConvention()) {
        OS << " foreign_error=";
        OS << getForeignErrorConventionKindString(fec->getKind());
        bool wantResultType = (
          fec->getKind() == ForeignErrorConvention::ZeroResult ||
          fec->getKind() == ForeignErrorConvention::NonZeroResult);

        OS << ((fec->isErrorOwned() == ForeignErrorConvention::IsOwned)
                ? ",owned"
                : ",unowned");
        OS << ",param=" << llvm::utostr(fec->getErrorParameterIndex());
        OS << ",paramtype=" << fec->getErrorParameterType().getString();
        if (wantResultType)
          OS << ",resulttype=" << fec->getResultType().getString();
      }
    }

    void printParameter(const ParamDecl *P) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ParameterColor) << "parameter ";
      if (P->getAttrs().hasAttribute<KnownToBeLocalAttr>()) {
        OS << "known-to-be-local ";
      }
      printDeclName(P);
      if (!P->getArgumentName().empty())
        PrintWithColorRAII(OS, IdentifierColor)
          << " apiName=" << P->getArgumentName();

      if (P->hasInterfaceType()) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        P->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        PrintWithColorRAII(OS, TypeColor) << "'";

        PrintWithColorRAII(OS, InterfaceTypeColor) << " interface type='";
        P->getInterfaceType().print(
            PrintWithColorRAII(OS, InterfaceTypeColor).getOS());
        PrintWithColorRAII(OS, InterfaceTypeColor) << "'";
      }

      if (auto specifier = P->getCachedSpecifier()) {
        if (*specifier != ParamDecl::Specifier::Default) {
          OS << ' ' << ParamDecl::getSpecifierSpelling(*specifier);
        }
      }

      if (P->hasInterfaceType())
        if (P->isVariadic())
          OS << " variadic";

      if (P->isAutoClosure())
        OS << " autoclosure";

      if (P->getAttrs().hasAttribute<NonEphemeralAttr>())
        OS << " nonEphemeral";

      switch (P->getLifetimeAnnotationFromAttributes()) {
      case LifetimeAnnotation::EagerMove:
        OS << " _eagerMove";
        break;
      case LifetimeAnnotation::Lexical:
        OS << " _lexical";
        break;
      case LifetimeAnnotation::None:
        break;
      }

      if (P->isNoImplicitCopy())
        OS << " noImplicitCopy";

      if (P->getDefaultArgumentKind() != DefaultArgumentKind::None) {
        printField("default_arg",
                   getDefaultArgumentKindString(P->getDefaultArgumentKind()));
      }

      if (P->hasDefaultExpr() &&
          !P->getDefaultArgumentCaptureInfo().isTrivial()) {
        OS << " ";
        P->getDefaultArgumentCaptureInfo().print(
          PrintWithColorRAII(OS, CapturesColor).getOS());
      }

      if (auto init = P->getStructuralDefaultExpr()) {
        OS << " expression=\n";
        printRec(init);
      }

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printParameterList(const ParameterList *params, const ASTContext *ctx = nullptr) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ParameterColor) << "parameter_list";

      if (!ctx && params->size() != 0 && params->get(0))
        ctx = &params->get(0)->getASTContext();

      if (ctx) {
        printSourceRange(OS, params->getSourceRange(), *ctx);
      }

      Indent += 2;
      for (auto P : *params) {
        OS << '\n';
        printParameter(P);
      }
      Indent -= 2;

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printAbstractFunctionDecl(AbstractFunctionDecl *D) {
      Indent += 2;
      if (auto *P = D->getImplicitSelfDecl()) {
        OS << '\n';
        printParameter(P);
      }

      OS << '\n';
      printParameterList(D->getParameters(), &D->getASTContext());
      Indent -= 2;

      if (auto FD = dyn_cast<FuncDecl>(D)) {
        if (FD->getResultTypeRepr()) {
          OS << '\n';
          Indent += 2;
          OS.indent(Indent);
          PrintWithColorRAII(OS, ParenthesisColor) << '(';
          OS << "result\n";
          printRec(FD->getResultTypeRepr());
          PrintWithColorRAII(OS, ParenthesisColor) << ')';
          if (auto opaque = FD->getOpaqueResultTypeDecl()) {
            OS << '\n';
            OS.indent(Indent);
            PrintWithColorRAII(OS, ParenthesisColor) << '(';
            OS << "opaque_result_decl\n";
            printRec(opaque);
            PrintWithColorRAII(OS, ParenthesisColor) << ')';
          }
          Indent -= 2;
        }
      }

      if (D->hasSingleExpressionBody()) {
        // There won't be an expression if this is an initializer that was
        // originally spelled "init?(...) { nil }", because "nil" is modeled
        // via FailStmt in this context.
        if (auto *Body = D->getSingleExpressionBody()) {
          OS << '\n';
          printRec(Body);

          return;
        }
      }

      if (auto Body = D->getBody(/*canSynthesize=*/false)) {
        OS << '\n';
        printRec(Body, D->getASTContext());
      }
    }

    void printCommonFD(FuncDecl *FD, const char *type) {
      printCommonAFD(FD, type);
      if (FD->isStatic())
        OS << " type";
    }

    void visitFuncDecl(FuncDecl *FD) {
      printCommonFD(FD, "func_decl");
      printAbstractFunctionDecl(FD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitAccessorDecl(AccessorDecl *AD) {
      printCommonFD(AD, "accessor_decl");
      OS << " " << getAccessorKindString(AD->getAccessorKind());
      OS << "_for=" << AD->getStorage()->getName();
      printAbstractFunctionDecl(AD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitConstructorDecl(ConstructorDecl *CD) {
      printCommonAFD(CD, "constructor_decl");
      if (CD->isRequired())
        PrintWithColorRAII(OS, DeclModifierColor) << " required";
      PrintWithColorRAII(OS, DeclModifierColor) << " "
        << getCtorInitializerKindString(CD->getInitKind());
      if (CD->isFailable())
        PrintWithColorRAII(OS, DeclModifierColor) << " failable="
          << (CD->isImplicitlyUnwrappedOptional()
              ? "ImplicitlyUnwrappedOptional"
              : "Optional");
      printAbstractFunctionDecl(CD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDestructorDecl(DestructorDecl *DD) {
      printCommonAFD(DD, "destructor_decl");
      printAbstractFunctionDecl(DD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
      printCommon(TLCD, "top_level_code_decl");
      if (TLCD->getBody()) {
        OS << "\n";
        printRec(TLCD->getBody(), static_cast<Decl *>(TLCD)->getASTContext());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    
    void printASTNodes(const ArrayRef<ASTNode> &Elements, const ASTContext &Ctx, StringRef Name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << "(";
      PrintWithColorRAII(OS, ASTNodeColor) << Name;
      for (auto Elt : Elements) {
        OS << '\n';
        if (auto *SubExpr = Elt.dyn_cast<Expr*>())
          printRec(SubExpr);
        else if (auto *SubStmt = Elt.dyn_cast<Stmt*>())
          printRec(SubStmt, Ctx);
        else
          printRec(Elt.get<Decl*>());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitIfConfigDecl(IfConfigDecl *ICD) {
      printCommon(ICD, "if_config_decl");
      Indent += 2;
      for (auto &Clause : ICD->getClauses()) {
        OS << '\n';
        OS.indent(Indent);
        PrintWithColorRAII(OS, StmtColor) << (Clause.Cond ? "#if:" : "#else:");
        if (Clause.isActive)
          PrintWithColorRAII(OS, DeclModifierColor) << " active";
        if (Clause.Cond) {
          OS << "\n";
          printRec(Clause.Cond);
        }

        OS << '\n';
        Indent += 2;
        printASTNodes(Clause.Elements, ICD->getASTContext(), "elements");
        Indent -= 2;
      }

      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPoundDiagnosticDecl(PoundDiagnosticDecl *PDD) {
      printCommon(PDD, "pound_diagnostic_decl");
      auto kind = PDD->isError() ? "error" : "warning";
      OS << " kind=" << kind << "\n";
      Indent += 2;
      printRec(PDD->getMessage());
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD) {
      printCommon(PGD, "precedence_group_decl ");
      OS << PGD->getName() << "\n";

      OS.indent(Indent+2);
      OS << "associativity "
         << getAssociativityString(PGD->getAssociativity()) << "\n";

      OS.indent(Indent+2);
      OS << "assignment " << (PGD->isAssignment() ? "true" : "false");

      auto printRelations =
          [&](StringRef label, ArrayRef<PrecedenceGroupDecl::Relation> rels) {
        if (rels.empty()) return;
        OS << '\n';
        OS.indent(Indent+2);
        OS << label << ' ' << rels[0].Name;
        for (auto &rel : rels.slice(1))
          OS << ", " << rel.Name;
      };
      printRelations("higherThan", PGD->getHigherThan());
      printRelations("lowerThan", PGD->getLowerThan());

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitInfixOperatorDecl(InfixOperatorDecl *IOD) {
      printCommon(IOD, "infix_operator_decl");
      OS << " " << IOD->getName();
      if (!IOD->getPrecedenceGroupName().empty())
        OS << " precedence_group_name=" << IOD->getPrecedenceGroupName();
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPrefixOperatorDecl(PrefixOperatorDecl *POD) {
      printCommon(POD, "prefix_operator_decl");
      OS << " " << POD->getName();
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPostfixOperatorDecl(PostfixOperatorDecl *POD) {
      printCommon(POD, "postfix_operator_decl");
      OS << " " << POD->getName();
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitModuleDecl(ModuleDecl *MD) {
      printCommon(MD, "module");

      if (MD->isNonSwiftModule())
        OS << " non_swift";
      
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitMissingDecl(MissingDecl *missing) {
      printCommon(missing, "missing_decl");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitMissingMemberDecl(MissingMemberDecl *MMD) {
      printCommon(MMD, "missing_member_decl ");
      PrintWithColorRAII(OS, IdentifierColor)
          << '\"' << MMD->getName() << '\"';
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitMacroDecl(MacroDecl *MD) {
      printCommon(MD, "macro_decl");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitMacroExpansionDecl(MacroExpansionDecl *MED) {
      printCommon(MED, "macro_expansion_decl ");
      OS << MED->getMacroName();
      OS << '\n';
      printArgumentList(OS, MED->getArgs(), Indent,
                        [&](Expr *E) { printRec(E); });
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
  };
} // end anonymous namespace

void ParameterList::dump() const {
  dump(llvm::errs(), 0);
}

void ParameterList::dump(raw_ostream &OS, unsigned Indent) const {
  PrintDecl(OS, Indent).printParameterList(this);
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
  PrintDecl(OS, Indent).visit(const_cast<Decl *>(this));
  OS << '\n';
}

/// Print the given declaration context (with its parents).
void swift::printContext(raw_ostream &os, DeclContext *dc) {
  if (auto parent = dc->getParent()) {
    printContext(os, parent);
    os << '.';
  }

  switch (dc->getContextKind()) {
  case DeclContextKind::Package:
    printName(os, cast<PackageUnit>(dc)->getName());
    break;

  case DeclContextKind::Module:
    printName(os, cast<ModuleDecl>(dc)->getRealName());
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

    // If we aren't printing to standard error or the debugger output stream,
    // this client expects to see the computed discriminator. Compute it now.
    if (&os != &llvm::errs() && &os != &llvm::dbgs())
      (void)ACE->getDiscriminator();

    PrintWithColorRAII(os, DiscriminatorColor) << ACE->getRawDiscriminator();
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
    case InitializerKind::RuntimeAttribute:
      os << "runtime attribute initializer";
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

  case DeclContextKind::MacroDecl:
    printName(os, cast<MacroDecl>(dc)->getName());
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
  if (!isa<ModuleDecl>(this)) {
    // Print the context.
    printContext(os, getDeclContext());
    os << ".";
    // Print name.
    getName().printPretty(os);
  } else {
    auto moduleName = cast<ModuleDecl>(this)->getRealName();
    os << moduleName;
  }

  if (getAttrs().hasAttribute<KnownToBeLocalAttr>()) {
    os << " known-to-be-local";
  }

  // Print location.
  auto &srcMgr = getASTContext().SourceMgr;
  if (getLoc().isValid()) {
    os << '@';
    getLoc().print(os, srcMgr);
  }
}

void LLVM_ATTRIBUTE_USED ValueDecl::dumpRef() const {
  dumpRef(llvm::errs());
  llvm::errs() << "\n";
}

void SourceFile::dump() const {
  dump(llvm::errs());
}

void SourceFile::dump(llvm::raw_ostream &OS, bool parseIfNeeded) const {
  // If we're allowed to parse the SourceFile, do so now. We need to force the
  // parsing request as by default the dumping logic tries not to kick any
  // requests.
  if (parseIfNeeded)
    (void)getTopLevelItems();

  PrintDecl(OS).visitSourceFile(*this);
  llvm::errs() << '\n';
}

void Pattern::dump() const {
  dump(llvm::errs());
}

void Pattern::dump(raw_ostream &OS, unsigned Indent) const {
  PrintPattern(OS, Indent).visit(const_cast<Pattern*>(this));
  OS << '\n';
}

//===----------------------------------------------------------------------===//
// Printing for Stmt and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintStmt - Visitor implementation of Stmt::dump.
class PrintStmt : public StmtVisitor<PrintStmt> {
public:
  raw_ostream &OS;
  const ASTContext *Ctx;
  unsigned Indent;

  PrintStmt(raw_ostream &os, const ASTContext *ctx, unsigned indent)
    : OS(os), Ctx(ctx), Indent(indent) {
  }

  void printRec(Stmt *S) {
    Indent += 2;
    if (S)
      visit(S);
    else
      OS.indent(Indent) << "(**NULL STATEMENT**)";
    Indent -= 2;
  }

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Expr *E) { E->dump(OS, Indent + 2); }
  void printRec(const Pattern *P) {
    PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
  }

  void printRec(StmtConditionElement C) {
    switch (C.getKind()) {
    case StmtConditionElement::CK_Boolean:
      return printRec(C.getBoolean());
    case StmtConditionElement::CK_PatternBinding:
      Indent += 2;
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, PatternColor) << "pattern\n";

      printRec(C.getPattern());
      OS << "\n";
      printRec(C.getInitializer());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
      Indent -= 2;
      break;
    case StmtConditionElement::CK_Availability:
      Indent += 2;
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      OS << "#available\n";
      for (auto *Query : C.getAvailability()->getQueries()) {
        OS << '\n';
        switch (Query->getKind()) {
        case AvailabilitySpecKind::PlatformVersionConstraint:
          cast<PlatformVersionConstraintAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        case AvailabilitySpecKind::LanguageVersionConstraint:
        case AvailabilitySpecKind::PackageDescriptionVersionConstraint:
          cast<PlatformVersionConstraintAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        case AvailabilitySpecKind::OtherPlatform:
          cast<OtherPlatformAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        }
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ")";
      Indent -= 2;
      break;
    case StmtConditionElement::CK_HasSymbol:
      Indent += 2;
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      OS << "#_hasSymbol";
      if (Ctx)
        printSourceRange(OS, C.getSourceRange(), *Ctx);
      OS << "\n";
      printRec(C.getHasSymbolInfo()->getSymbolExpr());
      PrintWithColorRAII(OS, ParenthesisColor) << ")";
      Indent -= 2;
      break;
    }
  }

  raw_ostream &printCommon(Stmt *S, const char *Name) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << Name;

    if (S->isImplicit())
      OS << " implicit";

    if (Ctx)
      printSourceRange(OS, S->getSourceRange(), *Ctx);

    if (S->TrailingSemiLoc.isValid())
      OS << " trailing_semi";

    return OS;
  }

  void visitBraceStmt(BraceStmt *S) {
    printCommon(S, "brace_stmt");
    printASTNodes(S->getElements());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printASTNodes(const ArrayRef<ASTNode> &Elements) {
    for (auto Elt : Elements) {
      OS << '\n';
      if (auto *SubExpr = Elt.dyn_cast<Expr*>())
        printRec(SubExpr);
      else if (auto *SubStmt = Elt.dyn_cast<Stmt*>())
        printRec(SubStmt);
      else
        printRec(Elt.get<Decl*>());
    }
  }

  void visitReturnStmt(ReturnStmt *S) {
    printCommon(S, "return_stmt");
    if (S->hasResult()) {
      OS << '\n';
      printRec(S->getResult());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitYieldStmt(YieldStmt *S) {
    printCommon(S, "yield_stmt");
    for (auto yield : S->getYields()) {
      OS << '\n';
      printRec(yield);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDeferStmt(DeferStmt *S) {
    printCommon(S, "defer_stmt") << '\n';
    printRec(S->getTempDecl());
    OS << '\n';
    printRec(S->getCallExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitIfStmt(IfStmt *S) {
    printCommon(S, "if_stmt") << '\n';
    for (auto elt : S->getCond()) {
      printRec(elt);
      OS << "\n";
    }
    printRec(S->getThenStmt());
    if (S->getElseStmt()) {
      OS << '\n';
      printRec(S->getElseStmt());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitGuardStmt(GuardStmt *S) {
    printCommon(S, "guard_stmt") << '\n';
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDoStmt(DoStmt *S) {
    printCommon(S, "do_stmt") << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitWhileStmt(WhileStmt *S) {
    printCommon(S, "while_stmt") << '\n';
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitRepeatWhileStmt(RepeatWhileStmt *S) {
    printCommon(S, "repeat_while_stmt") << '\n';
    printRec(S->getBody());
    OS << '\n';
    printRec(S->getCond());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForEachStmt(ForEachStmt *S) {
    printCommon(S, "for_each_stmt");
    OS << '\n';
    printRec(S->getPattern());
    OS << '\n';
    if (S->getWhere()) {
      Indent += 2;
      OS.indent(Indent) << "(where\n";
      printRec(S->getWhere());
      OS << ")\n";
      Indent -= 2;
    }
    printRec(S->getParsedSequence());
    OS << '\n';
    if (S->getIteratorVar()) {
      printRec(S->getIteratorVar());
      OS << '\n';
    }
    if (S->getNextCall()) {
      printRec(S->getNextCall());
      OS << '\n';
    }
    if (S->getConvertElementExpr()) {
      printRec(S->getConvertElementExpr());
      OS << '\n';
    }
    if (S->getElementExpr()) {
      printRec(S->getElementExpr());
      OS << '\n';
    }
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBreakStmt(BreakStmt *S) {
    printCommon(S, "break_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitContinueStmt(ContinueStmt *S) {
    printCommon(S, "continue_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFallthroughStmt(FallthroughStmt *S) {
    printCommon(S, "fallthrough_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSwitchStmt(SwitchStmt *S) {
    printCommon(S, "switch_stmt") << '\n';
    printRec(S->getSubjectExpr());
    for (auto N : S->getRawCases()) {
      OS << '\n';
      if (N.is<Stmt*>())
        printRec(N.get<Stmt*>());
      else
        printRec(N.get<Decl*>());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCaseStmt(CaseStmt *S) {
    printCommon(S, "case_stmt");
    if (S->hasUnknownAttr())
      OS << " @unknown";

    if (S->hasCaseBodyVariables()) {
      OS << '\n';
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, StmtColor) << "case_body_variables";
      OS << '\n';
      for (auto *vd : S->getCaseBodyVariables()) {
        OS.indent(2);
        // TODO: Printing a var decl does an Indent ... dump(vd) ... '\n'. We
        // should see if we can factor this dumping so that the caller of
        // printRec(VarDecl) has more control over the printing.
        printRec(vd);
      }
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    for (const auto &LabelItem : S->getCaseLabelItems()) {
      OS << '\n';
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, StmtColor) << "case_label_item";
      if (LabelItem.isDefault())
        OS << " default";
      if (auto *CasePattern = LabelItem.getPattern()) {
        OS << '\n';
        printRec(CasePattern);
      }
      if (auto *Guard = LabelItem.getGuardExpr()) {
        OS << '\n';
        Guard->dump(OS, Indent+4);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFailStmt(FailStmt *S) {
    printCommon(S, "fail_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitThrowStmt(ThrowStmt *S) {
    printCommon(S, "throw_stmt") << '\n';
    printRec(S->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDiscardStmt(DiscardStmt *S) {
    printCommon(S, "discard_stmt") << '\n';
    printRec(S->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPoundAssertStmt(PoundAssertStmt *S) {
    printCommon(S, "pound_assert");
    OS << " message=" << QuotedString(S->getMessage()) << "\n";
    printRec(S->getCondition());
    OS << ")";
  }

  void visitDoCatchStmt(DoCatchStmt *S) {
    printCommon(S, "do_catch_stmt") << '\n';
    printRec(S->getBody());
    OS << '\n';
    Indent += 2;
    visitCatches(S->getCatches());
    Indent -= 2;
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCatches(ArrayRef<CaseStmt *> clauses) {
    for (auto clause : clauses) {
      visitCaseStmt(clause);
    }
  }
};

} // end anonymous namespace

void Stmt::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}

void Stmt::dump(raw_ostream &OS, const ASTContext *Ctx, unsigned Indent) const {
  PrintStmt(OS, Ctx, Indent).visit(const_cast<Stmt*>(this));
}

//===----------------------------------------------------------------------===//
// Printing for Expr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintExpr - Visitor implementation of Expr::dump.
class PrintExpr : public ExprVisitor<PrintExpr> {
public:
  raw_ostream &OS;
  llvm::function_ref<Type(Expr *)> GetTypeOfExpr;
  llvm::function_ref<Type(TypeRepr *)> GetTypeOfTypeRepr;
  llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
      GetTypeOfKeyPathComponent;
  unsigned Indent;

  PrintExpr(raw_ostream &os, llvm::function_ref<Type(Expr *)> getTypeOfExpr,
            llvm::function_ref<Type(TypeRepr *)> getTypeOfTypeRepr,
            llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
                getTypeOfKeyPathComponent,
            unsigned indent)
      : OS(os), GetTypeOfExpr(getTypeOfExpr),
        GetTypeOfTypeRepr(getTypeOfTypeRepr),
        GetTypeOfKeyPathComponent(getTypeOfKeyPathComponent), Indent(indent) {}

  void printRec(Expr *E) {
    Indent += 2;
    if (E)
      visit(E);
    else
      OS.indent(Indent) << "(**NULL EXPRESSION**)";
    Indent -= 2;
  }

  void printRecLabeled(Expr *E, StringRef label) {
    Indent += 2;
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, ExprColor) << label;
    OS << '\n';
    printRec(E);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;
  }

  /// FIXME: This should use ExprWalker to print children.

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Stmt *S, const ASTContext &Ctx) { S->dump(OS, &Ctx, Indent + 2); }
  void printRec(const Pattern *P) {
    PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
  }
  void printRec(TypeRepr *T);
  void printRec(ProtocolConformanceRef conf) {
    conf.dump(OS, Indent + 2);
  }

  void printDeclRef(ConcreteDeclRef declRef) {
    declRef.dump(PrintWithColorRAII(OS, DeclColor).getOS());
  }

  raw_ostream &printCommon(Expr *E, const char *C) {
    PrintOptions PO;
    PO.PrintTypesForDebugging = true;

    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, ExprColor) << C;

    if (E->isImplicit())
      PrintWithColorRAII(OS, ExprModifierColor) << " implicit";
    PrintWithColorRAII(OS, TypeColor) << " type='";
    PrintWithColorRAII(OS, TypeColor) << GetTypeOfExpr(E).getString(PO) << '\'';

    // If we have a source range and an ASTContext, print the source range.
    if (auto Ty = GetTypeOfExpr(E)) {
      auto &Ctx = Ty->getASTContext();
      auto L = E->getLoc();
      if (L.isValid()) {
        PrintWithColorRAII(OS, LocationColor) << " location=";
        L.print(PrintWithColorRAII(OS, LocationColor).getOS(), Ctx.SourceMgr);
      }

      printSourceRange(OS, E->getSourceRange(), Ctx);
    }

    if (E->TrailingSemiLoc.isValid())
      OS << " trailing_semi";

    return OS;
  }
  
  void printSemanticExpr(Expr * semanticExpr) {
    if (semanticExpr == nullptr) {
      return;
    }
    
    OS << '\n';
    printRecLabeled(semanticExpr, "semantic_expr");
  }

  void visitErrorExpr(ErrorExpr *E) {
    printCommon(E, "error_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCodeCompletionExpr(CodeCompletionExpr *E) {
    printCommon(E, "code_completion_expr");
    if (E->getBase()) {
      OS << '\n';
      printRec(E->getBase());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitNilLiteralExpr(NilLiteralExpr *E) {
    printCommon(E, "nil_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    printCommon(E, "integer_literal_expr");
    if (E->isNegative())
      PrintWithColorRAII(OS, LiteralValueColor) << " negative";
    PrintWithColorRAII(OS, LiteralValueColor) << " value=";
    Type T = GetTypeOfExpr(E);
    if (T.isNull() || !T->is<BuiltinIntegerType>())
      PrintWithColorRAII(OS, LiteralValueColor) << E->getDigitsText();
    else
      PrintWithColorRAII(OS, LiteralValueColor) << E->getValue();
    PrintWithColorRAII(OS, LiteralValueColor) << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
        PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFloatLiteralExpr(FloatLiteralExpr *E) {
    printCommon(E, "float_literal_expr");
    if (E->isNegative())
      PrintWithColorRAII(OS, LiteralValueColor) << " negative";
    PrintWithColorRAII(OS, LiteralValueColor)
      << " value=" << E->getDigitsText();
    PrintWithColorRAII(OS, LiteralValueColor) << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
        PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    if (!E->getBuiltinType().isNull()) {
      PrintWithColorRAII(OS, TypeColor) << " builtin_type='";
      E->getBuiltinType().print(PrintWithColorRAII(OS, TypeColor).getOS());
      PrintWithColorRAII(OS, TypeColor) << "'";
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitBooleanLiteralExpr(BooleanLiteralExpr *E) {
    printCommon(E, "boolean_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor)
      << " value=" << (E->getValue() ? "true" : "false")
      << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
      PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitStringLiteralExpr(StringLiteralExpr *E) {
    printCommon(E, "string_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " encoding="
      << getStringLiteralExprEncodingString(E->getEncoding())
      << " value=" << QuotedString(E->getValue())
      << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
      PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E) {
    printCommon(E, "interpolated_string_literal_expr");
    
    // Print the trailing quote location
    if (auto Ty = GetTypeOfExpr(E)) {
      auto &Ctx = Ty->getASTContext();
      auto TQL = E->getTrailingQuoteLoc();
      if (TQL.isValid()) {
        PrintWithColorRAII(OS, LocationColor) << " trailing_quote_loc=";
        TQL.print(PrintWithColorRAII(OS, LocationColor).getOS(),
                  Ctx.SourceMgr);
      }
    }
    PrintWithColorRAII(OS, LiteralValueColor)
      << " literal_capacity="
      << E->getLiteralCapacity() << " interpolation_count="
      << E->getInterpolationCount();
    PrintWithColorRAII(OS, LiteralValueColor) << " builder_init=";
    E->getBuilderInit().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " result_init=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    OS << "\n";
    printRec(E->getAppendingExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *E) {
    printCommon(E, "magic_identifier_literal_expr")
      << " kind=" << MagicIdentifierLiteralExpr::getKindString(E->getKind());

    if (E->isString()) {
      OS << " encoding="
         << getStringLiteralExprEncodingString(E->getStringEncoding());
    }
    OS << " builtin_initializer=";
    E->getBuiltinInitializer().dump(OS);
    OS << " initializer=";
    E->getInitializer().dump(OS);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitRegexLiteralExpr(RegexLiteralExpr *E) {
    printCommon(E, "regex_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor)
        << " text=" << QuotedString(E->getRegexText())
        << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitObjectLiteralExpr(ObjectLiteralExpr *E) {
    printCommon(E, "object_literal") 
      << " kind='" << E->getLiteralKindPlainName() << "'";
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    OS << "\n";
    printArgumentList(E->getArgs());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDiscardAssignmentExpr(DiscardAssignmentExpr *E) {
    printCommon(E, "discard_assignment_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDeclRefExpr(DeclRefExpr *E) {
    printCommon(E, "declref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getDeclRef());
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      PrintWithColorRAII(OS, AccessLevelColor)
        << " " << getAccessSemanticsString(E->getAccessSemantics());
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSuperRefExpr(SuperRefExpr *E) {
    printCommon(E, "super_ref_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTypeExpr(TypeExpr *E) {
    printCommon(E, "type_expr");
    PrintWithColorRAII(OS, TypeReprColor) << " typerepr='";
    if (E->getTypeRepr())
      E->getTypeRepr()->print(PrintWithColorRAII(OS, TypeReprColor).getOS());
    else
      PrintWithColorRAII(OS, TypeReprColor) << "<<NULL>>";
    PrintWithColorRAII(OS, TypeReprColor) << "'";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E) {
    printCommon(E, "other_constructor_ref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getDeclRef());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *E) {
    printCommon(E, "overloaded_decl_ref_expr");
    PrintWithColorRAII(OS, IdentifierColor) << " name="
      << E->getDecls()[0]->getBaseName();
    PrintWithColorRAII(OS, ExprModifierColor)
      << " number_of_decls=" << E->getDecls().size()
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E) {
    printCommon(E, "unresolved_decl_ref_expr");
    PrintWithColorRAII(OS, IdentifierColor) << " name=" << E->getName();
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *E) {
    printCommon(E, "unresolved_specialize_expr") << '\n';
    printRec(E->getSubExpr());
    for (TypeLoc T : E->getUnresolvedParams()) {
      OS << '\n';
      printRec(T.getTypeRepr());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMemberRefExpr(MemberRefExpr *E) {
    printCommon(E, "member_ref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getMember());
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      PrintWithColorRAII(OS, AccessLevelColor)
        << " " << getAccessSemanticsString(E->getAccessSemantics());
    if (E->isSuper())
      OS << " super";

    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDynamicMemberRefExpr(DynamicMemberRefExpr *E) {
    printCommon(E, "dynamic_member_ref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    E->getMember().dump(OS);
    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedMemberExpr(UnresolvedMemberExpr *E) {
    printCommon(E, "unresolved_member_expr")
      << " name='" << E->getName() << "'";
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDotSelfExpr(DotSelfExpr *E) {
    printCommon(E, "dot_self_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitParenExpr(ParenExpr *E) {
    printCommon(E, "paren_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAwaitExpr(AwaitExpr *E) {
    printCommon(E, "await_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitConsumeExpr(ConsumeExpr *E) {
    printCommon(E, "consume_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCopyExpr(CopyExpr *E) {
    printCommon(E, "copy_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBorrowExpr(BorrowExpr *E) {
    printCommon(E, "borrow_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedMemberChainResultExpr(UnresolvedMemberChainResultExpr *E){
    printCommon(E, "unresolved_member_chain_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTupleExpr(TupleExpr *E) {
    printCommon(E, "tuple_expr");

    if (E->hasElementNames()) {
      PrintWithColorRAII(OS, IdentifierColor) << " names=";

      interleave(E->getElementNames(),
                 [&](Identifier name) {
                   PrintWithColorRAII(OS, IdentifierColor)
                     << (name.empty()?"''":name.str());
                 },
                 [&] { PrintWithColorRAII(OS, IdentifierColor) << ","; });
    }

    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      if (E->getElement(i))
        printRec(E->getElement(i));
      else
        OS.indent(Indent+2) << "<<tuple element default value>>";
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArrayExpr(ArrayExpr *E) {
    printCommon(E, "array_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    for (auto elt : E->getElements()) {
      OS << '\n';
      printRec(elt);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDictionaryExpr(DictionaryExpr *E) {
    printCommon(E, "dictionary_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    for (auto elt : E->getElements()) {
      OS << '\n';
      printRec(elt);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSubscriptExpr(SubscriptExpr *E) {
    printCommon(E, "subscript_expr");
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      PrintWithColorRAII(OS, AccessLevelColor)
        << " " << getAccessSemanticsString(E->getAccessSemantics());
    if (E->isSuper())
      OS << " super";
    if (E->hasDecl()) {
      PrintWithColorRAII(OS, DeclColor) << " decl=";
      printDeclRef(E->getDecl());
    }
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printArgumentList(E->getArgs());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitKeyPathApplicationExpr(KeyPathApplicationExpr *E) {
    printCommon(E, "keypath_application_expr");
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getKeyPath());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDynamicSubscriptExpr(DynamicSubscriptExpr *E) {
    printCommon(E, "dynamic_subscript_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getMember());
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printArgumentList(E->getArgs());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedDotExpr(UnresolvedDotExpr *E) {
    printCommon(E, "unresolved_dot_expr")
      << " field '" << E->getName() << "'";
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    if (E->getBase()) {
      OS << '\n';
      printRec(E->getBase());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTupleElementExpr(TupleElementExpr *E) {
    printCommon(E, "tuple_element_expr")
      << " field #" << E->getFieldNumber() << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDestructureTupleExpr(DestructureTupleExpr *E) {
    printCommon(E, "destructure_tuple_expr");
    OS << " destructured=";
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    Indent += 2;
    for (auto *elt : E->getDestructuredElements()) {
      OS << "\n";
      printRec(elt);
    }
    Indent -= 2;
    PrintWithColorRAII(OS, ParenthesisColor) << ")\n";
    printRec(E->getSubExpr());
    OS << "\n";
    printRec(E->getResultExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedTypeConversionExpr(UnresolvedTypeConversionExpr *E) {
    printCommon(E, "unresolvedtype_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFunctionConversionExpr(FunctionConversionExpr *E) {
    printCommon(E, "function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCovariantFunctionConversionExpr(CovariantFunctionConversionExpr *E){
    printCommon(E, "covariant_function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCovariantReturnConversionExpr(CovariantReturnConversionExpr *E){
    printCommon(E, "covariant_return_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnderlyingToOpaqueExpr(UnderlyingToOpaqueExpr *E){
    printCommon(E, "underlying_to_opaque_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitErasureExpr(ErasureExpr *E) {
    printCommon(E, "erasure_expr") << '\n';
    for (auto conf : E->getConformances()) {
      printRec(conf);
      OS << '\n';
    }
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAnyHashableErasureExpr(AnyHashableErasureExpr *E) {
    printCommon(E, "any_hashable_erasure_expr") << '\n';
    printRec(E->getConformance());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitConditionalBridgeFromObjCExpr(ConditionalBridgeFromObjCExpr *E) {
    printCommon(E, "conditional_bridge_from_objc_expr") << " conversion=";
    printDeclRef(E->getConversion());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBridgeFromObjCExpr(BridgeFromObjCExpr *E) {
    printCommon(E, "bridge_from_objc_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBridgeToObjCExpr(BridgeToObjCExpr *E) {
    printCommon(E, "bridge_to_objc_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLoadExpr(LoadExpr *E) {
    printCommon(E, "load_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitABISafeConversionExpr(ABISafeConversionExpr *E) {
    printCommon(E, "abi_safe_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMetatypeConversionExpr(MetatypeConversionExpr *E) {
    printCommon(E, "metatype_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCollectionUpcastConversionExpr(CollectionUpcastConversionExpr *E) {
    printCommon(E, "collection_upcast_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    if (auto keyConversion = E->getKeyConversion()) {
      OS << '\n';
      printRecLabeled(keyConversion.Conversion, "key_conversion");
    }
    if (auto valueConversion = E->getValueConversion()) {
      OS << '\n';
      printRecLabeled(valueConversion.Conversion, "value_conversion");
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDerivedToBaseExpr(DerivedToBaseExpr *E) {
    printCommon(E, "derived_to_base_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArchetypeToSuperExpr(ArchetypeToSuperExpr *E) {
    printCommon(E, "archetype_to_super_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInjectIntoOptionalExpr(InjectIntoOptionalExpr *E) {
    printCommon(E, "inject_into_optional") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitClassMetatypeToObjectExpr(ClassMetatypeToObjectExpr *E) {
    printCommon(E, "class_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitExistentialMetatypeToObjectExpr(ExistentialMetatypeToObjectExpr *E) {
    printCommon(E, "existential_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitProtocolMetatypeToObjectExpr(ProtocolMetatypeToObjectExpr *E) {
    printCommon(E, "protocol_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInOutToPointerExpr(InOutToPointerExpr *E) {
    printCommon(E, "inout_to_pointer")
      << (E->isNonAccessing() ? " nonaccessing" : "") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArrayToPointerExpr(ArrayToPointerExpr *E) {
    printCommon(E, "array_to_pointer")
      << (E->isNonAccessing() ? " nonaccessing" : "") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitStringToPointerExpr(StringToPointerExpr *E) {
    printCommon(E, "string_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitPointerToPointerExpr(PointerToPointerExpr *E) {
    printCommon(E, "pointer_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForeignObjectConversionExpr(ForeignObjectConversionExpr *E) {
    printCommon(E, "foreign_object_conversion") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnevaluatedInstanceExpr(UnevaluatedInstanceExpr *E) {
    printCommon(E, "unevaluated_instance") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDifferentiableFunctionExpr(DifferentiableFunctionExpr *E) {
    printCommon(E, "differentiable_function") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLinearFunctionExpr(LinearFunctionExpr *E) {
    printCommon(E, "linear_function") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDifferentiableFunctionExtractOriginalExpr(
      DifferentiableFunctionExtractOriginalExpr *E) {
    printCommon(E, "differentiable_function_extract_original") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLinearFunctionExtractOriginalExpr(
      LinearFunctionExtractOriginalExpr *E) {
    printCommon(E, "linear_function_extract_original") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLinearToDifferentiableFunctionExpr(
      LinearToDifferentiableFunctionExpr *E) {
    printCommon(E, "linear_to_differentiable_function") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitInOutExpr(InOutExpr *E) {
    printCommon(E, "inout_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitVarargExpansionExpr(VarargExpansionExpr *E) {
    printCommon(E, "vararg_expansion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPackExpansionExpr(PackExpansionExpr *E) {
    printCommon(E, "pack_expansion_expr") << "\n";
    printRec(E->getPatternExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPackElementExpr(PackElementExpr *E) {
    printCommon(E, "pack_element_expr") << "\n";
    printRec(E->getPackRefExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMaterializePackExpr(MaterializePackExpr *E) {
    printCommon(E, "materialize_pack_expr") << "\n";
    printRec(E->getFromExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitForceTryExpr(ForceTryExpr *E) {
    printCommon(E, "force_try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOptionalTryExpr(OptionalTryExpr *E) {
    printCommon(E, "optional_try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTryExpr(TryExpr *E) {
    printCommon(E, "try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitSequenceExpr(SequenceExpr *E) {
    printCommon(E, "sequence_expr");
    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      printRec(E->getElement(i));
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCaptureListExpr(CaptureListExpr *E) {
    printCommon(E, "capture_list");
    for (auto capture : E->getCaptureList()) {
      OS << '\n';
      Indent += 2;
      printRec(capture.PBD);
      Indent -= 2;
    }
    printRec(E->getClosureBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  llvm::raw_ostream &printClosure(AbstractClosureExpr *E, char const *name) {
    printCommon(E, name);

    // If we aren't printing to standard error or the debugger output stream,
    // this client expects to see the computed discriminator. Compute it now.
    if (&OS != &llvm::errs() && &OS != &llvm::dbgs())
      (void)E->getDiscriminator();

    PrintWithColorRAII(OS, DiscriminatorColor)
      << " discriminator=" << E->getRawDiscriminator();

    switch (auto isolation = E->getActorIsolation()) {
    case ClosureActorIsolation::Independent:
      break;

    case ClosureActorIsolation::ActorInstance:
      PrintWithColorRAII(OS, CapturesColor) << " actor-isolated="
        << isolation.getActorInstance()->printRef();
      break;

    case ClosureActorIsolation::GlobalActor:
      PrintWithColorRAII(OS, CapturesColor) << " global-actor-isolated="
        << isolation.getGlobalActor().getString();
      break;
    }

    if (!E->getCaptureInfo().isTrivial()) {
      OS << " ";
      E->getCaptureInfo().print(PrintWithColorRAII(OS, CapturesColor).getOS());
    }
    // Printing a function type doesn't indicate whether it's escaping because it doesn't 
    // matter in 99% of contexts. AbstractClosureExpr nodes are one of the only exceptions.
    if (auto Ty = GetTypeOfExpr(E)) {
      if (auto fType = Ty->getAs<AnyFunctionType>()) {
        if (!fType->getExtInfo().isNoEscape())
          PrintWithColorRAII(OS, ClosureModifierColor) << " escaping";
        if (fType->getExtInfo().isSendable())
          PrintWithColorRAII(OS, ClosureModifierColor) << " concurrent";
      }
    }

    return OS;
  }

  void visitClosureExpr(ClosureExpr *E) {
    printClosure(E, "closure_expr");
    if (E->hasSingleExpressionBody())
      PrintWithColorRAII(OS, ClosureModifierColor) << " single-expression";
    if (E->allowsImplicitSelfCapture())
      PrintWithColorRAII(OS, ClosureModifierColor) << " implicit-self";
    if (E->inheritsActorContext())
      PrintWithColorRAII(OS, ClosureModifierColor) << " inherits-actor-context";

    if (E->getParameters()) {
      OS << '\n';
      PrintDecl(OS, Indent+2).printParameterList(E->getParameters(), &E->getASTContext());
    }

    OS << '\n';
    printRec(E->getBody(), E->getASTContext());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAutoClosureExpr(AutoClosureExpr *E) {
    printClosure(E, "autoclosure_expr") << '\n';

    if (E->getParameters()) {
      OS << '\n';
      PrintDecl(OS, Indent+2).printParameterList(E->getParameters(), &E->getASTContext());
    }

    OS << '\n';
    printRec(E->getSingleExpressionBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDynamicTypeExpr(DynamicTypeExpr *E) {
    printCommon(E, "metatype_expr");
    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOpaqueValueExpr(OpaqueValueExpr *E) {
    printCommon(E, "opaque_value_expr") << " @ " << (void*)E;
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPropertyWrapperValuePlaceholderExpr(
      PropertyWrapperValuePlaceholderExpr *E) {
    printCommon(E, "property_wrapper_value_placeholder_expr");
    OS << '\n';
    printRec(E->getOpaqueValuePlaceholder());
    if (auto *value = E->getOriginalWrappedValue()) {
      OS << '\n';
      printRec(value);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitAppliedPropertyWrapperExpr(AppliedPropertyWrapperExpr *E) {
    printCommon(E, "applied_property_wrapper_expr");
    OS << '\n';
    printRec(E->getValue());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDefaultArgumentExpr(DefaultArgumentExpr *E) {
    printCommon(E, "default_argument_expr");
    OS << " default_args_owner=";
    E->getDefaultArgsOwner().dump(OS);
    OS << " param=" << E->getParamIndex();
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printArgumentList(const ArgumentList *argList, bool indent = true) {
    ::printArgumentList(OS, argList, Indent, [&](Expr *E) { printRec(E); },
                        indent);
  }

  void printApplyExpr(ApplyExpr *E, const char *NodeName) {
    printCommon(E, NodeName);
    if (E->isThrowsSet()) {
      PrintWithColorRAII(OS, ExprModifierColor)
        << (E->throws() ? " throws" : " nothrow");
    }
    OS << '\n';
    printRec(E->getFn());
    OS << '\n';
    printArgumentList(E->getArgs());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCallExpr(CallExpr *E) {
    printApplyExpr(E, "call_expr");
  }
  void visitPrefixUnaryExpr(PrefixUnaryExpr *E) {
    printApplyExpr(E, "prefix_unary_expr");
  }
  void visitPostfixUnaryExpr(PostfixUnaryExpr *E) {
    printApplyExpr(E, "postfix_unary_expr");
  }
  void visitBinaryExpr(BinaryExpr *E) {
    printApplyExpr(E, "binary_expr");
  }
  void visitDotSyntaxCallExpr(DotSyntaxCallExpr *E) {
    printApplyExpr(E, "dot_syntax_call_expr");
  }
  void visitConstructorRefCallExpr(ConstructorRefCallExpr *E) {
    printApplyExpr(E, "constructor_ref_call_expr");
  }
  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *E) {
    printCommon(E, "dot_syntax_base_ignored") << '\n';
    printRec(E->getLHS());
    OS << '\n';
    printRec(E->getRHS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printExplicitCastExpr(ExplicitCastExpr *E, const char *name) {
    printCommon(E, name) << ' ';
    if (auto checkedCast = dyn_cast<CheckedCastExpr>(E))
      OS << getCheckedCastKindName(checkedCast->getCastKind()) << ' ';
    OS << "writtenType='";
    if (GetTypeOfTypeRepr)
      GetTypeOfTypeRepr(E->getCastTypeRepr()).print(OS);
    else
      E->getCastType().print(OS);
    OS << "'\n";
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForcedCheckedCastExpr(ForcedCheckedCastExpr *E) {
    printExplicitCastExpr(E, "forced_checked_cast_expr");
  }
  void visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *E) {
    printExplicitCastExpr(E, "conditional_checked_cast_expr");
  }
  void visitIsExpr(IsExpr *E) {
    printExplicitCastExpr(E, "is_subtype_expr");
  }
  void visitCoerceExpr(CoerceExpr *E) {
    printExplicitCastExpr(E, "coerce_expr");
  }
  void visitArrowExpr(ArrowExpr *E) {
    printCommon(E, "arrow");
    if (E->getAsyncLoc().isValid())
      OS << " async";
    if (E->getThrowsLoc().isValid())
      OS << " throws";
    OS << '\n';
    printRec(E->getArgsExpr());
    OS << '\n';
    printRec(E->getResultExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *E) {
    printCommon(E, "rebind_self_in_constructor_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTernaryExpr(TernaryExpr *E) {
    printCommon(E, "ternary_expr") << '\n';
    printRec(E->getCondExpr());
    OS << '\n';
    printRec(E->getThenExpr());
    OS << '\n';
    printRec(E->getElseExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAssignExpr(AssignExpr *E) {
    printCommon(E, "assign_expr") << '\n';
    printRec(E->getDest());
    OS << '\n';
    printRec(E->getSrc());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitEnumIsCaseExpr(EnumIsCaseExpr *E) {
    printCommon(E, "enum_is_case_expr") << ' ' <<
      E->getEnumElement()->getBaseIdentifier() << "\n";
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedPatternExpr(UnresolvedPatternExpr *E) {
    printCommon(E, "unresolved_pattern_expr") << '\n';
    printRec(E->getSubPattern());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBindOptionalExpr(BindOptionalExpr *E) {
    printCommon(E, "bind_optional_expr")
      << " depth=" << E->getDepth() << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOptionalEvaluationExpr(OptionalEvaluationExpr *E) {
    printCommon(E, "optional_evaluation_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForceValueExpr(ForceValueExpr *E) {
    printCommon(E, "force_value_expr");
    if (E->isForceOfImplicitlyUnwrappedOptional())
      PrintWithColorRAII(OS, ExprModifierColor) << " implicit_iuo_unwrap";
    OS << '\n';

    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOpenExistentialExpr(OpenExistentialExpr *E) {
    printCommon(E, "open_existential_expr") << '\n';
    printRec(E->getOpaqueValue());
    OS << '\n';
    printRec(E->getExistentialValue());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMakeTemporarilyEscapableExpr(MakeTemporarilyEscapableExpr *E) {
    printCommon(E, "make_temporarily_escapable_expr") << '\n';
    printRec(E->getOpaqueValue());
    OS << '\n';
    printRec(E->getNonescapingClosureValue());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitEditorPlaceholderExpr(EditorPlaceholderExpr *E) {
    printCommon(E, "editor_placeholder_expr") << ' ';

    // Print the trailing angle bracket location
    if (auto Ty = GetTypeOfExpr(E)) {
      auto &Ctx = Ty->getASTContext();
      auto TABL = E->getTrailingAngleBracketLoc();
      if (TABL.isValid()) {
        PrintWithColorRAII(OS, LocationColor) << " trailing_angle_bracket_loc=";
        TABL.print(PrintWithColorRAII(OS, LocationColor).getOS(),
                   Ctx.SourceMgr);
      }
    }
    OS << '\n';
    auto *TyR = E->getPlaceholderTypeRepr();
    auto *ExpTyR = E->getTypeForExpansion();
    if (TyR)
      printRec(TyR);
    if (ExpTyR && ExpTyR != TyR) {
      OS << '\n';
      printRec(ExpTyR);
    }
    printSemanticExpr(E->getSemanticExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLazyInitializerExpr(LazyInitializerExpr *E) {
    printCommon(E, "lazy_initializer_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitObjCSelectorExpr(ObjCSelectorExpr *E) {
    printCommon(E, "objc_selector_expr");
    OS << " kind=" << getObjCSelectorExprKindString(E->getSelectorKind());
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getMethod());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitKeyPathExpr(KeyPathExpr *E) {
    printCommon(E, "keypath_expr");
    if (E->isObjC())
      OS << " objc";

    OS << '\n';
    Indent += 2;
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, ExprColor) << "components";
    OS.indent(Indent + 2);
    for (unsigned i : indices(E->getComponents())) {
      auto &component = E->getComponents()[i];
      OS << '\n';
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      switch (component.getKind()) {
      case KeyPathExpr::Component::Kind::Invalid:
        PrintWithColorRAII(OS, ASTNodeColor) << "invalid";
        break;

      case KeyPathExpr::Component::Kind::OptionalChain:
        PrintWithColorRAII(OS, ASTNodeColor) << "optional_chain";
        break;
        
      case KeyPathExpr::Component::Kind::OptionalForce:
        PrintWithColorRAII(OS, ASTNodeColor) << "optional_force";
        break;
        
      case KeyPathExpr::Component::Kind::OptionalWrap:
        PrintWithColorRAII(OS, ASTNodeColor) << "optional_wrap";
        break;
        
      case KeyPathExpr::Component::Kind::Property:
        PrintWithColorRAII(OS, ASTNodeColor) << "property";
        PrintWithColorRAII(OS, DeclColor) << " decl=";
        printDeclRef(component.getDeclRef());
        break;
      
      case KeyPathExpr::Component::Kind::Subscript:
        PrintWithColorRAII(OS, ASTNodeColor) << "subscript";
        PrintWithColorRAII(OS, DeclColor) << " decl='";
        printDeclRef(component.getDeclRef());
        PrintWithColorRAII(OS, DeclColor) << "'";
        break;
      
      case KeyPathExpr::Component::Kind::UnresolvedProperty:
        PrintWithColorRAII(OS, ASTNodeColor) << "unresolved_property";
        PrintWithColorRAII(OS, IdentifierColor)
          << " decl_name='" << component.getUnresolvedDeclName() << "'";
        break;
        
      case KeyPathExpr::Component::Kind::UnresolvedSubscript:
        PrintWithColorRAII(OS, ASTNodeColor) << "unresolved_subscript";
        break;
      case KeyPathExpr::Component::Kind::Identity:
        PrintWithColorRAII(OS, ASTNodeColor) << "identity";
        break;

      case KeyPathExpr::Component::Kind::TupleElement:
        PrintWithColorRAII(OS, ASTNodeColor) << "tuple_element ";
        PrintWithColorRAII(OS, DiscriminatorColor)
          << "#" << component.getTupleIndex();
        break;
      case KeyPathExpr::Component::Kind::DictionaryKey:
        PrintWithColorRAII(OS, ASTNodeColor) << "dict_key";
        PrintWithColorRAII(OS, IdentifierColor)
          << "  key='" << component.getUnresolvedDeclName() << "'";
        break;
      case KeyPathExpr::Component::Kind::CodeCompletion:
        PrintWithColorRAII(OS, ASTNodeColor) << "completion";
        break;
      }
      PrintWithColorRAII(OS, TypeColor)
        << " type='" << GetTypeOfKeyPathComponent(E, i) << "'";
      if (auto *args = component.getSubscriptArgs()) {
        OS << '\n';
        printArgumentList(args);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;

    if (auto stringLiteral = E->getObjCStringLiteralExpr()) {
      OS << '\n';
      printRecLabeled(stringLiteral, "objc_string_literal");
    }
    if (!E->isObjC()) {
      if (auto root = E->getParsedRoot()) {
        OS << "\n";
        printRecLabeled(root, "parsed_root");
      }
      if (auto path = E->getParsedPath()) {
        OS << "\n";
        printRecLabeled(path, "parsed_path");
      }
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitKeyPathDotExpr(KeyPathDotExpr *E) {
    printCommon(E, "key_path_dot_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitSingleValueStmtExpr(SingleValueStmtExpr *E) {
    printCommon(E, "single_value_stmt_expr");
    OS << '\n';
    printRec(E->getStmt(), E->getDeclContext()->getASTContext());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOneWayExpr(OneWayExpr *E) {
    printCommon(E, "one_way_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTapExpr(TapExpr *E) {
    printCommon(E, "tap_expr");
    PrintWithColorRAII(OS, DeclColor) << " var=";
    printDeclRef(E->getVar());
    OS << '\n';

    printRec(E->getSubExpr());
    OS << '\n';

    printRec(E->getBody(), E->getVar()->getDeclContext()->getASTContext());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTypeJoinExpr(TypeJoinExpr *E) {
    printCommon(E, "type_join_expr");

    if (auto *var = E->getVar()) {
      PrintWithColorRAII(OS, DeclColor) << " var=";
      printRec(var);
      OS << '\n';
    }

    if (auto *SVE = E->getSingleValueStmtExpr()) {
      PrintWithColorRAII(OS, ExprColor) << "single_value_stmt_expr=";
      printRec(SVE);
      OS << '\n';
    }

    OS << '\n';

    for (auto *member : E->getElements()) {
      printRec(member);
      OS << '\n';
    }

    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMacroExpansionExpr(MacroExpansionExpr *E) {
    printCommon(E, "macro_expansion_expr");
    PrintWithColorRAII(OS, IdentifierColor) << " name=" << E->getMacroName();
    PrintWithColorRAII(OS, DiscriminatorColor)
      << " discriminator=" << E->getRawDiscriminator();
    if (E->getArgs()) {
      OS << '\n';
      printArgumentList(E->getArgs());
    }
    if (auto rewritten = E->getRewritten()) {
      OS << " rewritten=\n";
      printRec(rewritten);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
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
  PrintExpr(OS, getTypeOfExpr, getTypeOfTypeRepr, getTypeOfKeyPathComponent,
            Indent)
      .visit(const_cast<Expr *>(this));
}

void Expr::dump(raw_ostream &OS, unsigned Indent) const {
  auto getTypeOfExpr = [](Expr *E) -> Type { return E->getType(); };
  auto getTypeOfKeyPathComponent = [](KeyPathExpr *E, unsigned index) -> Type {
    return E->getComponents()[index].getComponentType();
  };
  dump(OS, getTypeOfExpr, /*getTypeOfTypeRepr*/ nullptr,
       getTypeOfKeyPathComponent, Indent);
}

void Expr::print(ASTPrinter &Printer, const PrintOptions &Opts) const {
  // FIXME: Fully use the ASTPrinter.
  llvm::SmallString<128> Str;
  llvm::raw_svector_ostream OS(Str);
  dump(OS);
  Printer << OS.str();
}

void ArgumentList::dump() const {
  dump(llvm::errs(), 0);
}

void ArgumentList::dump(raw_ostream &OS, unsigned Indent) const {
  auto getTypeOfExpr = [](Expr *E) -> Type { return E->getType(); };
  auto getTypeOfKeyPathComponent = [](KeyPathExpr *E, unsigned index) -> Type {
    return E->getComponents()[index].getComponentType();
  };
  PrintExpr printer(OS, getTypeOfExpr, /*getTypeOfTypeRepr*/ nullptr,
                    getTypeOfKeyPathComponent, Indent);
  printer.printArgumentList(this, /*indent*/ false);
  llvm::errs() << '\n';
}

//===----------------------------------------------------------------------===//
// Printing for TypeRepr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
class PrintTypeRepr : public TypeReprVisitor<PrintTypeRepr> {
public:
  raw_ostream &OS;
  unsigned Indent;

  PrintTypeRepr(raw_ostream &os, unsigned indent)
    : OS(os), Indent(indent) { }

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Expr *E) { E->dump(OS, Indent + 2); }
  void printRec(TypeRepr *T) { PrintTypeRepr(OS, Indent + 2).visit(T); }

  raw_ostream &printCommon(const char *Name) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, TypeReprColor) << Name;
    return OS;
  }

  void visitErrorTypeRepr(ErrorTypeRepr *T) {
    printCommon("type_error");
  }

  void visitAttributedTypeRepr(AttributedTypeRepr *T) {
    printCommon("type_attributed") << " attrs=";
    T->printAttrs(OS);
    OS << '\n';
    printRec(T->getTypeRepr());
  }

  void visitIdentTypeRepr(IdentTypeRepr *T) {
    printCommon("type_ident");

    PrintWithColorRAII(OS, IdentifierColor)
        << " id='" << T->getNameRef() << '\'';
    OS << " bind=";
    if (T->isBound())
      T->getBoundDecl()->dumpRef(OS);
    else
      OS << "none";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    if (auto *GenIdT = dyn_cast<GenericIdentTypeRepr>(T)) {
      for (auto genArg : GenIdT->getGenericArgs()) {
        OS << '\n';
        printRec(genArg);
      }
    }
  }

  void visitMemberTypeRepr(MemberTypeRepr *T) {
    printCommon("type_member");

    OS << '\n';
    printRec(T->getBaseComponent());
    for (auto *comp : T->getMemberComponents()) {
      OS << '\n';
      printRec(comp);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitFunctionTypeRepr(FunctionTypeRepr *T) {
    printCommon("type_function");
    OS << '\n'; printRec(T->getArgsTypeRepr());
    if (T->isAsync())
      OS << " async ";
    if (T->isThrowing())
      OS << " throws ";
    OS << '\n'; printRec(T->getResultTypeRepr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitArrayTypeRepr(ArrayTypeRepr *T) {
    printCommon("type_array") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDictionaryTypeRepr(DictionaryTypeRepr *T) {
    printCommon("type_dictionary") << '\n';
    printRec(T->getKey());
    OS << '\n';
    printRec(T->getValue());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitVarargTypeRepr(VarargTypeRepr *T) {
    printCommon("vararg") << '\n';
    printRec(T->getElementType());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPackTypeRepr(PackTypeRepr *T) {
    printCommon("pack") << '\n';
    for (auto elt : T->getElements())
      printRec(elt);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPackExpansionTypeRepr(PackExpansionTypeRepr *T) {
    printCommon("pack_expansion") << '\n';
    printRec(T->getPatternType());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPackElementTypeRepr(PackElementTypeRepr *T) {
    printCommon("pack_element");
    printRec(T->getPackType());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTupleTypeRepr(TupleTypeRepr *T) {
    printCommon("type_tuple");

    if (T->hasElementNames()) {
      OS << " names=";
      for (unsigned i = 0, end = T->getNumElements(); i != end; ++i) {
        if (i) OS << ",";
        auto name = T->getElementName(i);
        if (T->isNamedParameter(i))
          OS << (name.empty() ? "_" : "_ " + name.str());
        else
          OS << (name.empty() ? "''" : name.str());
      }
    }

    for (auto elem : T->getElements()) {
      OS << '\n';
      printRec(elem.Type);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCompositionTypeRepr(CompositionTypeRepr *T) {
    printCommon("type_composite");
    for (auto elem : T->getTypes()) {
      OS << '\n';
      printRec(elem);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMetatypeTypeRepr(MetatypeTypeRepr *T) {
    printCommon("type_metatype") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitProtocolTypeRepr(ProtocolTypeRepr *T) {
    printCommon("type_protocol") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOwnershipTypeRepr(OwnershipTypeRepr *T) {
    printCommon("type_ownership")
      << ' '
      << T->getSpecifierSpelling()
      << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  
  void visitIsolatedTypeRepr(IsolatedTypeRepr *T) {
    printCommon("isolated") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCompileTimeConstTypeRepr(CompileTimeConstTypeRepr *T) {
    printCommon("_const") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOptionalTypeRepr(OptionalTypeRepr *T) {
    printCommon("type_optional") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitImplicitlyUnwrappedOptionalTypeRepr(
      ImplicitlyUnwrappedOptionalTypeRepr *T) {
    printCommon("type_implicitly_unwrapped_optional") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOpaqueReturnTypeRepr(OpaqueReturnTypeRepr *T) {
    printCommon("type_opaque_return");
    printRec(T->getConstraint());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitNamedOpaqueReturnTypeRepr(NamedOpaqueReturnTypeRepr *T) {
    printCommon("type_named_opaque_return") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitExistentialTypeRepr(ExistentialTypeRepr *T) {
    printCommon("type_existential");
    printRec(T->getConstraint());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPlaceholderTypeRepr(PlaceholderTypeRepr *T) {
    printCommon("type_placeholder");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitFixedTypeRepr(FixedTypeRepr *T) {
    printCommon("type_fixed");
    auto Ty = T->getType();
    if (Ty) {
      auto &srcMgr =  Ty->getASTContext().SourceMgr;
      if (T->getLoc().isValid()) {
        OS << " location=@";
        T->getLoc().print(OS, srcMgr);
      } else {
        OS << " location=<<invalid>>";
      }
    }
    OS << " type="; Ty.dump(OS);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitSILBoxTypeRepr(SILBoxTypeRepr *T) {
    printCommon("sil_box");
    Indent += 2;

    ArrayRef<SILBoxTypeReprField> Fields = T->getFields();
    for (unsigned i = 0, end = Fields.size(); i != end; ++i) {
      OS << '\n';
      printCommon("sil_box_field");
      if (Fields[i].isMutable()) {
        OS << " mutable";
      }
      OS << '\n';
      printRec(Fields[i].getFieldType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    for (auto genArg : T->getGenericArguments()) {
      OS << '\n';
      printRec(genArg);
    }

    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;
  }
};

} // end anonymous namespace

void PrintDecl::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void PrintExpr::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void PrintPattern::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void TypeRepr::dump() const {
  PrintTypeRepr(llvm::errs(), 0).visit(const_cast<TypeRepr*>(this));
  llvm::errs() << '\n';
}

// Recursive helpers to avoid infinite recursion for recursive protocol
// conformances.
static void dumpProtocolConformanceRec(
    const ProtocolConformance *conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited);

static void dumpPackConformanceRec(
    const PackConformance *conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited);

static void dumpProtocolConformanceRefRec(
    const ProtocolConformanceRef conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  if (conformance.isInvalid()) {
    out.indent(indent) << "(invalid_conformance)";
  } else if (conformance.isConcrete()) {
    dumpProtocolConformanceRec(conformance.getConcrete(), out, indent, visited);
  } else if (conformance.isPack()) {
    dumpPackConformanceRec(conformance.getPack(), out, indent, visited);
  } else {
    assert(conformance.isAbstract());

    out.indent(indent) << "(abstract_conformance protocol="
                       << conformance.getAbstract()->getName();
    PrintWithColorRAII(out, ParenthesisColor) << ')';
  }
}

static void dumpProtocolConformanceRec(
    const ProtocolConformance *conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  // A recursive conformance shouldn't have its contents printed, or there's
  // infinite recursion. (This also avoids printing things that occur multiple
  // times in a conformance hierarchy.)
  auto shouldPrintDetails = visited.insert(conformance).second;

  auto printCommon = [&](StringRef kind) {
    out.indent(indent);
    PrintWithColorRAII(out, ParenthesisColor) << '(';
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
        dumpProtocolConformanceRefRec(sigConf, out, indent + 2, visited);
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
                               visited);
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
                               visited);
    break;
  }

  case ProtocolConformanceKind::Builtin: {
    printCommon("builtin");
  }
  }

  PrintWithColorRAII(out, ParenthesisColor) << ')';
}

static void dumpPackConformanceRec(
    const PackConformance *conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  out.indent(indent);
  PrintWithColorRAII(out, ParenthesisColor) << '(';
  out << "pack_conformance type=" << Type(conformance->getType())
      << " protocol=" << conformance->getProtocol()->getName();

  auto conformances = conformance->getPatternConformances();
  if (!conformances.empty()) {
    out << "\n";

    for (auto conformanceRef : conformances) {
      dumpProtocolConformanceRefRec(conformanceRef, out, indent, visited);
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
  auto genericParams = genericSig.getGenericParams();
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
      opts.PrintTypesForDebugging = true;
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
  for (const auto &req : genericSig.getRequirements()) {
    if (req.getKind() != RequirementKind::Conformance)
      continue;

    out << "\n";
    out.indent(indent + 2);
    printParen('(');
    out << "conformance type=";
    req.getFirstType()->print(out);
    out << "\n";
    dumpProtocolConformanceRefRec(conformances.front(), out, indent + 4,
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

  dumpProtocolConformanceRefRec(*this, out, indent, visited);
}

void ProtocolConformanceRef::print(llvm::raw_ostream &out) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpProtocolConformanceRefRec(*this, out, 0, visited);
}

void ProtocolConformance::dump() const {
  auto &out = llvm::errs();
  dump(out);
  out << '\n';
}

void ProtocolConformance::dump(llvm::raw_ostream &out, unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpProtocolConformanceRec(this, out, indent, visited);
}

void PackConformance::dump(llvm::raw_ostream &out, unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpPackConformanceRec(this, out, indent, visited);
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
    raw_ostream &OS;
    unsigned Indent;

    raw_ostream &printCommon(StringRef label, StringRef name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      if (!label.empty()) {
        PrintWithColorRAII(OS, TypeFieldColor) << label;
        OS << "=";
      }

      PrintWithColorRAII(OS, TypeColor) << name;
      return OS;
    }

    // Print a single flag.
    raw_ostream &printFlag(StringRef name) {
      PrintWithColorRAII(OS, TypeFieldColor) << " " << name;
      return OS;
    }

    // Print a single flag if it is set.
    raw_ostream &printFlag(bool isSet, StringRef name) {
      if (isSet)
        printFlag(name);

      return OS;
    }

    // Print a field with a value.
    template<typename T>
    raw_ostream &printField(StringRef name, const T &value) {
      OS << " ";
      PrintWithColorRAII(OS, TypeFieldColor) << name;
      OS << "=" << value;
      return OS;
    }

    void dumpParameterFlags(ParameterTypeFlags paramFlags) {
      printFlag(paramFlags.isVariadic(), "vararg");
      printFlag(paramFlags.isAutoClosure(), "autoclosure");
      printFlag(paramFlags.isNonEphemeral(), "nonEphemeral");
      printFlag(paramFlags.isCompileTimeConst(), "compileTimeConst");
      switch (paramFlags.getValueOwnership()) {
      case ValueOwnership::Default: break;
      case ValueOwnership::Owned: printFlag("owned"); break;
      case ValueOwnership::Shared: printFlag("shared"); break;
      case ValueOwnership::InOut: printFlag("inout"); break;
      }
    }

  public:
    PrintType(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) { }

    void printRec(Type type) {
      printRec("", type);
    }

    void printRec(StringRef label, Type type) {
      OS << "\n";

      if (type.isNull())
        OS << "<<null>>";
      else {
        Indent += 2;
        visit(type, label);
        Indent -=2;
      }
    }

#define TRIVIAL_TYPE_PRINTER(Class,Name)                        \
    void visit##Class##Type(Class##Type *T, StringRef label) {  \
      printCommon(label, #Name "_type") << ")";              \
    }

    void visitErrorType(ErrorType *T, StringRef label) {
      printCommon(label, "error_type");
      if (auto originalType = T->getOriginalType())
        printRec("original_type", originalType);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    TRIVIAL_TYPE_PRINTER(Unresolved, unresolved)

    void visitPlaceholderType(PlaceholderType *T, StringRef label) {
      printCommon(label, "placeholder_type");
      auto originator = T->getOriginator();
      if (auto *typeVar = originator.dyn_cast<TypeVariableType *>()) {
        printRec("type_variable", typeVar);
      } else if (auto *VD = originator.dyn_cast<VarDecl *>()) {
        VD->dumpRef(PrintWithColorRAII(OS, DeclColor).getOS());
      } else if (auto *EE = originator.dyn_cast<ErrorExpr *>()) {
        printFlag("error_expr");
      } else if (auto *DMT = originator.dyn_cast<DependentMemberType *>()) {
        printRec("dependent_member_type", DMT);
      } else if (originator.is<PlaceholderTypeRepr *>()) {
        printFlag("placeholder_type_repr");
      } else {
        assert(false && "unknown originator");
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBuiltinIntegerType(BuiltinIntegerType *T, StringRef label) {
      printCommon(label, "builtin_integer_type");
      if (T->isFixedWidth())
        printField("bit_width", T->getFixedWidth());
      else
        printFlag("word_sized");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBuiltinFloatType(BuiltinFloatType *T, StringRef label) {
      printCommon(label, "builtin_float_type");
      printField("bit_width", T->getBitWidth());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    TRIVIAL_TYPE_PRINTER(BuiltinIntegerLiteral, builtin_integer_literal)
    TRIVIAL_TYPE_PRINTER(BuiltinJob, builtin_job)
    TRIVIAL_TYPE_PRINTER(BuiltinExecutor, builtin_executor_ref)
    TRIVIAL_TYPE_PRINTER(BuiltinDefaultActorStorage, builtin_default_actor_storage)
    TRIVIAL_TYPE_PRINTER(BuiltinNonDefaultDistributedActorStorage, builtin_non_default_distributed_actor_storage)
    TRIVIAL_TYPE_PRINTER(BuiltinPackIndex, builtin_pack_index)
    TRIVIAL_TYPE_PRINTER(BuiltinRawPointer, builtin_raw_pointer)
    TRIVIAL_TYPE_PRINTER(BuiltinRawUnsafeContinuation, builtin_raw_unsafe_continuation)
    TRIVIAL_TYPE_PRINTER(BuiltinNativeObject, builtin_native_object)
    TRIVIAL_TYPE_PRINTER(BuiltinBridgeObject, builtin_bridge_object)
    TRIVIAL_TYPE_PRINTER(BuiltinUnsafeValueBuffer, builtin_unsafe_value_buffer)
    TRIVIAL_TYPE_PRINTER(SILToken, sil_token)

    void visitBuiltinVectorType(BuiltinVectorType *T, StringRef label) {
      printCommon(label, "builtin_vector_type");
      printField("num_elements", T->getNumElements());
      printRec(T->getElementType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTypeAliasType(TypeAliasType *T, StringRef label) {
      printCommon(label, "type_alias_type");
      printField("decl", T->getDecl()->printRef());
      PrintWithColorRAII(OS, TypeColor) << " underlying=";
      if (auto underlying = T->getSinglyDesugaredType()) {
        PrintWithColorRAII(OS, TypeColor)
          << "'" << underlying->getString() << "'";
      } else {
        PrintWithColorRAII(OS, TypeColor) << "<<<unresolved>>>";
      }
      if (T->getParent())
        printRec("parent", T->getParent());

      for (const auto &arg : T->getDirectGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPackType(PackType *T, StringRef label) {
      printCommon(label, "pack_type");
      printField("num_elements", T->getNumElements());
      Indent += 2;
      for (Type elt : T->getElementTypes()) {
        printRec(elt);
      }
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILPackType(SILPackType *T, StringRef label) {
      printCommon(label, "sil_pack_type");
      printField("element_is_address", T->isElementAddress());
      printField("num_elements", T->getNumElements());
      Indent += 2;
      for (Type elt : T->getElementTypes()) {
        printRec(elt);
      }
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPackExpansionType(PackExpansionType *T, StringRef label) {
      printCommon(label, "pack_expansion_type");
      printRec("pattern", T->getPatternType());
      printRec("count", T->getCountType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPackElementType(PackElementType *T, StringRef label) {
      printCommon(label, "element_type");
      printField("level", T->getLevel());
      printRec("pack", T->getPackType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitParenType(ParenType *T, StringRef label) {
      printCommon(label, "paren_type");
      printRec(T->getUnderlyingType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTupleType(TupleType *T, StringRef label) {
      printCommon(label, "tuple_type");
      printField("num_elements", T->getNumElements());
      Indent += 2;
      for (const auto &elt : T->getElements()) {
        OS << "\n";
        OS.indent(Indent) << "(";
        PrintWithColorRAII(OS, TypeFieldColor) << "tuple_type_elt";
        if (elt.hasName())
          printField("name", elt.getName().str());
        printRec(elt.getType());
        OS << ")";
      }
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

#define REF_STORAGE(Name, name, ...) \
    void visit##Name##StorageType(Name##StorageType *T, StringRef label) { \
      printCommon(label, #name "_storage_type"); \
      printRec(T->getReferentType()); \
      PrintWithColorRAII(OS, ParenthesisColor) << ')'; \
    }
#include "swift/AST/ReferenceStorage.def"

    void visitEnumType(EnumType *T, StringRef label) {
      printCommon(label, "enum_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitStructType(StructType *T, StringRef label) {
      printCommon(label, "struct_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitClassType(ClassType *T, StringRef label) {
      printCommon(label, "class_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitProtocolType(ProtocolType *T, StringRef label) {
      printCommon(label, "protocol_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBuiltinTupleType(BuiltinTupleType *T, StringRef label) {
      printCommon(label, "builtin_tuple_type");
      printField("decl", T->getDecl()->printRef());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitMetatypeType(MetatypeType *T, StringRef label) {
      printCommon(label, "metatype_type");
      if (T->hasRepresentation())
        OS << " " << getMetatypeRepresentationString(T->getRepresentation());
      printRec(T->getInstanceType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitExistentialMetatypeType(ExistentialMetatypeType *T,
                                      StringRef label) {
      printCommon(label, "existential_metatype_type");
      if (T->hasRepresentation())
        OS << " " << getMetatypeRepresentationString(T->getRepresentation());
      printRec(T->getInstanceType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitModuleType(ModuleType *T, StringRef label) {
      printCommon(label, "module_type");
      printField("module", T->getModule()->getName());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDynamicSelfType(DynamicSelfType *T, StringRef label) {
      printCommon(label, "dynamic_self_type");
      printRec(T->getSelfType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    
    void printArchetypeCommon(ArchetypeType *T,
                              StringRef className,
                              StringRef label) {
      printCommon(label, className);
      printField("address", static_cast<void *>(T));
      printRec("interface_type", T->getInterfaceType());
      printFlag(T->requiresClass(), "class");
      if (auto layout = T->getLayoutConstraint()) {
        OS << " layout=";
        layout->print(OS);
      }
      for (auto proto : T->getConformsTo())
        printField("conforms_to", proto->printRef());
      if (auto superclass = T->getSuperclass())
        printRec("superclass", superclass);

    }
    
    void visitPrimaryArchetypeType(PrimaryArchetypeType *T, StringRef label) {
      printArchetypeCommon(T, "primary_archetype_type", label);
      printField("name", T->getFullName());
      OS << "\n";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitOpenedArchetypeType(OpenedArchetypeType *T, StringRef label) {
      printArchetypeCommon(T, "opened_archetype_type", label);
      printRec("opened_existential",
               T->getGenericEnvironment()->getOpenedExistentialType());
      printField("opened_existential_id", T->getOpenedExistentialID());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitOpaqueTypeArchetypeType(OpaqueTypeArchetypeType *T,
                                      StringRef label) {
      printArchetypeCommon(T, "opaque_type", label);
      printField("decl", T->getDecl()->getNamingDecl()->printRef());
      if (!T->getSubstitutions().empty()) {
        OS << '\n';
        SmallPtrSet<const ProtocolConformance *, 4> Dumped;
        dumpSubstitutionMapRec(T->getSubstitutions(), OS,
                               SubstitutionMap::DumpStyle::Full,
                               Indent + 2, Dumped);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitPackArchetypeType(PackArchetypeType *T, StringRef label) {
      printArchetypeCommon(T, "pack_archetype_type", label);
      printField("name", T->getFullName());
      OS << "\n";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitElementArchetypeType(ElementArchetypeType *T, StringRef label) {
      printArchetypeCommon(T, "element_archetype_type", label);
      printField("opened_element_id", T->getOpenedElementID());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitGenericTypeParamType(GenericTypeParamType *T, StringRef label) {
      printCommon(label, "generic_type_param_type");
      printField("depth", T->getDepth());
      printField("index", T->getIndex());
      if (auto decl = T->getDecl())
        printField("decl", decl->printRef());
      printFlag(T->isParameterPack(), "pack");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDependentMemberType(DependentMemberType *T, StringRef label) {
      printCommon(label, "dependent_member_type");
      if (auto assocType = T->getAssocType()) {
        printField("assoc_type", assocType->printRef());
      } else {
        printField("name", T->getName());
      }
      printRec("base", T->getBase());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printAnyFunctionParams(ArrayRef<AnyFunctionType::Param> params,
                                StringRef label) {
      printCommon(label, "function_params");
      printField("num_params", params.size());
      Indent += 2;
      for (const auto &param : params) {
        OS << "\n";
        OS.indent(Indent) << "(";
        PrintWithColorRAII(OS, TypeFieldColor) << "param";
        if (param.hasLabel())
          printField("name", param.getLabel().str());
        if (param.hasInternalLabel())
          printField("internal_name", param.getInternalLabel().str());
        dumpParameterFlags(param.getParameterFlags());
        printRec(param.getPlainType());
        OS << ")";
      }
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printAnyFunctionTypeCommon(AnyFunctionType *T, StringRef label,
                                    StringRef name) {
      printCommon(label, name);

      if (T->hasExtInfo()) {
        SILFunctionType::Representation representation =
            T->getExtInfo().getSILRepresentation();

        if (representation != SILFunctionType::Representation::Thick) {
          printField("representation",
                     getSILFunctionTypeRepresentationString(representation));
        }
        printFlag(!T->isNoEscape(), "escaping");
        printFlag(T->isSendable(), "Sendable");
        printFlag(T->isAsync(), "async");
        printFlag(T->isThrowing(), "throws");
      }

      if (Type globalActor = T->getGlobalActor()) {
        printField("global_actor", globalActor.getString());
      }

      OS << "\n";
      Indent += 2;
      // [TODO: Improve-Clang-type-printing]
      if (!T->getClangTypeInfo().empty()) {
        std::string s;
        llvm::raw_string_ostream os(s);
        auto &ctx = T->getASTContext().getClangModuleLoader()
          ->getClangASTContext();
        T->getClangTypeInfo().dump(os, ctx);
        printField("clang_type", os.str());
      }

      printAnyFunctionParams(T->getParams(), "input");
      Indent -=2;
      printRec("output", T->getResult());
    }

    void visitFunctionType(FunctionType *T, StringRef label) {
      printAnyFunctionTypeCommon(T, label, "function_type");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitGenericFunctionType(GenericFunctionType *T, StringRef label) {
      printAnyFunctionTypeCommon(T, label, "generic_function_type");
      // FIXME: generic signature dumping needs improvement
      OS << "\n";
      OS.indent(Indent + 2) << "(";
      printField("generic_sig", T->getGenericSignature()->getAsString());
      OS << ")";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILFunctionType(SILFunctionType *T, StringRef label) {
      printCommon(label, "sil_function_type");
      printField("type", T->getString());

      for (auto param : T->getParameters()) {
        printRec("input", param.getInterfaceType());
      }
      for (auto yield : T->getYields()) {
        printRec("yield", yield.getInterfaceType());
      }
      for (auto result : T->getResults()) {
        printRec("result", result.getInterfaceType());
      }
      if (auto error  = T->getOptionalErrorResult()) {
        printRec("error", error->getInterfaceType());
      }
      OS << '\n';
      T->getPatternSubstitutions().dump(OS, SubstitutionMap::DumpStyle::Full,
                                        Indent+2);
      OS << '\n';
      T->getInvocationSubstitutions().dump(OS, SubstitutionMap::DumpStyle::Full,
                                           Indent+2);
      // [TODO: Improve-Clang-type-printing]
      if (!T->getClangTypeInfo().empty()) {
        std::string s;
        llvm::raw_string_ostream os(s);
        auto &ctx =
            T->getASTContext().getClangModuleLoader()->getClangASTContext();
        T->getClangTypeInfo().dump(os, ctx);
        printField("clang_type", os.str());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILBlockStorageType(SILBlockStorageType *T, StringRef label) {
      printCommon(label, "sil_block_storage_type");
      printRec(T->getCaptureType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILMoveOnlyWrappedType(SILMoveOnlyWrappedType *T,
                                     StringRef label) {
      printCommon(label, "sil_move_only_type");
      printRec(T->getInnerType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILBoxType(SILBoxType *T, StringRef label) {
      printCommon(label, "sil_box_type");
      // FIXME: Print the structure of the type.
      printField("type", T->getString());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitArraySliceType(ArraySliceType *T, StringRef label) {
      printCommon(label, "array_slice_type");
      printRec(T->getBaseType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitOptionalType(OptionalType *T, StringRef label) {
      printCommon(label, "optional_type");
      printRec(T->getBaseType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDictionaryType(DictionaryType *T, StringRef label) {
      printCommon(label, "dictionary_type");
      printRec("key", T->getKeyType());
      printRec("value", T->getValueType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitVariadicSequenceType(VariadicSequenceType *T, StringRef label) {
      printCommon(label, "variadic_sequence_type");
      printRec(T->getBaseType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitProtocolCompositionType(ProtocolCompositionType *T,
                                      StringRef label) {
      printCommon(label, "protocol_composition_type");
      if (T->hasExplicitAnyObject())
        OS << " any_object";
      for (auto proto : T->getMembers()) {
        printRec(proto);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitParameterizedProtocolType(ParameterizedProtocolType *T,
                                        StringRef label) {
      printCommon(label, "parameterized_protocol_type");
      printRec("base", T->getBaseType());
      for (auto arg : T->getArgs()) {
        printRec(arg);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitExistentialType(ExistentialType *T,
                              StringRef label) {
      printCommon(label, "existential_type");
      printRec(T->getConstraintType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitLValueType(LValueType *T, StringRef label) {
      printCommon(label, "lvalue_type");
      printRec(T->getObjectType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitInOutType(InOutType *T, StringRef label) {
      printCommon(label, "inout_type");
      printRec(T->getObjectType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitUnboundGenericType(UnboundGenericType *T, StringRef label) {
      printCommon(label, "unbound_generic_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBoundGenericClassType(BoundGenericClassType *T, StringRef label) {
      printCommon(label, "bound_generic_class_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBoundGenericStructType(BoundGenericStructType *T,
                                     StringRef label) {
      printCommon(label, "bound_generic_struct_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBoundGenericEnumType(BoundGenericEnumType *T, StringRef label) {
      printCommon(label, "bound_generic_enum_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTypeVariableType(TypeVariableType *T, StringRef label) {
      printCommon(label, "type_variable_type");
      printField("id", T->getID());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

#undef TRIVIAL_TYPE_PRINTER
  };
} // end anonymous namespace

void Type::dump() const {
  dump(llvm::errs());
}

void Type::dump(raw_ostream &os, unsigned indent) const {
  PrintType(os, indent).visit(*this, "");
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

void RequirementRepr::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void GenericParamList::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void LayoutConstraint::dump() const {
  if (!*this) {
    llvm::errs() << "(null)\n";
    return;
  }
  getPointer()->print(llvm::errs());
}

void GenericSignature::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void Requirement::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}
void Requirement::dump(raw_ostream &out) const {
  switch (getKind()) {
  case RequirementKind::SameShape:
    out << "same_shape: ";
    break;
  case RequirementKind::Conformance:
    out << "conforms_to: ";
    break;
  case RequirementKind::Layout:
    out << "layout: ";
    break;
  case RequirementKind::Superclass:
    out << "superclass: ";
    break;
  case RequirementKind::SameType:
    out << "same_type: ";
    break;
  }

  PrintOptions opts;
  opts.ProtocolQualifiedDependentMemberTypes = true;

  getFirstType().print(out, opts);
  out << " ";

  if (getKind() != RequirementKind::Layout && getSecondType())
    getSecondType().print(out, opts);
  else if (getLayoutConstraint())
    out << getLayoutConstraint();
}

void SILParameterInfo::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void SILResultInfo::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}
