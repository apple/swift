// RUN: %target-swift-frontend -module-name specialize_checked_cast_branch -emit-sil -O -sil-inline-threshold 0 -Xllvm -sil-disable-pass=function-signature-opts %s -o - | %FileCheck %s

class C {}
class D : C {}
class E {}

struct NotUInt8 { var value: UInt8 }
struct NotUInt64 { var value: UInt64 }

var b = NotUInt8(value: 0)
var c = C()
var d = D()
var e = E()
var f = NotUInt64(value: 0)
var o : AnyObject = c

////////////////////////
// Arch to Arch Casts //
////////////////////////

public func ArchetypeToArchetypeCast<T1, T2>(t1 : T1, t2 : T2) -> T2 {
  if let x = t1 as? T2 {
    return x
  }
  preconditionFailure("??? Profit?")
}

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AA1DCTg5 : $@convention(thin) (@guaranteed C, @guaranteed D) -> @owned D
// CHECK: bb0([[ARG:%.*]] : $C, [[ARG2:%.*]] : $D):
// CHECK:  checked_cast_br C in [[ARG]] : $C to D, bb1, bb2
//
// CHECK: bb1([[T0:%.*]] : $D):
// CHECK:   strong_retain [[ARG]]
// CHECK:   return [[T0]]
//
// CHECK: bb2
// CHECK:   cond_fail {{%.*}}, "precondition failure"
// CHECK:   unreachable
// CHECK: } // end sil function '$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AA1DCTg5'
_ = ArchetypeToArchetypeCast(t1: c, t2: d)

