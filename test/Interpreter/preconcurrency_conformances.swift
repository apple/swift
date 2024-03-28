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
// RUN:    -Xfrontend -enable-experimental-feature -Xfrontend DynamicActorIsolation

// RUN: %target-build-swift -Xfrontend -enable-experimental-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash1.swift -o %t/crash1.out
// RUN: %target-codesign %t/crash1.out
// RUN: not --crash env SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash1.out 2>&1 | %FileCheck %t/src/Crash1.swift

// RUN: %target-build-swift -Xfrontend -enable-experimental-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash2.swift -o %t/crash2.out
// RUN: %target-codesign %t/crash2.out
// RUN: not --crash env SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash2.out 2>&1 | %FileCheck %t/src/Crash2.swift

// RUN: %target-build-swift -Xfrontend -enable-experimental-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash3.swift -o %t/crash3.out
// RUN: %target-codesign %t/crash3.out
// RUN: not --crash env SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash3.out 2>&1 | %FileCheck %t/src/Crash3.swift

// RUN: %target-build-swift -Xfrontend -enable-experimental-feature -Xfrontend DynamicActorIsolation -I %t -L %t -l Types %t/src/Crash4.swift -o %t/crash4.out
// RUN: %target-codesign %t/crash4.out
// RUN: not --crash env SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=2 %target-run %t/crash4.out 2>&1 | %FileCheck %t/src/Crash4.swift

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

//--- Crash1.swift
import Types
print(await runTest(Test.self))
// CHECK: Incorrect actor executor assumption; Expected MainActor executor

//--- Crash2.swift
import Types
print(await runAccessors(Test.self))
// CHECK: Incorrect actor executor assumption; Expected MainActor executor

//--- Crash3.swift
import Types
print(await runTest(ActorTest.self))
// CHECK: Incorrect actor executor assumption

//--- Crash4.swift
import Types
print(await runAccessors(ActorTest.self))
// CHECK: Incorrect actor executor assumption
