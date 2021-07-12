//===--- RewriteSystem.cpp - Generics with term rewriting -----------------===//
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

#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>

#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

/// Atoms are uniqued and immutable, stored as a single pointer;
/// the Storage type is the allocated backing storage.
struct Atom::Storage final
  : public llvm::FoldingSetNode,
    public llvm::TrailingObjects<Storage, const ProtocolDecl *, Term> {
  friend class Atom;

  unsigned Kind : 3;
  unsigned NumProtocols : 15;
  unsigned NumSubstitutions : 14;

  union {
    Identifier Name;
    CanType ConcreteType;
    LayoutConstraint Layout;
    const ProtocolDecl *Proto;
    GenericTypeParamType *GenericParam;
  };

  explicit Storage(Identifier name) {
    Kind = unsigned(Atom::Kind::Name);
    NumProtocols = 0;
    NumSubstitutions = 0;
    Name = name;
  }

  explicit Storage(LayoutConstraint layout) {
    Kind = unsigned(Atom::Kind::Layout);
    NumProtocols = 0;
    NumSubstitutions = 0;
    Layout = layout;
  }

  explicit Storage(const ProtocolDecl *proto) {
    Kind = unsigned(Atom::Kind::Protocol);
    NumProtocols = 0;
    NumSubstitutions = 0;
    Proto = proto;
  }

  explicit Storage(GenericTypeParamType *param) {
    Kind = unsigned(Atom::Kind::GenericParam);
    NumProtocols = 0;
    NumSubstitutions = 0;
    GenericParam = param;
  }

  Storage(ArrayRef<const ProtocolDecl *> protos, Identifier name) {
    assert(!protos.empty());

    Kind = unsigned(Atom::Kind::AssociatedType);
    NumProtocols = protos.size();
    assert(NumProtocols == protos.size() && "Overflow");
    NumSubstitutions = 0;
    Name = name;

    for (unsigned i : indices(protos))
      getProtocols()[i] = protos[i];
  }

  Storage(Atom::Kind kind, CanType type, ArrayRef<Term> substitutions) {
    assert(kind == Atom::Kind::Superclass ||
           kind == Atom::Kind::ConcreteType);
    assert(type->hasTypeParameter() != substitutions.empty());

    Kind = unsigned(kind);
    NumProtocols = 0;
    NumSubstitutions = substitutions.size();
    ConcreteType = type;

    for (unsigned i : indices(substitutions))
      getSubstitutions()[i] = substitutions[i];
  }

  size_t numTrailingObjects(OverloadToken<const ProtocolDecl *>) const {
    return NumProtocols;
  }

  size_t numTrailingObjects(OverloadToken<Term>) const {
    return NumSubstitutions;
  }

  MutableArrayRef<const ProtocolDecl *> getProtocols() {
    return {getTrailingObjects<const ProtocolDecl *>(), NumProtocols};
  }

  ArrayRef<const ProtocolDecl *> getProtocols() const {
    return {getTrailingObjects<const ProtocolDecl *>(), NumProtocols};
  }

  MutableArrayRef<Term> getSubstitutions() {
    return {getTrailingObjects<Term>(), NumSubstitutions};
  }

  ArrayRef<Term> getSubstitutions() const {
    return {getTrailingObjects<Term>(), NumSubstitutions};
  }

  void Profile(llvm::FoldingSetNodeID &id) const;
};

Atom::Kind Atom::getKind() const {
  return Kind(Ptr->Kind);
}

/// Get the identifier associated with an unbound name atom or an
/// associated type atom.
Identifier Atom::getName() const {
  assert(getKind() == Kind::Name ||
         getKind() == Kind::AssociatedType);
  return Ptr->Name;
}

/// Get the single protocol declaration associated with a protocol atom.
const ProtocolDecl *Atom::getProtocol() const {
  assert(getKind() == Kind::Protocol);
  return Ptr->Proto;
}

/// Get the list of protocols associated with an associated type atom.
ArrayRef<const ProtocolDecl *> Atom::getProtocols() const {
  assert(getKind() == Kind::AssociatedType);
  auto protos = Ptr->getProtocols();
  assert(!protos.empty());
  return protos;
}

/// Get the generic parameter associated with a generic parameter atom.
GenericTypeParamType *Atom::getGenericParam() const {
  assert(getKind() == Kind::GenericParam);
  return Ptr->GenericParam;
}

/// Get the layout constraint associated with a layout constraint atom.
LayoutConstraint Atom::getLayoutConstraint() const {
  assert(getKind() == Kind::Layout);
  return Ptr->Layout;
}

