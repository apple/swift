//===--- AutoDiff.h - Swift Differentiable Programming --------------------===//
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
//  SWIFT_ENABLE_TENSORFLOW
//  This file defines AST support for the experimental differentiable
//  programming feature.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_AUTODIFF_H
#define SWIFT_AST_AUTODIFF_H

#include "ASTContext.h"
#include "llvm/ADT/SmallBitVector.h"
#include "swift/Basic/Range.h"

namespace swift {

class AnyFunctionType;
class AutoDiffIndexSubset;
class AutoDiffIndexSubsetBuilder;
class SILFunctionType;
typedef CanTypeWrapper<SILFunctionType> CanSILFunctionType;
class Type;
enum class SILLinkage : uint8_t;

enum class DifferentiabilityKind: uint8_t {
  NonDifferentiable = 0b00,
  Normal = 0b01,
  Linear = 0b11
};

class ParsedAutoDiffParameter {
public:
  enum class Kind { Named, Ordered, Self };

private:
  SourceLoc Loc;
  Kind Kind;
  union Value {
    struct { Identifier Name; }; // Named
    struct { unsigned Index; }; // Ordered
    struct {};                  // Self
    Value(Identifier name) : Name(name) {}
    Value(unsigned index) : Index(index) {}
    Value() {}
  } V;

public:
  ParsedAutoDiffParameter(SourceLoc loc, enum Kind kind, Value value)
    : Loc(loc), Kind(kind), V(value) {}
  
  ParsedAutoDiffParameter(SourceLoc loc, enum Kind kind, unsigned index)
  : Loc(loc), Kind(kind), V(index) {}

  static ParsedAutoDiffParameter getNamedParameter(SourceLoc loc,
                                                   Identifier name) {
    return { loc, Kind::Named, name };
  }
  
  static ParsedAutoDiffParameter getOrderedParameter(SourceLoc loc,
                                                     unsigned index) {
    return { loc, Kind::Ordered, index };
  }

  static ParsedAutoDiffParameter getSelfParameter(SourceLoc loc) {
    return { loc, Kind::Self, {} };
  }

  Identifier getName() const {
    assert(Kind == Kind::Named);
    return V.Name;
  }
  
  unsigned getIndex() const {
    return V.Index;
  }

  enum Kind getKind() const {
    return Kind;
  }

  SourceLoc getLoc() const {
    return Loc;
  }

  bool isEqual(const ParsedAutoDiffParameter &other) const {
    if (getKind() != other.getKind())
      return false;
    if (getKind() == Kind::Named)
      return getName() == other.getName();
    return getKind() == Kind::Self;
  }
};

class AnyFunctionType;
class AutoDiffIndexSubset;
class Type;
class SILModule;
enum class SILLinkage : uint8_t;

/// An efficient index subset data structure, uniqued in `ASTContext`.
/// Stores a bit vector representing set indices and a total capacity.
class AutoDiffIndexSubset : public llvm::FoldingSetNode {
public:
  typedef uint64_t BitWord;

  static constexpr unsigned bitWordSize = sizeof(BitWord);
  static constexpr unsigned numBitsPerBitWord = bitWordSize * 8;

  static std::pair<unsigned, unsigned>
  getBitWordIndexAndOffset(unsigned index) {
    auto bitWordIndex = index / numBitsPerBitWord;
    auto bitWordOffset = index % numBitsPerBitWord;
    return {bitWordIndex, bitWordOffset};
  }

  static unsigned getNumBitWordsNeededForCapacity(unsigned capacity) {
    if (capacity == 0) return 0;
    return capacity / numBitsPerBitWord + 1;
  }
  
private:
  /// The total capacity of the index subset, which is `1` less than the largest
  /// index.
  unsigned capacity;
  /// The number of bit words in the index subset.
  unsigned numBitWords;

  BitWord *getBitWordsData() {
    return reinterpret_cast<BitWord *>(this + 1);
  }

  const BitWord *getBitWordsData() const {
    return reinterpret_cast<const BitWord *>(this + 1);
  }

  ArrayRef<BitWord> getBitWords() const {
    return {getBitWordsData(), getNumBitWords()};
  }

