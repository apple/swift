
// RUN: %target-swift-emit-silgen -swift-version 5 -module-name generic_casts -Xllvm -sil-full-demangle %s | %FileCheck %s

protocol ClassBound : class {}
protocol NotClassBound {}

class C : ClassBound, NotClassBound {}
struct S : NotClassBound {}
struct Unloadable : NotClassBound { var x : NotClassBound }

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts020opaque_archetype_to_c1_D0{{[_0-9a-zA-Z]*}}F
func opaque_archetype_to_opaque_archetype
<T:NotClassBound, U>(_ t:T) -> U {
  return t as! U
  // CHECK: bb0([[RET:%.*]] : $*U, {{%.*}}: $*T):
  // CHECK:   unconditional_checked_cast_addr T in {{%.*}} : $*T to U in [[RET]] : $*U
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts020opaque_archetype_is_c1_D0{{[_0-9a-zA-Z]*}}F
func opaque_archetype_is_opaque_archetype
<T:NotClassBound, U>(_ t:T, u:U.Type) -> Bool {
  return t is U
  // CHECK:   checked_cast_addr_br take_always T in [[VAL:%.*]] : $*T to U in [[DEST:%.*]] : $*U, [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]
  // CHECK: [[YES]]:
  // CHECK:   [[Y:%.*]] = integer_literal $Builtin.Int1, -1
  // CHECK:   destroy_addr [[DEST]]
  // CHECK:   br [[CONT:bb[0-9]+]]([[Y]] : $Builtin.Int1)
  // CHECK: [[NO]]:
  // CHECK:   [[N:%.*]] = integer_literal $Builtin.Int1, 0
  // CHECK:   br [[CONT]]([[N]] : $Builtin.Int1)
  // CHECK: [[CONT]]([[I1:%.*]] : $Builtin.Int1):
  // CHECK-NEXT:  [[META:%.*]] = metatype $@thin Bool.Type
  // CHECK-NEXT:  function_ref Swift.Bool.init(_builtinBooleanLiteral: Builtin.Int1)
  // CHECK-NEXT:  [[BOOL:%.*]] = function_ref @$sSb22_builtinBooleanLiteralSbBi1__tcfC :
  // CHECK-NEXT:  [[RES:%.*]] = apply [[BOOL]]([[I1]], [[META]])
  // -- we don't consume the checked value
  // CHECK:   return [[RES]] : $Bool
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts026opaque_archetype_to_class_D0{{[_0-9a-zA-Z]*}}F
func opaque_archetype_to_class_archetype
<T:NotClassBound, U:ClassBound> (_ t:T) -> U {
  return t as! U
  // CHECK: unconditional_checked_cast_addr T in {{%.*}} : $*T to U in [[DOWNCAST_ADDR:%.*]] : $*U
  // CHECK: [[DOWNCAST:%.*]] = load [take] [[DOWNCAST_ADDR]] : $*U
  // CHECK: return [[DOWNCAST]] : $U
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts026opaque_archetype_is_class_D0{{[_0-9a-zA-Z]*}}F
func opaque_archetype_is_class_archetype
<T:NotClassBound, U:ClassBound> (_ t:T, u:U.Type) -> Bool {
  return t is U
  // CHECK: copy_addr {{.*}} : $*T
  // CHECK: checked_cast_addr_br take_always T in [[VAL:%.*]] : {{.*}} to U
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts019class_archetype_to_c1_D0{{[_0-9a-zA-Z]*}}F
func class_archetype_to_class_archetype
<T:ClassBound, U:ClassBound>(_ t:T) -> U {
  return t as! U
  // Error bridging can change the identity of class-constrained archetypes.
  // CHECK: unconditional_checked_cast_addr T in {{%.*}} : $*T to U in [[DOWNCAST_ADDR:%.*]] : $*U
  // CHECK: [[DOWNCAST:%.*]] = load [take] [[DOWNCAST_ADDR]]
  // CHECK: return [[DOWNCAST]] : $U
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts019class_archetype_is_c1_D0{{[_0-9a-zA-Z]*}}F
func class_archetype_is_class_archetype
<T:ClassBound, U:ClassBound>(_ t:T, u:U.Type) -> Bool {
  return t is U
  // Error bridging can change the identity of class-constrained archetypes.
  // CHECK: checked_cast_addr_br {{.*}} T in {{%.*}} : $*T to U in {{%.*}} : $*U
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts38opaque_archetype_to_addr_only_concrete{{[_0-9a-zA-Z]*}}F
func opaque_archetype_to_addr_only_concrete
<T:NotClassBound> (_ t:T) -> Unloadable {
  return t as! Unloadable
  // CHECK: bb0([[RET:%.*]] : $*Unloadable, {{%.*}}: $*T):
  // CHECK:   unconditional_checked_cast_addr T in {{%.*}} : $*T to Unloadable in [[RET]] : $*Unloadable
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts38opaque_archetype_is_addr_only_concrete{{[_0-9a-zA-Z]*}}F
func opaque_archetype_is_addr_only_concrete
<T:NotClassBound> (_ t:T) -> Bool {
  return t is Unloadable
  // CHECK: checked_cast_addr_br take_always T in [[VAL:%.*]] : {{.*}} to Unloadable in
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts37opaque_archetype_to_loadable_concrete{{[_0-9a-zA-Z]*}}F
func opaque_archetype_to_loadable_concrete
<T:NotClassBound>(_ t:T) -> S {
  return t as! S
  // CHECK: unconditional_checked_cast_addr T in {{%.*}} : $*T to S in [[DOWNCAST_ADDR:%.*]] : $*S
  // CHECK: [[DOWNCAST:%.*]] = load [trivial] [[DOWNCAST_ADDR]] : $*S
  // CHECK: return [[DOWNCAST]] : $S
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts37opaque_archetype_is_loadable_concrete{{[_0-9a-zA-Z]*}}F
func opaque_archetype_is_loadable_concrete
<T:NotClassBound>(_ t:T) -> Bool {
  return t is S
  // CHECK: checked_cast_addr_br take_always T in {{%.*}} : $*T to S in
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts019class_archetype_to_C0{{[_0-9a-zA-Z]*}}F
func class_archetype_to_class
<T:ClassBound>(_ t:T) -> C {
  return t as! C
  // CHECK: [[DOWNCAST:%.*]] = unconditional_checked_cast {{%.*}} to C
  // CHECK: return [[DOWNCAST]] : $C
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts019class_archetype_is_C0{{[_0-9a-zA-Z]*}}F
func class_archetype_is_class
<T:ClassBound>(_ t:T) -> Bool {
  return t is C
  // CHECK: checked_cast_br {{.*}} to C
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts022opaque_existential_to_C10_archetype{{[_0-9a-zA-Z]*}}F
func opaque_existential_to_opaque_archetype
<T:NotClassBound>(_ p:NotClassBound) -> T {
  return p as! T
  // CHECK: bb0([[RET:%.*]] : $*T, [[ARG:%.*]] : $*any NotClassBound):
  // CHECK:      [[TEMP:%.*]] = alloc_stack $any NotClassBound
  // CHECK-NEXT: copy_addr [[ARG]] to [init] [[TEMP]]
  // CHECK-NEXT: unconditional_checked_cast_addr any NotClassBound in [[TEMP]] : $*any NotClassBound to T in [[RET]] : $*T
  // CHECK-NEXT: dealloc_stack [[TEMP]]
  // CHECK-NEXT: [[T0:%.*]] = tuple ()
  // CHECK-NEXT: return [[T0]]
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts022opaque_existential_is_C10_archetype{{[_0-9a-zA-Z]*}}F
func opaque_existential_is_opaque_archetype
<T:NotClassBound>(_ p:NotClassBound, _: T) -> Bool {
  return p is T
  // CHECK:   checked_cast_addr_br take_always any NotClassBound in [[CONTAINER:%.*]] : {{.*}} to T in
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts37opaque_existential_to_class_archetype{{[_0-9a-zA-Z]*}}F
func opaque_existential_to_class_archetype
<T:ClassBound>(_ p:NotClassBound) -> T {
  return p as! T
  // CHECK: unconditional_checked_cast_addr any NotClassBound in {{%.*}} : $*any NotClassBound to T in [[DOWNCAST_ADDR:%.*]] : $*T
  // CHECK: [[DOWNCAST:%.*]] = load [take] [[DOWNCAST_ADDR]] : $*T
  // CHECK: return [[DOWNCAST]] : $T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts37opaque_existential_is_class_archetype{{[_0-9a-zA-Z]*}}F
func opaque_existential_is_class_archetype
<T:ClassBound>(_ p:NotClassBound, _: T) -> Bool {
  return p is T
  // CHECK: checked_cast_addr_br take_always any NotClassBound in {{%.*}} : $*any NotClassBound to T in
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts021class_existential_to_C10_archetype{{[_0-9a-zA-Z]*}}F
func class_existential_to_class_archetype
<T:ClassBound>(_ p:ClassBound) -> T {
  return p as! T
  // CHECK: unconditional_checked_cast_addr any ClassBound in {{%.*}} : $*any ClassBound to T in [[DOWNCAST_ADDR:%.*]] : $*T
  // CHECK: [[DOWNCAST:%.*]] = load [take] [[DOWNCAST_ADDR]]
  // CHECK: return [[DOWNCAST]] : $T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts021class_existential_is_C10_archetype{{[_0-9a-zA-Z]*}}F
func class_existential_is_class_archetype
<T:ClassBound>(_ p:ClassBound, _: T) -> Bool {
  return p is T
  // CHECK: checked_cast_addr_br {{.*}} any ClassBound in {{%.*}} : $*any ClassBound to T in {{%.*}} : $*T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts40opaque_existential_to_addr_only_concrete{{[_0-9a-zA-Z]*}}F
func opaque_existential_to_addr_only_concrete(_ p: NotClassBound) -> Unloadable {
  return p as! Unloadable
  // CHECK: bb0([[RET:%.*]] : $*Unloadable, {{%.*}}: $*any NotClassBound):
  // CHECK:   unconditional_checked_cast_addr any NotClassBound in {{%.*}} : $*any NotClassBound to Unloadable in [[RET]] : $*Unloadable
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts40opaque_existential_is_addr_only_concrete{{[_0-9a-zA-Z]*}}F
func opaque_existential_is_addr_only_concrete(_ p: NotClassBound) -> Bool {
  return p is Unloadable
  // CHECK:   checked_cast_addr_br take_always any NotClassBound in {{%.*}} : $*any NotClassBound to Unloadable in
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts39opaque_existential_to_loadable_concrete{{[_0-9a-zA-Z]*}}F
func opaque_existential_to_loadable_concrete(_ p: NotClassBound) -> S {
  return p as! S
  // CHECK:   unconditional_checked_cast_addr any NotClassBound in {{%.*}} : $*any NotClassBound to S in [[DOWNCAST_ADDR:%.*]] : $*S
  // CHECK: [[DOWNCAST:%.*]] = load [trivial] [[DOWNCAST_ADDR]] : $*S
  // CHECK: return [[DOWNCAST]] : $S
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts39opaque_existential_is_loadable_concrete{{[_0-9a-zA-Z]*}}F
func opaque_existential_is_loadable_concrete(_ p: NotClassBound) -> Bool {
  return p is S
  // CHECK: checked_cast_addr_br take_always any NotClassBound in {{%.*}} : $*any NotClassBound to S in
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts021class_existential_to_C0{{[_0-9a-zA-Z]*}}F
func class_existential_to_class(_ p: ClassBound) -> C {
  return p as! C
  // CHECK: [[DOWNCAST:%.*]] = unconditional_checked_cast {{%.*}} to C
  // CHECK: return [[DOWNCAST]] : $C
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts021class_existential_is_C0{{[_0-9a-zA-Z]*}}F
func class_existential_is_class(_ p: ClassBound) -> Bool {
  return p is C
  // CHECK: checked_cast_br {{.*}} to C
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts27optional_anyobject_to_classyAA1CCSgyXlSgF
func optional_anyobject_to_class(_ p: AnyObject?) -> C? {
  return p as? C
  // CHECK: checked_cast_br AnyObject in {{%.*}} : $AnyObject to C
}

// The below tests are to ensure we don't dig into an optional operand when
// casting to a non-class archetype, as it could dynamically be an optional type.

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts32optional_any_to_opaque_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_to_opaque_archetype<T>(_ x: Any?) -> T {
  return x as! T
  // CHECK: bb0([[RET:%.*]] : $*T, {{%.*}} : $*Optional<Any>):
  // CHECK: unconditional_checked_cast_addr Optional<Any> in {{%.*}} : $*Optional<Any> to T in [[RET]] : $*T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts46optional_any_conditionally_to_opaque_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_conditionally_to_opaque_archetype<T>(_ x: Any?) -> T? {
  return x as? T
  // CHECK: checked_cast_addr_br take_always Optional<Any> in {{%.*}} : $*Optional<Any> to T in {{%.*}} : $*T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts32optional_any_is_opaque_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_is_opaque_archetype<T>(_ x: Any?, _: T) -> Bool {
  return x is T
  // CHECK: checked_cast_addr_br take_always Optional<Any> in {{%.*}} : $*Optional<Any> to T in {{%.*}} : $*T
}

// But we can dig into at most one layer of the operand if it's
// an optional archetype...

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts016optional_any_to_C17_opaque_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_to_optional_opaque_archetype<T>(_ x: Any?) -> T? {
  return x as! T?
  // CHECK: unconditional_checked_cast_addr Any in {{%.*}} : $*Any to T in {{%.*}} : $*T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts030optional_any_conditionally_to_C17_opaque_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_conditionally_to_optional_opaque_archetype<T>(_ x: Any?) -> T?? {
  return x as? T?
  // CHECK: checked_cast_addr_br take_always Any in {{%.*}} : $*Any to T in {{%.*}} : $*T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts016optional_any_is_C17_opaque_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_is_optional_opaque_archetype<T>(_ x: Any?, _: T) -> Bool {
  return x is T?
  //   Because the levels of optional are the same, 'is' doesn't transform into an 'as?',
  //   so we just cast directly without digging into the optional operand.
  // CHECK: checked_cast_addr_br take_always Optional<Any> in {{%.*}} : $*Optional<Any> to Optional<T> in {{%.*}} : $*Optional<T>
}

// And we can dig into the operand when casting to a class archetype, as it
// cannot dynamically be optional...

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts31optional_any_to_class_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_to_class_archetype<T : AnyObject>(_ x: Any?) -> T {
  return x as! T
  // CHECK: unconditional_checked_cast_addr Any in {{%.*}} : $*Any to T in {{%.*}} : $*T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts45optional_any_conditionally_to_class_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_conditionally_to_class_archetype<T : AnyObject>(_ x: Any?) -> T? {
  return x as? T
  // CHECK: checked_cast_addr_br take_always Any in {{%.*}} : $*Any to T in {{%.*}} : $*T
}

// CHECK-LABEL: sil hidden [ossa] @$s13generic_casts31optional_any_is_class_archetype{{[_0-9a-zA-Z]*}}F
func optional_any_is_class_archetype<T : AnyObject>(_ x: Any?, _: T) -> Bool {
  return x is T
  // CHECK: checked_cast_addr_br take_always Any in {{%.*}} : $*Any to T in {{%.*}} : $*T
}