/// Get the superclass type associated with a superclass atom.
CanType Atom::getSuperclass() const {
  assert(getKind() == Kind::Superclass);
  return Ptr->ConcreteType;
}

/// Get the concrete type associated with a concrete type atom.
CanType Atom::getConcreteType() const {
  assert(getKind() == Kind::ConcreteType);
  return Ptr->ConcreteType;
}

ArrayRef<Term> Atom::getSubstitutions() const {
  assert(getKind() == Kind::Superclass ||
         getKind() == Kind::ConcreteType);
  return Ptr->getSubstitutions();
}

/// Creates a new name atom.
Atom Atom::forName(Identifier name,
                   RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Name));
  id.AddPointer(name.get());

  void *insertPos = nullptr;
  if (auto *atom = ctx.Atoms.FindNodeOrInsertPos(id, insertPos))
    return atom;

  unsigned size = Storage::totalSizeToAlloc<const ProtocolDecl *, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *atom = new (mem) Storage(name);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  atom->Profile(newID);
  assert(id == newID);
#endif

  ctx.Atoms.InsertNode(atom, insertPos);

  return atom;
}

/// Creates a new protocol atom.
Atom Atom::forProtocol(const ProtocolDecl *proto,
                       RewriteContext &ctx) {
  assert(proto != nullptr);

  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Protocol));
  id.AddPointer(proto);

  void *insertPos = nullptr;
  if (auto *atom = ctx.Atoms.FindNodeOrInsertPos(id, insertPos))
    return atom;

  unsigned size = Storage::totalSizeToAlloc<const ProtocolDecl *, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *atom = new (mem) Storage(proto);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  atom->Profile(newID);
  assert(id == newID);
#endif

  ctx.Atoms.InsertNode(atom, insertPos);

  return atom;
}

/// Creates a new associated type atom for a single protocol.
Atom Atom::forAssociatedType(const ProtocolDecl *proto,
                             Identifier name,
                             RewriteContext &ctx) {
  SmallVector<const ProtocolDecl *, 1> protos;
  protos.push_back(proto);

  return forAssociatedType(protos, name, ctx);
}

/// Creates a merged associated type atom to represent a nested
/// type that conforms to multiple protocols, all of which have
/// an associated type with the same name.
Atom Atom::forAssociatedType(ArrayRef<const ProtocolDecl *> protos,
                             Identifier name,
                             RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::AssociatedType));
  id.AddInteger(protos.size());
  for (const auto *proto : protos)
    id.AddPointer(proto);
  id.AddPointer(name.get());

  void *insertPos = nullptr;
  if (auto *atom = ctx.Atoms.FindNodeOrInsertPos(id, insertPos))
    return atom;

  unsigned size = Storage::totalSizeToAlloc<const ProtocolDecl *, Term>(
      protos.size(), 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *atom = new (mem) Storage(protos, name);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  atom->Profile(newID);
  assert(id == newID);
#endif

  ctx.Atoms.InsertNode(atom, insertPos);

  return atom;
}

/// Creates a generic parameter atom, representing a generic
/// parameter in the top-level generic signature from which the
/// rewrite system is built.
Atom Atom::forGenericParam(GenericTypeParamType *param,
                           RewriteContext &ctx) {
  assert(param->isCanonical());

  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::GenericParam));
  id.AddPointer(param);

  void *insertPos = nullptr;
  if (auto *atom = ctx.Atoms.FindNodeOrInsertPos(id, insertPos))
    return atom;

  unsigned size = Storage::totalSizeToAlloc<const ProtocolDecl *, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *atom = new (mem) Storage(param);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  atom->Profile(newID);
  assert(id == newID);
#endif

  ctx.Atoms.InsertNode(atom, insertPos);

  return atom;
}

/// Creates a layout atom, representing a layout constraint.
Atom Atom::forLayout(LayoutConstraint layout,
                     RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Layout));
  id.AddPointer(layout.getPointer());

  void *insertPos = nullptr;
  if (auto *atom = ctx.Atoms.FindNodeOrInsertPos(id, insertPos))
    return atom;

  unsigned size = Storage::totalSizeToAlloc<const ProtocolDecl *, Term>(0, 0);
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *atom = new (mem) Storage(layout);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  atom->Profile(newID);
  assert(id == newID);
#endif

  ctx.Atoms.InsertNode(atom, insertPos);

  return atom;
}