  BitWord getBitWord(unsigned i) const {
    return getBitWordsData()[i];
  }

  BitWord &getBitWord(unsigned i) {
    return getBitWordsData()[i];
  }

  MutableArrayRef<BitWord> getMutableBitWords() {
    return {const_cast<BitWord *>(getBitWordsData()), getNumBitWords()};
  }

  explicit AutoDiffIndexSubset(const SmallBitVector &indices)
      : capacity((unsigned)indices.size()),
        numBitWords(getNumBitWordsNeededForCapacity(capacity)) {
    std::uninitialized_fill_n(getBitWordsData(), numBitWords, 0);
    for (auto i : indices.set_bits()) {
      unsigned bitWordIndex, offset;
      std::tie(bitWordIndex, offset) = getBitWordIndexAndOffset(i);
      getBitWord(bitWordIndex) |= (1 << offset);
    }
  }

public:
  AutoDiffIndexSubset() = delete;
  AutoDiffIndexSubset(const AutoDiffIndexSubset &) = delete;
  AutoDiffIndexSubset &operator=(const AutoDiffIndexSubset &) = delete;

  // Defined in ASTContext.cpp.
  static AutoDiffIndexSubset *get(ASTContext &ctx,
                                  const SmallBitVector &indices);

  static AutoDiffIndexSubset *get(ASTContext &ctx, unsigned capacity,
                                  ArrayRef<unsigned> indices) {
    SmallBitVector indicesBitVec(capacity, false);
    for (auto index : indices)
      indicesBitVec.set(index);
    return AutoDiffIndexSubset::get(ctx, indicesBitVec);
  }

  static AutoDiffIndexSubset *getDefault(ASTContext &ctx, unsigned capacity,
                                         bool includeAll = false) {
    return get(ctx, SmallBitVector(capacity, includeAll));
  }

  static AutoDiffIndexSubset *getFromRange(ASTContext &ctx, unsigned capacity,
                                           unsigned start, unsigned end) {
    assert(start < capacity);
    assert(end <= capacity);
    SmallBitVector bitVec(capacity);
    bitVec.set(start, end);
    return get(ctx, bitVec);
  }
  
  /// Creates an index subset corresponding to the given string generated by
  /// `getString()`. If the string is invalid, returns nullptr.
  static AutoDiffIndexSubset *getFromString(ASTContext &ctx, StringRef string);

  /// Returns the number of bit words used to store the index subset.
  // Note: Use `getCapacity()` to get the total index subset capacity.
  // This is public only for unit testing
  // (in unittests/AST/SILAutoDiffIndices.cpp).
  unsigned getNumBitWords() const {
    return numBitWords;
  }

  /// Returns the capacity of the index subset.
  unsigned getCapacity() const {
    return capacity;
  }

  /// Returns a textual string description of these indices.
  ///
  /// It has the format `[SU]+`, where the total number of characters is equal
  /// to the capacity, and where "S" means that the corresponding index is
  /// contained and "U" means that the corresponding index is not.
  std::string getString() const;

  class iterator;

  iterator begin() const {
    return iterator(this);
  }
  
  iterator end() const {
    return iterator(this, (int)capacity);
  }
  
  /// Returns an iterator range of indices in the index subset.
  iterator_range<iterator> getIndices() const {
    return make_range(begin(), end());
  }

  /// Returns the number of indices in the index subset.
  unsigned getNumIndices() const {
    return (unsigned)std::distance(begin(), end());
  }
  
  SmallBitVector getBitVector() const {
    SmallBitVector indicesBitVec(capacity, false);
    for (auto index : getIndices())
      indicesBitVec.set(index);
    return indicesBitVec;
  }

  bool contains(unsigned index) const {
    unsigned bitWordIndex, offset;
    std::tie(bitWordIndex, offset) = getBitWordIndexAndOffset(index);
    return getBitWord(bitWordIndex) & (1 << offset);
  }

  bool isEmpty() const {
    return llvm::all_of(getBitWords(), [](BitWord bw) { return !(bool)bw; });
  }
  
  bool equals(AutoDiffIndexSubset *other) const {
    return capacity == other->getCapacity() &&
        getBitWords().equals(other->getBitWords());
  }

