// RUN: %target-swift-frontend -strict-concurrency=minimal -o /dev/null -emit-sil %s -verify
// RUN: %target-swift-frontend -strict-concurrency=targeted -verify-additional-prefix targeted- -o /dev/null -emit-sil %s -verify
// RUN: %target-swift-frontend -strict-concurrency=complete -verify-additional-prefix targeted- -o /dev/null -emit-sil %s -verify

// REQUIRES: concurrency
// REQUIRES: asserts

class C1 { }
// expected-note @-1 {{class 'C1' does not conform to the 'Sendable' protocol}}
// expected-targeted-note @-2 {{class 'C1' does not conform to the 'Sendable' protocol}}

@_nonSendable class C2 { }
// expected-note@-1 2{{class 'C2' does not conform to the 'Sendable' protocol}}

class C3 { }
// expected-note@-1 2{{class 'C3' does not conform to the 'Sendable' protocol}}

@available(*, unavailable)
extension C3: Sendable { }

struct S1: Sendable {
  let c1: C1 // expected-warning{{stored property 'c1' of 'Sendable'-conforming struct 'S1' has non-sendable type 'C1'}}
  let c2: C2 // expected-warning{{stored property 'c2' of 'Sendable'-conforming struct 'S1' has non-sendable type 'C2'}}
  let c3: C3 // expected-warning{{stored property 'c3'}}
}

struct S2 { // expected-targeted-note {{consider making struct 'S2' conform to the 'Sendable' protocol}}
  let c1: C1
}

struct S3 { // expected-targeted-note {{consider making struct 'S3' conform to the 'Sendable' protocol}}
  let c2: C2
}


func takeSendable(_ body: @Sendable () -> Void) {
}

@available(SwiftStdlib 5.1, *)
func passSendable(
    c1: C1, c2: C2, c3: C3, fn: @escaping () -> Void, s1: S1, s2: S2, s3: S3
) async {
  // Don't warn about implicitly non-Sendable types when minimal is
  // enabled... but do when we are doing targeted
  takeSendable { print(c1) } // expected-targeted-warning {{capture of 'c1' with non-sendable type 'C1' in a `@Sendable` closure}}
  takeSendable { print(fn) } // expected-targeted-warning {{capture of 'fn' with non-sendable type '() -> Void' in a `@Sendable` closure}}
  // expected-targeted-note @-1 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

  // Warn about explicitly non-Sendable types
  takeSendable { print(c2) } // expected-warning {{capture of 'c2' with non-sendable type 'C2' in a `@Sendable` closure}}
  takeSendable { print(c3) } // expected-warning {{capture of 'c3' with non-sendable type 'C3' in a `@Sendable` closure}}

  // Don't warn about explicitly Sendable type, even when it's wrong.
  takeSendable { print(s1) }

  // Don't warn when we wrapped an implicitly non-Sendable type in a struct unless we are >= targeted
  takeSendable { print(s2) } // expected-targeted-warning {{capture of 's2' with non-sendable type 'S2' in a `@Sendable` closure}}

  // FIXME: Ideally, we would warn about cases where a type in this module is
  // inferred to be non-Sendable based on something explicitly non-Sendable,
  // like in the case below. We do warn about it with >= targeted.
  takeSendable { print(s3) } // expected-targeted-warning {{capture of 's3' with non-sendable type 'S3' in a `@Sendable` closure}}
}