/// Creates a superclass atom, representing a superclass constraint.
Atom Atom::forSuperclass(CanType type, ArrayRef<Term> substitutions,
                         RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::Superclass));
  id.AddPointer(type.getPointer());
  id.AddInteger(unsigned(substitutions.size()));

  for (auto substitution : substitutions)
    id.AddPointer(substitution.getOpaquePointer());

  void *insertPos = nullptr;
  if (auto *atom = ctx.Atoms.FindNodeOrInsertPos(id, insertPos))
    return atom;

  unsigned size = Storage::totalSizeToAlloc<const ProtocolDecl *, Term>(
      0, substitutions.size());
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *atom = new (mem) Storage(Kind::Superclass, type, substitutions);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  atom->Profile(newID);
  assert(id == newID);
#endif

  ctx.Atoms.InsertNode(atom, insertPos);

  return atom;
}

/// Creates a concrete type atom, representing a superclass constraint.
Atom Atom::forConcreteType(CanType type, ArrayRef<Term> substitutions,
                           RewriteContext &ctx) {
  llvm::FoldingSetNodeID id;
  id.AddInteger(unsigned(Kind::ConcreteType));
  id.AddPointer(type.getPointer());
  id.AddInteger(unsigned(substitutions.size()));
  for (auto substitution : substitutions)
    id.AddPointer(substitution.getOpaquePointer());

  void *insertPos = nullptr;
  if (auto *atom = ctx.Atoms.FindNodeOrInsertPos(id, insertPos))
    return atom;

  unsigned size = Storage::totalSizeToAlloc<const ProtocolDecl *, Term>(
      0, substitutions.size());
  void *mem = ctx.Allocator.Allocate(size, alignof(Storage));
  auto *atom = new (mem) Storage(Kind::ConcreteType, type, substitutions);

#ifndef NDEBUG
  llvm::FoldingSetNodeID newID;
  atom->Profile(newID);
  assert(id == newID);
#endif

  ctx.Atoms.InsertNode(atom, insertPos);

  return atom;
}

/// Linear order on atoms.
///
/// First, we order different kinds as follows, from smallest to largest:
///
/// - Protocol
/// - AssociatedType
/// - GenericParam
/// - Name
/// - Layout
/// - Superclass
/// - ConcreteType
///
/// Then we break ties when both atoms have the same kind as follows:
///
/// * For associated type atoms, we first order the number of protocols,
///   with atoms containing more protocols coming first. This ensures
///   that the following holds:
///
///     [P1&P2:T] < [P1:T]
///     [P1&P2:T] < [P2:T]
///
///   If both atoms have the same number of protocols, we perform a
///   lexicographic comparison on the protocols pair-wise, using the
///   protocol order defined by \p graph (see
///   ProtocolGraph::compareProtocols()).
///
/// * For generic parameter atoms, we first order by depth, then index.
///
/// * For unbound name atoms, we compare identifiers lexicographically.
///
/// * For protocol atoms, we compare the protocols using the protocol
///   linear order on \p graph.
///
/// * For layout atoms, we use LayoutConstraint::compare().
int Atom::compare(Atom other, const ProtocolGraph &graph) const {
  // Exit early if the atoms are equal.
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
    result = graph.compareProtocols(getProtocol(), other.getProtocol());
    break;

  case Kind::AssociatedType: {
    auto protos = getProtocols();
    auto otherProtos = other.getProtocols();

    // Atoms with more protocols are 'smaller' than those with fewer.
    if (protos.size() != otherProtos.size())
      return protos.size() > otherProtos.size() ? -1 : 1;

    for (unsigned i : indices(protos)) {
      int result = graph.compareProtocols(protos[i], otherProtos[i]);
      if (result)
        return result;
    }

    result = getName().compare(other.getName());
    break;
  }

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

  case Kind::Superclass:
  case Kind::ConcreteType: {
    assert(false && "Cannot compare concrete types yet");
    break;
  }
  }

  assert(result != 0 && "Two distinct atoms should not compare equal");
  return result;
}

/// For a superclass or concrete type atom
///
///   [concrete: Foo<X1, ..., Xn>]
///   [superclass: Foo<X1, ..., Xn>]
///
/// Return a new atom where the function fn is applied to each of the
/// substitutions:
///
///   [concrete: Foo<fn(X1), ..., fn(Xn)>]
///   [superclass: Foo<fn(X1), ..., fn(Xn)>]
///
/// Asserts if this is not a superclass or concrete type atom.
Atom Atom::transformConcreteSubstitutions(
    llvm::function_ref<Term(Term)> fn,
    RewriteContext &ctx) const {
  assert(isSuperclassOrConcreteType());

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

  switch (getKind()) {
  case Kind::Superclass:
    return Atom::forSuperclass(getSuperclass(), substitutions, ctx);
  case Kind::ConcreteType:
    return Atom::forConcreteType(getConcreteType(), substitutions, ctx);

  case Kind::GenericParam:
  case Kind::Name:
  case Kind::Protocol:
  case Kind::AssociatedType:
  case Kind::Layout:
    break;
  }

  llvm_unreachable("Bad atom kind");
}

