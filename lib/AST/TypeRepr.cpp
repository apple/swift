//===--- TypeRepr.cpp - Swift Language Type Representation ----------------===//
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
// This file implements the TypeRepr and related classes.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/TypeRepr.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

#define TYPEREPR(Id, _) \
  static_assert(IsTriviallyDestructible<Id##TypeRepr>::value, \
                "TypeReprs are BumpPtrAllocated; the d'tor is never called");
#include "swift/AST/TypeReprNodes.def"

SourceLoc TypeRepr::getLoc() const {
  switch (getKind()) {
#define TYPEREPR(CLASS, PARENT) \
case TypeReprKind::CLASS: \
return static_cast<const CLASS##TypeRepr*>(this)->getLocImpl();
#include "swift/AST/TypeReprNodes.def"
  }
  llvm_unreachable("unknown kind!");
}

SourceLoc TypeRepr::getStartLoc() const {
  switch (getKind()) {
#define TYPEREPR(CLASS, PARENT) \
  case TypeReprKind::CLASS: \
    return static_cast<const CLASS##TypeRepr*>(this)->getStartLocImpl();
#include "swift/AST/TypeReprNodes.def"
  }
  llvm_unreachable("unknown kind!");
}
SourceLoc TypeRepr::getEndLoc() const {
  switch (getKind()) {
#define TYPEREPR(CLASS, PARENT) \
  case TypeReprKind::CLASS: \
    return static_cast<const CLASS##TypeRepr*>(this)->getEndLocImpl();
#include "swift/AST/TypeReprNodes.def"
  }
  llvm_unreachable("unknown kind!");
}
SourceRange TypeRepr::getSourceRange() const {
  switch (getKind()) {
#define TYPEREPR(CLASS, PARENT) \
  case TypeReprKind::CLASS: { \
    auto Ty = static_cast<const CLASS##TypeRepr*>(this); \
    return SourceRange(Ty->getStartLocImpl(), Ty->getEndLocImpl()); \
  }
#include "swift/AST/TypeReprNodes.def"
  }
  llvm_unreachable("unknown kind!");
}

bool TypeRepr::findIf(llvm::function_ref<bool(TypeRepr *)> pred) {
  struct Walker : ASTWalker {
    llvm::function_ref<bool(TypeRepr *)> Pred;
    bool FoundIt;

    explicit Walker(llvm::function_ref<bool(TypeRepr *)> pred)
        : Pred(pred), FoundIt(false) {}

    /// Walk everything in a macro
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    PreWalkAction walkToTypeReprPre(TypeRepr *ty) override {
      if (Pred(ty)) {
        FoundIt = true;
        return Action::Stop();
      }
      return Action::Continue();
    }
  };

  Walker walker(pred);
  walk(walker);
  return walker.FoundIt;
}

// TODO [OPAQUE SUPPORT]: We should probably use something like `Type`'s
// `RecursiveProperties` to track this instead of computing it.
bool TypeRepr::hasOpaque() {
  return isa<NamedOpaqueReturnTypeRepr>(this) ||
    findIf([](TypeRepr *ty) { return isa<OpaqueReturnTypeRepr>(ty); });
}

TypeRepr *TypeRepr::getWithoutParens() const {
  auto *repr = const_cast<TypeRepr *>(this);
  while (auto *tupleRepr = dyn_cast<TupleTypeRepr>(repr)) {
    if (!tupleRepr->isParenType())
      break;
    repr = tupleRepr->getElementType(0);
  }
  return repr;
}

SourceLoc TypeRepr::findUncheckedAttrLoc() const {
  auto typeRepr = this;
  while (auto attrTypeRepr = dyn_cast<AttributedTypeRepr>(typeRepr)) {
    if (attrTypeRepr->getAttrs().has(TAK_unchecked)) {
      return attrTypeRepr->getAttrs().getLoc(TAK_unchecked);
    }

    typeRepr = attrTypeRepr->getTypeRepr();
  }

  return SourceLoc();
}

TypeDecl *DeclRefTypeRepr::getBoundDecl() const {
  return const_cast<DeclRefTypeRepr *>(this)
      ->getLastComponent()
      ->getBoundDecl();
}

