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
#include "swift/Basic/Assertions.h"
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

bool TypeRepr::isParenType() const {
  auto *tuple = dyn_cast<TupleTypeRepr>(this);
  return tuple && tuple->isParenType();
}

TypeRepr *TypeRepr::getWithoutParens() const {
  auto *result = this;
  while (result->isParenType()) {
    result = cast<TupleTypeRepr>(result)->getElementType(0);
  }

  return const_cast<TypeRepr *>(result);
}

bool TypeRepr::isSimpleUnqualifiedIdentifier(Identifier identifier) const {
  if (auto *unqualIdentTR = dyn_cast<UnqualifiedIdentTypeRepr>(this)) {
    if (!unqualIdentTR->hasGenericArgList() &&
        unqualIdentTR->getNameRef().getBaseIdentifier() == identifier) {
      return true;
    }
  }

  return false;
}

bool TypeRepr::isSimpleUnqualifiedIdentifier(StringRef str) const {
  if (auto *unqualIdentTR = dyn_cast<UnqualifiedIdentTypeRepr>(this)) {
    if (!unqualIdentTR->hasGenericArgList() &&
        unqualIdentTR->getNameRef().getBaseIdentifier().is(str)) {
      return true;
    }
  }

  return false;
}

SourceLoc TypeRepr::findAttrLoc(TypeAttrKind kind) const {
  auto typeRepr = this;
  while (auto attrTypeRepr = dyn_cast<AttributedTypeRepr>(typeRepr)) {
    for (auto attr : attrTypeRepr->getAttrs()) {
      if (auto typeAttr = attr.dyn_cast<TypeAttribute*>())
        if (typeAttr->getKind() == kind)
          return typeAttr->getStartLoc();
    }

    typeRepr = attrTypeRepr->getTypeRepr();
  }

  return SourceLoc();
}

DeclRefTypeRepr::DeclRefTypeRepr(TypeReprKind K, DeclNameRef Name,
                                 DeclNameLoc NameLoc, unsigned NumGenericArgs,
                                 bool HasAngleBrackets)
    : TypeRepr(K), NameLoc(NameLoc), NameOrDecl(Name), DC(nullptr) {
  assert(Name.isSimpleName() && !Name.isSpecial() && !Name.isOperator());
  Bits.DeclRefTypeRepr.HasAngleBrackets = HasAngleBrackets;
  Bits.DeclRefTypeRepr.NumGenericArgs = NumGenericArgs;
}

DeclRefTypeRepr *DeclRefTypeRepr::create(const ASTContext &C, TypeRepr *Base,
                                         DeclNameLoc NameLoc,
                                         DeclNameRef Name) {
  if (Base) {
    return QualifiedIdentTypeRepr::create(C, Base, NameLoc, Name);
  }

  return UnqualifiedIdentTypeRepr::create(C, NameLoc, Name);
}

DeclRefTypeRepr *DeclRefTypeRepr::create(const ASTContext &C, TypeRepr *Base,
                                         DeclNameLoc NameLoc, DeclNameRef Name,
                                         ArrayRef<TypeRepr *> GenericArgs,
                                         SourceRange AngleBrackets) {
  if (Base) {
    return QualifiedIdentTypeRepr::create(C, Base, NameLoc, Name, GenericArgs,
                                          AngleBrackets);
  }

  return UnqualifiedIdentTypeRepr::create(C, NameLoc, Name, GenericArgs,
                                          AngleBrackets);
}

TypeRepr *DeclRefTypeRepr::getBase() const {
  if (isa<UnqualifiedIdentTypeRepr>(this)) {
    return nullptr;
  }

  return cast<QualifiedIdentTypeRepr>(this)->getBase();
}

TypeRepr *DeclRefTypeRepr::getRoot() {
  return const_cast<TypeRepr *>(
      const_cast<const DeclRefTypeRepr *>(this)->getRoot());
}

const TypeRepr *DeclRefTypeRepr::getRoot() const {
  if (auto *UITR = dyn_cast<UnqualifiedIdentTypeRepr>(this))
    return UITR;

  return cast<QualifiedIdentTypeRepr>(this)->getRoot();
}

DeclNameLoc DeclRefTypeRepr::getNameLoc() const { return NameLoc; }

DeclNameRef DeclRefTypeRepr::getNameRef() const {
  if (NameOrDecl.is<DeclNameRef>())
    return NameOrDecl.get<DeclNameRef>();

  return NameOrDecl.get<TypeDecl *>()->createNameRef();
}

