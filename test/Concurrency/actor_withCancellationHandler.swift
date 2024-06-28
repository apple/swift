// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/OtherActors.swiftmodule -module-name OtherActors %S/Inputs/OtherActors.swift -disable-availability-checking

// RUN: %target-swift-frontend -I %t  -disable-availability-checking -strict-concurrency=complete -parse-as-library %s -emit-sil -o /dev/null -verify
// RUN: %target-swift-frontend -I %t  -disable-availability-checking -strict-concurrency=complete -parse-as-library %s -emit-sil -o /dev/null -verify -enable-upcoming-feature RegionBasedIsolation

// REQUIRES: concurrency
// REQUIRES: asserts

actor Foo {
  var t: Task<Void, Error>?

  func access() {}

  func bar() {
    self.t = Task {
      await withTaskCancellationHandler {
        self.access()
      } onCancel: { @Sendable in

      }
    }
  }
}

actor Container {
  var num: Int = 0 // expected-note{{mutation of this property is only permitted within the actor}}
  func test() async {

    // no warnings:
    await withTaskCancellationHandler {
      num += 1
    } onCancel: {
      // nothing
    }
  }

  func errors() async {
    await withTaskCancellationHandler {
      num += 1 // no error, this runs synchronously on caller context
    } onCancel: {
      // this should error because cancellation is invoked concurrently
      num += 10 // expected-error{{actor-isolated property 'num' can not be mutated from a Sendable closure}}
    }
  }
}