//===---- Lifetime.h - How long a value should be kept alive ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Defines a simple type-safe wrapper type to indicate the kind of lifetime that
// a value has--whether it is tied to a lexical scope or not.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/LifetimeAnnotation.h"

#ifndef SWIFT_SIL_LIFETIME_H
#define SWIFT_SIL_LIFETIME_H

namespace swift {

/// How long a value (such as instances of a type) should be kept alive--how
/// aggressively its destroys may be hoisted.
///
/// By default, types have lifetimes inferred from their structure, see
/// TypeLayoutInfo::isLexical.  It can be overridden both on
/// the type level and the value level via attributes.
struct Lifetime {
  enum Storage : uint8_t {
    /// No lifetime.  Applicable to values which aren't destroyed.
    None,
    /// A lifetime independent from the lexical scope of the value: its
    /// releases are hoisted without respect to deinit barriers.
    EagerMove,
    /// A lifetime tied to the lexical scope of the value: its releases are
    /// not hoisted over deinit barriers.
    Lexical,
  } value;

  Lifetime(decltype(value) newValue) : value(newValue) {}

  operator Storage() const { return value; }

  bool isLexical() { return value == Lifetime::Lexical; }

  bool isEagerMove() { return value == Lifetime::EagerMove; }

  /// Given a lifetime for a type and the lifetime annotation on a value of that
  /// type, the lifetime appropriate for that value.
  ///
  /// Value annotations override a type's lifetime, so the result is just the
  /// lifetime indicated by the annotation, if there is one; otherwise its the
  /// lifetime from the type.
  Lifetime getLifetimeForAnnotatedValue(LifetimeAnnotation annotation) const {
    switch (annotation) {
    case LifetimeAnnotation::None:
      return *this;
    case LifetimeAnnotation::EagerMove:
      return Lifetime::EagerMove;
    case LifetimeAnnotation::Lexical:
      return Lifetime::Lexical;
    }
  }
};

} // end swift namespace

#endif
