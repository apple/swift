// RUN: %target-swift-emit-silgen %s -disable-availability-checking | %FileCheck %s

class C<each T> {
  var values: (repeat each T)

  // CHECK-LABEL: sil hidden [exact_self_class] [ossa] @$s32variadic_generic_allocating_init1CC9fromTupleACyxxQp_QPGxxQp_t_tcfC : $@convention(method) <each T> (@pack_owned Pack{repeat each T}, @thick C<repeat each T>.Type) -> @owned C<repeat each T> {
  // CHECK: bb0(%0 : $*Pack{repeat each T}, %1 : $@thick C<repeat each T>.Type):
  init(fromTuple: (repeat each T)) {
    self.values = fromTuple
  }

  // CHECK-LABEL: sil hidden [exact_self_class] [ossa] @$s32variadic_generic_allocating_init1CC8fromPackACyxxQp_QPGxxQp_tcfC : $@convention(method) <each T> (@pack_owned Pack{repeat each T}, @thick C<repeat each T>.Type) -> @owned C<repeat each T> {
  // CHECK: bb0(%0 : $*Pack{repeat each T}, %1 : $@thick C<repeat each T>.Type):
  init(fromPack: repeat each T) {
    self.values = (repeat each fromPack)
  }
}
