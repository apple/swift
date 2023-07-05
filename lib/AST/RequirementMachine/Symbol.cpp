//===--- Symbol.cpp - The generics rewrite system alphabet  ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>
#include "RewriteContext.h"
#include "Symbol.h"
#include "Term.h"

using namespace swift;
using namespace rewriting;

const StringRef Symbol::Kinds[] = {
  "conformance",
  "protocol",
  "assocty",
  "generic",
  "name",
  "layout",
  "super",
  "concrete"
};

/// Symbols are uniqued and immutable, stored as a single pointer;
/// the Storage type is the allocated backing storage.
struct Symbol::Storage final
  : public llvm::FoldingSetNode,
    public llvm::TrailingObjects<Storage, unsigned, Term> {
  friend class Symbol;

  Symbol::Kind Kind;
  const ProtocolDecl *Proto = nullptr;

  union {
    Identifier Name;
    CanType ConcreteType;
    LayoutConstraint Layout;
    GenericTypeParamType *GenericParam;
  };

  explicit Storage(Identifier name) {
    Kind = Symbol::Kind::Name;
    Name = name;
  }

  explicit Storage(LayoutConstraint layout) {
    Kind = Symbol::Kind::Layout;
    Layout = layout;
  }

  explicit Storage(const ProtocolDecl *proto) {
    Kind = Symbol::Kind::Protocol;
    Proto = proto;
  }

  explicit Storage(GenericTypeParamType *param) {
    Kind = Symbol::Kind::GenericParam;
    GenericParam = param;
  }

  /// A dummy type for overload resolution of the
  /// 'shape' constructor for Storage.
  struct ForShape {};

  explicit Storage(ForShape shape) {
    Kind = Kind::Shape;
  }

  Storage(const ProtocolDecl *proto, Identifier name) {
    Kind = Symbol::Kind::AssociatedType;
    Proto = proto;
    Name = name;
  }

  Storage(Symbol::Kind kind, CanType type, ArrayRef<Term> substitutions) {
    assert(kind == Symbol::Kind::Superclass ||
           kind == Symbol::Kind::ConcreteType);
    assert(!type->hasUnboundGenericType());
    assert(!type->hasTypeVariable());
    assert(type->hasTypeParameter() != substitutions.empty());

    Kind = kind;
    ConcreteType = type;

    *getTrailingObjects<unsigned>() = substitutions.size();

    for (unsigned i : indices(substitutions))
      getSubstitutions()[i] = substitutions[i];
  }

  Storage(CanType type, ArrayRef<Term> substitutions, const ProtocolDecl *proto) {
    assert(!type->hasTypeVariable());
    assert(type->hasTypeParameter() != substitutions.empty());

    Kind = Symbol::Kind::ConcreteConformance;
    Proto = proto;

    *getTrailingObjects<unsigned>() = substitutions.size();
    ConcreteType = type;

    for (unsigned i : indices(substitutions))
      getSubstitutions()[i] = substitutions[i];
  }

  size_t numTrailingObjects(OverloadToken<unsigned>) const {
    return (Kind == Symbol::Kind::Superclass ||
            Kind == Symbol::Kind::ConcreteType ||
            Kind == Symbol::Kind::ConcreteConformance);
  }

  size_t numTrailingObjects(OverloadToken<Term>) const {
    return getNumSubstitutions();
  }

  unsigned getNumSubstitutions() const {
    assert(numTrailingObjects(OverloadToken<unsigned>()) == 1);
    return *getTrailingObjects<unsigned>();
  }

  MutableArrayRef<Term> getSubstitutions() {
    return {getTrailingObjects<Term>(), getNumSubstitutions()};
  }

  ArrayRef<Term> getSubstitutions() const {
    return {getTrailingObjects<Term>(), getNumSubstitutions()};
  }

  void Profile(llvm::FoldingSetNodeID &id) const;
};

Symbol::Kind Symbol::getKind() const {
  return Ptr->Kind;
}

/// Get the identifier associated with an unbound name symbol or an
/// associated type symbol.
Identifier Symbol::getName() const {
  assert(getKind() == Kind::Name ||
         getKind() == Kind::AssociatedType);
  return Ptr->Name;
}

/// Get the protocol declaration associated with a protocol or associated type
/// symbol.
const ProtocolDecl *Symbol::getProtocol() const {
  assert(getKind() == Kind::Protocol ||
         getKind() == Kind::AssociatedType ||
         getKind() == Kind::ConcreteConformance);
  return Ptr->Proto;
}

/// Get the generic parameter associated with a generic parameter symbol.
GenericTypeParamType *Symbol::getGenericParam() const {
  assert(getKind() == Kind::GenericParam);
  return Ptr->GenericParam;
}

