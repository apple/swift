// RUN: %target-typecheck-verify-swift

func f<each T: P>(_: repeat each T) {}

protocol P {}

struct S {}

@available(*, unavailable)
extension S: P {}
// expected-note@-1 {{conformance of 'S' to 'P' has been explicitly marked unavailable here}}

f(S())
// expected-error@-1 {{conformance of 'S' to 'P' is unavailable}}