  bool isSubsetOf(AutoDiffIndexSubset *other) const;
  bool isSupersetOf(AutoDiffIndexSubset *other) const;

  AutoDiffIndexSubset *adding(unsigned index, ASTContext &ctx) const;
  AutoDiffIndexSubset *extendingCapacity(ASTContext &ctx,
                                         unsigned newCapacity) const;

  void Profile(llvm::FoldingSetNodeID &id) const {
    id.AddInteger(capacity);
    for (auto index : getIndices())
      id.AddInteger(index);
  }

  void print(llvm::raw_ostream &s = llvm::outs()) const {
    s << '{';
    interleave(range(capacity), [this, &s](unsigned i) { s << contains(i); },
               [&s] { s << ", "; });
    s << '}';
  }

  void dump(llvm::raw_ostream &s = llvm::errs()) const {
    s << "(autodiff_index_subset capacity=" << capacity << " indices=(";
    interleave(getIndices(), [&s](unsigned i) { s << i; },
               [&s] { s << ", "; });
    s << "))";
  }

  int findNext(int startIndex) const;
  int findFirst() const { return findNext(-1); }
  int findPrevious(int endIndex) const;
  int findLast() const { return findPrevious(capacity); }

  class iterator {
  public:
    typedef unsigned value_type;
    typedef unsigned difference_type;
    typedef unsigned * pointer;
    typedef unsigned & reference;
    typedef std::forward_iterator_tag iterator_category;

  private:
    const AutoDiffIndexSubset *parent;
    int current = 0;

    void advance() {
      assert(current != -1 && "Trying to advance past end.");
      current = parent->findNext(current);
    }

  public:
    iterator(const AutoDiffIndexSubset *parent, int current)
        : parent(parent), current(current) {}
    explicit iterator(const AutoDiffIndexSubset *parent)
        : iterator(parent, parent->findFirst()) {}
    iterator(const iterator &) = default;

    iterator operator++(int) {
      auto prev = *this;
      advance();
      return prev;
    }

    iterator &operator++() {
      advance();
      return *this;
    }

    unsigned operator*() const { return current; }

    bool operator==(const iterator &other) const {
      assert(parent == other.parent &&
             "Comparing iterators from different AutoDiffIndexSubsets");
      return current == other.current;
    }

    bool operator!=(const iterator &other) const {
      assert(parent == other.parent &&
             "Comparing iterators from different AutoDiffIndexSubsets");
      return current != other.current;
    }
  };
};

/// SIL-level automatic differentiation indices. Consists of a source index,
/// i.e. index of the dependent result to differentiate from, and parameter
/// indices, i.e. index of independent parameters to differentiate with
/// respect to.
///
/// When a function is curried, parameter indices can refer to parameters from
/// all parameter lists. When differentiating such functions, we treat them as
/// fully uncurried.
struct SILAutoDiffIndices {
  /// The index of the dependent result to differentiate from.
  unsigned source;
  /// Independent parameters to differentiate with respect to. The bits
  /// correspond to the function's parameters in order. For example,
  ///
  ///   Function type: (A, B, C) -> R
  ///   Bits: [A][B][C]
  ///
  /// When the function is curried, the bits for the first parameter list come
  /// last. For example,
  ///
  ///   Function type: (A, B) -> (C, D) -> R
  ///   Bits: [C][D][A][B]
  ///
  AutoDiffIndexSubset *parameters;

  /// Creates a set of AD indices from the given source index and a bit vector
  /// representing parameter indices.
  /*implicit*/ SILAutoDiffIndices(unsigned source,
                                  AutoDiffIndexSubset *parameters)
      : source(source), parameters(parameters) {}

  bool operator==(const SILAutoDiffIndices &other) const;

  bool operator!=(const SILAutoDiffIndices &other) const {
    return !(*this == other);
  };

  /// Queries whether the function's parameter with index `parameterIndex` is
  /// one of the parameters to differentiate with respect to.
  bool isWrtParameter(unsigned parameterIndex) const {
    return parameterIndex < parameters->getCapacity() &&
           parameters->contains(parameterIndex);
  }

