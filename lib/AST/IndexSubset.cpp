//===--- IndexSubset.cpp - Fixed-size subset of indices -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/IndexSubset.h"
#include "swift/Basic/Assertions.h"

using namespace swift;

IndexSubset *
IndexSubset::getFromString(ASTContext &ctx, StringRef string) {
  unsigned capacity = string.size();
  llvm::SmallBitVector indices(capacity);
  for (unsigned i : range(capacity)) {
    if (string[i] == 'S')
      indices.set(i);
    else if (string[i] != 'U')
      return nullptr;
  }
  return get(ctx, indices);
}

std::string IndexSubset::getString() const {
  std::string result;
  result.reserve(capacity);
  for (unsigned i : range(capacity))
    result += contains(i) ? 'S' : 'U';
  return result;
}

bool IndexSubset::isSubsetOf(IndexSubset *other) const {
  assert(capacity == other->capacity);
  for (auto index : range(numBitWords))
    if (getBitWord(index) & ~other->getBitWord(index))
      return false;
  return true;
}

bool IndexSubset::isSupersetOf(IndexSubset *other) const {
  assert(capacity == other->capacity);
  for (auto index : range(numBitWords))
    if (~getBitWord(index) & other->getBitWord(index))
      return false;
  return true;
}

IndexSubset *IndexSubset::adding(unsigned index, ASTContext &ctx) const {
  assert(index < getCapacity());
  if (contains(index))
    return const_cast<IndexSubset *>(this);
  SmallBitVector newIndices(capacity);
  bool inserted = false;
  for (auto curIndex : getIndices()) {
    if (!inserted && curIndex > index) {
      newIndices.set(index);
      inserted = true;
    }
    newIndices.set(curIndex);
  }
  return get(ctx, newIndices);
}

IndexSubset *IndexSubset::extendingCapacity(
    ASTContext &ctx, unsigned newCapacity) const {
  assert(newCapacity >= capacity);
  if (newCapacity == capacity)
    return const_cast<IndexSubset *>(this);
  SmallBitVector indices(newCapacity);
  for (auto index : getIndices())
    indices.set(index);
  return IndexSubset::get(ctx, indices);
}

void IndexSubset::print(llvm::raw_ostream &s) const {
  s << '{';
  llvm::interleave(
      range(capacity), [this, &s](unsigned i) { s << contains(i); },
      [&s] { s << ", "; });
  s << '}';
}

void IndexSubset::dump() const {
  auto &s = llvm::errs();
  s << "(index_subset capacity=" << capacity << " indices=(";
  interleave(getIndices(), [&s](unsigned i) { s << i; },
             [&s] { s << ", "; });
  s << "))\n";
}

int IndexSubset::findNext(int startIndex) const {
  assert(startIndex < (int)capacity && "Start index cannot be past the end");
  unsigned bitWordIndex = 0, offset = 0;
  if (startIndex >= 0) {
    auto indexAndOffset = getBitWordIndexAndOffset(startIndex);
    bitWordIndex = indexAndOffset.first;
    offset = indexAndOffset.second + 1;
  }
  for (; bitWordIndex < numBitWords; ++bitWordIndex, offset = 0) {
    for (; offset < numBitsPerBitWord; ++offset) {
      auto index = bitWordIndex * numBitsPerBitWord + offset;
      auto bitWord = getBitWord(bitWordIndex);
      if (!bitWord)
        break;
      if (index >= capacity)
        return capacity;
      if (bitWord & ((BitWord)1 << offset))
        return index;
    }
  }
  return capacity;
}

int IndexSubset::findPrevious(int endIndex) const {
  assert(endIndex >= 0 && "End index cannot be before the start");
  int bitWordIndex = numBitWords - 1, offset = numBitsPerBitWord - 1;
  if (endIndex < (int)capacity) {
    auto indexAndOffset = getBitWordIndexAndOffset(endIndex);
    bitWordIndex = (int)indexAndOffset.first;
    offset = (int)indexAndOffset.second - 1;
  }
  for (; bitWordIndex >= 0; --bitWordIndex, offset = numBitsPerBitWord - 1) {
    for (; offset >= 0; --offset) {
      auto index = bitWordIndex * (int)numBitsPerBitWord + offset;
      auto bitWord = getBitWord(bitWordIndex);
      if (!bitWord)
        break;
      if (index < 0)
        return -1;
      if (bitWord & ((BitWord)1 << offset))
        return index;
    }
  }
  return -1;
}
