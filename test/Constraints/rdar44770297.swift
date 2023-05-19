// RUN: %target-typecheck-verify-swift

protocol P {
  associatedtype A
}

func foo<T: P>(_: () throws -> T) -> T.A? { // expected-note {{where 'T' = 'Never'}}
  fatalError()
}

let _ = foo() {fatalError()} & nil
// expected-error@-1 {{global function 'foo' requires that 'Never' conform to 'P'}}
// expected-error@-2 {{value of optional type 'Never.A?' must be unwrapped to a value of type 'Never.A'}}
// expected-note@-3 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
// expected-note@-4 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
