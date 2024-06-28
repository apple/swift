// Pullback inlining tests.

// RUN: %target-swift-frontend -emit-sil -O -verify -Xllvm -debug-only=sil-inliner %s 2>&1 | %FileCheck %s

// REQUIRES: asserts
// REQUIRES: swift_in_compiler
// UNSUPPORTED: OS=windows-msvc

import _Differentiation
#if canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#else
import Foundation
#endif

// =============================================================== //
// Pullbacks with control-flow are inlined into non-VJP callers
// =============================================================== //

@differentiable(reverse)
func with_control_flow(_ x: Float) -> Float {
  if (x > 0) {
    return sin(x) * cos(x)
  } else {
    return sin(x) + cos(x)
  }
}

@inline(never)
@_silgen_name("caller_of_with_control_flow")
func caller_of_with_control_flow(x: Float) -> Float {
    gradient(at: x, of: with_control_flow)
}
// CHECK-LABEL: decision {{.*}} $s17pullback_inlining17with_control_flowyS2fFTJpSpSr
// CHECK-NEXT: "pullback of pullback_inlining.with_control_flow(_:)" inlined into "caller_of_with_control_flow"

// ====================================================================== //
// Pullbacks with complex control-flow are inlined into non-VJP callers
// ====================================================================== //
@differentiable(reverse)
func more_complex_pb_with_control_flow(x: Float) -> Float {
    if (x > 0) {
        if ((x+1) < 5) {
            if (x*2 > 4) {
                let y = square(x: x)
                if (y >= x) {
                    let d = double(x: x)
                    return x - (d*y)
                } else {
                    let e = square(x: y)
                    return x + (e*y)
                }
            }
        }
    } else {
        let y = double(x: x)
        return x * y
    }

    return x*3
}


@differentiable(reverse)
func double(x: Float) -> Float {
    return x + x
}

@differentiable(reverse)
func square(x: Float) -> Float {
    return x * x
}

@inline(never)
@_silgen_name("caller_of_more_complex_pb_with_control_flow")
func caller_of_more_complex_pb_with_control_flow() -> Float {
    // Need to pass a constant argument to `gradient` so that the call
    // to VJP also receives a constant argument, and it is in turn inlined
    // as a "pure" call. 
    //
    // Only after the VJP is inlined can the pullback be inlined, as the 
    // full signature of the pullback (with the branch-trace enum) is never
    // visible at the call site otherwise.
    gradient(at: Float(1), of: more_complex_pb_with_control_flow)
}

// CHECK: decision {{.*}} $s17pullback_inlining33more_complex_pb_with_control_flow1xS2f_tFTJpSpSr
// CHECK-NEXT: "pullback of pullback_inlining.more_complex_pb_with_control_flow(x:)" inlined into "caller_of_more_complex_pb_with_control_flow"