DeclNameRef DeclRefTypeRepr::getNameRef() const {
  return const_cast<DeclRefTypeRepr *>(this)->getLastComponent()->getNameRef();
}

TypeRepr *DeclRefTypeRepr::getBaseComponent() {
  if (auto *ITR = dyn_cast<IdentTypeRepr>(this))
    return ITR;

  return cast<MemberTypeRepr>(this)->getBaseComponent();
}

IdentTypeRepr *DeclRefTypeRepr::getLastComponent() {
  if (auto *ITR = dyn_cast<IdentTypeRepr>(this))
    return ITR;

  return cast<MemberTypeRepr>(this)->getLastComponent();
}

DeclNameRef IdentTypeRepr::getNameRef() const {
  if (IdOrDecl.is<DeclNameRef>())
    return IdOrDecl.get<DeclNameRef>();

  return IdOrDecl.get<TypeDecl *>()->createNameRef();
}

static void printTypeRepr(const TypeRepr *TyR, ASTPrinter &Printer,
                          const PrintOptions &Opts) {
  if (TyR == nullptr)
    Printer << "<null>";
  else
    TyR->print(Printer, Opts);
}

void TypeRepr::print(raw_ostream &OS, const PrintOptions &Opts) const {
  StreamPrinter Printer(OS);
  print(Printer, Opts);
}

void TypeRepr::print(ASTPrinter &Printer, const PrintOptions &Opts) const {
  Printer.printTypePre(TypeLoc(const_cast<TypeRepr *>(this)));
  SWIFT_DEFER {
    Printer.printTypePost(TypeLoc(const_cast<TypeRepr *>(this)));
  };

  switch (getKind()) {
#define TYPEREPR(CLASS, PARENT) \
  case TypeReprKind::CLASS: { \
    auto Ty = static_cast<const CLASS##TypeRepr*>(this); \
    return Ty->printImpl(Printer, Opts); \
  }
#include "swift/AST/TypeReprNodes.def"
  }
  llvm_unreachable("unknown kind!");
}

void ErrorTypeRepr::printImpl(ASTPrinter &Printer,
                              const PrintOptions &Opts) const {
  Printer << "<<error type>>";
}

void AttributedTypeRepr::printImpl(ASTPrinter &Printer,
                                   const PrintOptions &Opts) const {
  printAttrs(Printer, Opts);
  printTypeRepr(Ty, Printer, Opts);
}

void AttributedTypeRepr::printAttrs(llvm::raw_ostream &OS) const {
  StreamPrinter Printer(OS);
  printAttrs(Printer, PrintOptions());
}

void AttributedTypeRepr::printAttrs(ASTPrinter &Printer,
                                    const PrintOptions &Options) const {
  const TypeAttributes &Attrs = getAttrs();

  auto hasAttr = [&](TypeAttrKind K) -> bool {
    if (Options.excludeAttrKind(K))
      return false;
    return Attrs.has(K);
  };

  if (hasAttr(TAK_autoclosure))
    Printer.printSimpleAttr("@autoclosure") << " ";
  if (hasAttr(TAK_escaping))
    Printer.printSimpleAttr("@escaping") << " ";

  for (auto customAttr : Attrs.getCustomAttrs()) {
    Printer.callPrintStructurePre(PrintStructureKind::BuiltinAttribute);
    Printer << "@";
    customAttr->getTypeRepr()->print(Printer, Options);
    Printer.printStructurePost(PrintStructureKind::BuiltinAttribute);
    Printer << " ";
  }

  if (hasAttr(TAK_Sendable))
    Printer.printSimpleAttr("@Sendable") << " ";
  if (hasAttr(TAK_noDerivative))
    Printer.printSimpleAttr("@noDerivative") << " ";

  if (hasAttr(TAK_differentiable)) {
    Printer.callPrintStructurePre(PrintStructureKind::BuiltinAttribute);
    Printer.printAttrName("@differentiable");
    switch (Attrs.differentiabilityKind) {
    case DifferentiabilityKind::Normal:
      break;
    case DifferentiabilityKind::Forward:
      Printer << "(_forward)";
      break;
    case DifferentiabilityKind::Reverse:
      Printer << "(reverse)";
      break;
    case DifferentiabilityKind::Linear:
      Printer << "(_linear)";
      break;
    case DifferentiabilityKind::NonDifferentiable:
      llvm_unreachable("Unexpected case 'NonDifferentiable'");
    }
    Printer << ' ';
    Printer.printStructurePost(PrintStructureKind::BuiltinAttribute);
  }

  if (hasAttr(TAK_thin))
    Printer.printSimpleAttr("@thin") << " ";
  if (hasAttr(TAK_thick))
    Printer.printSimpleAttr("@thick") << " ";

  if (hasAttr(TAK_convention) && Attrs.hasConvention()) {
    Printer.callPrintStructurePre(PrintStructureKind::BuiltinAttribute);
    Printer.printAttrName("@convention");
    SmallString<32> convention;
    Attrs.getConventionArguments(convention);
    Printer << "(" << convention << ")";
    Printer.printStructurePost(PrintStructureKind::BuiltinAttribute);
    Printer << " ";
  }

  if (hasAttr(TAK_async))
    Printer.printSimpleAttr("@async") << " ";
  if (hasAttr(TAK_opened))
    Printer.printSimpleAttr("@opened") << " ";

  if (hasAttr(TAK__noMetadata))
    Printer.printSimpleAttr("@_noMetadata") << " ";
}

