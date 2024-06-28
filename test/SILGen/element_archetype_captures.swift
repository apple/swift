// RUN: %target-swift-emit-silgen %s | %FileCheck %s

// CHECK-LABEL: sil hidden [ossa] @$s26element_archetype_captures6calleeyyx_q_q0_tSTRzSTR_7ElementQy_ACRtzr1_lF : $@convention(thin) <T, U, V where T : Sequence, U : Sequence, T.Element == U.Element> (@in_guaranteed T, @in_guaranteed U, @in_guaranteed V) -> () {

func callee<T, U, V>(_: T, _: U, _: V) where T: Sequence, U: Sequence, T.Element == U.Element {}

// CHECK-LABEL: sil hidden [ossa] @$s26element_archetype_captures12packFunction2ts2us2vsyxxQp_q_xQpq0_q0_QptRvzRv_Rv0_STRzSTR_7ElementQy_AFRtzr1_lF : $@convention(thin) <each T, each U, each V where repeat each T : Sequence, repeat each U : Sequence, repeat (each T).Element == (each U).Element> (@pack_guaranteed Pack{repeat each T}, @pack_guaranteed Pack{repeat each U}, @pack_guaranteed Pack{repeat each V}) -> () {
func packFunction<each T, each U, each V>(ts: repeat each T, us: repeat each U, vs: repeat each V)
    where repeat each T: Sequence,
          repeat each U: Sequence,
          repeat (each T).Element == (each U).Element {
  for (t, u) in repeat (each ts, each us) {

    // // CHECK-LABEL: sil private [ossa] @$s26element_archetype_captures12packFunction2ts2us2vsyxxQp_q_xQpq0_q0_QptRvzRv_Rv0_STRzSTR_7ElementQy_AFRtzr1_lF10middleFuncL_yyRvzRv_Rv0_STRzSTR_AgHRSr1_lF : $@convention(thin) <each T, each U, each V where repeat each T : Sequence, repeat each U : Sequence, repeat (each T).Element == (each U).Element><τ_1_0, τ_1_1 where τ_1_0 : Sequence, τ_1_1 : Sequence, τ_1_0.Element == τ_1_1.Element> (@in_guaranteed (repeat each V), @in_guaranteed τ_1_0, @in_guaranteed τ_1_1) -> () {
    func middleFunc() {
      for v in repeat each vs {

        // CHECK-LABEL: sil private [ossa] @$s26element_archetype_captures12packFunction2ts2us2vsyxxQp_q_xQpq0_q0_QptRvzRv_Rv0_STRzSTR_7ElementQy_AFRtzr1_lF10middleFuncL_yyRvzRv_Rv0_STRzSTR_AgHRSr1_lF05innerK0L_yyRvzRv_Rv0_STRzSTR_AgHRSr1_lF : $@convention(thin) <each T, each U, each V where repeat each T : Sequence, repeat each U : Sequence, repeat (each T).Element == (each U).Element><τ_1_0, τ_1_1 where τ_1_0 : Sequence, τ_1_1 : Sequence, τ_1_0.Element == τ_1_1.Element><τ_2_0> (@in_guaranteed τ_1_0, @in_guaranteed τ_1_1, @in_guaranteed τ_2_0) -> () {
        func innerFunc() {

          // CHECK: [[FN:%.*]] = function_ref @$s26element_archetype_captures6calleeyyx_q_q0_tSTRzSTR_7ElementQy_ACRtzr1_lF : $@convention(thin) <τ_0_0, τ_0_1, τ_0_2 where τ_0_0 : Sequence, τ_0_1 : Sequence, τ_0_0.Element == τ_0_1.Element> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1, @in_guaranteed τ_0_2) -> ()
          // CHECK: apply [[FN]]<τ_1_0, τ_1_1, τ_2_0>(%0, %1, %2) : $@convention(thin) <τ_0_0, τ_0_1, τ_0_2 where τ_0_0 : Sequence, τ_0_1 : Sequence, τ_0_0.Element == τ_0_1.Element> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1, @in_guaranteed τ_0_2) -> ()
          callee(t, u, v)
        }

        innerFunc()
      }
    }

    middleFunc()
  }
}

// Make sure we don't inadvertently say that `each T` is captured by
// `localFunc()`.
public func anotherPackFunction<each T>(_ ts: repeat each T) {
  // CHECK-LABEL: sil private [ossa] @$s26element_archetype_captures19anotherPackFunctionyyxxQpRvzlF9localFuncL_yyRvzlF : $@convention(thin) <each T> (@in_guaranteed (repeat each T)) -> () {
  func localFunc() {
    for t in repeat each ts {
      _ = t
    }
  }
}

public func varCaptures<each T, each U>(ts: repeat each T, us: repeat each U) {
  for t in repeat each ts {
    for u in repeat each us {
      var both = (t, u)
      both = (t, u)
      let capture_both = { both = (t, u) }
      capture_both()

      var just_u = u
      just_u = u
      let capture_u = { _ = t; just_u = u }
      capture_u()

      var just_t = t
      just_t = t
      let capture_t = { just_t = t; _ = u }
      capture_t()
    }
  }
}

