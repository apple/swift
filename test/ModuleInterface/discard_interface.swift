// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t/Library.swiftinterface) %s -module-name Library -verify -enable-experimental-feature MoveOnlyResilientTypes
// RUN: %target-swift-typecheck-module-from-interface(%t/Library.swiftinterface) -I %t -enable-experimental-feature MoveOnlyResilientTypes
// RUN: %FileCheck %s < %t/Library.swiftinterface

// This test makes sure that discard is emitted correctly in the interfaces.

// CHECK: @_alwaysEmitIntoClient public consuming func AEIC_discard() { discard self }
// CHECK: @inlinable public consuming func inlinable_discard() { discard self }

public struct MoveOnlyStruct: ~Copyable {
  let x = 0

  @_alwaysEmitIntoClient public consuming func AEIC_discard() { discard self }
  @inlinable public consuming func inlinable_discard() { discard self }

  deinit {}
}
