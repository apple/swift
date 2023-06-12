// RUN: %target-typecheck-verify-swift -enable-experimental-feature MoveOnlyEnumDeinits

// REQUIRES: objc_interop

// Validate that we can't mark an objc enum as move only.

@_moveOnly
@objc enum Foo : Int { // expected-error {{noncopyable enums cannot be marked '@objc'}}
  case X, Y, Z
  deinit {} // expected-error {{deinitializers cannot be declared on an @objc enum type}}
}

@_moveOnly
@objc enum Foo2 : Int { // expected-error {{noncopyable enums cannot be marked '@objc'}}
  case X, Y, Z
}

