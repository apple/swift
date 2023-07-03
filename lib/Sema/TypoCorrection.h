//===--- TypoCorrection.h - Typo correction ---------------------*- C++ -*-===//
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
//  This file defines the interface for doing typo correction.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_TYPOCORRECTION_H
#define SWIFT_SEMA_TYPOCORRECTION_H

#include "swift/Basic/LLVM.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/DeclNameLoc.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

namespace swift {

class LookupResult;

/// A summary of how to fix a typo.  Note that this intentionally doesn't
/// carry a candidate declaration because we should be able to apply a typo
/// correction even if the corrected name resolves to an overload set.
class SyntacticTypoCorrection {
public:
  DeclNameRef WrittenName;
  DeclNameLoc Loc;
  DeclName CorrectedName;

  SyntacticTypoCorrection(DeclNameRef writtenName, DeclNameLoc writtenLoc,
                          DeclName correctedName)
    : WrittenName(writtenName), Loc(writtenLoc), CorrectedName(correctedName) {}

  void addFixits(InFlightDiagnostic &diagnostic) const;
};

/// A collection of typo-correction candidates.
class TypoCorrectionResults {
public:
  DeclNameRef WrittenName;
  DeclNameLoc Loc;
  bool ClaimedCorrection = false;

  SmallVector<ValueDecl *, 4> Candidates;

  TypoCorrectionResults(DeclNameRef writtenName, DeclNameLoc loc)
    : WrittenName(writtenName), Loc(loc) {}

  /// Try to claim a unique correction from this collection that's simple
  /// enough to include "inline" in the primary diagnostic.  Note that
  /// a single correction might still correspond to multiple candidates.
  ///
  /// The expected pattern is that CorrectedName will be added to the
  /// diagnostic (as in "did you mean <<CorrectedName>>"), and then addFixits
  /// will be called on the still-in-flight diagnostic.
  ///
  /// If this returns a correction, it flags that that's been done, and
  /// the notes subsequently emitted by noteAllCandidates will only make
  /// sense in the context of a diagnostic that suggests that the correction
  /// has happened.
  llvm::Optional<SyntacticTypoCorrection> claimUniqueCorrection();

  /// Emit a note for every candidate in the set.
  void noteAllCandidates() const;

  /// Add all the candidates to a lookup set.
  void addAllCandidatesToLookup(LookupResult &lookup) const;

  /// Look for a single candidate in this set that matches the given predicate.
  template <class Fn>
  ValueDecl *getUniqueCandidateMatching(Fn &&predicate) {
    ValueDecl *match = nullptr;

    for (auto candidate : Candidates) {
      // Ignore candidates that don't match the predicate.
      if (!predicate(candidate)) continue;

      // If we've already got a match, the match is no longer unique.
      if (match) return nullptr;

      // Record the still-unique match.
      match = candidate;
    }

    return match;
  }
};

} // end namespace swift

#endif
