//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_IDEBRIDGING
#define SWIFT_IDE_IDEBRIDGING

#include "swift/Basic/BasicBridging.h"

#ifdef USED_IN_CPP_SOURCE
#include "swift/Basic/SourceLoc.h"
#include "llvm/CAS/CASReference.h"
#include <optional>
#include <vector>
#endif

enum class LabelRangeType {
  /// The matched location did not have any arguments.
  None,

  /// The argument of a function/initializer/macro/... call
  ///
  /// ### Example
  /// `foo([a: ]2) or .foo([a: ]String)`
  CallArg,

  /// The parameter of a function/initializer/macro/... declaration
  ///
  /// ### Example
  /// `func foo([a b]: Int)`
  Param,

  /// The parameter of an enum case declaration
  ///
  /// ### Examples
  ///  - `case myCase([a]: Int)`
  ///  - `case myCase([]Int)`
  EnumCaseParam,

  /// Parameters of a function that can't be collapsed if they are the same.
  ///
  /// This is the case for parameters of subscripts since subscripts have
  /// unnamed
  /// parameters by default.
  ///
  /// ### Example
  /// `subscript([a a]: Int)`
  NoncollapsibleParam,

  /// A reference to a function that also specifies argument labels to
  /// disambiguate.
  ///
  /// ### Examples
  /// - `#selector(foo.func([a]:))`
  /// - `foo.func([a]:)`
  CompoundName,
};

enum class ResolvedLocContext { Default, Selector, Comment, StringLiteral };

#ifdef USED_IN_CPP_SOURCE
struct ResolvedLoc {
  /// The range of the call's base name.
  swift::CharSourceRange range;

  /// The range of the labels.
  ///
  /// What the label range contains depends on the `labelRangeType`:
  /// - Labels of calls span from the label name (excluding trivia) to the end
  ///   of the colon's trivia.
  /// - Declaration labels contain the first name and the second name, excluding
  ///   the trivia on their sides
  /// - For function arguments that don't have a label, this is an empty range
  ///   that points to the start of the argument (exculding trivia).
  ///
  /// See documentation on `DeclNameLocation.Argument` in swift-syntax for more
  /// background.
  std::vector<swift::CharSourceRange> labelRanges;

  /// The in index in `labelRanges` that belongs to the first trailing closure
  /// or `std::nullopt` if there is no trailing closure.
  std::optional<unsigned> firstTrailingLabel;

  LabelRangeType labelType;

  /// Whether the location is in an active `#if` region or not.
  bool isActive;

  ResolvedLocContext context;

  ResolvedLoc(swift::CharSourceRange range,
              std::vector<swift::CharSourceRange> labelRanges,
              std::optional<unsigned> firstTrailingLabel,
              LabelRangeType labelType, bool isActive,
              ResolvedLocContext context);

  ResolvedLoc();
};

#endif // USED_IN_CPP_SOURCE

/// An opaque, heap-allocated `ResolvedLoc`.
///
/// This type is manually memory managed. The creator of the object needs to
/// ensure that `takeUnbridged` is called to free the memory.
struct BridgedResolvedLoc {
  /// Opaque pointer to `ResolvedLoc`.
  void *resolvedLoc;

  /// This consumes `labelRanges` by calling `takeUnbridged` on it.
  SWIFT_NAME(
      "init(range:labelRanges:firstTrailingLabel:labelType:isActive:context:)")
  BridgedResolvedLoc(BridgedCharSourceRange range,
                     BridgedCharSourceRangeVector labelRanges,
                     unsigned firstTrailingLabel, LabelRangeType labelType,
                     bool isActive, ResolvedLocContext context);

#ifdef USED_IN_CPP_SOURCE
  ResolvedLoc takeUnbridged() {
    ResolvedLoc *resolvedLocPtr = static_cast<ResolvedLoc *>(resolvedLoc);
    ResolvedLoc unbridged = *resolvedLocPtr;
    delete resolvedLocPtr;
    return unbridged;
  }
#endif
};

/// A heap-allocated `std::vector<ResoledLoc>` that can be represented by an
/// opaque pointer value.
///
/// This type is manually memory managed. The creator of the object needs to
/// ensure that `takeUnbridged` is called to free the memory.
class BridgedResolvedLocVector {
  /// Opaque pointer to `std::vector<ResolvedLoc>`
  void *vector;

public:
  BridgedResolvedLocVector();

  /// Create a `BridgedResolvedLocVector` from an opaque value obtained from
  /// `getOpaqueValue`.
  BridgedResolvedLocVector(void *opaqueValue);

  /// This consumes `Loc`, calling `takeUnbridged` on it.
  SWIFT_NAME("append(_:)")
  void push_back(BridgedResolvedLoc Loc);

#ifdef USED_IN_CPP_SOURCE
  std::vector<ResolvedLoc> takeUnbridged() {
    std::vector<ResolvedLoc> *vectorPtr =
        static_cast<std::vector<ResolvedLoc> *>(vector);
    std::vector<ResolvedLoc> unbridged = *vectorPtr;
    delete vectorPtr;
    return unbridged;
  }
#endif

  SWIFT_IMPORT_UNSAFE
  void *getOpaqueValue() const;
};

#endif
