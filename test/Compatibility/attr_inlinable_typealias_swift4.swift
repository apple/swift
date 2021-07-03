// RUN: %target-typecheck-verify-swift -swift-version 4 -enable-access-control-hacks

// No diagnostics at all in Swift 4.0 mode.

private typealias PrivateAlias = Int

internal typealias InternalAlias = Int

@usableFromInline typealias UsableFromInlineAlias = Int

public typealias PublicAlias = Int

@inlinable public func f() {
  _ = PrivateAlias.self

  _ = InternalAlias.self

  _ = UsableFromInlineAlias.self

  _ = PublicAlias.self
}