/// Get the layout constraint associated with a layout constraint symbol.
LayoutConstraint Symbol::getLayoutConstraint() const {
  assert(getKind() == Kind::Layout);
  return Ptr->Layout;
}

/// Get the concrete type associated with a superclass, concrete type or
/// concrete conformance symbol.
CanType Symbol::getConcreteType() const {
  assert(getKind() == Kind::Superclass ||
         getKind() == Kind::ConcreteType ||
         getKind() == Kind::ConcreteConformance);
  return Ptr->ConcreteType;
}

/// Get the list of substitution terms associated with a superclass,
/// concrete type or concrete conformance symbol.
ArrayRef<Term> Symbol::getSubstitutions() const {
  return Ptr->getSubstitutions();
}

/// Creates a new name symbol.
Symbol Symbol::forName(Identifier name,
                       RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Name));
  id.AddPointer(name.get());

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(name);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::Name));

  return symbol;
}

/// Creates a new protocol symbol.
Symbol Symbol::forProtocol(const ProtocolDecl *proto,
                           RewriteContext &ctx) {
  assert(proto != nullptr);

  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Protocol));
  id.AddPointer(proto);

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(proto);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::Protocol));

  return symbol;
}

/// Creates a new associated type symbol.
Symbol Symbol::forAssociatedType(const ProtocolDecl *proto,
                                 Identifier name,
                                 RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::AssociatedType));
  id.AddPointer(proto);
  id.AddPointer(name.get());

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(proto, name);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::AssociatedType));

  return symbol;
}

/// Creates a generic parameter symbol, representing a generic
/// parameter in the top-level generic signature from which the
/// rewrite system is built.
Symbol Symbol::forGenericParam(GenericTypeParamType *param,
                               RewriteContext &ctx) {
  assert(param->isCanonical());

  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::GenericParam));
  id.AddPointer(param);

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(param);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::GenericParam));

  return symbol;
}

Symbol Symbol::forShape(RewriteContext &ctx) {
  if (auto *symbol = ctx.TheShapeSymbol)
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(Storage::ForShape());
  return (ctx.TheShapeSymbol = symbol);
}

/// Creates a layout symbol, representing a layout constraint.
Symbol Symbol::forLayout(LayoutConstraint layout,
                         RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Layout));
  id.AddPointer(layout.getPointer());

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(layout);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::Layout));

  return symbol;
}

/// Creates a superclass symbol, representing a superclass constraint.
Symbol Symbol::forSuperclass(CanType type, ArrayRef<Term> substitutions,
                             RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Superclass));
  id.AddPointer(type.getPointer());
  id.AddInteger(unsigned(substitutions.size()));

  for (auto substitution : substitutions)
    id.AddPointer(substitution.getOpaquePointer());

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(
      1, substitutions.size());
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(Kind::Superclass, type, substitutions);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::Superclass));

  return symbol;
}

/// Creates a concrete type symbol, representing a superclass constraint.
Symbol Symbol::forConcreteType(CanType type, ArrayRef<Term> substitutions,
                               RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::ConcreteType));
  id.AddPointer(type.getPointer());
  id.AddInteger(unsigned(substitutions.size()));
  for (auto substitution : substitutions)
    id.AddPointer(substitution.getOpaquePointer());

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(
      1, substitutions.size());
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(Kind::ConcreteType, type, substitutions);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::ConcreteType));

  return symbol;
}

/// Creates a concrete type symbol, representing a superclass constraint.
Symbol Symbol::forConcreteConformance(CanType type,
                                      ArrayRef<Term> substitutions,
                                      const ProtocolDecl *proto,
                                      RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::ConcreteConformance));
  id.AddPointer(proto);
  id.AddPointer(type.getPointer());
  id.AddInteger(unsigned(substitutions.size()));
  for (auto substitution : substitutions)
    id.AddPointer(substitution.getOpaquePointer());

  void *insertPos = nullptr;
  if (auto *symbol = ctx.Symbols.FindNodeOrInsertPos(id, insertPos))
    return symbol;

  unsigned size = Storage::totalSizeToAlloc<unsigned, Term>(
      1, substitutions.size());
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *symbol = new (mem) Storage(type, substitutions, proto);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  symbol->Profile(newID);
  assert(id == newID);
#endif

  ctx.Symbols.InsertNode(symbol, insertPos);
  ctx.SymbolHistogram.add(unsigned(Kind::ConcreteConformance));

  return symbol;
}