/// Print the atom using our mnemonic representation.
void Atom::dump(llvm::raw_ostream &out) const {
  auto dumpSubstitutions = [&]() {
    if (getSubstitutions().size() > 0) {
      out << " with <";

      bool first = true;
      for (auto substitution : getSubstitutions()) {
        if (first) {
          first = false;
        } else {
          out << ", ";
        }
        substitution.dump(out);
      }

      out << ">";
    }
  };

  switch (getKind()) {
  case Kind::Name:
    out << getName();
    return;

  case Kind::Protocol:
    out << "[" << getProtocol()->getName() << "]";
    return;

  case Kind::AssociatedType: {
    out << "[";
    bool first = true;
    for (const auto *proto : getProtocols()) {
      if (first) {
        first = false;
      } else {
        out << "&";
      }
      out << proto->getName();
    }
    out << ":" << getName() << "]";
    return;
  }

  case Kind::GenericParam:
    out << Type(getGenericParam());
    return;

  case Kind::Layout:
    out << "[layout: ";
    getLayoutConstraint()->print(out);
    out << "]";
    return;

  case Kind::Superclass:
    out << "[superclass: " << getSuperclass();
    dumpSubstitutions();
    out << "]";
    return;

  case Kind::ConcreteType:
    out << "[concrete: " << getConcreteType();
    dumpSubstitutions();
    out << "]";
    return;
  }

  llvm_unreachable("Bad atom kind");
}

void Atom::Storage::Profile(llvm::FoldingSetNodeID &id) const {
  id.AddInteger(Kind);

  switch (Atom::Kind(Kind)) {
  case Atom::Kind::Name:
    id.AddPointer(Name.get());
    return;

  case Atom::Kind::Layout:
    id.AddPointer(Layout.getPointer());
    return;

  case Atom::Kind::Protocol:
    id.AddPointer(Proto);
    return;

  case Atom::Kind::GenericParam:
    id.AddPointer(GenericParam);
    return;

  case Atom::Kind::AssociatedType: {
    auto protos = getProtocols();
    id.AddInteger(protos.size());

    for (const auto *proto : protos)
      id.AddPointer(proto);

    id.AddPointer(Name.get());
    return;
  }

  case Atom::Kind::Superclass:
  case Atom::Kind::ConcreteType: {
    id.AddPointer(ConcreteType.getPointer());

    id.AddInteger(NumSubstitutions);
    for (auto term : getSubstitutions())
      id.AddPointer(term.getOpaquePointer());

    return;
  }
  }

  llvm_unreachable("Bad atom kind");
}

/// Terms are uniqued and immutable, stored as a single pointer;
/// the Storage type is the allocated backing storage.
struct Term::Storage final
  : public llvm::FoldingSetNode,
    public llvm::TrailingObjects<Storage, Atom> {
  friend class Atom;

  unsigned Size;

  explicit Storage(unsigned size) : Size(size) {}

  size_t numTrailingObjects(OverloadToken<Atom>) const {
    return Size;
  }

  MutableArrayRef<Atom> getElements() {
    return {getTrailingObjects<Atom>(), Size};
  }

  ArrayRef<Atom> getElements() const {
    return {getTrailingObjects<Atom>(), Size};
  }

  void Profile(llvm::FoldingSetNodeID &id) const;
};

size_t Term::size() const { return Ptr->Size; }

ArrayRef<Atom>::iterator Term::begin() const {
  return Ptr->getElements().begin();
}

ArrayRef<Atom>::iterator Term::end() const {
  return Ptr->getElements().end();
}

ArrayRef<Atom>::reverse_iterator Term::rbegin() const {
  return Ptr->getElements().rbegin();
}

ArrayRef<Atom>::reverse_iterator Term::rend() const {
  return Ptr->getElements().rend();
}

Atom Term::back() const {
  return Ptr->getElements().back();
}

Atom Term::operator[](size_t index) const {
  return Ptr->getElements()[index];
}

void Term::dump(llvm::raw_ostream &out) const {
  MutableTerm(*this).dump(out);
}

Term Term::get(const MutableTerm &mutableTerm, RewriteContext &ctx) {
  unsigned size = mutableTerm.size();
  assert(size > 0 && "Term must have at least one atom");

  llvm::FoldingSetNodeID id;
  id.AddInteger(size);
  for (auto atom : mutableTerm)
    id.AddPointer(atom.getOpaquePointer());

  void *insertPos = nullptr;
  if (auto *term = ctx.Terms.FindNodeOrInsertPos(id, insertPos))
    return term;

  void *mem = ctx.Allocator.Allocate(
      Storage::totalSizeToAlloc<Atom>(size),
      alignof(Storage));
  auto *term = new (mem) Storage(size);
  for (unsigned i = 0; i < size; ++i)
    term->getElements()[i] = mutableTerm[i];

  ctx.Terms.InsertNode(term, insertPos);

  return term;
}