// x -> x where x is a class.
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AFTg5 : $@convention(thin) (@guaranteed C, @guaranteed C) -> @owned C {
// CHECK: bb0
// CHECK-NOT: bb1
// CHECK: strong_retain %0 : $C
// CHECK: return %0 : $C
// CHECK: } // end sil function '$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AFTg5'
_ = ArchetypeToArchetypeCast(t1: c, t2: c)

// TODO: x -> x where x is not a class.
_ = ArchetypeToArchetypeCast(t1: b, t2: b)

// TODO: x -> y where x is not a class and y is not a class.
_ = ArchetypeToArchetypeCast(t1: b, t2: f)

// x -> y where x is not a class but y is.
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA8NotUInt8V_AA1CCTg5 : $@convention(thin) (NotUInt8, @guaranteed C) -> @owned C {
// CHECK: bb0
// CHECK-NOT: bb1
// CHECK: cond_fail {{%.*}}, "precondition failure"
// CHECK: unreachable
// CHECK: } // end sil function '$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA8NotUInt8V_AA1CCTg5'
_ = ArchetypeToArchetypeCast(t1: b, t2: c)

// y -> x where x is a class but y is not.
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AA8NotUInt8VTg5 : $@convention(thin) (@guaranteed C, NotUInt8) -> NotUInt8 {
// CHECK: bb0
// CHECK-NOT: bb1
// CHECK: cond_fail {{%.*}}, "precondition failure"
// CHECK: unreachable
// CHECK: } // end sil function '$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AA8NotUInt8VTg5'
_ = ArchetypeToArchetypeCast(t1: c, t2: b)

// y -> x where x is a super class of y.
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1DC_AA1CCTg5 : $@convention(thin) (@guaranteed D, @guaranteed C) -> @owned C {
// CHECK: [[T1:%.*]] = upcast %0 : $D to $C
// CHECK: strong_retain %0 : $D
// CHECK: return [[T1]] : $C
// CHECK: } // end sil function '$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1DC_AA1CCTg5'
_ = ArchetypeToArchetypeCast(t1: d, t2: c)

// x -> y where x and y are unrelated.
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AA1ECTg5 : $@convention(thin) (@guaranteed C, @guaranteed E) -> @owned E {
// CHECK: bb0
// CHECK-NOT: bb1
// CHECK: cond_fail {{%.*}}, "precondition failure"
// CHECK: unreachable
// CHECK: } // end sil function '$s30specialize_checked_cast_branch011ArchetypeToE4Cast2t12t2q_x_q_tr0_lFAA1CC_AA1ECTg5'
_ = ArchetypeToArchetypeCast(t1: c, t2: e)

///////////////////////////
// Archetype To Concrete //
///////////////////////////

func ArchetypeToConcreteCastUInt8<T>(t : T) -> NotUInt8 {
  if let x = t as? NotUInt8 {
    return x
  }
  preconditionFailure("??? Profit?")
}

func ArchetypeToConcreteCastC<T>(t : T) -> C {
  if let x = t as? C {
    return x
  }
  preconditionFailure("??? Profit?")
}

func ArchetypeToConcreteCastD<T>(t : T) -> D {
  if let x = t as? D {
    return x
  }
  preconditionFailure("??? Profit?")
}

func ArchetypeToConcreteCastE<T>(t : T) -> E {
  if let x = t as? E {
    return x
  }
  preconditionFailure("??? Profit?")
}

// uint8 -> uint8
// CHECK-LABEL: sil shared @$s30specialize_checked_cast_branch28ArchetypeToConcreteCastUInt81tAA03NotI0Vx_tlFAE_Tg5 : $@convention(thin) (NotUInt8) -> NotUInt8 {
// CHECK: bb0([[ARG:%.*]] :
// CHECK:   return [[ARG]] : $NotUInt8
// CHECK: } // end sil function '$s30specialize_checked_cast_branch28ArchetypeToConcreteCastUInt81tAA03NotI0Vx_tlFAE_Tg5'
_ = ArchetypeToConcreteCastUInt8(t: b)

// TODO: This needs FileCheck love.
_ = ArchetypeToConcreteCastUInt8(t: c)

// UInt64 -> Uint8
// CHECK-LABEL: sil shared @$s30specialize_checked_cast_branch28ArchetypeToConcreteCastUInt81tAA03NotI0Vx_tlFAA0J6UInt64V_Tg5 : $@convention(thin) (NotUInt64) -> NotUInt8 {
// CHECK: bb0
// CHECK-NOT: checked_cast_br
// CHECK: cond_fail {{%.*}}, "precondition failure"
// CHECK: unreachable
_ = ArchetypeToConcreteCastUInt8(t: f)

// NotUInt8 -> C
// CHECK-LABEL: sil shared @$s30specialize_checked_cast_branch24ArchetypeToConcreteCastC1tAA1CCx_tlFAA8NotUInt8V_Tg5 : $@convention(thin) (NotUInt8) -> @owned C {
// CHECK: bb0
// CHECK-NOT: checked_cast_br
// CHECK: unreachable
// CHECK: } // end sil function '$s30specialize_checked_cast_branch24ArchetypeToConcreteCastC1tAA1CCx_tlFAA8NotUInt8V_Tg5'
_ = ArchetypeToConcreteCastC(t: b)

// C -> C
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ArchetypeToConcreteCastC1tAA1CCx_tlFAE_Tg5 : $@convention(thin) (@guaranteed C) -> @owned C {
// CHECK: bb0([[ARG:%.*]] : $C)
// CHECK:   strong_retain [[ARG]]
// CHECK:   return [[ARG]] : $C
// CHECK: } // end sil function '$s30specialize_checked_cast_branch24ArchetypeToConcreteCastC1tAA1CCx_tlFAE_Tg5'
_ = ArchetypeToConcreteCastC(t: c)

// D -> C
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ArchetypeToConcreteCastC1tAA1CCx_tlFAA1DC_Tg5 : $@convention(thin) (@guaranteed D) -> @owned C {
// CHECK: bb0([[ARG:%.*]] : $D):
// CHECK:  [[CAST:%.*]] = upcast [[ARG]] : $D to $C
// CHECK:  strong_retain [[ARG]]
// CHECK:  return [[CAST]]
// CHECK: } // end sil function '$s30specialize_checked_cast_branch24ArchetypeToConcreteCastC1tAA1CCx_tlFAA1DC_Tg5'
_ = ArchetypeToConcreteCastC(t: d)

// E -> C
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ArchetypeToConcreteCastC1tAA1CCx_tlFAA1EC_Tg5 : $@convention(thin) (@guaranteed E) -> @owned C {
// CHECK: bb0
// CHECK: cond_fail {{%.*}}, "precondition failure"
// CHECK: unreachable
_ = ArchetypeToConcreteCastC(t: e)

// C -> D
// x -> y where x is a super class of y.
// CHECK-LABEL: sil shared @$s30specialize_checked_cast_branch24ArchetypeToConcreteCastD1tAA1DCx_tlFAA1CC_Tg5 : $@convention(thin) (@guaranteed C) -> @owned D {
// CHECK: bb0([[ARG:%.*]] : $C):
// CHECK:   checked_cast_br C in [[ARG]] : $C to D, [[SUCC_BB:bb[0-9]+]], [[FAIL_BB:bb[0-9]+]]
//
// CHECK: [[SUCC_BB]]([[T0:%.*]] : $D):
// CHECK:   strong_retain [[ARG]]
// CHECK:   return [[T0]] : $D
//
// CHECK: [[FAIL_BB]]:
// CHECK:   cond_fail {{%.*}}, "precondition failure"
// CHECK:   unreachable
// CHECK: } // end sil function '$s30specialize_checked_cast_branch24ArchetypeToConcreteCastD1tAA1DCx_tlFAA1CC_Tg5'
_ = ArchetypeToConcreteCastD(t: c)

// C -> E
// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ArchetypeToConcreteCastE1tAA1ECx_tlFAA1CC_Tg5 : $@convention(thin) (@guaranteed C) -> @owned E {
// CHECK: bb0
// CHECK: cond_fail {{%.*}}, "precondition failure"
// CHECK: unreachable
_ = ArchetypeToConcreteCastE(t: c)

///////////////////////////
// Concrete To Archetype //
///////////////////////////

func ConcreteToArchetypeCastUInt8<T>(t: NotUInt8, t2: T) -> T {
  if let x = t as? T {
    return x
  }
  preconditionFailure("??? Profit?")
}
func ConcreteToArchetypeCastC<T>(t: C, t2: T) -> T {
  if let x = t as? T {
    return x
  }
  preconditionFailure("??? Profit?")
}
func ConcreteToArchetypeCastD<T>(t: D, t2: T) -> T {
  if let x = t as? T {
    return x
  }
  preconditionFailure("??? Profit?")
}

// x -> x where x is not a class.
// CHECK-LABEL: sil shared @$s30specialize_checked_cast_branch28ConcreteToArchetypeCastUInt81t2t2xAA03NotI0V_xtlFAF_Tg5 : $@convention(thin) (NotUInt8, NotUInt8) -> NotUInt8 {
// CHECK: bb0
// CHECK-NOT: bb1
// CHECK: return %0 : $NotUInt8
// CHECK: } // end sil function '$s30specialize_checked_cast_branch28ConcreteToArchetypeCastUInt81t2t2xAA03NotI0V_xtlFAF_Tg5'
_ = ConcreteToArchetypeCastUInt8(t: b, t2: b)

// x -> y where x,y are not classes and x is a different type from y.
// CHECK-LABEL: sil shared @$s30specialize_checked_cast_branch28ConcreteToArchetypeCastUInt81t2t2xAA03NotI0V_xtlFAA0K6UInt64V_Tg5 : $@convention(thin) (NotUInt8, NotUInt64) -> NotUInt64 {
// CHECK: bb0
// CHECK-NOT: bb1
// CHECK: unreachable
// CHECK: } // end sil function '$s30specialize_checked_cast_branch28ConcreteToArchetypeCastUInt81t2t2xAA03NotI0V_xtlFAA0K6UInt64V_Tg5
_ = ConcreteToArchetypeCastUInt8(t: b, t2: f)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch28ConcreteToArchetypeCastUInt81t2t2xAA03NotI0V_xtlFAA1CC_Tg5 : $@convention(thin) (NotUInt8, @guaranteed C) -> @owned C
// CHECK: bb0
// CHECK: unreachable
_ = ConcreteToArchetypeCastUInt8(t: b, t2: c)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ConcreteToArchetypeCastC1t2t2xAA1CC_xtlFAF_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed C) -> @owned C
// CHECK: bb0
// CHECK: return %0
_ = ConcreteToArchetypeCastC(t: c, t2: c)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ConcreteToArchetypeCastC1t2t2xAA1CC_xtlFAA8NotUInt8V_Tg5 : $@convention(thin) (@guaranteed C, NotUInt8) -> NotUInt8
// CHECK: bb0
// CHECK: unreachable
_ = ConcreteToArchetypeCastC(t: c, t2: b)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ConcreteToArchetypeCastC1t2t2xAA1CC_xtlFAA1DC_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed D) -> @owned D
// CHECK: bb0
// CHECK:  checked_cast_br C in %0 : $C to D
// CHECK: bb1
_ = ConcreteToArchetypeCastC(t: c, t2: d)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ConcreteToArchetypeCastC1t2t2xAA1CC_xtlFAA1EC_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed E) -> @owned E
// CHECK: bb0
// CHECK: unreachable
_ = ConcreteToArchetypeCastC(t: c, t2: e)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch24ConcreteToArchetypeCastD1t2t2xAA1DC_xtlFAA1CC_Tg5 : $@convention(thin) (@guaranteed D, @guaranteed C) -> @owned C
// CHECK: bb0
// CHECK:  [[T0:%.*]] = upcast %0 : $D to $C
// CHECK:  return [[T0]]
_ = ConcreteToArchetypeCastD(t: d, t2: c)

////////////////////////
// Super To Archetype //
////////////////////////

func SuperToArchetypeCastC<T>(c : C, t : T) -> T {
  if let x = c as? T {
    return x
  }
  preconditionFailure("??? Profit?")
}

func SuperToArchetypeCastD<T>(d : D, t : T) -> T {
  if let x = d as? T {
    return x
  }
  preconditionFailure("??? Profit?")
}

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch21SuperToArchetypeCastC1c1txAA1CC_xtlFAF_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed C) -> @owned C
// CHECK: bb0
// CHECK: return %0 : $C
_ = SuperToArchetypeCastC(c: c, t: c)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch21SuperToArchetypeCastC1c1txAA1CC_xtlFAA1DC_Tg5 : $@convention(thin) (@guaranteed C, @guaranteed D) -> @owned D
// CHECK: bb0
// CHECK:  checked_cast_br C in %0 : $C to D
// CHECK: bb1
_ = SuperToArchetypeCastC(c: c, t: d)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch21SuperToArchetypeCastC1c1txAA1CC_xtlFAA8NotUInt8V_Tg5 : $@convention(thin) (@guaranteed C, NotUInt8) -> NotUInt8
// CHECK: bb0
// CHECK: unreachable
_ = SuperToArchetypeCastC(c: c, t: b)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch21SuperToArchetypeCastD1d1txAA1DC_xtlFAA1CC_Tg5 : $@convention(thin) (@guaranteed D, @guaranteed C) -> @owned C
// CHECK: bb0
// CHECK:  [[T0:%.*]] = upcast %0 : $D to $C
// CHECK:  return [[T0]]
_ = SuperToArchetypeCastD(d: d, t: c)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch21SuperToArchetypeCastD1d1txAA1DC_xtlFAF_Tg5 : $@convention(thin) (@guaranteed D, @guaranteed D) -> @owned D
// CHECK: bb0
// CHECK: return %0 : $D
_ = SuperToArchetypeCastD(d: d, t: d)

//////////////////////////////
// Existential To Archetype //
//////////////////////////////

func ExistentialToArchetypeCast<T>(o : AnyObject, t : T) -> T {
  if let x = o as? T {
    return x
  }
  preconditionFailure("??? Profit?")
}

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch26ExistentialToArchetypeCast1o1txyXl_xtlFAA1CC_Tg5 : $@convention(thin) (@guaranteed AnyObject, @guaranteed C) -> @owned C
// CHECK: bb0
// CHECK:  checked_cast_br AnyObject in %0 : $AnyObject to C
// CHECK: bb1
_ = ExistentialToArchetypeCast(o: o, t: c)

// CHECK-LABEL: sil shared @$s30specialize_checked_cast_branch26ExistentialToArchetypeCast1o1txyXl_xtlFAA8NotUInt8V_Tg5 : $@convention(thin) (@guaranteed AnyObject, NotUInt8) -> NotUInt8
// CHECK: bb0
// CHECK:  checked_cast_addr_br take_always AnyObject in {{%.*}} : $*AnyObject to NotUInt8 in {{%.*}} : $*NotUInt8,
// CHECK: bb1
_ = ExistentialToArchetypeCast(o: o, t: b)

// CHECK-LABEL: sil shared {{.*}}@$s30specialize_checked_cast_branch26ExistentialToArchetypeCast1o1txyXl_xtlFyXl_Tg5 : $@convention(thin) (@guaranteed AnyObject, @guaranteed AnyObject) -> @owned AnyObject
// CHECK: bb0
// CHECK-NOT: checked_cast_br %
// CHECK: return %0 : $AnyObject
// CHECK-NOT: checked_cast_br %
// CHECK: } // end sil function '$s30specialize_checked_cast_branch26ExistentialToArchetypeCast1o1txyXl_xtlFyXl_Tg5'
_ = ExistentialToArchetypeCast(o: o, t: o)
