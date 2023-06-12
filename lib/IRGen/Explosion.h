//===--- Explosion.h - Exploded R-Value Representation ----------*- C++ -*-===//
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
// A storage structure for holding an exploded r-value.  An exploded
// r-value has been separated into individual components.  Only types
// with non-resilient structure may be exploded.
//
// The standard use for explosions is for argument-passing.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_EXPLOSION_H
#define SWIFT_IRGEN_EXPLOSION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "IRGen.h"
#include "IRGenFunction.h"

namespace swift {
namespace irgen {

/// The representation for an explosion is just a list of raw LLVM
/// values.  The meaning of these values is imposed externally by the
/// type infos, except that it is expected that they will be passed
/// as arguments in exactly this way.
class Explosion {
  unsigned NextValue;
  SmallVector<llvm::Value*, 8> Values;

public:
  Explosion() : NextValue(0) {}

  // We want to be a move-only type.
  Explosion(const Explosion &) = delete;
  Explosion &operator=(const Explosion &) = delete;
  Explosion(Explosion &&other) : NextValue(0) {
    // Do an uninitialized copy of the non-consumed elements.
    Values.resize_for_overwrite(other.size());
    std::uninitialized_copy(other.begin(), other.end(), Values.begin());

    // Remove everything from the other explosion.
    other.reset();
  }

  Explosion &operator=(Explosion &&o) {
    assert(empty() && "explosion had values remaining when reassigned!");
    NextValue = o.NextValue;
    Values.swap(o.Values);
    o.reset();
    return *this;
  }

  Explosion(llvm::Value *singleValue) : NextValue(0) {
    add(singleValue);
  }

  ~Explosion() {
    assert(empty() && "explosion had values remaining when destroyed!");
  }

  bool empty() const {
    return NextValue == Values.size();
  }

  size_t size() const {
    return Values.size() - NextValue;
  }

  using iterator = SmallVector<llvm::Value *, 8>::iterator;
  iterator begin() { return Values.begin() + NextValue; }
  iterator end() { return Values.end(); }

  using const_iterator = SmallVector<llvm::Value *, 8>::const_iterator;
  const_iterator begin() const { return Values.begin() + NextValue; }
  const_iterator end() const { return Values.end(); }

  /// Add a value to the end of this exploded r-value.
  void add(llvm::Value *value) {
    assert(value && "adding null value to explosion");
    assert(NextValue == 0 && "adding to partially-claimed explosion?");
    Values.push_back(value);
  }

  void add(ArrayRef<llvm::Value*> values) {
#ifndef NDEBUG
    for (auto value : values)
      assert(value && "adding null value to explosion");
#endif
    assert(NextValue == 0 && "adding to partially-claimed explosion?");
    Values.append(values.begin(), values.end());
  }

  void insert(unsigned index, llvm::Value *value) {
    Values.insert(Values.begin() + index, value);
  }

  /// Return an array containing the given range of values.  The values
  /// are not claimed.
  ArrayRef<llvm::Value*> getRange(unsigned from, unsigned to) const {
    assert(from <= to);
    assert(to <= Values.size());
    return llvm::makeArrayRef(begin() + from, to - from);
  }

  /// Return an array containing all of the remaining values.  The values
  /// are not claimed.
  ArrayRef<llvm::Value *> getAll() {
    return llvm::makeArrayRef(begin(), Values.size() - NextValue);
  }

  /// Transfer ownership of the next N values to the given explosion.
  void transferInto(Explosion &other, unsigned n) {
    other.add(claim(n));
  }


  /// The next N values have been claimed in some indirect way (e.g.
  /// using getRange() and the like); just give up on them.
  void markClaimed(unsigned n) {
    assert(NextValue + n <= Values.size());
    NextValue += n;
  }

  /// Claim and return the next value in this explosion.
  llvm::Value *claimNext() {
    assert(NextValue < Values.size());
    return Values[NextValue++];
  }

  llvm::Constant *claimNextConstant() {
    return cast<llvm::Constant>(claimNext());
  }

  /// Claim and return the next N values in this explosion.
  ArrayRef<llvm::Value*> claim(unsigned n) {
    assert(NextValue + n <= Values.size());
    auto array = llvm::makeArrayRef(begin(), n);
    NextValue += n;
    return array;
  }

  /// Claim and return all the values in this explosion.
  ArrayRef<llvm::Value*> claimAll() {
    return claim(size());
  }

  // These are all kindof questionable.

  /// Without changing any state, take the last claimed value,
  /// if there is one.
  llvm::Value *getLastClaimed() {
    assert(NextValue > 0);
    return Values[NextValue-1];
  }

  /// Claim and remove the last item in the array.
  /// Unlike the normal 'claim' methods, the item is gone forever.
  llvm::Value *takeLast() {
    assert(!empty());
    auto result = Values.back();
    Values.pop_back();
    return result;
  }

  /// Reset this explosion.
  void reset() {
    NextValue = 0;
    Values.clear();
  }

  void print(llvm::raw_ostream &OS);
  void dump();
};

/// An explosion schema is essentially the type of an Explosion.
class ExplosionSchema {
public:

  /// The schema for one atom of the explosion.
  class Element {
    llvm::Type *Type;
    Alignment::int_type Align;
    Element() = default;
  public:
    static Element forScalar(llvm::Type *type) {
      Element e;
      e.Type = type;
      e.Align = 0;
      return e;
    }

    static Element forAggregate(llvm::Type *type, Alignment align) {
      assert(align.getValue() != 0 && "alignment with zero value!");
      Element e;
      e.Type = type;
      e.Align = align.getValue();
      return e;
    }

    bool isScalar() const { return Align == 0; }
    llvm::Type *getScalarType() const { assert(isScalar()); return Type; }

    bool isAggregate() const { return !isScalar(); }
    llvm::Type *getAggregateType() const {
      assert(isAggregate());
      return Type;
    }
    Alignment getAggregateAlignment() const {
      assert(isAggregate());
      return Alignment(Align);
    }
  };
  
private:
  SmallVector<Element, 8> Elements;
  bool ContainsAggregate;

public:
  ExplosionSchema() : ContainsAggregate(false) {}

  /// Return the number of elements in this schema.
  unsigned size() const { return Elements.size(); }
  bool empty() const { return Elements.empty(); }

  /// Does this schema contain an aggregate element?
  bool containsAggregate() const { return ContainsAggregate; }

  /// Does this schema consist solely of one aggregate element?
  bool isSingleAggregate() const {
    return size() == 1 && containsAggregate();
  }

  const Element &operator[](size_t index) const {
    return Elements[index];
  }

  using iterator = SmallVectorImpl<Element>::iterator;
  using const_iterator = SmallVectorImpl<Element>::const_iterator;

  iterator begin() { return Elements.begin(); }
  iterator end() { return Elements.end(); }
  const_iterator begin() const { return Elements.begin(); }
  const_iterator end() const { return Elements.end(); }

  void add(Element e) {
    Elements.push_back(e);
    ContainsAggregate |= e.isAggregate();
  }

  /// Produce the correct type for a direct return of this schema,
  /// which is assumed to contain only scalars.  This is defined as:
  ///   - void, if the schema is empty;
  ///   - the element type, if the schema contains exactly one element;
  ///   - an anonymous struct type concatenating those types, otherwise.
  llvm::Type *getScalarResultType(IRGenModule &IGM) const;
};

} // end namespace irgen
} // end namespace swift

#endif
