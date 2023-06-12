// RUN: %target-typecheck-verify-swift

// this test verifies what features are still behind the experimental flag by not providing it.

struct SomeValue {}

@_moveOnly class NoncopyableClass {} // expected-error {{'@_moveOnly' attribute is only valid on structs or enums}}

func checkOldConsumeName() {
  let x = SomeValue()

  let _ = _move x // expected-error {{cannot find '_move' in scope}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-2 {{expression of type 'SomeValue' is unused}}
}

func checkBorrow() {
  let x = SomeValue()

  let _ = _borrow x // expected-error {{cannot find '_borrow' in scope}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-2 {{expression of type 'SomeValue' is unused}}
}

func checkNoImplicitCopy1(@_noImplicitCopy x: SomeValue) {}
// expected-error@-1 {{Can not use feature when experimental move only is disabled! Pass the frontend flag -enable-experimental-move-only to swift to enable the usage of this language feature}}

func checkNoImplicitCopy2(_ x: SomeValue) {
  @_noImplicitCopy let y = x
  // expected-error@-1 {{Can not use feature when experimental move only is disabled! Pass the frontend flag -enable-experimental-move-only to swift to enable the usage of this language feature}}
  checkNoImplicitCopy2(y)
}

// coverage to ensure the feature flag is working
#if $MoveOnly
  func guardedFn() {}
#endif

func caller() {
  guardedFn()
}



