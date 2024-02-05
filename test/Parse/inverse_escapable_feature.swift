// RUN: %target-typecheck-verify-swift -parse-stdlib -module-name Swift \
// RUN:    -enable-experimental-feature NoncopyableGenerics

// REQUIRES: noncopyable_generics

struct S: ~Escapable {} // expected-error {{type '~Escapable' requires -enable-experimental-feature NonescapableTypes}}

func whatever<T: ~Escapable>(_ t: T) {} // expected-error {{type '~Escapable' requires -enable-experimental-feature NonescapableTypes}}

protocol P: ~Escapable {} // expected-error {{type '~Escapable' requires -enable-experimental-feature NonescapableTypes}}

protocol Sendable: ~Escapable {}

func example(_ t: any Sendable & ~Escapable) {} // expected-error {{type '~Escapable' requires -enable-experimental-feature NonescapableTypes}}
