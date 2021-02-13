// RUN: %target-swift-frontend -primary-file %s -module-name Swift -g -module-link-name swiftCore -O -parse-as-library -parse-stdlib -emit-module -emit-module-path - -o /dev/null | %target-sil-func-extractor -module-name="Swift" -func='$ss1XV4testyyF' | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -module-name Swift -g -O -parse-as-library -parse-stdlib -emit-sib -o - | %target-sil-func-extractor -module-name="Swift" -func='$ss1XV4testyyF' | %FileCheck %s -check-prefix=SIB-CHECK

// CHECK: import Builtin
// CHECK: import Swift

// CHECK: func unknown()

// CHECK: struct X {
// CHECK-NEXT:  @usableFromInline
// CHECK-NEXT:  @inlinable func test()
// CHECK-NEXT:  init
// CHECK-NEXT: }

// CHECK-LABEL: sil [serialized] [canonical] [ossa] @$ss1XV4testyyF : $@convention(method) (X) -> ()
// CHECK: bb0
// CHECK-NEXT: function_ref
// CHECK-NEXT: function_ref @unknown : $@convention(thin) () -> ()
// CHECK-NEXT: apply
// CHECK-NEXT: tuple
// CHECK-NEXT: return

// CHECK: sil [canonical] @unknown : $@convention(thin) () -> ()

// CHECK-NOT: sil {{.*}} @$ss1XVABycfC : $@convention(thin) (@thin X.Type) -> X


// SIB-CHECK: import Builtin
// SIB-CHECK: import Swift

// SIB-CHECK: func unknown()

// SIB-CHECK: struct X {
// SIB-CHECK-NEXT:  @usableFromInline
// SIB-CHECK-NEXT:  @inlinable func test()
// SIB-CHECK-NEXT:  init
// SIB-CHECK-NEXT: }

// SIB-CHECK-LABEL: sil [serialized] [canonical] [ossa] @$ss1XV4testyyF : $@convention(method) (X) -> ()
// SIB-CHECK: bb0
// SIB-CHECK-NEXT: function_ref
// SIB-CHECK-NEXT: function_ref @unknown : $@convention(thin) () -> ()
// SIB-CHECK-NEXT: apply
// SIB-CHECK-NEXT: tuple
// SIB-CHECK-NEXT: return

// SIB-CHECK: sil [canonical] @unknown : $@convention(thin) () -> ()

// SIB-CHECK-NOT: sil {{.*}} @$ss1XVABycfC : $@convention(thin) (@thin X.Type) -> X

@_silgen_name("unknown")
public func unknown() -> ()

public struct X {
  @usableFromInline
  @inlinable
  func test() {
    unknown()
  }
}
