// VJP and pullback inlining tests.

// RUN: %target-swift-frontend -emit-sil -O -verify -Xllvm -debug-only=sil-inliner %s 2>&1 | %FileCheck %s

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

import _Differentiation
#if canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#else
import Foundation
#endif

// ======================== Simple case without control-flow ======================== //
@differentiable(reverse)
@_silgen_name("simple")
func simple(x: Float) -> Float {
    let a = x * x;
    let b = x + x;
    let c = x * a;
    let d = a + b;
    let e = b * c;

    return a * b / c + d - e ;
}

@inline(never)
@_silgen_name("caller_of_simple")
func caller_of_simple(x: Float) -> Float {
  gradient(at: Float(4), of: simple)
}

// CHECK: decision {{{.*}}, b=30, {{.*}}} simpleTJrSpSr
// CHECK-NEXT: "simpleTJrSpSr" inlined into "caller_of_simple"
// PB inlining check
// CHECK: decision {{{.*}}, b=70, {{.*}}} simpleTJpSpSr
// CHECK-NEXT: "simpleTJpSpSr" inlined into "caller_of_simple"

// ======================== Simple case with control-flow ======================== //
@differentiable(reverse)
@_silgen_name("with_control_flow")
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

// VJP inlining check
// CHECK: decision {{{.*}}, b=30, {{.*}}} with_control_flowTJrSpSr
// CHECK-NEXT: "with_control_flowTJrSpSr" inlined into "caller_of_with_control_flow"
// PB inlining check
// CHECK: decision {{{.*}}, b=70, {{.*}}} with_control_flowTJpSpSr
// CHECK-NEXT: "with_control_flowTJpSpSr" inlined into "caller_of_with_control_flow"

// ======================== Complex case with control-flow ======================== //
@differentiable(reverse)
@_silgen_name("more_complex_pb_with_control_flow")
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

// CHECK: decision {{{.*}}, b=70, {{.*}}} more_complex_pb_with_control_flowTJpSpSr
// CHECK-NEXT: "more_complex_pb_with_control_flowTJpSpSr" inlined into "caller_of_more_complex_pb_with_control_flow"