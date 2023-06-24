
// RUN: %target-swift-frontend -module-name specialize_unconditional_checked_cast -Xllvm -sil-disable-pass=FunctionSignatureOpts -emit-sil -o - -O %s | %FileCheck %s

//////////////////
// Declarations //
//////////////////

public class C {}
public class D : C {}
public class E {}

public struct NotUInt8 { var value: UInt8 }
public struct NotUInt64 { var value: UInt64 }

var b = NotUInt8(value: 0)
var c = C()
var d = D()
var e = E()
var f = NotUInt64(value: 0)
var o : AnyObject = c

////////////////////////////
// Archetype To Archetype //
////////////////////////////

@inline(never)
public func ArchetypeToArchetype<T1, T2>(t t: T1, t2: T2) -> T2 {
  return t as! T2
}

ArchetypeToArchetype(t: b, t2: b)
ArchetypeToArchetype(t: c, t2: c)
ArchetypeToArchetype(t: b, t2: c)
ArchetypeToArchetype(t: c, t2: b)
ArchetypeToArchetype(t: c, t2: d)
ArchetypeToArchetype(t: d, t2: c)
ArchetypeToArchetype(t: c, t2: e)
ArchetypeToArchetype(t: b, t2: f)

// x -> x where x is not a class.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA8NotUInt8V{{.*}}Tg5 : $@convention(thin) (NotUInt8, NotUInt8) -> NotUInt8 {
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype

// x -> x where x is a class.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA1CC{{.*}}Tg5 : $@convention(thin) (@guaranteed C, @guaranteed C) -> @owned C {
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype

// x -> y where x is not a class but y is.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA8NotUInt8V_AA1CCTg5 : $@convention(thin) (NotUInt8, @guaranteed C) -> @owned C {
// CHECK-NOT: unconditional_checked_cast_addr
// CHECK-NOT: unconditional_checked_cast_addr
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NOT: unconditional_checked_cast_addr

// y -> x where x is not a class but y is.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA1CC_AA8NotUInt8VTg5 : $@convention(thin) (@guaranteed C, NotUInt8) -> NotUInt8 {
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype

// x -> y where x is a super class of y.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA1CC_AA1DCTg5 : $@convention(thin) (@guaranteed C, @guaranteed D) -> @owned D {
// CHECK: [[STACK:%[0-9]+]] = alloc_stack $C
// TODO: This should be optimized to an unconditional_checked_cast without the need of alloc_stack: rdar://problem/24775038
// CHECK: unconditional_checked_cast_addr C in [[STACK]] : $*C to D in

// y -> x where x is a super class of y.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA1DC_AA1CCTg5 : $@convention(thin) (@guaranteed D, @guaranteed C) -> @owned C {
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype
// CHECK: upcast {{%[0-9]+}} : $D to $C
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype

// x -> y where x and y are unrelated classes.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA1CC_AA1ECTg5 : $@convention(thin) (@guaranteed C, @guaranteed E) -> @owned E {
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype

// x -> y where x and y are unrelated non classes.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast011ArchetypeToE0{{[_0-9a-zA-Z]*}}FAA8NotUInt8V_AA0H6UInt64VTg5 : $@convention(thin) (NotUInt8, NotUInt64) -> NotUInt64 {
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NOT: unconditional_checked_cast archetype_to_archetype


///////////////////////////
// Archetype To Concrete //
///////////////////////////

@inline(never)
public func ArchetypeToConcreteConvertUInt8<T>(t t: T) -> NotUInt8 {
  return t as! NotUInt8
}
ArchetypeToConcreteConvertUInt8(t: b)
ArchetypeToConcreteConvertUInt8(t: c)
ArchetypeToConcreteConvertUInt8(t: f)

// x -> x where x is not a class.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast31ArchetypeToConcreteConvertUInt8{{[_0-9a-zA-Z]*}}3Not{{.*}}Tg5 : $@convention(thin) (NotUInt8) -> NotUInt8 {
// CHECK: bb0
// CHECK-NEXT: debug_value %0
// CHECK-NEXT: return %0

// x -> y where y is a class but x is not.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast31ArchetypeToConcreteConvertUInt8{{[_0-9a-zA-Z]*}}FAA1CC_Tg5
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