void Term::Storage::Profile(llvm::FoldingSetNodeID &id) const {
  id.AddInteger(Size);

  for (auto atom : getElements())
    id.AddPointer(atom.getOpaquePointer());
}

/// Linear order on terms.
///
/// First we compare length, then perform a lexicographic comparison
/// on atoms if the two terms have the same length.
int MutableTerm::compare(const MutableTerm &other,
                         const ProtocolGraph &graph) const {
  if (size() != other.size())
    return size() < other.size() ? -1 : 1;

  for (unsigned i = 0, e = size(); i < e; ++i) {
    auto lhs = (*this)[i];
    auto rhs = other[i];

    int result = lhs.compare(rhs, graph);
    if (result != 0) {
      assert(lhs != rhs);
      return result;
    }

    assert(lhs == rhs);
  }

  return 0;
}

/// Find the start of \p other in this term, returning end() if
/// \p other does not occur as a subterm of this term.
decltype(MutableTerm::Atoms)::const_iterator
MutableTerm::findSubTerm(const MutableTerm &other) const {
  if (other.size() > size())
    return end();

  return std::search(begin(), end(), other.begin(), other.end());
}

/// Non-const variant of the above.
decltype(MutableTerm::Atoms)::iterator
MutableTerm::findSubTerm(const MutableTerm &other) {
  if (other.size() > size())
    return end();

  return std::search(begin(), end(), other.begin(), other.end());
}

/// Replace the first occurrence of \p lhs in this term with
/// \p rhs. Note that \p rhs must precede \p lhs in the linear
/// order on terms. Returns true if the term contained \p lhs;
/// otherwise returns false, in which case the term remains
/// unchanged.
bool MutableTerm::rewriteSubTerm(const MutableTerm &lhs,
                                 const MutableTerm &rhs) {
  // Find the start of lhs in this term.
  auto found = findSubTerm(lhs);

  // This term cannot be reduced using this rule.
  if (found == end())
    return false;

  auto oldSize = size();

  assert(rhs.size() <= lhs.size());

  // Overwrite the occurrence of the left hand side with the
  // right hand side.
  auto newIter = std::copy(rhs.begin(), rhs.end(), found);
  auto oldIter = found + lhs.size();

  // If the right hand side is shorter than the left hand side,
  // then newIter will point to a location before oldIter, eg
  // if this term is 'T.A.B.C', lhs is 'A.B' and rhs is 'X',
  // then we now have:
  //
  // T.X  .C
  //       ^--- oldIter
  //     ^--- newIter
  //
  // Shift everything over to close the gap (by one location,
  // in this case).
  if (newIter != oldIter) {
    auto newEnd = std::copy(oldIter, end(), newIter);

    // Now, we've moved the gap to the end of the term; close
    // it by shortening the term.
    Atoms.erase(newEnd, end());
  }

  assert(size() == oldSize - lhs.size() + rhs.size());
  return true;
}

void MutableTerm::dump(llvm::raw_ostream &out) const {
  bool first = true;

  for (auto atom : Atoms) {
    if (!first)
      out << ".";
    else
      first = false;

    atom.dump(out);
  }
}

Term RewriteContext::getTermForType(CanType paramType,
                                    const ProtocolDecl *proto) {
  return Term::get(getMutableTermForType(paramType, proto), *this);
}

