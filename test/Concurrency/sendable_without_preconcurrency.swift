// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/StrictModule.swiftmodule -module-name StrictModule -swift-version 6 %S/Inputs/StrictModule.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/NonStrictModule.swiftmodule -module-name NonStrictModule %S/Inputs/NonStrictModule.swift
// RUN: %target-swift-frontend -strict-concurrency=minimal -disable-availability-checking -I %t -verify %s -o /dev/null -emit-sil
// RUN: %target-swift-frontend -strict-concurrency=targeted -disable-availability-checking -I %t -verify %s -o /dev/null -emit-sil -verify-additional-prefix targeted-complete-
// RUN: %target-swift-frontend -strict-concurrency=complete -disable-availability-checking -I %t -verify %s -o /dev/null -emit-sil -verify-additional-prefix complete- -verify-additional-prefix targeted-complete- -verify-additional-prefix tns-

// REQUIRES: concurrency

import StrictModule // no remark: we never recommend @preconcurrency due to an explicitly non-Sendable (via -strict-concurrency=complete) type
import NonStrictModule

actor A {
  func f() -> [StrictStruct: NonStrictClass] { [:] }
}

class NS { }

struct MyType {
  var nsc: NonStrictClass
}

struct MyType2 {
  var nsc: NonStrictClass
  var ns: NS
}

func testA(ns: NS, mt: MyType, mt2: MyType2, sc: StrictClass, nsc: NonStrictClass) async {
  Task { // expected-tns-warning {{task-isolated value of type '() async -> ()' passed as a strongly transferred parameter; later accesses could race}}
    print(ns)
    print(mt) // no warning by default: MyType is Sendable because we suppressed NonStrictClass's warning
    print(mt2)
    print(sc)
  }
}

extension NonStrictStruct: @retroactive @unchecked Sendable { }

class StrictSubclass: StrictClass {
  override func send(_ body: () -> ()) {}
  override func dontSend(_ body: () -> ()) {}
}

struct StrictConformer: StrictProtocol {
  func send(_ body: () -> Void) {}
  func dontSend(_ body: () -> Void) {}
}

class NonStrictSubclass: NonStrictClass {
  override func send(_ body: () -> ()) {}
  override func dontSend(_ body: () -> ()) {}
}

struct NonStrictConformer: NonStrictProtocol {
  func send(_ body: () -> Void) {}
  func dontSend(_ body: () -> Void) {}
}
