// REQUIRES: swift_swift_parser
// REQUIRES: asserts

// Checks that skipping function bodies doesn't cause the new parser validation
// to fail. This can currently be the case because the new parser doesn't
// support skipping, causing validation fail as it generates diagnostics when
// the C++ parser would not.

// RUN: %target-typecheck-verify-swift -enable-experimental-feature ParserValidation
// RUN: %target-swift-frontend -typecheck %s -enable-experimental-feature ParserValidation -experimental-skip-all-function-bodies

func bad() {
  _ = [(Int) -> async throws Int]()
  // expected-error@-1{{'throws' may only occur before '->'}}
  // expected-error@-2{{'async' may only occur before '->'}}
}