/// Map an interface type to a term.
///
/// If \p proto is null, this is a term relative to a generic
/// parameter in a top-level signature. The term is rooted in a generic
/// parameter atom.
///
/// If \p proto is non-null, this is a term relative to a protocol's
/// 'Self' type. The term is rooted in a protocol atom for this protocol,
/// or an associated type atom for some associated type in this protocol.
///
/// Resolved DependentMemberTypes map to associated type atoms.
/// Unresolved DependentMemberTypes map to name atoms.
///
/// Note the behavior of the root term is special if it is an associated
/// type atom. The protocol of the associated type is always mapped to
/// \p proto if it was provided. This ensures we get the correct behavior
/// if a protocol places a constraint on an associated type inherited from
/// another protocol:
///
/// protocol P {
///   associatedtype Foo
/// }
///
/// protocol Q : P where Foo : R {}
///
/// protocol R {}
///
/// The DependentMemberType in the requirement signature of Q refers to
/// P::Foo.
///
/// However, we want Q's requirement signature to introduce the rewrite rule
///
///   [Q:Foo].[R] => [Q:Foo]
///
/// and not
///
///   [P:Foo].[R] => [P:Foo]
///
/// This is because the rule only applies to Q's logical override of Foo, and
/// not P's Foo.
///
/// To handle this, getMutableTermForType() behaves as follows:
///
/// Self.P::Foo with proto = P         => [P:Foo]
/// Self.P::Foo with proto = Q         => [Q:Foo]
/// τ_0_0.P::Foo with proto == nullptr => τ_0_0.[P:Foo]
///
MutableTerm RewriteContext::getMutableTermForType(CanType paramType,
                                                  const ProtocolDecl *proto) {
  assert(paramType->isTypeParameter());

  // Collect zero or more nested type names in reverse order.
  bool innermostAssocTypeWasResolved = false;

  SmallVector<Atom, 3> atoms;
  while (auto memberType = dyn_cast<DependentMemberType>(paramType)) {
    paramType = memberType.getBase();

    if (auto *assocType = memberType->getAssocType()) {
      const auto *thisProto = assocType->getProtocol();
      if (proto && isa<GenericTypeParamType>(paramType)) {
        thisProto = proto;
        innermostAssocTypeWasResolved = true;
      }
      atoms.push_back(Atom::forAssociatedType(thisProto,
                                              assocType->getName(),
                                              *this));
    } else {
      atoms.push_back(Atom::forName(memberType->getName(), *this));
      innermostAssocTypeWasResolved = false;
    }
  }

  // Add the root atom at the end.
  if (proto) {
    assert(proto->getSelfInterfaceType()->isEqual(paramType));

    // Self.Foo becomes [P].Foo
    // Self.Q::Foo becomes [P:Foo] (not [Q:Foo] or [P].[Q:Foo])
    if (!innermostAssocTypeWasResolved)
      atoms.push_back(Atom::forProtocol(proto, *this));
  } else {
    atoms.push_back(Atom::forGenericParam(
        cast<GenericTypeParamType>(paramType), *this));
  }

  std::reverse(atoms.begin(), atoms.end());

  return MutableTerm(atoms);
}

/// Compute the interface type for a range of atoms, with an optional
/// root type.
///
/// If the root type is specified, we wrap it in a series of
/// DependentMemberTypes. Otherwise, the root is computed from
/// the first atom of the range.
template<typename Iter>
Type getTypeForAtomRange(Iter begin, Iter end, Type root,
                         TypeArrayView<GenericTypeParamType> genericParams,
                         const ProtocolGraph &protos,
                         ASTContext &ctx) {
  Type result = root;

  auto handleRoot = [&](GenericTypeParamType *genericParam) {
    assert(genericParam->isCanonical());

    if (!genericParams.empty()) {
      // Return a sugared GenericTypeParamType if we're given an array of
      // sugared types to substitute.
      unsigned index = GenericParamKey(genericParam).findIndexIn(genericParams);
      result = genericParams[index];
      return;
    }

    // Otherwise, we're going to return a canonical type.
    result = genericParam;
  };

  for (; begin != end; ++begin) {
    auto atom = *begin;

    if (!result) {
      // A valid term always begins with a generic parameter, protocol or
      // associated type atom.
      switch (atom.getKind()) {
      case Atom::Kind::GenericParam:
        handleRoot(atom.getGenericParam());
        continue;

      case Atom::Kind::Protocol:
        handleRoot(GenericTypeParamType::get(0, 0, ctx));
        continue;

      case Atom::Kind::AssociatedType:
        handleRoot(GenericTypeParamType::get(0, 0, ctx));

        // An associated type term at the root means we have a dependent
        // member type rooted at Self; handle the associated type below.
        break;

      case Atom::Kind::Name:
      case Atom::Kind::Layout:
      case Atom::Kind::Superclass:
      case Atom::Kind::ConcreteType:
        llvm_unreachable("Term has invalid root atom");
      }
    }

    // An unresolved type can appear if we have invalid requirements.
    if (atom.getKind() == Atom::Kind::Name) {
      result = DependentMemberType::get(result, atom.getName());
      continue;
    }

    // We should have a resolved type at this point.
    assert(atom.getKind() == Atom::Kind::AssociatedType);
    auto *proto = atom.getProtocols()[0];
    auto name = atom.getName();

    AssociatedTypeDecl *assocType = nullptr;

    // Special case: handle unknown protocols, since they can appear in the
    // invalid types that getCanonicalTypeInContext() must handle via
    // concrete substitution; see the definition of getCanonicalTypeInContext()
    // below for details.
    if (!protos.isKnownProtocol(proto)) {
      assert(root &&
             "We only allow unknown protocols in getRelativeTypeForTerm()");
      assert(atom.getProtocols().size() == 1 &&
             "Unknown associated type atom must have a single protocol");
      assocType = proto->getAssociatedType(name)->getAssociatedTypeAnchor();
    } else {
      // FIXME: Cache this
      //
      // An associated type atom [P1&P1&...&Pn:A] has one or more protocols
      // P0...Pn and an identifier 'A'.
      //
      // We map it back to a AssociatedTypeDecl as follows:
      //
      // - For each protocol Pn, look for associated types A in Pn itself,
      //   and all protocols that Pn refines.
      //
      // - For each candidate associated type An in protocol Qn where
      //   Pn refines Qn, get the associated type anchor An' defined in
      //   protocol Qn', where Qn refines Qn'.
      //
      // - Out of all the candidiate pairs (Qn', An'), pick the one where
      //   the protocol Qn' is the lowest element according to the linear
      //   order defined by TypeDecl::compare().
      //
      // The associated type An' is then the canonical associated type
      // representative of the associated type atom [P0&...&Pn:A].
      //
      for (auto *proto : atom.getProtocols()) {
        const auto &info = protos.getProtocolInfo(proto);
        auto checkOtherAssocType = [&](AssociatedTypeDecl *otherAssocType) {
          otherAssocType = otherAssocType->getAssociatedTypeAnchor();

          if (otherAssocType->getName() == name &&
              (assocType == nullptr ||
               TypeDecl::compare(otherAssocType->getProtocol(),
                                 assocType->getProtocol()) < 0)) {
            assocType = otherAssocType;
          }
        };

        for (auto *otherAssocType : info.AssociatedTypes) {
          checkOtherAssocType(otherAssocType);
        }

        for (auto *otherAssocType : info.InheritedAssociatedTypes) {
          checkOtherAssocType(otherAssocType);
        }
      }
    }

    assert(assocType && "Need to look harder");
    result = DependentMemberType::get(result, assocType);
  }

  return result;
}

