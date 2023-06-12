// RUN: %target-swift-emit-silgen %s -disable-availability-checking | %FileCheck %s

typealias A<each T, U, V> = (repeat (each T, U, V))

struct G<each T> {
  typealias B<each U, V> = (repeat A<repeat each T, each U, V>)

  struct H<each U> {
    typealias C<each V> = (repeat B<repeat each U, each V>)
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s21nested_pack_expansion2fb1tyx_q_Qe_q0_txQp_tq_Qp_t_tRvzRv_r1_lF : $@convention(thin) <each T, each U, V> (@pack_guaranteed Pack{repeat (repeat (each T, /* level: 1 */ each U, V))}) -> () {
// CHECK: bb0(%0 : $*Pack{repeat (repeat (each T, /* level: 1 */ each U, V))}):
func fb<each T, each U, V>(t: G<repeat each T>.B<repeat each U, V>) {}