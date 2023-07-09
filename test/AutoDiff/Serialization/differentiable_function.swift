// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -emit-module -parse-as-library -o %t
// RUN: llvm-bcanalyzer %t/differentiable_function.swiftmodule | %FileCheck %s -check-prefix=BCANALYZER
// RUN: %target-sil-opt -enable-sil-verify-all %t/differentiable_function.swiftmodule -o - | %FileCheck %s

// BCANALYZER-NOT: UnknownCode

import _Differentiation

func a(_ f: @differentiable(reverse) (Float) -> Float) {}
// CHECK: func a(_ f: @differentiable(reverse) (Float) -> Float)

func b(_ f: @differentiable(_linear) (Float) -> Float) {}
// CHECK: func b(_ f: @differentiable(_linear) (Float) -> Float)

func c(_ f: @differentiable(reverse) (Float, @noDerivative Float) -> Float) {}
// CHECK: func c(_ f: @differentiable(reverse) (Float, @noDerivative Float) -> Float)

func d(_ f: @differentiable(reverse) (inout Float) -> ()) {}
// CHECK: func d(_ f: @differentiable(reverse) (inout Float) -> ())

func e(_ f: @differentiable(reverse) (inout Float, inout Float) -> ()) {}
// CHECK: func e(_ f: @differentiable(reverse) (inout Float, inout Float) -> ())

func f(_ f: @differentiable(reverse) (inout Float) -> Float) {}
// CHECK: func f(_ f: @differentiable(reverse) (inout Float) -> Float)

func g(_ f: @differentiable(reverse) (inout Float, Float) -> Float) {}
// CHECK: func g(_ f: @differentiable(reverse) (inout Float, Float) -> Float)