/// Given that this symbol is the first symbol of a term, return the
/// "domain" of the term.
///
/// - If the first symbol is a protocol symbol [P] or associated type
/// symbol [P:T], the domain is P.
/// - If the first symbol is a generic parameter symbol, the domain is
///   nullptr.
/// - Anything else will assert.
const ProtocolDecl *Symbol::getRootProtocol() const {
  switch (getKind()) {
  case Symbol::Kind::Protocol:
  case Symbol::Kind::AssociatedType:
    return getProtocol();

  case Symbol::Kind::GenericParam:
    return nullptr;

  case Symbol::Kind::Name:
  case Symbol::Kind::Layout:
  case Symbol::Kind::Superclass:
  case Symbol::Kind::ConcreteType:
  case Symbol::Kind::ConcreteConformance:
  case Symbol::Kind::Shape:
    break;
  }

  llvm_unreachable("Bad root symbol");
}

/// Linear order on symbols, returning -1, 0, 1 or None if the symbols are
/// incomparable.
///
/// First, we order different kinds as follows, from smallest to largest:
///
/// - ConcreteConformance
/// - Protocol
/// - AssociatedType
/// - GenericParam
/// - Name
/// - Layout
/// - Superclass
/// - ConcreteType
///
/// Then we break ties when both symbols have the same kind as follows:
///
/// * For associated type symbols, we compare the name first, followed by
///   the protocols, which are compared just like protocol symbols,
///   described below.
///
/// * For generic parameter symbols, we first order by depth, then index.
///
/// * For unbound name symbols, we compare identifiers lexicographically.
///
/// * For protocol symbols, protocols with more inherited protocols are ordered
///   before those with fewer inherited protocols. The type order defined in
///   TypeDecl::compare() is used to break ties; based on the protocol name
///   and parent module.
///
/// * For layout symbols, we use LayoutConstraint::compare().
///
/// * For concrete conformance symbols with distinct protocols, we compare
///   the protocols.
///
/// All other symbol kinds are incomparable, in which case we return None.
llvm::Optional<int> Symbol::compare(Symbol other, RewriteContext &ctx) const {
  // Exit early if the symbols are equal.
  if (Ptr == other.Ptr)
    return 0;

  auto kind = getKind();
  auto otherKind = other.getKind();

  if (kind != otherKind)
    return int(kind) < int(otherKind) ? -1 : 1;

  int result = 0;

  switch (kind) {
  case Kind::Name:
    result = getName().compare(other.getName());
    break;

  case Kind::Protocol:
    result = ctx.compareProtocols(getProtocol(), other.getProtocol());
    break;

  case Kind::AssociatedType: {
    if (getName() != other.getName())
      return getName().compare(other.getName());

    result = ctx.compareProtocols(getProtocol(), other.getProtocol());
    break;
  }

  case Kind::Shape:
  case Kind::GenericParam: {
    auto *param = getGenericParam();
    auto *otherParam = other.getGenericParam();

    if (param->getDepth() != otherParam->getDepth())
      return param->getDepth() < otherParam->getDepth() ? -1 : 1;

    if (param->getIndex() != otherParam->getIndex())
      return param->getIndex() < otherParam->getIndex() ? -1 : 1;

    break;
  }

  case Kind::Layout:
    result = getLayoutConstraint().compare(other.getLayoutConstraint());
    break;

  case Kind::ConcreteConformance: {
    auto *proto = getProtocol();
    auto *otherProto = other.getProtocol();

    // For concrete conformance symbols, order by protocol first.
    result = ctx.compareProtocols(proto, otherProto);
    if (result != 0)
      return result;

    // Then, check if they have the same concrete type and order
    // substitutions.
    LLVM_FALLTHROUGH;
  }

  case Kind::Superclass:
  case Kind::ConcreteType: {
    if (getConcreteType() == other.getConcreteType()) {
      // If the concrete types are identical, compare substitution terms.
      assert(getSubstitutions().size() == other.getSubstitutions().size());
      for (unsigned i : indices(getSubstitutions())) {
        auto term = getSubstitutions()[i];
        auto otherTerm = other.getSubstitutions()[i];

        llvm::Optional<int> result = term.compare(otherTerm, ctx);
        if (!result.has_value() || *result != 0)
          return result;
      }

      break;
    }

    // We don't support comparing arbitrary concrete types.
    return llvm::None;
  }
  }

  if (result == 0) {
    llvm::errs() << "Two distinct symbols should not compare equal\n";
    llvm::errs() << "LHS: " << *this << "\n";
    llvm::errs() << "RHS: " << other << "\n";
    abort();
  }

  return result;
}

Symbol Symbol::withConcreteSubstitutions(
    ArrayRef<Term> substitutions,
    RewriteContext &ctx) const {
  switch (getKind()) {
  case Kind::Superclass:
    return Symbol::forSuperclass(getConcreteType(), substitutions, ctx);

  case Kind::ConcreteType:
    return Symbol::forConcreteType(getConcreteType(), substitutions, ctx);

  case Kind::ConcreteConformance:
    return Symbol::forConcreteConformance(getConcreteType(), substitutions,
                                          getProtocol(), ctx);

  case Kind::GenericParam:
  case Kind::Name:
  case Kind::Protocol:
  case Kind::AssociatedType:
  case Kind::Shape:
  case Kind::Layout:
    break;
  }

  llvm::errs() << "Bad symbol kind: " << *this << "\n";
  abort();
}

