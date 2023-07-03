//===--- Term.h - A term in the generics rewrite system ---------*- C++ -*-===//
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

#include "Symbol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"

#ifndef SWIFT_RQM_TERM_H
#define SWIFT_RQM_TERM_H

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

/// A term is a sequence of one or more symbols.
///
/// The Term type is a uniqued, permanently-allocated representation,
/// used to represent terms in the rewrite rules themselves. See also
/// MutableTerm for the other representation.
///
/// The first symbol in the term must be a protocol, generic parameter, or
/// associated type symbol.
///
/// A layout, superclass or concrete type symbol must only appear at the
/// end of a term.
///
/// Out-of-line methods are documented in Term.cpp.
class Term final {
  friend class RewriteContext;

  struct Storage;

  const Storage *Ptr;

  Term(const Storage *ptr) : Ptr(ptr) {}

public:
  size_t size() const;

  const Symbol *begin() const;
  const Symbol *end() const;

  std::reverse_iterator<const Symbol *> rbegin() const;
  std::reverse_iterator<const Symbol *> rend() const;

  Symbol back() const;

  Symbol operator[](size_t index) const;

  /// Returns an opaque pointer that uniquely identifies this term.
  const void *getOpaquePointer() const {
    return Ptr;
  }

  static Term fromOpaquePointer(void *ptr) {
    return Term((Storage *) ptr);
  }

  static Term get(const MutableTerm &term, RewriteContext &ctx);

  const ProtocolDecl *getRootProtocol() const {
    return begin()->getRootProtocol();
  }

  bool containsUnresolvedSymbols() const;

  void dump(llvm::raw_ostream &out) const;

  llvm::Optional<int> compare(Term other, RewriteContext &ctx) const;

  friend bool operator==(Term lhs, Term rhs) {
    return lhs.Ptr == rhs.Ptr;
  }

  friend bool operator!=(Term lhs, Term rhs) {
    return !(lhs == rhs);
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out, Term term) {
    term.dump(out);
    return out;
  }
};

/// A term is a sequence of one or more symbols.
///
/// The MutableTerm type is a dynamically-allocated representation,
/// used to represent temporary values in simplification and completion.
/// See also Term for the other representation.
///
/// The first symbol in the term must be a protocol, generic parameter, or
/// associated type symbol.
///
/// A layout constraint symbol must only appear at the end of a term.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class MutableTerm final {
  llvm::SmallVector<Symbol, 3> Symbols;

public:
  /// Creates an empty term. At least one symbol must be added for the term
  /// to become valid.
  MutableTerm() {}

  explicit MutableTerm(const Symbol *begin,
                       const Symbol *end)
    : Symbols(begin, end) {}

  explicit MutableTerm(llvm::SmallVector<Symbol, 3> &&symbols)
    : Symbols(std::move(symbols)) {}

  explicit MutableTerm(llvm::ArrayRef<Symbol> symbols)
      : Symbols(symbols.begin(), symbols.end()) {}

  explicit MutableTerm(Term term)
    : Symbols(term.begin(), term.end()) {}

  void add(Symbol symbol) {
    Symbols.push_back(symbol);
  }

  void append(Term other) {
    Symbols.append(other.begin(), other.end());
  }

  void append(const MutableTerm &other) {
    Symbols.append(other.begin(), other.end());
  }

  void append(const Symbol *from, const Symbol *to) {
    Symbols.append(from, to);
  }

  llvm::Optional<int> compare(const MutableTerm &other,
                              RewriteContext &ctx) const;

  bool empty() const { return Symbols.empty(); }

  size_t size() const { return Symbols.size(); }

  const ProtocolDecl *getRootProtocol() const {
    return begin()->getRootProtocol();
  }

  const Symbol *begin() const { return Symbols.begin(); }
  const Symbol *end() const { return Symbols.end(); }

  Symbol *begin() { return Symbols.begin(); }
  Symbol *end() { return Symbols.end(); }

  std::reverse_iterator<const Symbol *> rbegin() const { return Symbols.rbegin(); }
  std::reverse_iterator<const Symbol *> rend() const { return Symbols.rend(); }

  std::reverse_iterator<Symbol *> rbegin() { return Symbols.rbegin(); }
  std::reverse_iterator<Symbol *> rend() { return Symbols.rend(); }

  Symbol back() const {
    return Symbols.back();
  }

  Symbol &back() {
    return Symbols.back();
  }

  Symbol operator[](size_t index) const {
    return Symbols[index];
  }

  Symbol &operator[](size_t index) {
    return Symbols[index];
  }

  void rewriteSubTerm(Symbol *from, Symbol *to, Term rhs);

  void dump(llvm::raw_ostream &out) const;

  friend bool operator==(const MutableTerm &lhs, const MutableTerm &rhs) {
    if (lhs.size() != rhs.size())
      return false;

    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }

  friend bool operator!=(const MutableTerm &lhs, const MutableTerm &rhs) {
    return !(lhs == rhs);
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       const MutableTerm &term) {
    term.dump(out);
    return out;
  }
};

} // end namespace rewriting

} // end namespace swift

namespace llvm {
  template<> struct DenseMapInfo<swift::rewriting::Term> {
    static swift::rewriting::Term getEmptyKey() {
      return swift::rewriting::Term::fromOpaquePointer(
        llvm::DenseMapInfo<void *>::getEmptyKey());
    }
    static swift::rewriting::Term getTombstoneKey() {
      return swift::rewriting::Term::fromOpaquePointer(
        llvm::DenseMapInfo<void *>::getTombstoneKey());
    }
    static unsigned getHashValue(swift::rewriting::Term Val) {
      return DenseMapInfo<void *>::getHashValue(Val.getOpaquePointer());
    }
    static bool isEqual(swift::rewriting::Term LHS,
                        swift::rewriting::Term RHS) {
      return LHS == RHS;
    }
  };

  template<>
  struct PointerLikeTypeTraits<swift::rewriting::Term> {
  public:
    static inline void *getAsVoidPointer(swift::rewriting::Term Val) {
      return const_cast<void *>(Val.getOpaquePointer());
    }
    static inline swift::rewriting::Term getFromVoidPointer(void *Ptr) {
      return swift::rewriting::Term::fromOpaquePointer(Ptr);
    }
    enum { NumLowBitsAvailable = 1 };
  };
} // end namespace llvm

#endif