void DeclRefTypeRepr::overwriteNameRef(DeclNameRef newId) {
  assert(newId.isSimpleName() && !newId.isSpecial() && !newId.isOperator());
  NameOrDecl = newId;
}

bool DeclRefTypeRepr::isBound() const { return NameOrDecl.is<TypeDecl *>(); }

TypeDecl *DeclRefTypeRepr::getBoundDecl() const {
  return NameOrDecl.dyn_cast<TypeDecl *>();
}

DeclContext *DeclRefTypeRepr::getDeclContext() const {
  assert(isBound());
  return DC;
}

void DeclRefTypeRepr::setValue(TypeDecl *TD, DeclContext *DC) {
  NameOrDecl = TD;
  this->DC = DC;
}

unsigned DeclRefTypeRepr::getNumGenericArgs() const {
  return Bits.DeclRefTypeRepr.NumGenericArgs;
}

bool DeclRefTypeRepr::hasGenericArgList() const {
  return (getNumGenericArgs() != 0) || hasAngleBrackets();
}

ArrayRef<TypeRepr *> DeclRefTypeRepr::getGenericArgs() const {
  if (getNumGenericArgs() == 0) {
    return {};
  }

  if (auto *unqualIdentTR = dyn_cast<UnqualifiedIdentTypeRepr>(this)) {
    return unqualIdentTR->getGenericArgs();
  }

  return cast<QualifiedIdentTypeRepr>(this)->getGenericArgs();
}

bool DeclRefTypeRepr::hasAngleBrackets() const {
  return Bits.DeclRefTypeRepr.HasAngleBrackets;
}

SourceRange DeclRefTypeRepr::getAngleBrackets() const {
  if (!hasAngleBrackets()) {
    return SourceRange();
  }

  if (auto *unqualIdentTR = dyn_cast<UnqualifiedIdentTypeRepr>(this)) {
    return unqualIdentTR->getAngleBrackets();
  }

  return cast<QualifiedIdentTypeRepr>(this)->getAngleBrackets();
}

SourceLoc DeclRefTypeRepr::getLocImpl() const {
  return NameLoc.getBaseNameLoc();
}

