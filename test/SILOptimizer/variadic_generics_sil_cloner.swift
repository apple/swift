// RUN: %target-swift-frontend -emit-sil -O %s | %FileCheck %s

@_optimize(none)
public func callee<T, each X>(_: T, _: repeat each X) {}

@_transparent
public func caller<each X>(_ x: repeat each X) {
  repeat callee(each x, repeat each x)
}

public func outerCaller<each X>(_ x: repeat each X) {
  caller(repeat each x)
}

// CHECK-LABEL: sil @$s28variadic_generics_sil_cloner11outerCalleryyxxQpRvzlF : $@convention(thin) <each X> (@pack_guaranteed Pack{repeat each X}) -> () {
// CHECK: [[CALLEE:%.*]] = function_ref @$s28variadic_generics_sil_cloner6calleeyyx_q_q_QptRv_r0_lF : $@convention(thin) <τ_0_0, each τ_0_1> (@in_guaranteed τ_0_0, @pack_guaranteed Pack{repeat each τ_0_1}) -> ()
// CHECK: apply [[CALLEE]]<@pack_element("{{.*}}") each X, Pack{repeat each X}>(
// CHECK: // end sil function '$s28variadic_generics_sil_cloner11outerCalleryyxxQpRvzlF'
