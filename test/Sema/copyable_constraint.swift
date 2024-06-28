
// >> First verify that when building the stdlib, we do have the copyable constraint. Note the module-name!!
// RUN: %target-swift-frontend -typecheck -verify -parse-stdlib -module-name Swift %s

// Demonstrate that plain -parse-stdlib, such as in some arbitrary test, does still get the Copyable constraint
// RUN: %target-swift-frontend -typecheck -verify -parse-stdlib %s

@_marker public protocol Copyable {}

func nextTime<T>(_ t: T) {} // expected-note {{Copyable' is implicit here}}

@_moveOnly struct MO {}

// NOTE: when building without being the stdlib, we get a Builtin.Copyable
nextTime(MO()) // expected-error {{global function 'nextTime' requires that 'MO' conform to}}
