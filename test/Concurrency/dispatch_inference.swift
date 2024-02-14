// RUN: %target-typecheck-verify-swift  -disable-availability-checking %import-libdispatch -warn-concurrency

// https://github.com/apple/swift/issues/70759
// REQUIRES: GH70759
// REQUIRES: concurrency
// REQUIRES: libdispatch

import Dispatch

// Tests the inference of @_unsafeSendable and @MainActor when working with
// the Dispatch library, and specifically, DispatchQueue.
@MainActor func onlyOnMainActor() { }

func testMe() {
  DispatchQueue.main.async {
    onlyOnMainActor() // okay, due to inference of @MainActor-ness
  }
}

func testUnsafeSendableInMainAsync() async {
  var x = 5
  DispatchQueue.main.async {
    x = 17 // expected-warning{{mutation of captured var 'x' in concurrently-executing code}}
  }
  print(x)
}

func testUnsafeSendableInAsync(queue: DispatchQueue) async {
  var x = 5
  queue.async {
    x = 17 // expected-warning{{mutation of captured var 'x' in concurrently-executing code}}
  }

  queue.sync {
    x = 17 // okay
  }

  print(x)
}
