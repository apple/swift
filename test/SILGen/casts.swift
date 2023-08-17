
// RUN: %target-swift-emit-silgen -module-name casts %s | %FileCheck %s

class B { }
class D : B { }

// CHECK-LABEL: sil hidden [ossa] @$s5casts6upcast{{[_0-9a-zA-Z]*}}F
func upcast(d: D) -> B {
  // CHECK: {{%.*}} = upcast
  return d
}
// CHECK-LABEL: sil hidden [ossa] @$s5casts8downcast{{[_0-9a-zA-Z]*}}F
func downcast(b: B) -> D {
  // CHECK: {{%.*}} = unconditional_checked_cast
  return b as! D
}

// CHECK-LABEL: sil hidden [ossa] @$s5casts3isa{{[_0-9a-zA-Z]*}}F
func isa(b: B) -> Bool {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $B):
  // CHECK:   [[COPIED_BORROWED_ARG:%.*]] = copy_value [[ARG]]
  // CHECK:   checked_cast_br B in [[COPIED_BORROWED_ARG]] : $B to D, [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]
  //
  // CHECK: [[YES]]([[CASTED_VALUE:%.*]] : @owned $D):
  // CHECK:   integer_literal {{.*}} -1
  // CHECK:   destroy_value [[CASTED_VALUE]]
  //
  // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : @owned $B):
  // CHECK:   destroy_value [[ORIGINAL_VALUE]]
  // CHECK:   integer_literal {{.*}} 0
  return b is D
}

// CHECK-LABEL: sil hidden [ossa] @$s5casts16upcast_archetype{{[_0-9a-zA-Z]*}}F
func upcast_archetype<T : B>(t: T) -> B {
  // CHECK: {{%.*}} = upcast
  return t
}

// CHECK-LABEL: sil hidden [ossa] @$s5casts25upcast_archetype_metatype{{[_0-9a-zA-Z]*}}F
func upcast_archetype_metatype<T : B>(t: T.Type) -> B.Type {
  // CHECK: {{%.*}} = upcast
  return t
}

// CHECK-LABEL: sil hidden [ossa] @$s5casts18downcast_archetype{{[_0-9a-zA-Z]*}}F
func downcast_archetype<T : B>(b: B) -> T {
  // CHECK: {{%.*}} = unconditional_checked_cast
  return b as! T
}

// This is making sure that we do not have the default propagating behavior in
// the address case.
//
// CHECK-LABEL: sil hidden [ossa] @$s5casts12is_archetype{{[_0-9a-zA-Z]*}}F
func is_archetype<T : B>(b: B, _: T) -> Bool {
  // CHECK: bb0([[ARG1:%.*]] : @guaranteed $B, [[ARG2:%.*]] : @guaranteed $T):
  // CHECK:   checked_cast_br {{.*}}, [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]
  // CHECK: [[YES]]([[CASTED_ARG:%.*]] : @owned $T):
  // CHECK:   integer_literal {{.*}} -1
  // CHECK:   destroy_value [[CASTED_ARG]]
  // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : @owned $B):
  // CHECK:   destroy_value [[ORIGINAL_VALUE]]
  // CHECK:   integer_literal {{.*}} 0
  return b is T
}
// CHECK: } // end sil function '$s5casts12is_archetype{{[_0-9a-zA-Z]*}}F'

// CHECK: sil hidden [ossa] @$s5casts20downcast_conditional{{[_0-9a-zA-Z]*}}F
// CHECK:   checked_cast_br B in {{%.*}} : $B to D
// CHECK:   bb{{[0-9]+}}({{.*}} : $Optional<D>)
func downcast_conditional(b: B) -> D? {
  return b as? D
}

protocol P {}
struct S : P {}

// CHECK: sil hidden [ossa] @$s5casts32downcast_existential_conditional{{[_0-9a-zA-Z]*}}F
// CHECK: bb0([[IN:%.*]] : $*any P):
// CHECK:   [[COPY:%.*]] = alloc_stack $any P
// CHECK:   copy_addr [[IN]] to [init] [[COPY]]
// CHECK:   [[TMP:%.*]] = alloc_stack $S
// CHECK:   checked_cast_addr_br take_always any P in [[COPY]] : $*any P to S in [[TMP]] : $*S, bb1, bb2
//   Success block.
// CHECK: bb1:
// CHECK:   [[T0:%.*]] = load [trivial] [[TMP]] : $*S
// CHECK:   [[T1:%.*]] = enum $Optional<S>, #Optional.some!enumelt, [[T0]] : $S
// CHECK:   dealloc_stack [[TMP]]
// CHECK:   br bb3([[T1]] : $Optional<S>)
//   Failure block.
// CHECK: bb2:
// CHECK:   [[T0:%.*]] = enum $Optional<S>, #Optional.none!enumelt
// CHECK:   dealloc_stack [[TMP]]
// CHECK:   br bb3([[T0]] : $Optional<S>)
//   Continuation block.
// CHECK: bb3([[RESULT:%.*]] : $Optional<S>):
// CHECK:   dealloc_stack [[COPY]]
// CHECK:   return [[RESULT]]
func downcast_existential_conditional(p: P) -> S? {
  return p as? S
}

