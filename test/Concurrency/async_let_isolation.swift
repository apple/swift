// First without any concurrency enabled.
// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -verify-additional-prefix without-transferring-

// Then with targeted.
// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -strict-concurrency=targeted -verify-additional-prefix without-transferring-

// Then strict-concurrency with everything.
// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -strict-concurrency=complete -verify-additional-prefix tns-

// REQUIRES: concurrency
// REQUIRES: asserts

actor MyActor {
  let immutable: Int = 17
  var text: [String] = []

  func synchronous() -> String { text.first ?? "nothing" }
  func asynchronous() async -> String { synchronous() }

  func testAsyncLetIsolation() async {
    async let x = self.synchronous()
    async let y = await self.asynchronous()
    async let z = synchronous()

    var localText = text

    async let w = localText.removeLast() // expected-without-transferring-warning {{mutation of captured var 'localText' in concurrently-executing code}}

    _ = await x
    _ = await y
    _ = await z
    _ = await w
  }
}

final class MyFinalActor {
  let immutable: Int = 17
  var text: [String] = []

  func testAsyncLetIsolation() async {
    var localText = text
    async let w = localText.removeLast() // expected-without-transferring-warning {{mutation of captured var 'localText' in concurrently-executing code}}

    _ = await w

  }
}

func outside() async {
  let a = MyActor()
  async let x = a.synchronous() // okay, await is implicit
  async let y = await a.synchronous()
  _ = await x
  _ = await y
}
