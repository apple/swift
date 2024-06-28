// RUN: %empty-directory(%t/src)
// RUN: split-file %s %t/src

// RUN: %target-build-swift %t/src/Interface.swift -swift-version 5 -emit-module -emit-library \
// RUN:    -target %target-cpu-apple-macosx10.15 -swift-version 5 \
// RUN:    -enable-library-evolution \
// RUN:    -module-name Interface \
// RUN:    -o %t/%target-library-name(Interface) \
// RUN:    -emit-module-interface-path %t/Interface.swiftinterface

// RUN: %target-build-swift %t/src/Types.swift -swift-version 5 -emit-module -emit-library -enable-library-evolution -module-name Types -o %t/%target-library-name(Types) \
// RUN:    -target %target-cpu-apple-macosx10.15 \
// RUN:    -I %t -L %t -l Interface \
// RUN:    -emit-module-interface-path %t/Types.swiftinterface \
// RUN:    -Xfrontend -enable-upcoming-feature -Xfrontend DynamicActorIsolation

// RUN: %target-build-swift -Xfrontend -enable-upcoming-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash1.swift -o %t/crash1.out
// RUN: %target-codesign %t/crash1.out
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash1.out 2>&1 | %FileCheck %t/src/Crash1.swift --check-prefix=LEGACY_CHECK
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash1.out 2>&1 | %FileCheck %t/src/Crash1.swift --check-prefix=SWIFT6_CHECK --dump-input=always

// RUN: %target-build-swift -Xfrontend -enable-upcoming-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash2.swift -o %t/crash2.out
// RUN: %target-codesign %t/crash2.out
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash2.out 2>&1 | %FileCheck %t/src/Crash2.swift --check-prefix=LEGACY_CHECK
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash2.out 2>&1 | %FileCheck %t/src/Crash2.swift --check-prefix=SWIFT6_CHECK --dump-input=always

// RUN: %target-build-swift -Xfrontend -enable-upcoming-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash3.swift -o %t/crash3.out
// RUN: %target-codesign %t/crash3.out
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash3.out 2>&1 | %FileCheck %t/src/Crash3.swift --check-prefix=LEGACY_CHECK
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash3.out 2>&1 | %FileCheck %t/src/Crash3.swift --check-prefix=SWIFT6_CHECK --dump-input=always

// RUN: %target-build-swift -Xfrontend -enable-upcoming-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash4.swift -o %t/crash4.out
// RUN: %target-codesign %t/crash4.out
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash4.out 2>&1 | %FileCheck %t/src/Crash4.swift --check-prefix=LEGACY_CHECK
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash4.out 2>&1 | %FileCheck %t/src/Crash4.swift --check-prefix=SWIFT6_CHECK --dump-input=always

// RUN: %target-build-swift -Xfrontend -enable-upcoming-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash5.swift -o %t/crash5.out
// RUN: %target-codesign %t/crash5.out
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash5.out 2>&1 | %FileCheck %t/src/Crash5.swift --check-prefix=LEGACY_CHECK
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash5.out 2>&1 | %FileCheck %t/src/Crash5.swift --check-prefix=SWIFT6_CHECK --dump-input=always

// RUN: %target-build-swift -Xfrontend -enable-upcoming-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash6.swift -o %t/crash6.out
// RUN: %target-codesign %t/crash6.out
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash6.out 2>&1 | %FileCheck %t/src/Crash6.swift --check-prefix=LEGACY_CHECK
// RUN: not --crash env SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash6.out 2>&1 | %FileCheck %t/src/Crash6.swift --check-prefix=SWIFT6_CHECK --dump-input=always

// REQUIRES: asserts
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime
// REQUIRES: executable_test
// REQUIRES: OS=macosx

// rdar://123810657
// UNSUPPORTED: back_deployment_runtime

//--- Interface.swift
public protocol P {
  init()

  var prop: [String] { get set }
  func test() -> Int
}

public protocol Q : P {
  func childTest()
}

//--- Types.swift
import Interface

public func runTest<T: P>(_ type: T.Type) async -> Int {
  let v = type.init()
  return v.test()
}

public func runAccessors<T: P>(_ type: T.Type) async -> [String] {
  var v = type.init()
  v.prop = ["a", "b", "c"]
  return v.prop
}

public final class Test : @preconcurrency P {
  @MainActor public var prop: [String] = []
  @MainActor public func test() -> Int { 42 }

  public init() {}
}

public actor ActorTest {
  var x: Int = 0

  public init() {}
}

extension ActorTest : @preconcurrency P {
  public var prop: [String] {
    get { [] }
    set { }
  }

  public func test() -> Int { x }
}

@MainActor
public struct TestWithParent : @preconcurrency Q {
  public var prop: [String] = []

  public init() {}

  public func test() -> Int { 42 }
  public func childTest() {}
}

public func runChildTest<T: Q>(_ type: T.Type) async {
  let v = type.init()
  return v.childTest()
}

//--- Crash1.swift
import Types
print(await runTest(Test.self))
print("OK")
// LEGACY_CHECK: @MainActor function at Types/Types.swift:16 was not called on the main thread

// Crash without good message, since via 'dispatch_assert_queue'
// SWIFT6_CHECK-NOT: OK

//--- Crash2.swift
import Types
print(await runAccessors(Test.self))
print("OK")
// LEGACY_CHECK: data race detected: @MainActor function at Types/Types.swift:15 was not called on the main thread

// Crash without good message, since via 'dispatch_assert_queue'
// SWIFT6_CHECK-NOT: OK

//--- Crash3.swift
import Types
print(await runTest(ActorTest.self))
print("OK")
// LEGACY_CHECK: data race detected: actor-isolated function at Types/Types.swift:33 was not called on the same actor

// SWIFT6_CHECK: Incorrect actor executor assumption
// SWIFT6_CHECK-NOT: OK

//--- Crash4.swift
import Types
print(await runAccessors(ActorTest.self))
print("OK")
// LEGACY_CHECK: data race detected: actor-isolated function at Types/Types.swift:30 was not called on the same actor

// SWIFT6_CHECK: Incorrect actor executor assumption
// SWIFT6_CHECK-NOT: OK

//--- Crash5.swift
import Types
print(await runTest(TestWithParent.self))
print("OK")
// LEGACY_CHECK: data race detected: @MainActor function at Types/Types.swift:40 was not called on the main thread

// Crash without good message, since via 'dispatch_assert_queue'
// SWIFT6_CHECK-NOT: OK

//--- Crash6.swift
import Types
print(await runChildTest(TestWithParent.self))
print("OK")
// LEGACY_CHECK: data race detected: @MainActor function at Types/Types.swift:40 was not called on the main thread

// Crash without good message, since via 'dispatch_assert_queue'
// SWIFT6_CHECK-NOT: OK