Type RewriteContext::getTypeForTerm(Term term,
                      TypeArrayView<GenericTypeParamType> genericParams,
                      const ProtocolGraph &protos) const {
  return getTypeForAtomRange(term.begin(), term.end(), Type(),
                             genericParams, protos, Context);
}

Type RewriteContext::getTypeForTerm(const MutableTerm &term,
                      TypeArrayView<GenericTypeParamType> genericParams,
                      const ProtocolGraph &protos) const {
  return getTypeForAtomRange(term.begin(), term.end(), Type(),
                             genericParams, protos, Context);
}

Type RewriteContext::getRelativeTypeForTerm(
    const MutableTerm &term, const MutableTerm &prefix,
    const ProtocolGraph &protos) const {
  assert(std::equal(prefix.begin(), prefix.end(), term.begin()));

  auto genericParam = CanGenericTypeParamType::get(0, 0, Context);
  return getTypeForAtomRange(
      term.begin() + prefix.size(), term.end(), genericParam,
      { }, protos, Context);
}

void Rule::dump(llvm::raw_ostream &out) const {
  out << LHS << " => " << RHS;
  if (deleted)
    out << " [deleted]";
}

void RewriteSystem::initialize(
    std::vector<std::pair<MutableTerm, MutableTerm>> &&rules,
    ProtocolGraph &&graph) {
  Protos = graph;

  for (const auto &rule : rules)
    addRule(rule.first, rule.second);
}

Atom RewriteSystem::simplifySubstitutionsInSuperclassOrConcreteAtom(
    Atom atom) const {
  return atom.transformConcreteSubstitutions(
    [&](Term term) -> Term {
      MutableTerm mutTerm(term);
      if (!simplify(mutTerm))
        return term;

      return Term::get(mutTerm, Context);
    }, Context);
}