static void printGenericArgs(ASTPrinter &Printer, const PrintOptions &Opts,
                             ArrayRef<TypeRepr *> Args) {
  if (Args.empty())
    return;

  Printer << "<";
  interleave(Args, [&](TypeRepr *Arg) { printTypeRepr(Arg, Printer, Opts); },
             [&] { Printer << ", "; });
  Printer << ">";
}

void IdentTypeRepr::printImpl(ASTPrinter &Printer,
                              const PrintOptions &Opts) const {
  if (auto *TD = dyn_cast_or_null<TypeDecl>(getBoundDecl())) {
    if (auto MD = dyn_cast<ModuleDecl>(TD))
      Printer.printModuleRef(MD, getNameRef().getBaseIdentifier());
    else
      Printer.printTypeRef(Type(), TD, getNameRef().getBaseIdentifier());
  } else {
    Printer.printName(getNameRef().getBaseIdentifier());
  }

  if (auto GenIdT = dyn_cast<GenericIdentTypeRepr>(this))
    printGenericArgs(Printer, Opts, GenIdT->getGenericArgs());
}

void MemberTypeRepr::printImpl(ASTPrinter &Printer,
                               const PrintOptions &Opts) const {
  printTypeRepr(getBaseComponent(), Printer, Opts);
  for (auto C : getMemberComponents()) {
    Printer << ".";
    printTypeRepr(C, Printer, Opts);
  }
}

void FunctionTypeRepr::printImpl(ASTPrinter &Printer,
                                 const PrintOptions &Opts) const {
  Printer.callPrintStructurePre(PrintStructureKind::FunctionType);
  printTypeRepr(ArgsTy, Printer, Opts);
  if (isAsync()) {
    Printer << " ";
    Printer.printKeyword("async", Opts);
  }
  if (isThrowing()) {
    Printer << " ";
    Printer.printKeyword("throws", Opts);
  }
  Printer << " -> ";
  Printer.callPrintStructurePre(PrintStructureKind::FunctionReturnType);
  printTypeRepr(RetTy, Printer, Opts);
  Printer.printStructurePost(PrintStructureKind::FunctionReturnType);
  Printer.printStructurePost(PrintStructureKind::FunctionType);
}

void ArrayTypeRepr::printImpl(ASTPrinter &Printer,
                              const PrintOptions &Opts) const {
  Printer << "[";
  printTypeRepr(getBase(), Printer, Opts);
  Printer << "]";
}

void DictionaryTypeRepr::printImpl(ASTPrinter &Printer,
                                   const PrintOptions &Opts) const {
  Printer << "[";
  printTypeRepr(Key, Printer, Opts);
  Printer << " : ";
  printTypeRepr(Value, Printer, Opts);
  Printer << "]";
}

