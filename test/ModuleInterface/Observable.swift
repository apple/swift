// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t/Library.swiftinterface) %s -module-name Library -plugin-path %swift-host-lib-dir/plugins -disable-availability-checking -enable-experimental-feature InitAccessors
// RUN: %target-swift-typecheck-module-from-interface(%t/Library.swiftinterface) -module-name Library -disable-availability-checking
// RUN: %FileCheck %s < %t/Library.swiftinterface

// Asserts is required for '-enable-experimental-feature InitAccessors'.
// REQUIRES: asserts

// REQUIRES: swift_swift_parser
// REQUIRES: observation

import Observation

// CHECK-NOT: @Observable
// CHECK-NOT: @ObservationIgnored
// CHECK-NOT: @ObservationTracked

@Observable
public class SomeClass {
  public var field = 3
  @ObservationIgnored public var ignored = 4
}

public func requiresObservable<T: Observable>(_: T) { }

@inlinable func useObservable(sc: SomeClass) {
  requiresObservable(sc)
}
