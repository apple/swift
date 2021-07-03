// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-access-control-hacks

// Only warnings in Swift 4.2 mode.

private typealias PrivateAlias = Int
// expected-note@-1 {{type alias 'PrivateAlias' is not '@usableFromInline' or public}}

internal typealias InternalAlias = Int
// expected-note@-1 {{type alias 'InternalAlias' is not '@usableFromInline' or public}}

@usableFromInline typealias UsableFromInlineAlias = Int

public typealias PublicAlias = Int

@inlinable public func f() {
  _ = PrivateAlias.self
  // expected-warning@-1 {{type alias 'PrivateAlias' is private and should not be referenced from an '@inlinable' function}}

  _ = InternalAlias.self
  // expected-warning@-1 {{type alias 'InternalAlias' is internal and should not be referenced from an '@inlinable' function}}

  _ = UsableFromInlineAlias.self

  _ = PublicAlias.self
}
