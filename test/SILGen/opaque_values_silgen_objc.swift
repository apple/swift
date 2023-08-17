// RUN: %target-swift-emit-silgen -enable-sil-opaque-values -Xllvm -sil-full-demangle -primary-file %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-runtime

// Test SILGen -enable-sil-opaque-values with tests that depend on the stdlib and objc interop.

// REQUIRES: objc_interop

// CHECK-LABEL: sil {{.*}}[ossa] @$s25opaque_values_silgen_objc10UnownedBoxV5valuexvg : {{.*}} {
// CHECK:       bb0([[INSTANCE:%[^,]+]] :
// CHECK:         [[UNOWNED_VALUE:%[^,]+]] = struct_extract [[INSTANCE]]
// CHECK:         [[STRONG_VALUE:%[^,]+]] = strong_copy_unowned_value [[UNOWNED_VALUE]]
// CHECK:         return [[STRONG_VALUE]]
// CHECK-LABEL: } // end sil function '$s25opaque_values_silgen_objc10UnownedBoxV5valuexvg'
// CHECK-LABEL: sil {{.*}}[ossa] @$s25opaque_values_silgen_objc10UnownedBoxV5valueACyxGx_tcfC : {{.*}} {
// CHECK:       bb0([[STRONG_VALUE:%[^,]+]] :
// CHECK:         [[UNOWNED_VALUE:%[^,]+]] = unowned_copy_value [[STRONG_VALUE]]
// CHECK:         destroy_value [[STRONG_VALUE]]
// CHECK:         [[RETVAL:%[^,]+]] = struct $UnownedBox<T> ([[UNOWNED_VALUE]] : $@sil_unowned T)
// CHECK:         return [[RETVAL]]
// CHECK-LABEL: } // end sil function '$s25opaque_values_silgen_objc10UnownedBoxV5valueACyxGx_tcfC'
struct UnownedBox<T : AnyObject> {
  unowned var value: T
}

