//===--- Term.cpp - A term in the generics rewrite system -----------------===//
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
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>
#include "RewriteContext.h"
#include "Symbol.h"
#include "Term.h"

using namespace swift;
using namespace rewriting;

/// Terms are uniqued and immutable, stored as a single pointer;
/// the Storage type is the allocated backing storage.
struct Term::Storage final
  : public llvm::FoldingSetNode,
    public llvm::TrailingObjects<Storage, Symbol> {
  friend class Symbol;

  unsigned Size;

  explicit Storage(unsigned size) : Size(size) {}

  size_t numTrailingObjects(OverloadToken<Symbol>) const {
    return Size;
  }

  MutableArrayRef<Symbol> getElements() {
    return {getTrailingObjects<Symbol>(), Size};
  }

  ArrayRef<Symbol> getElements() const {
    return {getTrailingObjects<Symbol>(), Size};
  }

  void Profile(llvm::FoldingSetNodeID &id) const;
};

size_t Term::size() const { return Ptr->Size; }

const Symbol *Term::begin() const {
  return Ptr->getElements().begin();
}

const Symbol *Term::end() const {
  return Ptr->getElements().end();
}

std::reverse_iterator<const Symbol *> Term::rbegin() const {
  return Ptr->getElements().rbegin();
}

std::reverse_iterator<const Symbol *> Term::rend() const {
  return Ptr->getElements().rend();
}

Symbol Term::back() const {
  return Ptr->getElements().back();
}

Symbol Term::operator[](size_t index) const {
  return Ptr->getElements()[index];
}

void Term::dump(llvm::raw_ostream &out) const {
  MutableTerm(*this).dump(out);
}

Term Term::get(const MutableTerm &mutableTerm, RewriteContext &ctx) {
  unsigned size = mutableTerm.size();
  assert(size > 0 && "Term must have at least one symbol");

  llvm::FoldingSetNodeID id;
  id.AddInteger(size);
  for (auto symbol : mutableTerm)
    id.AddPointer(symbol.getOpaquePointer());

  void *insertPos = nullptr;
  if (auto *term = ctx.Terms.FindNodeOrInsertPos(id, insertPos))
    return term;

  void *mem = ctx.Allocator.Allocate(
      Storage::totalSizeToAlloc<Symbol>(size),
      alignof(Storage));
  auto *term = new (mem) Storage(size);
  for (unsigned i = 0; i < size; ++i)
    term->getElements()[i] = mutableTerm[i];

  ctx.Terms.InsertNode(term, insertPos);
  ctx.TermHistogram.add(size);

  return term;
}

void Term::Storage::Profile(llvm::FoldingSetNodeID &id) const {
  id.AddInteger(Size);

  for (auto symbol : getElements())
    id.AddPointer(symbol.getOpaquePointer());
}

bool Term::containsUnresolvedSymbols() const {
  for (auto symbol : *this) {
    if (symbol.getKind() == Symbol::Kind::Name)
      return true;
  }

  return false;
}

/// Shortlex order on symbol ranges.
///
/// First we compare length, then perform a lexicographic comparison
/// on symbols if the two ranges have the same length.
///
/// This is used to implement Term::compare() and MutableTerm::compare()
/// below.
static llvm::Optional<int> shortlexCompare(const Symbol *lhsBegin,
                                           const Symbol *lhsEnd,
                                           const Symbol *rhsBegin,
                                           const Symbol *rhsEnd,
                                           RewriteContext &ctx) {
  // First, compare the number of name symbols.
  unsigned lhsNameCount = 0;
  for (auto *iter = lhsBegin; iter != lhsEnd; ++iter) {
    if (iter->getKind() == Symbol::Kind::Name)
      ++lhsNameCount;
  }

  unsigned rhsNameCount = 0;
  for (auto *iter = rhsBegin; iter != rhsEnd; ++iter) {
    if (iter->getKind() == Symbol::Kind::Name)
      ++rhsNameCount;
  }

  // A term with more name symbols orders after a term with fewer name symbols.
  if (lhsNameCount != rhsNameCount)
    return lhsNameCount > rhsNameCount ? 1 : -1;

  // Next, compare term length.
  unsigned lhsSize = (lhsEnd - lhsBegin);
  unsigned rhsSize = (rhsEnd - rhsBegin);

  // A longer term orders after a shorter term.
  if (lhsSize != rhsSize)
    return lhsSize < rhsSize ? -1 : 1;

  // Finally, compare symbols pairwise.
  while (lhsBegin != lhsEnd) {
    auto lhs = *lhsBegin;
    auto rhs = *rhsBegin;

    ++lhsBegin;
    ++rhsBegin;

    llvm::Optional<int> result = lhs.compare(rhs, ctx);
    if (!result.has_value() || *result != 0) {
      assert(lhs != rhs);
      return result;
    }

    assert(lhs == rhs);
  }

  return 0;
}

/// Shortlex order on terms. Returns None if the terms are identical except
/// for an incomparable superclass or concrete type symbol at the end.
llvm::Optional<int> Term::compare(Term other, RewriteContext &ctx) const {
  return shortlexCompare(begin(), end(), other.begin(), other.end(), ctx);
}

/// Shortlex order on mutable terms. Returns None if the terms are identical
/// except for an incomparable superclass or concrete type symbol at the end.
llvm::Optional<int> MutableTerm::compare(const MutableTerm &other,
                                         RewriteContext &ctx) const {
  return shortlexCompare(begin(), end(), other.begin(), other.end(), ctx);
}

/// Replace the subterm in the range [from,to) of this term with \p rhs.
void MutableTerm::rewriteSubTerm(Symbol *from, Symbol *to, Term rhs) {
  auto oldSize = size();
  size_t lhsLength = (size_t)(to - from);

  if (lhsLength == rhs.size()) {
    // Copy the RHS to the LHS.
    auto newTo = std::copy(rhs.begin(), rhs.end(), from);

    // The RHS has the same length as the LHS, so we're done.
    assert(newTo == to);
    (void) newTo;
  } else if (lhsLength > rhs.size()) {
    // Copy the RHS to the LHS.
    auto newTo = std::copy(rhs.begin(), rhs.end(), from);

    // Shorten the term.
    Symbols.erase(newTo, to);
  } else {
    assert(lhsLength < rhs.size());

    // Copy the LHS-sized prefix of RHS to the LHS.
    auto newTo = std::copy_n(rhs.begin(), lhsLength, from);
    assert(newTo == to);

    // Insert the remainder of the RHS term.
    Symbols.insert(to, rhs.begin() + lhsLength, rhs.end());
  }

  assert(size() == oldSize - lhsLength + rhs.size());
}

void MutableTerm::dump(llvm::raw_ostream &out) const {
  bool first = true;

  for (auto symbol : Symbols) {
    if (!first)
      out << ".";
    else
      first = false;

    symbol.dump(out);
  }
}
