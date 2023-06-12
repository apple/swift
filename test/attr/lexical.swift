// RUN: %target-typecheck-verify-swift

struct S {}

class C {}

enum E {}

@_eagerMove // expected-error {{'@_eagerMove' attribute cannot be applied to this declaration}}
typealias EagerTuple = (C, C)

func foo(@_noEagerMove @_eagerMove _ e: E) {} // expected-error {{@_eagerMove and @_noEagerMove attributes are alternate styles of lifetimes and can't be combined}}

func bar(@_noEagerMove _ s: S) {} // okay

func baz(@_eagerMove _ c: C) {} // okay

@_eagerMove // expected-error {{@_eagerMove is only valid on methods}}
func bazzoo(_ c: C) {}

@_noEagerMove // expected-error {{@_noEagerMove is only valid on methods}}
func bazzoozoo(_ c: C) {}

extension C {
  @_eagerMove
  func pazzoo() {}

  @_noEagerMove
  func pazzoozoo() {}
}

struct S2 {
  @_eagerMove let c: C // okay
  @_noEagerMove let e: E // okay
}

func foo() {
  @_noEagerMove let s1 = S()
  @_eagerMove var s2 = S()
  @_noEagerMove @_eagerMove let s3 = S() // expected-error {{@_eagerMove and @_noEagerMove attributes are alternate styles of lifetimes and can't be combined}}
  _ = s1
  s2 = S()
  _ = s2
  _ = s3
}

struct MoveOnly : ~Copyable {}

@_eagerMove struct MoveOnlyEagerly : ~Copyable {} // expected-error {{@_eagerMove cannot be applied to NonCopyable types}}

func zoo(@_eagerMove _ : consuming MoveOnly) {} // expected-error {{@_eagerMove cannot be applied to NonCopyable types}}

func zooo(@_noEagerMove  _ : consuming C) {} // ok, only way to spell this behavior

extension MoveOnly {
  @_eagerMove // expected-error {{@_eagerMove cannot be applied to NonCopyable types}}
  func zoo() {}
}
