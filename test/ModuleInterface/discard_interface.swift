// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t/Library.swiftinterface) %s -module-name Library -verify
// RUN: %target-swift-typecheck-module-from-interface(%t/Library.swiftinterface) -I %t
// RUN: %FileCheck %s < %t/Library.swiftinterface

// This test makes sure that discard and _forget are emitted correctly in the
// interfaces. We expect that if you use the old name _forget, you'll get that
// in the interface file.

// CHECK: @_alwaysEmitIntoClient public consuming func AEIC_discard() { discard self }
// CHECK: @inlinable public consuming func inlinable_discard() { discard self }

// CHECK: @_alwaysEmitIntoClient public consuming func AEIC_forget() { _forget self }
// CHECK: @inlinable public consuming func inlinable_forget() { _forget self }

public struct MoveOnlyStruct: ~Copyable {
  let x = 0

  @_alwaysEmitIntoClient public consuming func AEIC_discard() { discard self }
  @inlinable public consuming func inlinable_discard() { discard self }

  @_alwaysEmitIntoClient public consuming func AEIC_forget() { _forget self }
  // expected-warning@-1 {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}
  @inlinable public consuming func inlinable_forget() { _forget self }
  // expected-warning@-1 {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}

  deinit {}
}