  void print(llvm::raw_ostream &s = llvm::outs()) const {
    s << "(source=" << source << " parameters=(";
    interleave(parameters->getIndices(),
               [&s](unsigned p) { s << p; }, [&s]{ s << ' '; });
    s << "))";
  }

  std::string mangle() const {
    std::string result = "src_" + llvm::utostr(source) + "_wrt_";
    interleave(parameters->getIndices(),
               [&](unsigned idx) { result += llvm::utostr(idx); },
               [&] { result += '_'; });
    return result;
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &s,
                                     const SILAutoDiffIndices &indices) {
  indices.print(s);
  return s;
}

/// The kind of an linear map.
struct AutoDiffLinearMapKind {
  enum innerty : uint8_t {
    // The differential function.
    Differential = 0,
    // The pullback function.
    Pullback = 1
  } rawValue;

  AutoDiffLinearMapKind() = default;
  AutoDiffLinearMapKind(innerty rawValue) : rawValue(rawValue) {}
  operator innerty() const { return rawValue; }
};

/// The kind of an associated function.
struct AutoDiffAssociatedFunctionKind {
  enum innerty : uint8_t {
   // The Jacobian-vector products function.
   JVP = 0,
   // The vector-Jacobian products function.
   VJP = 1
  } rawValue;

  AutoDiffAssociatedFunctionKind() = default;
  AutoDiffAssociatedFunctionKind(innerty rawValue) : rawValue(rawValue) {}
  AutoDiffAssociatedFunctionKind(AutoDiffLinearMapKind linMapKind)
      : rawValue(static_cast<innerty>(linMapKind.rawValue)) {}
  explicit AutoDiffAssociatedFunctionKind(StringRef string);
  operator innerty() const { return rawValue; }
  AutoDiffLinearMapKind getLinearMapKind() {
    return (AutoDiffLinearMapKind::innerty)rawValue;
  }
};

/// In conjunction with the original function declaration, identifies an
/// autodiff associated function.
///
/// Is uniquely allocated within an ASTContext so that it can be hashed and
/// compared by opaque pointer value.
class AutoDiffAssociatedFunctionIdentifier : public llvm::FoldingSetNode {
  const AutoDiffAssociatedFunctionKind kind;
  AutoDiffIndexSubset *const parameterIndices;

  AutoDiffAssociatedFunctionIdentifier(
      AutoDiffAssociatedFunctionKind kind,
      AutoDiffIndexSubset *parameterIndices) :
    kind(kind), parameterIndices(parameterIndices) {}

public:
  AutoDiffAssociatedFunctionKind getKind() const { return kind; }
  AutoDiffIndexSubset *getParameterIndices() const {
    return parameterIndices;
  }

  static AutoDiffAssociatedFunctionIdentifier *get(
      AutoDiffAssociatedFunctionKind kind,
      AutoDiffIndexSubset *parameterIndices, ASTContext &C);

