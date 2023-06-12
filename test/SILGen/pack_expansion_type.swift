// RUN: %target-swift-emit-silgen %s -disable-availability-checking | %FileCheck %s

// CHECK-LABEL: sil [ossa] @$s19pack_expansion_type16variadicFunction1t1ux_q_txQp_txxQp_q_xQptRvzRv_q_Rhzr0_lF : $@convention(thin) <each T, each U where (repeat (each T, each U)) : Any> (@pack_guaranteed Pack{repeat each T}, @pack_guaranteed Pack{repeat each U}) -> @pack_out Pack{repeat (each T, each U)} {
// CHECK: bb0(%0 : $*Pack{repeat (each T, each U)}, %1 : $*Pack{repeat each T}, %2 : $*Pack{repeat each U}):
public func variadicFunction<each T, each U>(t: repeat each T, u: repeat each U) -> (repeat (each T, each U)) {}

public struct VariadicType<each T> {
  // CHECK-LABEL: sil [ossa] @$s19pack_expansion_type12VariadicTypeV14variadicMethod1t1ux_qd__txQp_txxQp_qd__xQptRvd__qd__RhzlF :
  // CHECK-SAME: $@convention(method) <each T><each U where (repeat (each T, each U)) : Any> (@pack_guaranteed Pack{repeat each T}, @pack_guaranteed Pack{repeat each U}, VariadicType<repeat each T>) -> @pack_out Pack{repeat (each T, each U)} {
  // CHECK: bb0(%0 : $*Pack{repeat (each T, each U)}, %1 : $*Pack{repeat each T}, %2 : $*Pack{repeat each U}, %3 : $VariadicType<repeat each T>):
  public func variadicMethod<each U>(t: repeat each T, u: repeat each U) -> (repeat (each T, each U)) {}

  // CHECK-LABEL: sil [ossa] @$s19pack_expansion_type12VariadicTypeV13takesFunction1tyqd__qd__Qp_txxQpXE_tRvd__lF :
  // CHECK-SAME: $@convention(method) <each T><each U> (@guaranteed @noescape @callee_guaranteed @substituted <each τ_0_0, each τ_0_1> (@pack_guaranteed Pack{repeat each τ_0_0}) -> @pack_out Pack{repeat each τ_0_1} for <Pack{repeat each T}, Pack{repeat each U}>, VariadicType<repeat each T>) -> () {
  // CHECK: bb0(%0 : @guaranteed $@noescape @callee_guaranteed @substituted <each τ_0_0, each τ_0_1> (@pack_guaranteed Pack{repeat each τ_0_0}) -> @pack_out Pack{repeat each τ_0_1} for <Pack{repeat each T}, Pack{repeat each U}>, %1 : $VariadicType<repeat each T>):
  public func takesFunction<each U>(t: (repeat each T) -> (repeat each U)) {}
}

// CHECK-LABEL: sil hidden [ossa] @$s19pack_expansion_type17variadicMetatypesyyxxQpRvzlF : $@convention(thin) <each T> (@pack_guaranteed Pack{repeat each T}) -> () {
// CHECK: bb0(%0 : $*Pack{repeat each T}):
// CHECK: metatype $@thin VariadicType<>.Type
// CHECK: metatype $@thin VariadicType<Int>.Type
// CHECK: metatype $@thin VariadicType<Int, String>.Type
// CHECK: metatype $@thin VariadicType<repeat each T>.Type
// CHECK: metatype $@thin VariadicType<Int, repeat Array<each T>>.Type
// CHECK: metatype $@thin (repeat each T).Type
// CHECK: metatype $@thin (Int, repeat Array<each T>).Type
// CHECK: metatype $@thin ((repeat each T) -> ()).Type
// CHECK: metatype $@thin ((Int, repeat Array<each T>) -> ()).Type
// CHECK: return

func variadicMetatypes<each T>(_: repeat each T) {
  _ = VariadicType< >.self
  _ = VariadicType<Int>.self
  _ = VariadicType<Int, String>.self
  _ = VariadicType<repeat each T>.self
  _ = VariadicType<Int, repeat Array<each T>>.self
  _ = (repeat each T).self
  _ = (Int, repeat Array<each T>).self
  _ = ((repeat each T) -> ()).self
  _ = ((Int, repeat Array<each T>) -> ()).self
}

// Mangling bugs with substitutions

// CHECK-LABEL: sil [ossa] @$s19pack_expansion_type18sameExpansionTwice2us05more_G02vsyxxQp_xxQpq_q_QptRvzRv_r0_lF : $@convention(thin) <each U, each V> (@pack_guaranteed Pack{repeat each U}, @pack_guaranteed Pack{repeat each U}, @pack_guaranteed Pack{repeat each V}) -> () {
public func sameExpansionTwice<each U, each V>(us: repeat each U, more_us: repeat each U, vs: repeat each V) {}

// SILVerifier bug
public func nonPackAndPackParameterInExpansion<each T, U, V>(t: repeat each T, u: U, v: V) -> (repeat (each T, U, V)) {
  return (repeat (each t, u, v))
}