SourceLoc DeclRefTypeRepr::getEndLocImpl() const {
  const auto range = getAngleBrackets();
  if (range.isValid()) {
    return range.End;
  }

  return getNameLoc().getEndLoc();
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

AttributedTypeRepr *AttributedTypeRepr::create(const ASTContext &C,
                                               ArrayRef<TypeOrCustomAttr> attrs,
                                               TypeRepr *ty) {
  size_t size = totalSizeToAlloc<TypeOrCustomAttr>(attrs.size());
  void *mem = C.Allocate(size, alignof(AttributedTypeRepr));
  return new (mem) AttributedTypeRepr(attrs, ty);
}

TypeAttribute *AttributedTypeRepr::get(TypeAttrKind kind) const {
  for (auto attr : getAttrs()) {
    auto typeAttr = attr.dyn_cast<TypeAttribute*>();
    if (typeAttr && typeAttr->getKind() == kind)
      return typeAttr;
  }
  return nullptr;
}

ReferenceOwnership AttributedTypeRepr::getSILOwnership() const {
  for (auto attr : getAttrs()) {
    auto typeAttr = attr.dyn_cast<TypeAttribute*>();
    if (!typeAttr) continue;
    switch (typeAttr->getKind()) {
#define REF_STORAGE(Name, name, ...)                                           \
  case TypeAttrKind::SIL##Name:                                                \
    return ReferenceOwnership::Name;
#include "swift/AST/ReferenceStorage.def"
    default: continue;
    }
  }
  return ReferenceOwnership::Strong;
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
  for (auto attr : getAttrs()) {
    if (auto customAttr = attr.dyn_cast<CustomAttr*>()) {
      Printer.callPrintStructurePre(PrintStructureKind::BuiltinAttribute);
      Printer << "@";
      customAttr->getTypeRepr()->print(Printer, Options);
      Printer.printStructurePost(PrintStructureKind::BuiltinAttribute);
    } else {
      auto typeAttr = attr.get<TypeAttribute*>();
      if (Options.excludeAttrKind(typeAttr->getKind()))
        continue;
      typeAttr->print(Printer, Options);
    }
    Printer << " ";
  }
}

void DeclRefTypeRepr::printImpl(ASTPrinter &Printer,
                                const PrintOptions &Opts) const {
  if (auto *qualIdentTR = dyn_cast<QualifiedIdentTypeRepr>(this)) {
    printTypeRepr(qualIdentTR->getBase(), Printer, Opts);
    Printer << ".";
  }

  if (auto *TD = dyn_cast_or_null<TypeDecl>(getBoundDecl())) {
    if (auto MD = dyn_cast<ModuleDecl>(TD))
      Printer.printModuleRef(MD, getNameRef().getBaseIdentifier());
    else
      Printer.printTypeRef(Type(), TD, getNameRef().getBaseIdentifier());
  } else {
    Printer.printName(getNameRef().getBaseIdentifier());
  }

  if (hasGenericArgList()) {
    Printer << "<";
    interleave(
        getGenericArgs(),
        [&](TypeRepr *Arg) { printTypeRepr(Arg, Printer, Opts); },
        [&] { Printer << ", "; });
    Printer << ">";
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

    if (ThrownTy) {
      // FIXME: Do we need a PrintStructureKind for this?
      Printer << "(";
      printTypeRepr(ThrownTy, Printer, Opts);
      Printer << ")";
    }
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

UnqualifiedIdentTypeRepr::UnqualifiedIdentTypeRepr(DeclNameRef Name,
                                                   DeclNameLoc NameLoc)
    : DeclRefTypeRepr(TypeReprKind::UnqualifiedIdent, Name, NameLoc,
                      /*NumGenericArgs=*/0,
                      /*HasAngleBrackets=*/false) {}

UnqualifiedIdentTypeRepr::UnqualifiedIdentTypeRepr(
    DeclNameRef Name, DeclNameLoc NameLoc, ArrayRef<TypeRepr *> GenericArgs,
    SourceRange AngleBrackets)
    : DeclRefTypeRepr(TypeReprKind::UnqualifiedIdent, Name, NameLoc,
                      /*NumGenericArgs=*/GenericArgs.size(),
                      /*HasAngleBrackets=*/AngleBrackets.isValid()) {
  if (AngleBrackets.isValid()) {
    *getTrailingObjects<SourceRange>() = AngleBrackets;
  }

#ifndef NDEBUG
  for (auto *repr : GenericArgs) {
    assert(repr);
  }
#endif

  if (!GenericArgs.empty()) {
    std::uninitialized_copy(GenericArgs.begin(), GenericArgs.end(),
                            getTrailingObjects<TypeRepr *>());
  }
}

UnqualifiedIdentTypeRepr *UnqualifiedIdentTypeRepr::create(const ASTContext &C,
                                                           DeclNameLoc NameLoc,
                                                           DeclNameRef Name) {
  return new (C) UnqualifiedIdentTypeRepr(Name, NameLoc);
}

UnqualifiedIdentTypeRepr *UnqualifiedIdentTypeRepr::create(
    const ASTContext &C, DeclNameLoc NameLoc, DeclNameRef Name,
    ArrayRef<TypeRepr *> GenericArgs, SourceRange AngleBrackets) {
  const auto size = totalSizeToAlloc<TypeRepr *, SourceRange>(
      GenericArgs.size(), AngleBrackets.isValid() ? 1 : 0);
  auto *mem = C.Allocate(size, alignof(UnqualifiedIdentTypeRepr));
  return new (mem)
      UnqualifiedIdentTypeRepr(Name, NameLoc, GenericArgs, AngleBrackets);
}

ArrayRef<TypeRepr *> UnqualifiedIdentTypeRepr::getGenericArgs() const {
  return {getTrailingObjects<TypeRepr *>(), getNumGenericArgs()};
}

SourceRange UnqualifiedIdentTypeRepr::getAngleBrackets() const {
  if (hasAngleBrackets()) {
    return *getTrailingObjects<SourceRange>();
  }

  return SourceRange();
}

QualifiedIdentTypeRepr::QualifiedIdentTypeRepr(TypeRepr *Base, DeclNameRef Name,
                                               DeclNameLoc NameLoc,
                                               ArrayRef<TypeRepr *> GenericArgs,
                                               SourceRange AngleBrackets)
    : DeclRefTypeRepr(TypeReprKind::QualifiedIdent, Name, NameLoc,
                      /*NumGenericArgs=*/GenericArgs.size(),
                      /*HasAngleBrackets=*/AngleBrackets.isValid()),
      Base(Base) {
  assert(Base);

  if (AngleBrackets.isValid()) {
    *getTrailingObjects<SourceRange>() = AngleBrackets;
  }

#ifndef NDEBUG
  for (auto *repr : GenericArgs) {
    assert(repr);
  }
#endif

  if (!GenericArgs.empty()) {
    std::uninitialized_copy(GenericArgs.begin(), GenericArgs.end(),
                            getTrailingObjects<TypeRepr *>());
  }
}

QualifiedIdentTypeRepr::QualifiedIdentTypeRepr(TypeRepr *Base, DeclNameRef Name,
                                               DeclNameLoc NameLoc)
    : DeclRefTypeRepr(TypeReprKind::QualifiedIdent, Name, NameLoc,
                      /*NumGenericArgs=*/0,
                      /*HasAngleBrackets=*/false),
      Base(Base) {
  assert(Base);
}

QualifiedIdentTypeRepr *QualifiedIdentTypeRepr::create(const ASTContext &C,
                                                       TypeRepr *Base,
                                                       DeclNameLoc NameLoc,
                                                       DeclNameRef Name) {
  return new (C) QualifiedIdentTypeRepr(Base, Name, NameLoc);
}

QualifiedIdentTypeRepr *QualifiedIdentTypeRepr::create(
    const ASTContext &C, TypeRepr *Base, DeclNameLoc NameLoc, DeclNameRef Name,
    ArrayRef<TypeRepr *> GenericArgs, SourceRange AngleBrackets) {
  const auto size = totalSizeToAlloc<TypeRepr *, SourceRange>(
      GenericArgs.size(), AngleBrackets.isValid() ? 1 : 0);
  auto *mem = C.Allocate(size, alignof(QualifiedIdentTypeRepr));
  return new (mem)
      QualifiedIdentTypeRepr(Base, Name, NameLoc, GenericArgs, AngleBrackets);
}

TypeRepr *QualifiedIdentTypeRepr::getBase() const { return Base; }

TypeRepr *QualifiedIdentTypeRepr::getRoot() const {
  auto *base = getBase();
  while (auto *memberTR = dyn_cast<QualifiedIdentTypeRepr>(base)) {
    base = memberTR->getBase();
  }

  return base;
}

ArrayRef<TypeRepr *> QualifiedIdentTypeRepr::getGenericArgs() const {
  return {getTrailingObjects<TypeRepr *>(), getNumGenericArgs()};
}

SourceRange QualifiedIdentTypeRepr::getAngleBrackets() const {
  if (hasAngleBrackets()) {
    return *getTrailingObjects<SourceRange>();
  }

  return SourceRange();
}

SourceLoc QualifiedIdentTypeRepr::getStartLocImpl() const {
  return getBase()->getStartLoc();
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

LifetimeDependentReturnTypeRepr *LifetimeDependentReturnTypeRepr::create(
    ASTContext &C, TypeRepr *base,
    ArrayRef<LifetimeDependenceSpecifier> specifiers) {
  auto size = totalSizeToAlloc<LifetimeDependenceSpecifier>(specifiers.size());
  auto mem = C.Allocate(size, alignof(LifetimeDependenceSpecifier));
  return new (mem) LifetimeDependentReturnTypeRepr(base, specifiers);
}

SourceLoc LifetimeDependentReturnTypeRepr::getStartLocImpl() const {
  return getLifetimeDependencies().front().getLoc();
}

SourceLoc LifetimeDependentReturnTypeRepr::getEndLocImpl() const {
  return getLifetimeDependencies().back().getLoc();
}

SourceLoc LifetimeDependentReturnTypeRepr::getLocImpl() const {
  return getBase()->getLoc();
}

void LifetimeDependentReturnTypeRepr::printImpl(
    ASTPrinter &Printer, const PrintOptions &Opts) const {
  Printer << " ";
  for (auto &dep : getLifetimeDependencies()) {
    Printer << dep.getLifetimeDependenceSpecifierString() << " ";
  }

  printTypeRepr(getBase(), Printer, Opts);
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

void InverseTypeRepr::printImpl(ASTPrinter &Printer,
                                const PrintOptions &Opts) const {
  Printer << "~";
  printTypeRepr(Constraint, Printer, Opts);
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
  case TypeReprKind::Transferring:
    Printer.printKeyword("transferring", Opts, " ");
    break;
  case TypeReprKind::Sending:
    Printer.printKeyword("sending", Opts, " ");
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

void ErrorTypeRepr::dischargeDiagnostic(swift::ASTContext &Context) {
  if (!DelayedDiag)
    return;

  // Consume and emit the diagnostic.
  Context.Diags.diagnose(Range.Start, *DelayedDiag).highlight(Range);
  DelayedDiag = std::nullopt;
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
