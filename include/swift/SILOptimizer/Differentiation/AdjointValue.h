//===--- AdjointValue.h - Helper class for differentiation ----*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// AdjointValue - a symbolic representation for adjoint values enabling
// efficient differentiation by avoiding zero materialization.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADJOINTVALUE_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADJOINTVALUE_H

#include "swift/AST/Decl.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Debug.h"

namespace swift {
namespace autodiff {

enum AdjointValueKind {
  /// An empty adjoint, i.e. zero. This case exists due to its special
  /// mathematical properties: `0 + x = x`. This is a guaranteed optimization
  /// when we combine a zero adjoint with another (e.g. differentiating a
  /// fanout).
  Zero,

  /// An aggregate of adjoint values: a struct or tuple.
  Aggregate,

  /// A concrete SIL value.
  Concrete,
};

class AdjointValue;

class AdjointValueBase {
  friend class AdjointValue;

  /// The kind of this adjoint value.
  AdjointValueKind kind;

  /// The type of this value as if it were materialized as a SIL value.
  SILType type;

  using DebugInfo = std::pair<SILDebugLocation, SILDebugVariable>;

  /// The debug location and variable info associated with the original value.
  llvm::Optional<DebugInfo> debugInfo;

  /// The underlying value.
  union Value {
    unsigned numAggregateElements;
    SILValue concrete;
    Value(unsigned numAggregateElements)
        : numAggregateElements(numAggregateElements) {}
    Value(SILValue v) : concrete(v) {}
    Value() {}
  } value;

  // Begins tail-allocated aggregate elements, if
  // `kind == AdjointValueKind::Aggregate`.

  explicit AdjointValueBase(SILType type,
                            llvm::ArrayRef<AdjointValue> aggregate,
                            llvm::Optional<DebugInfo> debugInfo)
      : kind(AdjointValueKind::Aggregate), type(type), debugInfo(debugInfo),
        value(aggregate.size()) {
    MutableArrayRef<AdjointValue> tailElements(
        reinterpret_cast<AdjointValue *>(this + 1), aggregate.size());
    std::uninitialized_copy(
        aggregate.begin(), aggregate.end(), tailElements.begin());
  }

  explicit AdjointValueBase(SILValue v, llvm::Optional<DebugInfo> debugInfo)
      : kind(AdjointValueKind::Concrete), type(v->getType()),
        debugInfo(debugInfo), value(v) {}

  explicit AdjointValueBase(SILType type, llvm::Optional<DebugInfo> debugInfo)
      : kind(AdjointValueKind::Zero), type(type), debugInfo(debugInfo) {}
};

/// A symbolic adjoint value that wraps a `SILValue`, a zero, or an aggregate
/// thereof.
class AdjointValue final {

private:
  /// The kind of this adjoint value.
  AdjointValueBase *base;
  /*implicit*/ AdjointValue(AdjointValueBase *base = nullptr) : base(base) {}

public:
  AdjointValueBase *operator->() const { return base; }
  AdjointValueBase &operator*() const { return *base; }

  using DebugInfo = AdjointValueBase::DebugInfo;

  static AdjointValue
  createConcrete(llvm::BumpPtrAllocator &allocator, SILValue value,
                 llvm::Optional<DebugInfo> debugInfo = llvm::None) {
    auto *buf = allocator.Allocate<AdjointValueBase>();
    return new (buf) AdjointValueBase(value, debugInfo);
  }

  static AdjointValue
  createZero(llvm::BumpPtrAllocator &allocator, SILType type,
             llvm::Optional<DebugInfo> debugInfo = llvm::None) {
    auto *buf = allocator.Allocate<AdjointValueBase>();
    return new (buf) AdjointValueBase(type, debugInfo);
  }

  static AdjointValue
  createAggregate(llvm::BumpPtrAllocator &allocator, SILType type,
                  ArrayRef<AdjointValue> elements,
                  llvm::Optional<DebugInfo> debugInfo = llvm::None) {
    AdjointValue *buf = reinterpret_cast<AdjointValue *>(allocator.Allocate(
        sizeof(AdjointValueBase) + elements.size() * sizeof(AdjointValue),
        alignof(AdjointValueBase)));
    return new (buf) AdjointValueBase(type, elements, debugInfo);
  }

  AdjointValueKind getKind() const { return base->kind; }
  SILType getType() const { return base->type; }
  CanType getSwiftType() const { return getType().getASTType(); }
  llvm::Optional<DebugInfo> getDebugInfo() const { return base->debugInfo; }
  void setDebugInfo(DebugInfo debugInfo) const { base->debugInfo = debugInfo; }

  NominalTypeDecl *getAnyNominal() const {
    return getSwiftType()->getAnyNominal();
  }

  bool isZero() const { return getKind() == AdjointValueKind::Zero; }
  bool isAggregate() const { return getKind() == AdjointValueKind::Aggregate; }
  bool isConcrete() const { return getKind() == AdjointValueKind::Concrete; }

  unsigned getNumAggregateElements() const {
    assert(isAggregate());
    return base->value.numAggregateElements;
  }

  AdjointValue getAggregateElement(unsigned i) const {
    return getAggregateElements()[i];
  }

  llvm::ArrayRef<AdjointValue> getAggregateElements() const {
    assert(isAggregate());
    return {
        reinterpret_cast<const AdjointValue *>(base + 1),
        getNumAggregateElements()};
  }

  SILValue getConcreteValue() const {
    assert(isConcrete());
    return base->value.concrete;
  }

  void print(llvm::raw_ostream &s) const {
    switch (getKind()) {
    case AdjointValueKind::Zero:
      s << "Zero[" << getType() << ']';
      break;
    case AdjointValueKind::Aggregate:
      s << "Aggregate[" << getType() << "](";
      if (auto *decl =
              getType().getASTType()->getStructOrBoundGenericStruct()) {
        interleave(
            llvm::zip(decl->getStoredProperties(), getAggregateElements()),
            [&s](std::tuple<VarDecl *, const AdjointValue &> elt) {
              s << std::get<0>(elt)->getName() << ": ";
              std::get<1>(elt).print(s);
            },
            [&s] { s << ", "; });
      } else if (getType().is<TupleType>()) {
        interleave(
            getAggregateElements(),
            [&s](const AdjointValue &elt) { elt.print(s); },
            [&s] { s << ", "; });
      } else {
        llvm_unreachable("Invalid aggregate");
      }
      s << ')';
      break;
    case AdjointValueKind::Concrete:
      s << "Concrete[" << getType() << "](" << base->value.concrete << ')';
      break;
    }
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); };
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                     const AdjointValue &adjVal) {
  adjVal.print(os);
  return os;
}

} // end namespace autodiff
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADJOINTVALUE_H
