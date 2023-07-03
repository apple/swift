//===--- PatternBindingState.h --------------------------------------------===//
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

#ifndef SWIFT_PARSE_PATTERNBINDINGSTATE_H
#define SWIFT_PARSE_PATTERNBINDINGSTATE_H

#include "swift/AST/Decl.h"
#include "swift/Parse/Token.h"

namespace swift {

class Token;

struct PatternBindingState {
  enum Kind {
    /// InBindingPattern has this value when not parsing a pattern.
    NotInBinding,

    /// InBindingPattern has this value when we're in a matching pattern, but
    /// not within a var/let pattern.  In this phase, identifiers are references
    /// to the enclosing scopes, not a variable binding.
    InMatchingPattern,

    /// InBindingPattern has this value when parsing a pattern in which bound
    /// variables are implicitly immutable, but allowed to be marked mutable by
    /// using a 'var' pattern.  This happens in for-each loop patterns.
    ImplicitlyImmutable,

    /// When InBindingPattern has this value, bound variables are mutable, and
    /// nested let/var patterns are not permitted. This happens when parsing a
    /// 'var' decl or when parsing inside a 'var' pattern.
    InVar,

    /// When InBindingPattern has this value, bound variables are immutable,and
    /// nested let/var patterns are not permitted. This happens when parsing a
    /// 'let' decl or when parsing inside a 'let' pattern.
    InLet,

    /// When InBindingPattern has this value, bound variables are mutable, and
    /// nested let/var/inout patterns are not permitted. This happens when
    /// parsing an 'inout' decl or when parsing inside an 'inout' pattern.
    InInOut,
  };

  Kind kind;

  PatternBindingState(Kind kind) : kind(kind) {}

  operator bool() const { return kind != Kind::NotInBinding; }

  operator Kind() const { return kind; }

  static llvm::Optional<PatternBindingState> get(StringRef str) {
    auto kind = llvm::StringSwitch<Kind>(str)
                    .Case("let", Kind::InLet)
                    .Case("var", Kind::InVar)
                    .Case("inout", Kind::InInOut)
                    .Default(Kind::NotInBinding);
    return PatternBindingState(kind);
  }

  /// Try to explicitly find the new pattern binding state from a token.
  explicit PatternBindingState(Token tok) : kind(NotInBinding) {
    switch (tok.getKind()) {
    case tok::kw_let:
      kind = InLet;
      break;
    case tok::kw_var:
      kind = InVar;
      break;
    case tok::kw_inout:
      kind = InInOut;
      break;
    default:
      break;
    }
  }

  /// Explicitly initialize from a VarDecl::Introducer.
  explicit PatternBindingState(VarDecl::Introducer introducer)
      : kind(NotInBinding) {
    switch (introducer) {
    case VarDecl::Introducer::Let:
      kind = InLet;
      break;
    case VarDecl::Introducer::Var:
      kind = InVar;
      break;
    case VarDecl::Introducer::InOut:
      kind = InInOut;
      break;
    }
  }

  /// If there is a direct introducer associated with this pattern binding
  /// state, return that. Return none otherwise.
  llvm::Optional<VarDecl::Introducer> getIntroducer() const {
    switch (kind) {
    case Kind::NotInBinding:
    case Kind::InMatchingPattern:
    case Kind::ImplicitlyImmutable:
      return llvm::None;
    case Kind::InVar:
      return VarDecl::Introducer::Var;
    case Kind::InLet:
      return VarDecl::Introducer::Let;
    case Kind::InInOut:
      return VarDecl::Introducer::InOut;
    }
  }

  PatternBindingState
  getPatternBindingStateForIntroducer(VarDecl::Introducer defaultValue) {
    return PatternBindingState(getIntroducer().value_or(defaultValue));
  }

  llvm::Optional<unsigned> getSelectIndexForIntroducer() const {
    switch (kind) {
    case Kind::InLet:
      return 0;
    case Kind::InInOut:
      return 1;
    case Kind::InVar:
      return 2;
    default:
      return llvm::None;
    }
  }

  bool isLet() const { return kind == Kind::InLet; }
};

} // namespace swift

#endif
