// RUN: %target-swift-frontend -experimental-performance-annotations %s -O -sil-verify-all -module-name=test -emit-sil | %FileCheck %s

// REQUIRES: swift_in_compiler

// Check that constant propagation of MemoryLayout is also done at -Onone to ensure that
// no metadata is created at runtime - which would violate the performance annotation.

// CHECK-LABEL: sil [no_locks] @$s4test7getSizeSiyF
// CHECK:         [[I:%[0-9]+]] = integer_literal {{.*}}, 4
// CHECK:         [[S:%[0-9]+]] = struct {{.*}}([[I]]
// CHECK:         return [[S]]
// CHECK:       } // end sil function '$s4test7getSizeSiyF'
@_noLocks
public func getSize() -> Int {
  return MemoryLayout<Int32>.size
}

// CHECK-LABEL: sil [no_locks] @$s4test12getAlignmentSiyF
// CHECK:         [[I:%[0-9]+]] = integer_literal {{.*}}, 4
// CHECK:         [[S:%[0-9]+]] = struct {{.*}}([[I]]
// CHECK:         return [[S]]
// CHECK:       } // end sil function '$s4test12getAlignmentSiyF'
@_noLocks
public func getAlignment() -> Int {
  return MemoryLayout<Int32>.alignment
}

// CHECK-LABEL: sil [no_locks] @$s4test9getStrideSiyF
// CHECK:         [[I:%[0-9]+]] = integer_literal {{.*}}, 4
// CHECK:         [[S:%[0-9]+]] = struct {{.*}}([[I]]
// CHECK:         return [[S]]
// CHECK:       } // end sil function '$s4test9getStrideSiyF'
@_noLocks
public func getStride() -> Int {
  return MemoryLayout<Int32>.stride
}

