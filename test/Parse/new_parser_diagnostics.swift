// RUN: %target-typecheck-verify-swift -enable-experimental-feature ParserDiagnostics

// FIXME: Swift parser is not enabled on Linux CI yet.
// REQUIRES: OS=macosx
// REQUIRES: asserts

_ = [(Int) -> async throws Int]()
// expected-error@-1{{'async throws' must precede '->'}}
// expected-note@-2{{move 'async throws' in front of '->'}}{{15-21=}} {{21-28=}} {{20-21= }} {{12-12=async }} {{12-12=throws }}