bool RewriteSystem::addRule(MutableTerm lhs, MutableTerm rhs) {
  assert(!lhs.empty());
  assert(!rhs.empty());

  // Simplify the rule as much as possible with the rules we have so far.
  //
  // This avoids unnecessary work in the completion algorithm.
  simplify(lhs);
  simplify(rhs);

  // If the left hand side and right hand side are already equivalent, we're
  // done.
  int result = lhs.compare(rhs, Protos);
  if (result == 0)
    return false;

  // Orient the two terms so that the left hand side is greater than the
  // right hand side.
  if (result < 0)
    std::swap(lhs, rhs);

  if (lhs.back().isSuperclassOrConcreteType())
    lhs.back() = simplifySubstitutionsInSuperclassOrConcreteAtom(lhs.back());

  assert(lhs.compare(rhs, Protos) > 0);

  if (DebugAdd) {
    llvm::dbgs() << "# Adding rule " << lhs << " => " << rhs << "\n";
  }

  unsigned i = Rules.size();
  Rules.emplace_back(lhs, rhs);

  // Check if we have a rule of the form
  //
  //   X.[P1:T] => X.[P2:T]
  //
  // If so, record this rule for later. We'll try to merge the associated
  // types in RewriteSystem::processMergedAssociatedTypes().
  if (lhs.size() == rhs.size() &&
      std::equal(lhs.begin(), lhs.end() - 1, rhs.begin()) &&
      lhs.back().getKind() == Atom::Kind::AssociatedType &&
      rhs.back().getKind() == Atom::Kind::AssociatedType &&
      lhs.back().getName() == rhs.back().getName()) {
    MergedAssociatedTypes.emplace_back(lhs, rhs);
  }

  // Since we added a new rule, we have to check for overlaps between the
  // new rule and all existing rules.
  for (unsigned j : indices(Rules)) {
    // A rule does not overlap with itself.
    if (i == j)
      continue;

    // We don't have to check for overlap with deleted rules.
    if (Rules[j].isDeleted())
      continue;

    // The overlap check is not commutative so we have to check both
    // directions.
    Worklist.emplace_back(i, j);
    Worklist.emplace_back(j, i);

    if (DebugCompletion) {
      llvm::dbgs() << "$ Queued up (" << i << ", " << j << ") and ";
      llvm::dbgs() << "(" << j << ", " << i << ")\n";
    }
  }

  // Tell the caller that we added a new rule.
  return true;
}

/// Reduce a term by applying all rewrite rules until fixed point.
bool RewriteSystem::simplify(MutableTerm &term) const {
  bool changed = false;

  if (DebugSimplify) {
    llvm::dbgs() << "= Term " << term << "\n";
  }

  while (true) {
    bool tryAgain = false;
    for (const auto &rule : Rules) {
      if (rule.isDeleted())
        continue;

      if (DebugSimplify) {
        llvm::dbgs() << "== Rule " << rule << "\n";
      }

      if (rule.apply(term)) {
        if (DebugSimplify) {
          llvm::dbgs() << "=== Result " << term << "\n";
        }

        changed = true;
        tryAgain = true;
      }
    }

    if (!tryAgain)
      break;
  }

  return changed;
}

void RewriteSystem::simplifyRightHandSides() {
  for (auto &rule : Rules) {
    if (rule.isDeleted())
      continue;

    auto rhs = rule.getRHS();
    simplify(rhs);
    rule = Rule(rule.getLHS(), rhs);
  }

#ifndef NDEBUG

#define ASSERT_RULE(expr) \
  if (!(expr)) { \
    llvm::errs() << "&&& Malformed rewrite rule: " << rule << "\n\n"; \
    dump(llvm::errs()); \
    assert(expr); \
  }

  for (const auto &rule : Rules) {
    if (rule.isDeleted())
      continue;

    const auto &lhs = rule.getLHS();
    const auto &rhs = rule.getRHS();

    for (unsigned index : indices(lhs)) {
      auto atom = lhs[index];

      if (index != lhs.size() - 1) {
        ASSERT_RULE(atom.getKind() != Atom::Kind::Layout);
        ASSERT_RULE(!atom.isSuperclassOrConcreteType());
      }

      if (index != 0) {
        ASSERT_RULE(atom.getKind() != Atom::Kind::GenericParam);
      }

      if (index != 0 && index != lhs.size() - 1) {
        ASSERT_RULE(atom.getKind() != Atom::Kind::Protocol);
      }
    }

    for (unsigned index : indices(rhs)) {
      auto atom = rhs[index];

      // FIXME: This is only true if the input requirements were valid.
      // On invalid code, we'll need to skip this assertion (and instead
      // assert that we diagnosed an error!)
      ASSERT_RULE(atom.getKind() != Atom::Kind::Name);

      ASSERT_RULE(atom.getKind() != Atom::Kind::Layout);
      ASSERT_RULE(!atom.isSuperclassOrConcreteType());

      if (index != 0) {
        ASSERT_RULE(atom.getKind() != Atom::Kind::GenericParam);
        ASSERT_RULE(atom.getKind() != Atom::Kind::Protocol);
      }
    }
  }

#undef ASSERT_RULE
#endif
}

void RewriteSystem::dump(llvm::raw_ostream &out) const {
  out << "Rewrite system: {\n";
  for (const auto &rule : Rules) {
    out << "- " << rule << "\n";
  }
  out << "}\n";
}