// x -> y where x,y are not classes and x is a different type from y.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast31ArchetypeToConcreteConvertUInt8{{[_0-9a-zA-Z]*}}3Not{{.*}}Tg5
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

// x -> x where x is a class.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ArchetypeToConcreteConvertC{{[_0-9a-zA-Z]*}}Tg5 : $@convention(thin) (@guaranteed C) -> @owned C {
// CHECK: bb0([[ARG:%.*]] : $C)
// CHECK-NEXT: debug_value [[ARG]]
// CHECK-NEXT: strong_retain [[ARG]]
// CHECK-NEXT: return [[ARG]]

// x -> y where x is a class but y is not.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast27ArchetypeToConcreteConvertC{{[_0-9a-zA-Z]*}}FAA8NotUInt8V_Tg5
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

// x -> y where x,y are classes and x is a super class of y.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ArchetypeToConcreteConvertC{{[_0-9a-zA-Z]*}}FAA1DC_Tg5 : $@convention(thin) (@guaranteed D) -> @owned C {
// CHECK: bb0([[ARG:%.*]] : $D):
// CHECK:   [[UPCAST:%.*]] = upcast [[ARG]] : $D to $C
// CHECK:   strong_retain [[ARG]]
// CHECK:   return [[UPCAST]]
// CHECK: } // end sil function '$s37specialize_unconditional_checked_cast27ArchetypeToConcreteConvertC{{[_0-9a-zA-Z]*}}FAA1DC_Tg5'

// x -> y where x,y are classes, but x is unrelated to y.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ArchetypeToConcreteConvertC{{[_0-9a-zA-Z]*}}FAA1EC_Tg5
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

@inline(never)
public func ArchetypeToConcreteConvertC<T>(t t: T) -> C {
  return t as! C
}

ArchetypeToConcreteConvertC(t: c)
ArchetypeToConcreteConvertC(t: b)
ArchetypeToConcreteConvertC(t: d)
ArchetypeToConcreteConvertC(t: e)

@inline(never)
public func ArchetypeToConcreteConvertD<T>(t t: T) -> D {
  return t as! D
}

ArchetypeToConcreteConvertD(t: c)

// x -> y where x,y are classes and x is a sub class of y.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast27ArchetypeToConcreteConvertD{{[_0-9a-zA-Z]*}}FAA1CC_Tg5 : $@convention(thin) (@guaranteed C) -> @owned D {
// CHECK: bb0(%0 : $C):
// CHECK-DAG: [[STACK_C:%[0-9]+]] = alloc_stack $C
// CHECK-DAG: store {{.*}} to [[STACK_C]]
// CHECK-DAG: [[STACK_D:%[0-9]+]] = alloc_stack $D
// TODO: This should be optimized to an unconditional_checked_cast without the need of alloc_stack: rdar://problem/24775038
// CHECK-DAG: unconditional_checked_cast_addr C in [[STACK_C]] : $*C to D in [[STACK_D]] : $*D
// CHECK-DAG: [[LOAD:%[0-9]+]] = load [[STACK_D]]
// CHECK: return [[LOAD]]

@inline(never)
public func ArchetypeToConcreteConvertE<T>(t t: T) -> E {
  return t as! E
}

ArchetypeToConcreteConvertE(t: c)

// x -> y where x,y are classes, but y is unrelated to x. The idea is
// to make sure that the fact that y is concrete does not affect the
// result.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ArchetypeToConcreteConvertE{{[_0-9a-zA-Z]*}}FAA1CC_Tg5
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }


///////////////////////////
// Concrete to Archetype //
///////////////////////////

@inline(never)
public func ConcreteToArchetypeConvertUInt8<T>(t t: NotUInt8, t2: T) -> T {
  return t as! T
}

ConcreteToArchetypeConvertUInt8(t: b, t2: b)
ConcreteToArchetypeConvertUInt8(t: b, t2: c)
ConcreteToArchetypeConvertUInt8(t: b, t2: f)

// x -> x where x is not a class.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast31ConcreteToArchetypeConvertUInt8{{[_0-9a-zA-Z]*}}3Not{{.*}}Tg5 : $@convention(thin) (NotUInt8, NotUInt8) -> NotUInt8 {
// CHECK: bb0(%0 : $NotUInt8, %1 : $NotUInt8):
// CHECK: debug_value %0
// CHECK: return %0

