//===--- DifferentiationInvoker.h -----------------------------*- C++ -*---===//
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
// Class that represents an invoker of differentiation.
// Used to track diagnostic source locations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_DIFFERENTIATIONINVOKER_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_DIFFERENTIATIONINVOKER_H

#include "swift/Basic/SourceLoc.h"
#include <utility>

namespace swift {

class ApplyInst;
class DifferentiableFunctionInst;
class LinearFunctionInst;
class SILDifferentiabilityWitness;

namespace autodiff {

/// The invoker of a differentiation task. It can be some user syntax, e.g.
/// an `differentiable_function` instruction lowered from an
/// `DifferentiableFunctionExpr` expression, the differentiation pass, or
/// nothing at all. This will be used to emit informative diagnostics.
struct DifferentiationInvoker {
public:
  /// The kind of the invoker of a differentiation task.
  enum class Kind {
    // Invoked by an `differentiable_function` instruction, which may or may not
    // be linked to a Swift AST node (e.g. an `DifferentiableFunctionExpr`
    // expression).
    DifferentiableFunctionInst,

    // Invoked by an `linear_function` instruction, which may or may not
    // be linked to a Swift AST node (e.g. an `LinearFunctionExpr` expression).
    LinearFunctionInst,

    // Invoked by the indirect application of differentiation. This case has an
    // associated original `apply` instruction and
    // `SILDifferentiabilityWitness`.
    IndirectDifferentiation,

    // Invoked by a `SILDifferentiabilityWitness` **without** being linked to a
    // Swift AST attribute. This case has an associated
    // `SILDifferentiabilityWitness`.
    SILDifferentiabilityWitnessInvoker
  };

private:
  Kind kind;
  union Value {
    /// The instruction associated with the `DifferentiableFunctionInst` case.
    DifferentiableFunctionInst *diffFuncInst;
    Value(DifferentiableFunctionInst *inst) : diffFuncInst(inst) {}

    /// The instruction associated with the `LinearFunctionInst` case.
    LinearFunctionInst *linearFuncInst;
    Value(LinearFunctionInst *inst) : linearFuncInst(inst) {}

    /// The parent `apply` instruction and the witness associated with the
    /// `IndirectDifferentiation` case.
    ///
    /// Note: This used to be a std::pair, but on FreeBSD std::pair can't be
    ///       used here, because it does not have a trivial copy constructor.
    struct IndirectDifferentiation {
        ApplyInst *applyInst;
        SILDifferentiabilityWitness *witness;
    };
    IndirectDifferentiation indirectDifferentiation;

    Value(ApplyInst *applyInst, SILDifferentiabilityWitness *witness)
        : indirectDifferentiation({applyInst, witness}) {}

    /// The witness associated with the `SILDifferentiabilityWitnessInvoker`
    /// case.
    SILDifferentiabilityWitness *witness;
    Value(SILDifferentiabilityWitness *witness) : witness(witness) {}
  } value;

  /*implicit*/
  DifferentiationInvoker(Kind kind, Value value) : kind(kind), value(value) {}

public:
  DifferentiationInvoker(DifferentiableFunctionInst *inst)
      : kind(Kind::DifferentiableFunctionInst), value(inst) {}
  DifferentiationInvoker(LinearFunctionInst *inst)
      : kind(Kind::LinearFunctionInst), value(inst) {}
  DifferentiationInvoker(ApplyInst *applyInst,
                         SILDifferentiabilityWitness *witness)
      : kind(Kind::IndirectDifferentiation), value({applyInst, witness}) {}
  DifferentiationInvoker(SILDifferentiabilityWitness *witness)
      : kind(Kind::SILDifferentiabilityWitnessInvoker), value(witness) {}

  Kind getKind() const { return kind; }

  DifferentiableFunctionInst *getDifferentiableFunctionInst() const {
    assert(kind == Kind::DifferentiableFunctionInst);
    return value.diffFuncInst;
  }

  LinearFunctionInst *getLinearFunctionInst() const {
    assert(kind == Kind::LinearFunctionInst);
    return value.linearFuncInst;
  }

  std::pair<ApplyInst *, SILDifferentiabilityWitness *>
  getIndirectDifferentiation() const {
    assert(kind == Kind::IndirectDifferentiation);
    return std::make_pair(value.indirectDifferentiation.applyInst,
                          value.indirectDifferentiation.witness);
  }

  SILDifferentiabilityWitness *getSILDifferentiabilityWitnessInvoker() const {
    assert(kind == Kind::SILDifferentiabilityWitnessInvoker);
    return value.witness;
  }

  SourceLoc getLocation() const;

  void print(llvm::raw_ostream &os) const;
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                     DifferentiationInvoker invoker) {
  invoker.print(os);
  return os;
}

} // end namespace autodiff
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_DIFFERENTIATIONINVOKER_H
