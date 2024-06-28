// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -emit-module-path %t/StrictModule.swiftmodule -module-name StrictModule -swift-version 6 %S/Inputs/StrictModule.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/NonStrictModule.swiftmodule -module-name NonStrictModule %S/Inputs/NonStrictModule.swift

// RUN: %target-swift-frontend -strict-concurrency=targeted -disable-availability-checking -I %t %s -o /dev/null -verify -emit-sil
// RUN: %target-swift-frontend -disable-availability-checking -I %t %s -o /dev/null -verify -emit-sil -strict-concurrency=complete -verify-additional-prefix tns-

// REQUIRES: concurrency

import StrictModule // no remark: we never recommend @preconcurrency due to an explicitly non-Sendable (via -strict-concurrency=complete) type
@preconcurrency import NonStrictModule

actor A {
  func f() -> [StrictStruct: NonStrictClass] { [:] }
}

class NS { } // expected-note {{class 'NS' does not conform to the 'Sendable' protocol}}

struct MyType {
  var nsc: NonStrictClass
}

struct MyType2: Sendable {
  var nsc: NonStrictClass // no warning; @preconcurrency suppressed it
  var ns: NS // expected-warning{{stored property 'ns' of 'Sendable'-conforming struct 'MyType2' has non-sendable type 'NS'}}
}

struct MyType3 {
  var nsc: NonStrictClass
}

func testA(ns: NS, mt: MyType, mt2: MyType2, mt3: MyType3, sc: StrictClass, nsc: NonStrictClass) async {
  // This is task isolated since we are capturing function arguments.
  Task { // expected-tns-warning {{task-isolated value of type '() async -> ()' passed as a strongly transferred parameter}}
    print(ns)
    print(mt) // no warning: MyType is Sendable because we suppressed NonStrictClass's warning
    print(mt2)
    print(mt3)
    print(sc)
    print(nsc)
  }
}

extension NonStrictStruct: @retroactive @unchecked Swift.Sendable { }

class StrictSubclass: StrictClass {
  override func send(_ body: () -> ()) {}
  override func dontSend(_ body: @Sendable () -> ()) {} // expected-warning {{declaration 'dontSend' has a type with different sendability from any potential overrides}}
}

struct StrictConformer: StrictProtocol {
  func send(_ body: () -> Void) {}
  func dontSend(_ body: @Sendable () -> Void) {} // expected-warning {{sendability of function types in instance method 'dontSend' does not match requirement in protocol 'StrictProtocol'}}
}

class NonStrictSubclass: NonStrictClass {
  override func send(_ body: () -> ()) {}
  override func dontSend(_ body: @Sendable () -> ()) {} // no-warning
}

struct NonStrictConformer: NonStrictProtocol {
  func send(_ body: () -> Void) {}
  func dontSend(_ body: @Sendable () -> Void) {} // no-warning
}