/// For a superclass or concrete type symbol
///
///   [concrete: Foo<X1, ..., Xn>]
///   [superclass: Foo<X1, ..., Xn>]
///
/// Return a new symbol where the function fn is applied to each of the
/// substitutions:
///
///   [concrete: Foo<fn(X1), ..., fn(Xn)>]
///   [superclass: Foo<fn(X1), ..., fn(Xn)>]
///
/// Asserts if this is not a superclass or concrete type symbol.
Symbol Symbol::transformConcreteSubstitutions(
    llvm::function_ref<Term(Term)> fn,
    RewriteContext &ctx) const {
  assert(hasSubstitutions());

  if (getSubstitutions().empty())
    return *this;

  bool anyChanged = false;
  SmallVector<Term, 2> substitutions;
  for (auto term : getSubstitutions()) {
    auto newTerm = fn(term);
    if (newTerm != term)
      anyChanged = true;

    substitutions.push_back(newTerm);
  }

  if (!anyChanged)
    return *this;

  return withConcreteSubstitutions(substitutions, ctx);
}

/// Print the symbol using our mnemonic representation.
void Symbol::dump(llvm::raw_ostream &out) const {
  llvm::DenseMap<CanType, Identifier> substitutionNames;
  if (hasSubstitutions()) {
    auto &ctx = getConcreteType()->getASTContext();

    for (unsigned index : indices(getSubstitutions())) {
      Term substitution = getSubstitutions()[index];

      std::string s;
      llvm::raw_string_ostream os(s);
      os << substitution;

      auto key = CanType(GenericTypeParamType::get(
          /*isParameterPack=*/false, 0, index, ctx));
      substitutionNames[key] = ctx.getIdentifier(s);
    }
  }

  PrintOptions opts;
  opts.AlternativeTypeNames = &substitutionNames;
  opts.OpaqueReturnTypePrinting =
      PrintOptions::OpaqueReturnTypePrintingMode::StableReference;

  switch (getKind()) {
  case Kind::Name:
    out << getName();
    return;

  case Kind::Protocol:
    out << "[" << getProtocol()->getName() << "]";
    return;

  case Kind::AssociatedType: {
    out << "[" << getProtocol()->getName() << ":" << getName() << "]";
    return;
  }

  case Kind::GenericParam: {
    out << Type(getGenericParam());
    return;
  }

  case Kind::Layout:
    out << "[layout: ";
    getLayoutConstraint()->print(out);
    out << "]";
    return;

  case Kind::Superclass:
    out << "[superclass: ";
    getConcreteType().print(out, opts);
    out << "]";
    return;

  case Kind::ConcreteType:
    out << "[concrete: ";
    getConcreteType().print(out, opts);
    out << "]";
    return;

  case Kind::ConcreteConformance:
    out << "[concrete: ";
    getConcreteType().print(out, opts);
    out << " : ";
    out << getProtocol()->getName();
    out << "]";
    return;

  case Kind::Shape: {
    out << "[shape]";
    return;
  }
  }

  llvm_unreachable("Bad symbol kind");
}

void Symbol::Storage::Profile(llvm::FoldingSetNodeID &id) const {
  id.AddInteger(unsigned(Kind));

  switch (Kind) {
  case Symbol::Kind::Name:
    id.AddPointer(Name.get());
    return;

  case Symbol::Kind::Layout:
    id.AddPointer(Layout.getPointer());
    return;

  case Symbol::Kind::Protocol:
    id.AddPointer(Proto);
    return;

  case Symbol::Kind::GenericParam:
    id.AddPointer(GenericParam);
    return;

  case Symbol::Kind::Shape:
    // Nothing more to add.
    return;

  case Symbol::Kind::AssociatedType: {
    id.AddPointer(Proto);
    id.AddPointer(Name.get());
    return;
  }

  case Symbol::Kind::Superclass:
  case Symbol::Kind::ConcreteType: {
    id.AddPointer(ConcreteType.getPointer());
    id.AddInteger(getNumSubstitutions());
    for (auto term : getSubstitutions())
      id.AddPointer(term.getOpaquePointer());

    return;
  }

  case Symbol::Kind::ConcreteConformance: {
    id.AddPointer(Proto);
    id.AddPointer(ConcreteType.getPointer());

    id.AddInteger(getNumSubstitutions());
    for (auto term : getSubstitutions())
      id.AddPointer(term.getOpaquePointer());

    return;
  }
  }

  llvm_unreachable("Bad symbol kind");
}
