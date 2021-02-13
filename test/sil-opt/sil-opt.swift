// RUN: %target-swift-frontend -primary-file %s -module-name Swift -g -module-link-name swiftCore -O -parse-as-library -parse-stdlib -emit-module -emit-module-path - -o /dev/null | %target-sil-opt -enable-sil-verify-all -module-name="Swift" -emit-sorted-sil | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -module-name Swift -g -O -parse-as-library -parse-stdlib -emit-sib -o - | %target-sil-opt -enable-sil-verify-all -module-name="Swift" -emit-sorted-sil | %FileCheck %s -check-prefix=SIB-CHECK

// CHECK: import Builtin
// CHECK: import Swift

// CHECK: func unknown()

// CHECK: struct X {
// CHECK-NEXT:  @inlinable func test()
// CHECK-NEXT:  @inlinable init
// CHECK-NEXT: }

// CHECK-LABEL: sil [serialized] [canonical] [ossa] @$ss1XV4testyyF : $@convention(method) (X) -> ()
// CHECK: bb0
// CHECK-NEXT: function_ref
// CHECK-NEXT: function_ref @unknown : $@convention(thin) () -> ()
// CHECK-NEXT: apply
// CHECK-NEXT: tuple
// CHECK-NEXT: return

// CHECK-LABEL: sil [serialized] [canonical] [ossa] @$ss1XVABycfC : $@convention(method) (@thin X.Type) -> X
// CHECK: bb0
// CHECK-NEXT: struct $X ()
// CHECK-NEXT: return

// CHECK-LABEL: sil{{.*}} @unknown : $@convention(thin) () -> ()


// SIB-CHECK: import Builtin
// SIB-CHECK: import Swift

// SIB-CHECK: func unknown()

// SIB-CHECK: struct X {
// SIB-CHECK-NEXT:  func test()
// SIB-CHECK-NEXT:  init
// SIB-CHECK-NEXT: }

// SIB-CHECK-LABEL: sil [serialized] [canonical] [ossa] @$ss1XV4testyyF : $@convention(method) (X) -> ()
// SIB-CHECK: bb0
// SIB-CHECK-NEXT: function_ref
// SIB-CHECK-NEXT: function_ref @unknown : $@convention(thin) () -> ()
// SIB-CHECK-NEXT: apply
// SIB-CHECK-NEXT: tuple
// SIB-CHECK-NEXT: return

// SIB-CHECK-LABEL: sil [serialized] [canonical] [ossa] @$ss1XVABycfC : $@convention(method) (@thin X.Type) -> X
// SIB-CHECK: bb0
// SIB-CHECK-NEXT: struct $X ()
// SIB-CHECK-NEXT: return

// SIB-CHECK-LABEL: sil [canonical] @unknown : $@convention(thin) () -> ()

@_silgen_name("unknown")
public func unknown() -> ()

@frozen
public struct X {
  @inlinable
  public func test() {
    unknown()
  }

  @inlinable
  public init() {}
}
