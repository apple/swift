// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -plugin-path %swift-plugin-dir -emit-module -emit-module-path %t/OtherActors.swiftmodule -module-name OtherActors %S/Inputs/OtherActors.swift -disable-availability-checking

// RUN: %target-swift-frontend -I %t -plugin-path %swift-plugin-dir -disable-availability-checking -strict-concurrency=complete -parse-as-library %s -emit-sil -o /dev/null -verify

// REQUIRES: concurrency
// REQUIRES: asserts

actor Test {

  @TaskLocal static var local: Int?

  func withTaskLocal(isolation: isolated (any Actor)? = #isolation,
                     _ body: (consuming NonSendableValue, isolated (any Actor)?) -> Void) async {
    Self.$local.withValue(12) {
      // Unexpected errors here:
      //  error: unexpected warning produced: sending 'body' risks causing data races; this is an error in the Swift 6 language mode
      //  error: unexpected note produced: actor-isolated 'body' is captured by a actor-isolated closure. actor-isolated uses in closure may race against later nonisolated uses
      body(NonSendableValue(), isolation)
    }
  }
}

class NonSendableValue {}