void OptionalTypeRepr::printImpl(ASTPrinter &Printer,
                                 const PrintOptions &Opts) const {
  printTypeRepr(Base, Printer, Opts);
  Printer << "?";
}

void ImplicitlyUnwrappedOptionalTypeRepr::printImpl(ASTPrinter &Printer,
                                          const PrintOptions &Opts) const {
  printTypeRepr(Base, Printer, Opts);
  Printer << "!";
}

TupleTypeRepr::TupleTypeRepr(ArrayRef<TupleTypeReprElement> Elements,
                             SourceRange Parens)
    : TypeRepr(TypeReprKind::Tuple), Parens(Parens) {
  Bits.TupleTypeRepr.NumElements = Elements.size();

  std::uninitialized_copy(Elements.begin(), Elements.end(),
                          getTrailingObjects<TupleTypeReprElement>());
}

TupleTypeRepr *TupleTypeRepr::create(const ASTContext &C,
                                     ArrayRef<TupleTypeReprElement> Elements,
                                     SourceRange Parens) {
  size_t size =
    totalSizeToAlloc<TupleTypeReprElement>(Elements.size());
  void *mem = C.Allocate(size, alignof(TupleTypeRepr));
  return new (mem) TupleTypeRepr(Elements, Parens);
}

TupleTypeRepr *TupleTypeRepr::createEmpty(const ASTContext &C,
                                          SourceRange Parens) {
  return create(C, {}, Parens);
}

GenericIdentTypeRepr *GenericIdentTypeRepr::create(const ASTContext &C,
                                                   DeclNameLoc Loc,
                                                   DeclNameRef Id,
                                                ArrayRef<TypeRepr*> GenericArgs,
                                                   SourceRange AngleBrackets) {
  auto size = totalSizeToAlloc<TypeRepr*>(GenericArgs.size());
  auto mem = C.Allocate(size, alignof(GenericIdentTypeRepr));
  return new (mem) GenericIdentTypeRepr(Loc, Id, GenericArgs, AngleBrackets);
}

TypeRepr *MemberTypeRepr::create(const ASTContext &C, TypeRepr *Base,
                                 ArrayRef<IdentTypeRepr *> MemberComponents) {
  if (MemberComponents.empty())
    return Base;

  auto size = totalSizeToAlloc<IdentTypeRepr *>(MemberComponents.size());
  auto mem = C.Allocate(size, alignof(MemberTypeRepr));
  return new (mem) MemberTypeRepr(Base, MemberComponents);
}

DeclRefTypeRepr *MemberTypeRepr::create(const ASTContext &Ctx,
                                        ArrayRef<IdentTypeRepr *> Components) {
  return cast<DeclRefTypeRepr>(
      create(Ctx, Components.front(), Components.drop_front()));
}

PackTypeRepr::PackTypeRepr(SourceLoc keywordLoc, SourceRange braceLocs,
                           ArrayRef<TypeRepr*> elements)
  : TypeRepr(TypeReprKind::Pack),
    KeywordLoc(keywordLoc), BraceLocs(braceLocs) {
  Bits.PackTypeRepr.NumElements = elements.size();
  memcpy(getTrailingObjects<TypeRepr*>(), elements.data(),
         elements.size() * sizeof(TypeRepr*));
}

PackTypeRepr *PackTypeRepr::create(const ASTContext &ctx,
                                   SourceLoc keywordLoc,
                                   SourceRange braceLocs,
                                   ArrayRef<TypeRepr*> elements) {
  auto size = totalSizeToAlloc<TypeRepr*>(elements.size());
  auto mem = ctx.Allocate(size, alignof(PackTypeRepr));
  return new (mem) PackTypeRepr(keywordLoc, braceLocs, elements);
}

SILBoxTypeRepr *SILBoxTypeRepr::create(ASTContext &C,
                      GenericParamList *GenericParams,
                      SourceLoc LBraceLoc, ArrayRef<Field> Fields,
                      SourceLoc RBraceLoc,
                      SourceLoc ArgLAngleLoc, ArrayRef<TypeRepr *> GenericArgs,
                      SourceLoc ArgRAngleLoc) {
  auto size = totalSizeToAlloc<Field, TypeRepr*>(Fields.size(),
                                                 GenericArgs.size());
  auto mem = C.Allocate(size, alignof(SILBoxTypeRepr));
  return new (mem) SILBoxTypeRepr(GenericParams, LBraceLoc, Fields, RBraceLoc,
                                  ArgLAngleLoc, GenericArgs, ArgRAngleLoc);
}