// x -> y where x is not a class but y is a class.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast31ConcreteToArchetypeConvertUInt8{{[_0-9a-zA-Z]*}}FAA1CC_Tg5 : $@convention(thin) (NotUInt8, @guaranteed C) -> @owned C {
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

// x -> y where x,y are different non class types.
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast31ConcreteToArchetypeConvertUInt8{{[_0-9a-zA-Z]*}}Not{{.*}}Tg5 : $@convention(thin) (NotUInt8, NotUInt64) -> NotUInt64 {
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

@inline(never)
public func ConcreteToArchetypeConvertC<T>(t t: C, t2: T) -> T {
  return t as! T
}

ConcreteToArchetypeConvertC(t: c, t2: c)
ConcreteToArchetypeConvertC(t: c, t2: b)
ConcreteToArchetypeConvertC(t: c, t2: d)
ConcreteToArchetypeConvertC(t: c, t2: e)


// x -> x where x is a class.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ConcreteToArchetypeConvertC{{[_0-9a-zA-Z]*}}Tg5 : $@convention(thin) (@guaranteed C, @guaranteed C) -> @owned C {
// CHECK: bb0(%0 : $C, %1 : $C):
// CHECK: return %0

// x -> y where x is a class but y is not.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ConcreteToArchetypeConvertC{{[_0-9a-zA-Z]*}}Not{{.*}}Tg5 : $@convention(thin) (@guaranteed C, NotUInt8) -> NotUInt8 {
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

// x -> y where x is a super class of y.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ConcreteToArchetypeConvertC{{[_0-9a-zA-Z]*}}FAA1DC_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed D) -> @owned D {
// CHECK: bb0(%0 : $C, %1 : $D):
// CHECK-DAG: [[STACK_C:%[0-9]+]] = alloc_stack $C
// CHECK-DAG: store %0 to [[STACK_C]]
// CHECK-DAG: [[STACK_D:%[0-9]+]] = alloc_stack $D
// TODO: This should be optimized to an unconditional_checked_cast without the need of alloc_stack: rdar://problem/24775038
// CHECK-DAG: unconditional_checked_cast_addr C in [[STACK_C]] : $*C to D in [[STACK_D]] : $*D
// CHECK-DAG: [[LOAD:%[0-9]+]] = load [[STACK_D]]
// CHECK: return [[LOAD]]

// x -> y where x and y are unrelated classes.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ConcreteToArchetypeConvertC{{[_0-9a-zA-Z]*}}FAA1EC_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed E) -> @owned E {
// CHECK: bb0(%0 : $C, %1 : $E):
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NEXT: unreachable
// CHECK-NEXT: }

@inline(never)
public func ConcreteToArchetypeConvertD<T>(t t: D, t2: T) -> T {
  return t as! T
}

ConcreteToArchetypeConvertD(t: d, t2: c)

// x -> y where x is a subclass of y.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast27ConcreteToArchetypeConvertD{{[_0-9a-zA-Z]*}}FAA1CC_Tg5 : $@convention(thin) (@guaranteed D, @guaranteed C) -> @owned C {
// CHECK: bb0(%0 : $D, %1 : $C):
// CHECK-DAG: [[UC:%[0-9]+]] = upcast %0
// CHECK: return [[UC]]


////////////////////////
// Super To Archetype //
////////////////////////

@inline(never)
public func SuperToArchetypeC<T>(c c : C, t : T) -> T {
  return c as! T
}

SuperToArchetypeC(c: c, t: c)
SuperToArchetypeC(c: c, t: d)
SuperToArchetypeC(c: c, t: b)


// x -> x where x is a class.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast17SuperToArchetypeC{{[_0-9a-zA-Z]*}}Tg5 : $@convention(thin) (@guaranteed C, @guaranteed C) -> @owned C {
// CHECK: bb0(%0 : $C, %1 : $C):
// CHECK: return %0

// x -> y where x is a super class of y.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast17SuperToArchetypeC{{[_0-9a-zA-Z]*}}FAA1DC_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed D) -> @owned D {
// CHECK: bb0
// CHECK: unconditional_checked_cast_addr C in

// x -> y where x is a class and y is not.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast17SuperToArchetypeC{{[_0-9a-zA-Z]*}}FAA8NotUInt8V_Tg5 : $@convention(thin) (@guaranteed C, NotUInt8) -> NotUInt8 {
// CHECK: bb0
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK: unreachable
// CHECK-NEXT: }

@inline(never)
public func SuperToArchetypeD<T>(d d : D, t : T) -> T {
  return d as! T
}

SuperToArchetypeD(d: d, t: c)
SuperToArchetypeD(d: d, t: d)

// *NOTE* The frontend is smart enough to turn this into an upcast. When this
// test is converted to SIL, this should be fixed appropriately.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast17SuperToArchetypeD{{[_0-9a-zA-Z]*}}FAA1CC_Tg5 : $@convention(thin) (@guaranteed D, @guaranteed C) -> @owned C {
// CHECK-NOT: unconditional_checked_cast super_to_archetype
// CHECK: upcast
// CHECK-NOT: unconditional_checked_cast super_to_archetype

// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast17SuperToArchetypeD{{[_0-9a-zA-Z]*}}Tg5 : $@convention(thin) (@guaranteed D, @guaranteed D) -> @owned D {
// CHECK: bb0(%0 : $D, %1 : $D):
// CHECK: return %0

//////////////////////////////
// Existential To Archetype //
//////////////////////////////

@inline(never)
public func ExistentialToArchetype<T>(o o : AnyObject, t : T) -> T {
  return o as! T
}

// AnyObject -> Class.
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast22ExistentialToArchetype{{[_0-9a-zA-Z]*}}FAA1CC_Tg5 : $@convention(thin) (@guaranteed AnyObject, @guaranteed C) -> @owned C {
// CHECK: unconditional_checked_cast_addr AnyObject in {{%.*}} : $*AnyObject to C

// AnyObject -> Non Class (should always fail)
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast22ExistentialToArchetype{{[_0-9a-zA-Z]*}}FAA8NotUInt8V_Tg5 : $@convention(thin) (@guaranteed AnyObject, NotUInt8) -> NotUInt8 {
// CHECK-NOT: cond_fail
// CHECK-NOT: unreachable
// CHECK: return

// AnyObject -> AnyObject
// CHECK-LABEL: sil shared [noinline] {{.*}}@$s37specialize_unconditional_checked_cast22ExistentialToArchetype{{[_0-9a-zA-Z]*}}yXl{{.*}}Tg5 : $@convention(thin) (@guaranteed AnyObject, @guaranteed AnyObject) -> @owned AnyObject {
// CHECK: bb0(%0 : $AnyObject, %1 : $AnyObject):
// CHECK: return %0

ExistentialToArchetype(o: o, t: c)
ExistentialToArchetype(o: o, t: b)
ExistentialToArchetype(o: o, t: o)

// Ensure that a downcast from an Optional source is not promoted to a
// value cast. We could do the promotion, but the optimizer would need
// to insert the Optional unwrapping logic before the cast.
//
// CHECK-LABEL: sil shared [noinline] @$s37specialize_unconditional_checked_cast15genericDownCastyq_x_q_mtr0_lFAA1CCSg_AA1DCTgm5 : $@convention(thin) (@guaranteed Optional<C>) -> @owned D {
// CHECK: bb0(%0 : $Optional<C>):
// CHECK-DAG: [[STACK_D:%[0-9]+]] = alloc_stack $D
// CHECK-DAG: [[STACK_C:%[0-9]+]] = alloc_stack $Optional<C>
// CHECK-DAG: store [[ARG]] to [[STACK_C]]
// CHECK-DAG: retain_value [[ARG]]
// TODO: This should be optimized to an unconditional_checked_cast without the need of alloc_stack: rdar://problem/24775038
// CHECK-DAG: unconditional_checked_cast_addr Optional<C> in [[STACK_C]] : $*Optional<C> to D in [[STACK_D]] : $*D
// CHECK-DAG: [[LOAD:%[0-9]+]] = load [[STACK_D]]
// CHECK: return [[LOAD]]
@inline(never)
public func genericDownCast<T, U>(_ a: T, _ : U.Type) -> U {
  return a as! U
}

public func callGenericDownCast(_ c: C?) -> D {
  return genericDownCast(c, D.self)
}

