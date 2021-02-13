// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -O -module-name Swift -module-link-name swiftCore -parse-as-library -parse-stdlib -emit-module %s -o %t/Swift.swiftmodule
// RUN: %target-sil-opt -enable-sil-verify-all %t/Swift.swiftmodule -emit-sorted-sil -o - | %FileCheck %s

// Test that early serialization works as expected:
// - it happens before the performance inlining and thus preserves @_semantics functions
// - it happens after generic specialization

@frozen
public struct Int {
  @inlinable
  public init() {}
}

@frozen
public struct Array<T> {
  @inlinable
  public init() {}

  // Check that the generic version of a @_semantics function is preserved.
  // CHECK: sil [serialized] [_semantics "array.get_capacity"] [canonical] [ossa] @$sSa12_getCapacitySiyF : $@convention(method) <T> (Array<T>) -> Int
  @inlinable
  @usableFromInline
  @_semantics("array.get_capacity")
  internal func _getCapacity() -> Int {
    return Int()
  }
}

// Check that a specialized version of a function is produced
// CHECK: sil shared [serializable] [_semantics "array.get_capacity"] [canonical] [ossa] @$sSa12_getCapacitySiyFSi_Tgq5 : $@convention(method) (Array<Int>) -> Int

// Check that a call of a @_semantics function was not inlined if early-serialization is enabled.
// CHECK: sil [serialized] [canonical] [ossa] @$ss28userOfSemanticsAnnotatedFuncySiSaySiGF
// CHECK: function_ref
// CHECK: apply
@inlinable
public func userOfSemanticsAnnotatedFunc(_ a: Array<Int>) -> Int {
  return a._getCapacity()
}