SourceLoc FunctionTypeRepr::getStartLocImpl() const {
  return ArgsTy->getStartLoc();
}

SourceLoc SILBoxTypeRepr::getStartLocImpl() const {
  if (GenericParams && GenericParams->getSourceRange().isValid())
    return GenericParams->getSourceRange().Start;
  return LBraceLoc;
}
SourceLoc SILBoxTypeRepr::getEndLocImpl() const {
  if (ArgRAngleLoc.isValid())
    return ArgRAngleLoc;
  return RBraceLoc;
}
SourceLoc SILBoxTypeRepr::getLocImpl() const {
  return LBraceLoc;
}

void VarargTypeRepr::printImpl(ASTPrinter &Printer,
                               const PrintOptions &Opts) const {
  printTypeRepr(Element, Printer, Opts);
  Printer << "...";
}

void PackTypeRepr::printImpl(ASTPrinter &Printer,
                             const PrintOptions &Opts) const {
  Printer.printKeyword("Pack", Opts);
  Printer << "{";
  auto elts = getElements();
  for (size_t i = 0, e = elts.size(); i != e; ++i) {
    if (i) Printer << ", ";
    printTypeRepr(elts[i], Printer, Opts);
  }
  Printer << "}";
}

void PackExpansionTypeRepr::printImpl(ASTPrinter &Printer,
                                      const PrintOptions &Opts) const {
  Printer.printKeyword("repeat", Opts, /*Suffix=*/" ");
  printTypeRepr(Pattern, Printer, Opts);
}

void PackElementTypeRepr::printImpl(ASTPrinter &Printer,
                                    const PrintOptions &Opts) const {
  Printer.printKeyword("each", Opts, /*Suffix=*/" ");
  printTypeRepr(PackType, Printer, Opts);
}

void TupleTypeRepr::printImpl(ASTPrinter &Printer,
                              const PrintOptions &Opts) const {
  Printer.callPrintStructurePre(PrintStructureKind::TupleType);
  SWIFT_DEFER { Printer.printStructurePost(PrintStructureKind::TupleType); };

  Printer << "(";

  for (unsigned i = 0, e = Bits.TupleTypeRepr.NumElements; i != e; ++i) {
    if (i) Printer << ", ";
    Printer.callPrintStructurePre(PrintStructureKind::TupleElement);
    auto name = getElementName(i);
    if (isNamedParameter(i)) {
      // Printing empty Identifier is same as printing '_'.
      Printer.printName(Identifier(),
                        PrintNameContext::FunctionParameterExternal);
      if (!name.empty()) {
        Printer << " ";
        Printer.printName(name, PrintNameContext::FunctionParameterLocal);
      }
      Printer << ": ";
    } else {
      if (!name.empty()) {
        Printer.printName(name, PrintNameContext::TupleElement);
        Printer << ": ";
      }
    }
    printTypeRepr(getElementType(i), Printer, Opts);
    Printer.printStructurePost(PrintStructureKind::TupleElement);
  }

  Printer << ")";
}

CompositionTypeRepr *CompositionTypeRepr::create(const ASTContext &C,
                                                 ArrayRef<TypeRepr *> Types,
                                                 SourceLoc FirstTypeLoc,
                                                 SourceRange CompositionRange) {
  auto size = totalSizeToAlloc<TypeRepr*>(Types.size());
  auto mem = C.Allocate(size, alignof(CompositionTypeRepr));
  return new (mem) CompositionTypeRepr(Types, FirstTypeLoc, CompositionRange);
}

void CompositionTypeRepr::printImpl(ASTPrinter &Printer,
                                    const PrintOptions &Opts) const {
  if (getTypes().empty()) {
    Printer.printKeyword("Any", Opts);
  } else {
    interleave(getTypes(), [&](TypeRepr *T) { printTypeRepr(T, Printer, Opts);},
               [&] { Printer << " & "; });
  }
}

