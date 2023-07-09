// RUN: %target-swift-ide-test -print-module -module-to-print=LargeClassTemplates -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: struct HasTypeWithSelfAsParam<T> {
// CHECK: }

// CHECK: struct HasTypeWithSelfAsParam<Int32> {
// CHECK:   init()
// CHECK:   typealias TT = HasTypeWithSelfAsParam<HasTypeWithSelfAsParam<Int32>>
// CHECK: }

// CHECK: typealias WillBeInfinite = HasTypeWithSelfAsParam<Int32>

// TODO: we should not be importing functions that use this type in their
// signature (such as the function below).
// CHECK: mutating func test1() -> RegressionTest.ValExpr<SliceExpr<SliceExpr<Array<Int32>, _Int32_1>, _Int32_1>>
