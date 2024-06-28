// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -emit-module-path %t/GlobalVariables.swiftmodule -module-name GlobalVariables %S/Inputs/GlobalVariables.swift -disable-availability-checking -parse-as-library

// RUN: %target-swift-frontend -I %t -strict-concurrency=complete -parse-as-library %s -emit-sil -o /dev/null -verify

// REQUIRES: concurrency
// REQUIRES: asserts

class GlobalCounter { // expected-note{{class 'GlobalCounter' does not conform to the 'Sendable' protocol}}
  var counter: Int = 0
}

let rs = GlobalCounter() // expected-warning {{let 'rs' is not concurrency-safe because non-'Sendable' type 'GlobalCounter' may have shared mutable state; this is an error in the Swift 6 language mode}}
// expected-note@-1 {{disable concurrency-safety checks if accesses are protected by an external synchronization mechanism}}
// expected-note@-2 {{annotate 'rs' with '@MainActor' if property should only be accessed from the main actor}}

import GlobalVariables

class MyError: Error { // expected-warning{{non-final class 'MyError' cannot conform to 'Sendable'; use '@unchecked Sendable'}}
  var storage = 0 // expected-warning{{stored property 'storage' of 'Sendable'-conforming class 'MyError' is mutable}}
}

func testWarnings() {
  _ = rs
  _ = globalInt // expected-warning{{reference to var 'globalInt' is not concurrency-safe because it involves shared mutable state}}
  globalInt += 1 // expected-warning{{reference to var 'globalInt' is not concurrency-safe because it involves shared mutable state}}
}
