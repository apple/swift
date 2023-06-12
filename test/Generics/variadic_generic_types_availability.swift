// RUN: %target-typecheck-verify-swift

// REQUIRES: OS=macosx

struct G<each T> {}
// expected-note@-1 {{add @available attribute to enclosing generic struct}}
// expected-error@-2 {{parameter packs in generic types are only available in macOS 99.99.0 or newer}}

// Type aliases are OK
typealias A<each T> = (repeat each T)