  void Profile(llvm::FoldingSetNodeID &ID) {
    ID.AddInteger(kind);
    ID.AddPointer(parameterIndices);
  }
};

/// Automatic differentiation utility namespace.
namespace autodiff {
/// Appends the subset's parameter's types to `result`, in the order in
/// which they appear in the function type.
void getSubsetParameterTypes(AutoDiffIndexSubset *indices,
                             AnyFunctionType *type,
                             SmallVectorImpl<Type> &result,
                             bool reverseCurryLevels = false);

/// Returns an index subset for the SIL function parameters corresponding to the
/// parameters in this subset. In particular, this explodes tuples. For example,
///
///   functionType = (A, B, C) -> R
///   if "A" and "C" are in the set,
///   ==> returns 101
///   (because the lowered SIL type is (A, B, C) -> R)
///
///   functionType = (Self) -> (A, B, C) -> R
///   if "Self" and "C" are in the set,
///   ==> returns 0011
///   (because the lowered SIL type is (A, B, C, Self) -> R)
///
///   functionType = (A, (B, C), D) -> R
///   if "A" and "(B, C)" are in the set,
///   ==> returns 1110
///   (because the lowered SIL type is (A, B, C, D) -> R)
///
/// Note:
/// - The function must not be curried unless it's a method. Otherwise, the
///   behavior is undefined.
/// - For methods, whether the self parameter is set is represented by the
///   inclusion of the `0` index in `indices`.
AutoDiffIndexSubset *getLoweredParameterIndices(AutoDiffIndexSubset *indices,
                                                AnyFunctionType *type);

/// Retrieve config from the function name of a variant of
/// `Builtin.autodiffApply`, e.g. `Builtin.autodiffApply_jvp_arity2_order1`.
/// Returns true if the function name is parsed successfully.
bool getBuiltinAutoDiffApplyConfig(StringRef operationName,
                                   AutoDiffAssociatedFunctionKind &kind,
                                   unsigned &arity, bool &rethrows);

/// Computes the correct linkage for an associated function given the linkage of
/// the original function. If the original linkage is not external and
/// `isAssocFnExported` is true, use the original function's linkage. Otherwise,
/// return hidden linkage.
SILLinkage getAutoDiffAssociatedFunctionLinkage(SILLinkage originalLinkage,
                                                bool isAssocFnExported);

} // end namespace autodiff

class BuiltinFloatType;
class NominalOrBoundGenericNominalType;
class TupleType;

/// A type that represents a vector space.
class VectorSpace {
public:
  /// A tangent space kind.
  enum class Kind {
    /// A type that conforms to `AdditiveArithmetic`.
    Vector,
    /// A product of vector spaces as a tuple.
    Tuple,
    /// A function type whose innermost result conforms to `AdditiveArithmetic`.
    Function
  };

private:
  Kind kind;
  union Value {
    // Vector
    Type vectorType;
    // Tuple
    TupleType *tupleType;
    // Function
    AnyFunctionType *functionType;

    Value(Type vectorType) : vectorType(vectorType) {}
    Value(TupleType *tupleType) : tupleType(tupleType) {}
    Value(AnyFunctionType *functionType) : functionType(functionType) {}
  } value;

  VectorSpace(Kind kind, Value value)
      : kind(kind), value(value) {}

public:
  VectorSpace() = delete;

  static VectorSpace getVector(Type vectorType) {
    return {Kind::Vector, vectorType};
  }
  static VectorSpace getTuple(TupleType *tupleTy) {
    return {Kind::Tuple, tupleTy};
  }
  static VectorSpace getFunction(AnyFunctionType *fnTy) {
    return {Kind::Function, fnTy};
  }

  bool isVector() const { return kind == Kind::Vector; }
  bool isTuple() const { return kind == Kind::Tuple; }

  Kind getKind() const { return kind; }
  Type getVector() const {
    assert(kind == Kind::Vector);
    return value.vectorType;
  }
  TupleType *getTuple() const {
    assert(kind == Kind::Tuple);
    return value.tupleType;
  }
  AnyFunctionType *getFunction() const {
    assert(kind == Kind::Function);
    return value.functionType;
  }

  Type getType() const;
  CanType getCanonicalType() const;
  NominalTypeDecl *getNominal() const;
};

} // end namespace swift

namespace llvm {

using swift::SILAutoDiffIndices;
using swift::OptionSet;

template<typename T> struct DenseMapInfo;

template<> struct DenseMapInfo<SILAutoDiffIndices> {
  static SILAutoDiffIndices getEmptyKey() {
    return { DenseMapInfo<unsigned>::getEmptyKey(), nullptr };
  }

  static SILAutoDiffIndices getTombstoneKey() {
    return { DenseMapInfo<unsigned>::getTombstoneKey(), nullptr };
  }

  static unsigned getHashValue(const SILAutoDiffIndices &Val) {
    unsigned combinedHash =
      hash_combine(~1U, DenseMapInfo<unsigned>::getHashValue(Val.source),
                   hash_combine_range(Val.parameters->begin(),
                                      Val.parameters->end()));
    return combinedHash;
  }

  static bool isEqual(const SILAutoDiffIndices &LHS,
                      const SILAutoDiffIndices &RHS) {
    return LHS == RHS;
  }
};

} // end namespace llvm

#endif // SWIFT_AST_AUTODIFF_H
