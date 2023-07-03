//===--- SolutionResult.h - Constraint System Solution ----------*- C++ -*-===//
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
//
//  This file defines the SolutionResult class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPECHECK_SOLUTION_RESULT_H
#define SWIFT_TYPECHECK_SOLUTION_RESULT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"

namespace swift {

using llvm::ArrayRef;
using llvm::makeArrayRef;

namespace constraints {

class Solution;

/// Describes the result of solving a constraint system, after
/// potentially taking various corrective actions.
class SolutionResult {
public:
  enum Kind : unsigned char {
    /// The constraint system was successfully solved, and one can
    /// retrieve the resulting solution.
    Success,
    /// The constraint system had multiple solutions, none of which
    /// was better than the others.
    Ambiguous,
    /// The constraint system had no solution, and a diagnostic has
    /// already been emitted.
    Error,
    /// The constraint system had no solution, but no diagnostic has
    /// been emitted yet.
    UndiagnosedError,
    /// The constraint system was too complex to solve, but no
    /// diagnostic has been emitted yet.
    TooComplex,
  };

private:
  /// The kind of solution result.
  Kind kind;

  /// Whether the client has emitted a diagnostic.
  unsigned emittedDiagnostic : 1;

  /// The number of solutions owned by this result.
  unsigned numSolutions = 0;

  /// A pointer to the set of solutions, of which there are
  /// \c numSolutions entries.
  Solution *solutions = nullptr;

  /// A source range that was too complex to solve.
  llvm::Optional<SourceRange> TooComplexAt = llvm::None;

  /// General constructor for the named constructors.
  SolutionResult(Kind kind) : kind(kind) {
    emittedDiagnostic = false;
  }

public:
  SolutionResult(const SolutionResult &other) = delete;

  SolutionResult(SolutionResult &&other)
      : kind(other.kind), numSolutions(other.numSolutions),
        solutions(other.solutions) {
    emittedDiagnostic = false;
    other.kind = Error;
    other.numSolutions = 0;
    other.solutions = nullptr;
  }

  SolutionResult &operator=(const SolutionResult &other) = delete;
  SolutionResult &operator=(SolutionResult &&other) = delete;

  ~SolutionResult();

  /// Produce a "solved" result, embedding the given solution.
  static SolutionResult forSolved(Solution &&solution);

  /// Produce an "ambiguous" result, providing the set of
  /// potential solutions.
  static SolutionResult forAmbiguous(MutableArrayRef<Solution> solutions);

  /// Produce a "too complex" failure, which was not yet been
  /// diagnosed.
  static SolutionResult forTooComplex(llvm::Optional<SourceRange> affected);

  /// Produce a failure that has already been diagnosed.
  static SolutionResult forError() {
    return SolutionResult(Error);
  }

  /// Produce a failure that has not yet been diagnosed.
  static SolutionResult forUndiagnosedError() {
    return SolutionResult(UndiagnosedError);
  }

  Kind getKind() const{ return kind; }

  /// Retrieve the solution, where there is one.
  const Solution &getSolution() const;

  /// Retrieve the solution, where there is one.
  Solution &&takeSolution() &&;

  /// Retrieve the set of solutions when there is an ambiguity.
  ArrayRef<Solution> getAmbiguousSolutions() const;

  /// Take the set of solutions when there is an ambiguity.
  MutableArrayRef<Solution> takeAmbiguousSolutions() &&;

  /// Retrieve a range of source that has been determined to be too
  /// complex to solve in a reasonable time.
  llvm::Optional<SourceRange> getTooComplexAt() const { return TooComplexAt; }

  /// Whether this solution requires the client to produce a diagnostic.
  bool requiresDiagnostic() const {
    switch (kind) {
    case Success:
    case Ambiguous:
    case Error:
      return false;

    case UndiagnosedError:
    case TooComplex:
      return true;
    }
    llvm_unreachable("invalid diagnostic kind");
  }

  /// Note that the failure has been diagnosed.
  void markAsDiagnosed() {
    emittedDiagnostic = true;
  }
};

} }

#endif /* SWIFT_TYPECHECK_SOLUTION_RESULT_H */