void MetatypeTypeRepr::printImpl(ASTPrinter &Printer,
                                 const PrintOptions &Opts) const {
  printTypeRepr(Base, Printer, Opts);
  Printer << ".Type";
}

void ProtocolTypeRepr::printImpl(ASTPrinter &Printer,
                                 const PrintOptions &Opts) const {
  printTypeRepr(Base, Printer, Opts);
  Printer << ".Protocol";
}

void OpaqueReturnTypeRepr::printImpl(ASTPrinter &Printer,
                                     const PrintOptions &Opts) const {
  Printer.printKeyword("some", Opts, /*Suffix=*/" ");
  printTypeRepr(Constraint, Printer, Opts);
}

void ExistentialTypeRepr::printImpl(ASTPrinter &Printer,
                                    const PrintOptions &Opts) const {
  Printer.printKeyword("any", Opts, /*Suffix=*/" ");
  printTypeRepr(Constraint, Printer, Opts);
}

SourceLoc NamedOpaqueReturnTypeRepr::getStartLocImpl() const {
  return GenericParams->getLAngleLoc();
}

SourceLoc NamedOpaqueReturnTypeRepr::getEndLocImpl() const {
  return Base->getEndLoc();
}

SourceLoc NamedOpaqueReturnTypeRepr::getLocImpl() const {
  return Base->getLoc();
}

void NamedOpaqueReturnTypeRepr::printImpl(ASTPrinter &Printer,
                                          const PrintOptions &Opts) const {
  GenericParams->print(Printer, Opts);
  Printer << ' ';
  printTypeRepr(Base, Printer, Opts);
}

void SpecifierTypeRepr::printImpl(ASTPrinter &Printer,
                                  const PrintOptions &Opts) const {
  switch (getKind()) {
#define TYPEREPR(CLASS, PARENT) case TypeReprKind::CLASS:
#define SPECIFIER_TYPEREPR(CLASS, PARENT)
#include "swift/AST/TypeReprNodes.def"
    llvm_unreachable("invalid repr kind");
    break;
  case TypeReprKind::Ownership: {
    auto ownershipRepr = cast<OwnershipTypeRepr>(this);
    Printer.printKeyword(ownershipRepr->getSpecifierSpelling(), Opts, " ");
    break;
  }
  case TypeReprKind::Isolated:
    Printer.printKeyword("isolated", Opts, " ");
    break;
  case TypeReprKind::CompileTimeConst:
    Printer.printKeyword("_const", Opts, " ");
    break;
  }
  printTypeRepr(Base, Printer, Opts);
}

StringRef OwnershipTypeRepr::getSpecifierSpelling() const {
  return ParamDecl::getSpecifierSpelling(getSpecifier());
}

ValueOwnership OwnershipTypeRepr::getValueOwnership() const {
  return ParamDecl::getValueOwnershipForSpecifier(getSpecifier());
}

void PlaceholderTypeRepr::printImpl(ASTPrinter &Printer,
                                    const PrintOptions &Opts) const {
  Printer.printText("_");
}

void FixedTypeRepr::printImpl(ASTPrinter &Printer,
                              const PrintOptions &Opts) const {
  getType().print(Printer, Opts);
}

void SelfTypeRepr::printImpl(ASTPrinter &Printer,
                             const PrintOptions &Opts) const {
  getType().print(Printer, Opts);
}

void SILBoxTypeRepr::printImpl(ASTPrinter &Printer,
                               const PrintOptions &Opts) const {
  // TODO
  Printer.printKeyword("sil_box", Opts);
}

// See swift/Basic/Statistic.h for declaration: this enables tracing
// TypeReprs, is defined here to avoid too much layering violation / circular
// linkage dependency.

struct TypeReprTraceFormatter : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const TypeRepr *TR = static_cast<const TypeRepr *>(Entity);
    TR->print(OS);
  }
  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const TypeRepr *TR = static_cast<const TypeRepr *>(Entity);
    TR->getSourceRange().print(OS, *SM, false);
  }
};

static TypeReprTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const TypeRepr *>() {
  return &TF;
}
