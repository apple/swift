
// RUN: %target-swift-emit-silgen -module-name subclass_existentials -Xllvm -sil-full-demangle -parse-as-library -primary-file %s -verify | %FileCheck %s
// RUN: %target-swift-emit-ir -module-name subclass_existentials -parse-as-library -primary-file %s

// Note: we pass -verify above to ensure there are no spurious
// compiler warnings relating to casts.

protocol Q {}

class Base<T> : Q {
  required init(classInit: ()) {}
  func classSelfReturn() -> Self {
    return self
  }
  static func classSelfReturn() -> Self {
    return self.init(classInit: ())
  }
}

protocol P {
  init(protocolInit: ())
  func protocolSelfReturn() -> Self
  static func protocolSelfReturn() -> Self
}

class Derived : Base<Int>, P {
  required init(protocolInit: ()) {
    super.init(classInit: ())
  }

  required init(classInit: ()) {
    super.init(classInit: ())
  }

  func protocolSelfReturn() -> Self {
    return self
  }
  static func protocolSelfReturn() -> Self {
    return self.init(classInit: ())
  }
}

protocol R {}

// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials11conversions8baseAndP7derived0fE1R0dE5PType0F4Type0fE5RTypeyAA1P_AA4BaseCySiGXc_AA7DerivedCAA1R_ANXcAaI_ALXcXpANmAaO_ANXcXptF : $@convention(thin) (@guaranteed any Base<Int> & P, @guaranteed Derived, @guaranteed any Derived & R, @thick any (Base<Int> & P).Type, @thick Derived.Type, @thick any (Derived & R).Type) -> () {
// CHECK: bb0([[ARG0:%.*]] : @guaranteed $any Base<Int> & P,

func conversions(
  baseAndP: Base<Int> & P,
  derived: Derived,
  derivedAndR: Derived & R,

  baseAndPType: (Base<Int> & P).Type,
  derivedType: Derived.Type,
  derivedAndRType: (Derived & R).Type) {

  // Values

  // CHECK: [[PAYLOAD:%.*]] = open_existential_ref [[ARG0]] : $any Base<Int> & P
  // CHECK: [[REF:%.*]] = copy_value [[PAYLOAD]]
  // CHECK: [[BASE:%.*]] = upcast [[REF]] : $@opened("{{.*}}", any Base<Int> & P) Self to $Base<Int>
  // CHECK: destroy_value [[BASE]] : $Base<Int>
  let _: Base<Int> = baseAndP

  // CHECK: [[PAYLOAD:%.*]] = open_existential_ref [[ARG0]] : $any Base<Int> & P
  // CHECK: [[RESULT:%.*]] = alloc_stack $any P
  // CHECK: [[RESULT_PAYLOAD:%.*]] = init_existential_addr [[RESULT]] : $*any P, $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[REF:%.*]] = copy_value [[PAYLOAD]]
  // CHECK: store [[REF]] to [init] [[RESULT_PAYLOAD]]
  // CHECK: destroy_addr [[RESULT]] : $*any P
  // CHECK: dealloc_stack [[RESULT]] : $*any P
  let _: P = baseAndP

  // CHECK: [[PAYLOAD:%.*]] = open_existential_ref [[ARG0]] : $any Base<Int> & P
  // CHECK: [[RESULT:%.*]] = alloc_stack $any Q
  // CHECK: [[RESULT_PAYLOAD:%.*]] = init_existential_addr [[RESULT]] : $*any Q, $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[REF:%.*]] = copy_value [[PAYLOAD]]
  // CHECK: store [[REF]] to [init] [[RESULT_PAYLOAD]]
  // CHECK: destroy_addr [[RESULT]] : $*any Q
  // CHECK: dealloc_stack [[RESULT]] : $*any Q
  let _: Q = baseAndP

  // CHECK: [[PAYLOAD:%.*]] = open_existential_ref [[ARG2:%.*]] : $any Derived & R
  // CHECK: [[REF:%.*]] = copy_value [[PAYLOAD]]
  // CHECK: [[RESULT:%.*]] = init_existential_ref [[REF]] : $@opened("{{.*}}", any Derived & R) Self : $@opened("{{.*}}", any Derived & R) Self, $any Base<Int> & P
  // CHECK: destroy_value [[RESULT]]
  let _: Base<Int> & P = derivedAndR

  // Metatypes

  // CHECK: [[PAYLOAD:%.*]] = open_existential_metatype %3 : $@thick any (Base<Int> & P).Type to $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type
  // CHECK: [[RESULT:%.*]] = upcast [[PAYLOAD]] : $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type to $@thick Base<Int>.Type
  let _: Base<Int>.Type = baseAndPType

  // CHECK: [[PAYLOAD:%.*]] = open_existential_metatype %3 : $@thick any (Base<Int> & P).Type to $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type
  // CHECK: [[RESULT:%.*]] = init_existential_metatype [[PAYLOAD]] : $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type, $@thick any P.Type
  let _: P.Type = baseAndPType

  // CHECK: [[PAYLOAD:%.*]] = open_existential_metatype %3 : $@thick any (Base<Int> & P).Type to $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type
  // CHECK: [[RESULT:%.*]] = init_existential_metatype [[PAYLOAD]] : $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type, $@thick any Q.Type
  let _: Q.Type = baseAndPType

  // CHECK: [[RESULT:%.*]] = init_existential_metatype %4 : $@thick Derived.Type, $@thick any (Base<Int> & P).Type
  let _: (Base<Int> & P).Type = derivedType

  // CHECK: [[PAYLOAD:%.*]] = open_existential_metatype %5 : $@thick any (Derived & R).Type to $@thick (@opened("{{.*}}", any Derived & R) Self).Type
  // CHECK: [[RESULT:%.*]] = init_existential_metatype [[PAYLOAD]] : $@thick (@opened("{{.*}}", any Derived & R) Self).Type, $@thick any (Base<Int> & P).Type
  let _: (Base<Int> & P).Type = derivedAndRType

  // CHECK: return
}

// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials11methodCalls8baseAndP0eF5PTypeyAA1P_AA4BaseCySiGXc_AaE_AHXcXptF : $@convention(thin) (@guaranteed any Base<Int> & P, @thick any (Base<Int> & P).Type) -> () {

func methodCalls(
  baseAndP: Base<Int> & P,
  baseAndPType: (Base<Int> & P).Type) {
  // CHECK: bb0([[ARG0:%.*]] : @guaranteed $any Base<Int> & P,
  // CHECK: [[PAYLOAD:%.*]] = open_existential_ref [[ARG0]] : $any Base<Int> & P to $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[REF:%.*]] = copy_value [[PAYLOAD]] : $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[CLASS_REF:%.*]] = upcast [[REF]] : $@opened("{{.*}}", any Base<Int> & P) Self to $Base<Int>
  // CHECK: [[METHOD:%.*]] = class_method [[CLASS_REF]] : $Base<Int>, #Base.classSelfReturn : <T> (Base<T>) -> () -> @dynamic_self Base<T>, $@convention(method) <τ_0_0> (@guaranteed Base<τ_0_0>) -> @owned Base<τ_0_0>
  // CHECK: [[RESULT_CLASS_REF:%.*]] = apply [[METHOD]]<Int>([[CLASS_REF]]) : $@convention(method) <τ_0_0> (@guaranteed Base<τ_0_0>) -> @owned Base<τ_0_0>
  // CHECK: destroy_value [[CLASS_REF]] : $Base<Int>
  // CHECK: [[RESULT_REF:%.*]] = unchecked_ref_cast [[RESULT_CLASS_REF]] : $Base<Int> to $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[RESULT:%.*]] = init_existential_ref [[RESULT_REF]] : $@opened("{{.*}}", any Base<Int> & P) Self, $any Base<Int> & P
  // CHECK: destroy_value [[RESULT]] : $any Base<Int> & P
  let _: Base<Int> & P = baseAndP.classSelfReturn()

  // CHECK: [[PAYLOAD:%.*]] = open_existential_ref [[ARG0]] : $any Base<Int> & P to $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[SELF_BOX:%.*]] = alloc_stack $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[SB:%.*]] = store_borrow [[PAYLOAD]] to [[SELF_BOX]] : $*@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[METHOD:%.*]] = witness_method $@opened("{{.*}}", any Base<Int> & P) Self, #P.protocolSelfReturn : <Self where Self : P> (Self) -> () -> Self, [[PAYLOAD]] : $@opened("{{.*}}", any Base<Int> & P) Self : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
  // CHECK: [[RESULT_BOX:%.*]] = alloc_box
  // CHECK: [[RESULT_LIFETIME:%[^,]+]] = begin_borrow [lexical] [[RESULT_BOX]]
  // CHECK: [[RESULT_BUF:%.*]] = project_box [[RESULT_LIFETIME]]
  // CHECK: apply [[METHOD]]<@opened("{{.*}}", any Base<Int> & P) Self>([[RESULT_BUF]], [[SB]]) : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
  // end_borrow [[SB]]
  // CHECK: dealloc_stack [[SELF_BOX]] : $*@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[RESULT_REF:%.*]] = load [take] [[RESULT_BUF]] : $*@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[RESULT:%.*]] = init_existential_ref [[RESULT_REF]] : $@opened("{{.*}}", any Base<Int> & P) Self : $@opened("{{.*}}", any Base<Int> & P) Self, $any Base<Int> & P
  // CHECK: destroy_value [[RESULT]] : $any Base<Int> & P
  // CHECK: end_borrow [[RESULT_LIFETIME]]
  // CHECK: dealloc_box [[RESULT_BOX]]
  let _: Base<Int> & P = baseAndP.protocolSelfReturn()

  // CHECK: [[METATYPE:%.*]] = open_existential_metatype %1 : $@thick any (Base<Int> & P).Type to $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type
  // CHECK: [[METATYPE_REF:%.*]] = upcast [[METATYPE]] : $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type to $@thick Base<Int>.Type
  // CHECK: [[METHOD:%.*]] = function_ref @$s21subclass_existentials4BaseC15classSelfReturnACyxGXDyFZ : $@convention(method) <τ_0_0> (@thick Base<τ_0_0>.Type) -> @owned Base<τ_0_0>
  // CHECK: [[RESULT_REF2:%.*]] = apply [[METHOD]]<Int>([[METATYPE_REF]])
  // CHECK: [[RESULT_REF:%.*]] = unchecked_ref_cast [[RESULT_REF2]] : $Base<Int> to $@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[RESULT:%.*]] = init_existential_ref [[RESULT_REF]] : $@opened("{{.*}}", any Base<Int> & P) Self : $@opened("{{.*}}", any Base<Int> & P) Self, $any Base<Int> & P
  // CHECK: destroy_value [[RESULT]]
  let _: Base<Int> & P = baseAndPType.classSelfReturn()

  // CHECK: [[METATYPE:%.*]] = open_existential_metatype %1 : $@thick any (Base<Int> & P).Type to $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type
  // CHECK: [[METHOD:%.*]] = witness_method $@opened("{{.*}}", any Base<Int> & P) Self, #P.protocolSelfReturn : <Self where Self : P> (Self.Type) -> () -> Self, [[METATYPE]] : $@thick (@opened("{{.*}}", any Base<Int> & P) Self).Type : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@thick τ_0_0.Type) -> @out τ_0_0
  // CHECK: [[RESULT_BOX:%.*]] = alloc_box
  // CHECK: [[RESULT_LIFETIME:%[^,]+]] = begin_borrow [lexical] [[RESULT_BOX]]
  // CHECK: [[RESULT_BUF:%.*]] = project_box
  // CHECK: apply [[METHOD]]<@opened("{{.*}}", any Base<Int> & P) Self>([[RESULT_BUF]], [[METATYPE]]) : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@thick τ_0_0.Type) -> @out τ_0_0
  // CHECK: [[RESULT_REF:%.*]] = load [take] [[RESULT_BUF]] : $*@opened("{{.*}}", any Base<Int> & P) Self
  // CHECK: [[RESULT_VALUE:%.*]] = init_existential_ref [[RESULT_REF]] : $@opened("{{.*}}", any Base<Int> & P) Self : $@opened("{{.*}}", any Base<Int> & P) Self, $any Base<Int> & P
  // CHECK: destroy_value [[RESULT_VALUE]]
  // CHECK: end_borrow [[RESULT_LIFETIME]]
  // CHECK: dealloc_box [[RESULT_BOX]]
  let _: Base<Int> & P = baseAndPType.protocolSelfReturn()

  // Partial applications
  let _: () -> (Base<Int> & P) = baseAndP.classSelfReturn
  let _: () -> (Base<Int> & P) = baseAndP.protocolSelfReturn

  let _: () -> (Base<Int> & P) = baseAndPType.classSelfReturn
  let _: () -> (Base<Int> & P) = baseAndPType.protocolSelfReturn

  let _: (()) -> (Base<Int> & P) = baseAndPType.init(classInit:)
  let _: (()) -> (Base<Int> & P) = baseAndPType.init(protocolInit:)

  // CHECK:      return
  // CHECK-NEXT: }
}


// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials29methodCallsOnProtocolMetatypeyyF : $@convention(thin) () -> () {
// CHECK: metatype $@thin (any Base<Int> & P).Type
// CHECK: function_ref @$[[THUNK1_NAME:[_a-zA-Z0-9]+]]
// CHECK: } // end sil function '$s21subclass_existentials29methodCallsOnProtocolMetatypeyyF'
//
// CHECK: sil private [ossa] @$[[THUNK1_NAME]] : $@convention(thin) (@guaranteed any Base<Int> & P) -> @owned @callee_guaranteed () -> @owned any Base<Int> & P {
// CHECK: bb0(%0 : @guaranteed $any Base<Int> & P):
// CHECK:   [[THUNK2:%[0-9]+]] = function_ref @$[[THUNK2_NAME:[_a-zA-Z0-9]+]]
// CHECK:   [[SELF_COPY:%[0-9]+]] = copy_value %0
// CHECK:   [[RESULT:%[0-9]+]] = partial_apply [callee_guaranteed] [[THUNK2]]([[SELF_COPY]])
// CHECK:  return [[RESULT]]
// CHECK: } // end sil function '$[[THUNK1_NAME]]'
//
// CHECK: sil private [ossa] @$[[THUNK2_NAME]] : $@convention(thin) (@guaranteed any Base<Int> & P) -> @owned any Base<Int> & P {
// CHECK: bb0(%0 : @closureCapture @guaranteed $any Base<Int> & P):
// CHECK:  [[OPENED:%[0-9]+]] = open_existential_ref %0 : $any Base<Int> & P to $[[OPENED_TY:@opened\("[-A-F0-9]+", any Base<Int> & P\) Self]]
// CHECK:  [[OPENED_COPY:%[0-9]+]] = copy_value [[OPENED]]
// CHECK:  [[CLASS:%[0-9]+]] = upcast [[OPENED_COPY]] : $[[OPENED_TY]] to $Base<Int>
// CHECK:  [[METHOD:%[0-9]+]] = class_method [[CLASS]] : $Base<Int>, #Base.classSelfReturn
// CHECK:  [[RESULT:%[0-9]+]] = apply [[METHOD]]<Int>([[CLASS]]) : $@convention(method) <τ_0_0> (@guaranteed Base<τ_0_0>) -> @owned Base<τ_0_0>
// CHECK:  destroy_value [[CLASS]]
// CHECK:  [[TO_OPENED:%[0-9]+]] = unchecked_ref_cast [[RESULT]] : $Base<Int> to $[[OPENED_TY]]
// CHECK:  [[ERASED:%[0-9]+]] = init_existential_ref [[TO_OPENED]] : $[[OPENED_TY]]
// CHECK:  return [[ERASED]]
// CHECK: } // end sil function '$[[THUNK2_NAME]]'
func methodCallsOnProtocolMetatype() {
  let _ = (Base<Int> & P).classSelfReturn
}

protocol PropertyP {
  var p: PropertyP & PropertyC { get set }

  subscript(key: Int) -> Int { get set }
}

class PropertyC {
  var c: PropertyP & PropertyC {
    get {
      return self as! PropertyP & PropertyC
    }
    set { }
  }

  subscript(key: (Int, Int)) -> Int {
    get {
      return 0
    } set { }
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials16propertyAccessesyyAA9PropertyP_AA0E1CCXcF : $@convention(thin) (@guaranteed any PropertyC & PropertyP) -> () {
func propertyAccesses(_ x: PropertyP & PropertyC) {
  var xx = x
  xx.p.p = x
  xx.c.c = x

  propertyAccesses(xx.p)
  propertyAccesses(xx.c)

  _ = xx[1]
  xx[1] = 1
  xx[1] += 1

  _ = xx[(1, 2)]
  xx[(1, 2)] = 1
  xx[(1, 2)] += 1
}

// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials19functionConversions15returnsBaseAndP0efG5PType0E7Derived0eI4Type0eiG1R0eiG5RTypeyAA1P_AA0F0CySiGXcyc_AaI_ALXcXpycAA0I0CycANmycAA1R_ANXcycAaO_ANXcXpyctF : $@convention(thin) (@guaranteed @callee_guaranteed () -> @owned any Base<Int> & P, @guaranteed @callee_guaranteed () -> @thick any (Base<Int> & P).Type, @guaranteed @callee_guaranteed () -> @owned Derived, @guaranteed @callee_guaranteed () -> @thick Derived.Type, @guaranteed @callee_guaranteed () -> @owned any Derived & R, @guaranteed @callee_guaranteed () -> @thick any (Derived & R).Type) -> () {
func functionConversions(
  returnsBaseAndP: @escaping () -> (Base<Int> & P),
  returnsBaseAndPType: @escaping () -> (Base<Int> & P).Type,
  returnsDerived: @escaping () -> Derived,
  returnsDerivedType: @escaping () -> Derived.Type,
  returnsDerivedAndR: @escaping () -> Derived & R,
  returnsDerivedAndRType: @escaping () -> (Derived & R).Type) {

  let _: () -> Base<Int> = returnsBaseAndP
  let _: () -> Base<Int>.Type = returnsBaseAndPType

  let _: () -> P = returnsBaseAndP
  let _: () -> P.Type = returnsBaseAndPType

  let _: () -> (Base<Int> & P) = returnsDerived
  let _: () -> (Base<Int> & P).Type = returnsDerivedType

  let _: () -> Base<Int> = returnsDerivedAndR
  let _: () -> Base<Int>.Type = returnsDerivedAndRType

  let _: () -> (Base<Int> & P) = returnsDerivedAndR
  let _: () -> (Base<Int> & P).Type = returnsDerivedAndRType

  let _: () -> P = returnsDerivedAndR
  let _: () -> P.Type = returnsDerivedAndRType

  // CHECK:      return %
  // CHECK-NEXT: }
}

// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials9downcasts8baseAndP7derived0dE5PType0F4TypeyAA1P_AA4BaseCySiGXc_AA7DerivedCAaG_AJXcXpALmtF : $@convention(thin) (@guaranteed any Base<Int> & P, @guaranteed Derived, @thick any (Base<Int> & P).Type, @thick Derived.Type) -> () {
func downcasts(
  baseAndP: Base<Int> & P,
  derived: Derived,
  baseAndPType: (Base<Int> & P).Type,
  derivedType: Derived.Type) {
  // CHECK: bb0([[ARG0:%.*]] : @guaranteed $any Base<Int> & P, [[ARG1:%.*]] : @guaranteed $Derived, [[ARG2:%.*]] : $@thick any (Base<Int> & P).Type, [[ARG3:%.*]] : $@thick Derived.Type):
  // CHECK: [[COPIED:%.*]] = copy_value [[ARG0]] : $any Base<Int> & P
  // CHECK-NEXT: checked_cast_br any Base<Int> & P in [[COPIED]] : $any Base<Int> & P to Derived
  let _ = baseAndP as? Derived

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG0]] : $any Base<Int> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<Int> & P to Derived
  let _ = baseAndP as! Derived

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG0]] : $any Base<Int> & P
  // CHECK-NEXT: checked_cast_br any Base<Int> & P in [[COPIED]] : $any Base<Int> & P to any Derived & R
  let _ = baseAndP as? (Derived & R)

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG0]] : $any Base<Int> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<Int> & P to any Derived & R
  let _ = baseAndP as! (Derived & R)

  // CHECK:      checked_cast_br Derived.Type in %3 : $@thick Derived.Type to any (Derived & R).Type
  let _ = derivedType as? (Derived & R).Type

  // CHECK:      unconditional_checked_cast %3 : $@thick Derived.Type to any (Derived & R).Type
  let _ = derivedType as! (Derived & R).Type

  // CHECK:      checked_cast_br any (Base<Int> & P).Type in %2 : $@thick any (Base<Int> & P).Type to Derived.Type
  let _ = baseAndPType as? Derived.Type

  // CHECK:      unconditional_checked_cast %2 : $@thick any (Base<Int> & P).Type to Derived.Type
  let _ = baseAndPType as! Derived.Type

  // CHECK:      checked_cast_br any (Base<Int> & P).Type in %2 : $@thick any (Base<Int> & P).Type to any (Derived & R).Type
  let _ = baseAndPType as? (Derived & R).Type

  // CHECK:      unconditional_checked_cast %2 : $@thick any (Base<Int> & P).Type to any (Derived & R).Type
  let _ = baseAndPType as! (Derived & R).Type

  // CHECK:      return
  // CHECK-NEXT: }
}

// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials16archetypeUpcasts9baseTAndP0E7IntAndP7derivedyq__q0_q1_tAA4BaseCyxGRb_AA1PR_AGySiGRb0_AaIR0_AA7DerivedCRb1_r2_lF : $@convention(thin) <T, BaseTAndP, BaseIntAndP, DerivedT where BaseTAndP : Base<T>, BaseTAndP : P, BaseIntAndP : Base<Int>, BaseIntAndP : P, DerivedT : Derived> (@guaranteed BaseTAndP, @guaranteed BaseIntAndP, @guaranteed DerivedT) -> () {
func archetypeUpcasts<T,
                      BaseTAndP : Base<T> & P,
                      BaseIntAndP : Base<Int> & P,
                      DerivedT : Derived>(
                      baseTAndP: BaseTAndP,
                      baseIntAndP : BaseIntAndP,
                      derived : DerivedT) {
  // CHECK: bb0([[ARG0:%.*]] : @guaranteed $BaseTAndP, [[ARG1:%.*]] : @guaranteed $BaseIntAndP, [[ARG2:%.*]] : @guaranteed $DerivedT)
  // CHECK: [[COPIED:%.*]] = copy_value [[ARG0]] : $BaseTAndP
  // CHECK-NEXT: init_existential_ref [[COPIED]] : $BaseTAndP : $BaseTAndP, $any Base<T> & P
  let _: Base<T> & P = baseTAndP

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG1]] : $BaseIntAndP
  // CHECK-NEXT: init_existential_ref [[COPIED]] : $BaseIntAndP : $BaseIntAndP, $any Base<Int> & P
  let _: Base<Int> & P = baseIntAndP

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG2]] : $DerivedT
  // CHECK-NEXT: init_existential_ref [[COPIED]] : $DerivedT : $DerivedT, $any Base<Int> & P
  let _: Base<Int> & P = derived

  // CHECK:      return
  // CHECK-NEXT: }
}

// CHECK-LABEL: sil hidden [ossa] @$s21subclass_existentials18archetypeDowncasts1s1t2pt5baseT0F3Int0f6TAndP_C00fg5AndP_C008derived_C00ji2R_C00fH10P_concrete0fgi2P_K0yx_q_q0_q1_q2_q3_q4_q5_AA1R_AA7DerivedCXcAA1P_AA4BaseCyq_GXcAaQ_ASySiGXctAaQR0_ATRb1_AURb2_ATRb3_AaQR3_AURb4_AaQR4_APRb5_r6_lF : $@convention(thin) <S, T, PT, BaseT, BaseInt, BaseTAndP, BaseIntAndP, DerivedT where PT : P, BaseT : Base<T>, BaseInt : Base<Int>, BaseTAndP : Base<T>, BaseTAndP : P, BaseIntAndP : Base<Int>, BaseIntAndP : P, DerivedT : Derived> (@in_guaranteed S, @in_guaranteed T, @in_guaranteed PT, @guaranteed BaseT, @guaranteed BaseInt, @guaranteed BaseTAndP, @guaranteed BaseIntAndP, @guaranteed DerivedT, @guaranteed any Derived & R, @guaranteed any Base<T> & P, @guaranteed any Base<Int> & P) -> () {
func archetypeDowncasts<S,
                        T,
                        PT : P,
                        BaseT : Base<T>,
                        BaseInt : Base<Int>,
                        BaseTAndP : Base<T> & P,
                        BaseIntAndP : Base<Int> & P,
                        DerivedT : Derived>(
  s: S,
  t: T,
  pt: PT,
  baseT : BaseT,
  baseInt : BaseInt,

  baseTAndP_archetype: BaseTAndP,
  baseIntAndP_archetype : BaseIntAndP,
  derived_archetype : DerivedT,
  derivedAndR_archetype : Derived & R,

  baseTAndP_concrete: Base<T> & P,
  baseIntAndP_concrete: Base<Int> & P) {

  // CHECK: ([[ARG0:%.*]] : $*S, [[ARG1:%.*]] : $*T, [[ARG2:%.*]] : $*PT, [[ARG3:%.*]] : @guaranteed $BaseT, [[ARG4:%.*]] : @guaranteed $BaseInt, [[ARG5:%.*]] : @guaranteed $BaseTAndP, [[ARG6:%.*]] : @guaranteed $BaseIntAndP, [[ARG7:%.*]] : @guaranteed $DerivedT, [[ARG8:%.*]] : @guaranteed $any Derived & R, [[ARG9:%.*]] : @guaranteed $any Base<T> & P, [[ARG10:%.*]] : @guaranteed $any Base<Int> & P)

  // CHECK:      [[COPY:%.*]] = alloc_stack $S
  // CHECK-NEXT: copy_addr %0 to [init] [[COPY]] : $*S
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $any Base<T> & P
  // CHECK-NEXT: checked_cast_addr_br take_always S in [[COPY]] : $*S to any Base<T> & P in [[RESULT]] : $*any Base<T> & P
  let _ = s as? (Base<T> & P)

  // CHECK:      [[COPY:%.*]] = alloc_stack $S
  // CHECK-NEXT: copy_addr [[ARG0]] to [init] [[COPY]] : $*S
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $any Base<T> & P
  // CHECK-NEXT: unconditional_checked_cast_addr S in [[COPY]] : $*S to any Base<T> & P in [[RESULT]] : $*any Base<T> & P
  let _ = s as! (Base<T> & P)

  // CHECK:      [[COPY:%.*]] = alloc_stack $S
  // CHECK-NEXT: copy_addr [[ARG0]] to [init] [[COPY]] : $*S
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $any Base<Int> & P
  // CHECK-NEXT: checked_cast_addr_br take_always S in [[COPY]] : $*S to any Base<Int> & P in [[RESULT]] : $*any Base<Int> & P
  let _ = s as? (Base<Int> & P)

  // CHECK:      [[COPY:%.*]] = alloc_stack $S
  // CHECK-NEXT: copy_addr [[ARG0]] to [init] [[COPY]] : $*S
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $any Base<Int> & P
  // CHECK-NEXT: unconditional_checked_cast_addr S in [[COPY]] : $*S to any Base<Int> & P in [[RESULT]] : $*any Base<Int> & P
  let _ = s as! (Base<Int> & P)

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG5]] : $BaseTAndP
  // CHECK-NEXT: checked_cast_br BaseTAndP in [[COPIED]] : $BaseTAndP to any Derived & R
  let _ = baseTAndP_archetype as? (Derived & R)

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG5]] : $BaseTAndP
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $BaseTAndP to any Derived & R
  let _ = baseTAndP_archetype as! (Derived & R)

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: [[COPY:%.*]] = alloc_stack $any Base<T> & P
  // CHECK-NEXT: store [[COPIED]] to [init] [[COPY]] : $*any Base<T> & P
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $Optional<S>
  // CHECK-NEXT: [[PAYLOAD:%.*]] = init_enum_data_addr [[RESULT]] : $*Optional<S>, #Optional.some
  // CHECK-NEXT: checked_cast_addr_br take_always any Base<T> & P in [[COPY]] : $*any Base<T> & P to S in [[PAYLOAD]] : $*S
  let _ = baseTAndP_concrete as? S

  // CHECK:      [[COPY:%.*]] = alloc_stack $any Base<T> & P
  // CHECK-NEXT: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: store [[COPIED]] to [init] [[COPY]] : $*any Base<T> & P
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $S
  // CHECK-NEXT: unconditional_checked_cast_addr any Base<T> & P in [[COPY]] : $*any Base<T> & P to S in [[RESULT]] : $*S
  let _ = baseTAndP_concrete as! S

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: checked_cast_br any Base<T> & P in [[COPIED]] : $any Base<T> & P to BaseT
  let _ = baseTAndP_concrete as? BaseT

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<T> & P to BaseT
  let _ = baseTAndP_concrete as! BaseT

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: checked_cast_br any Base<T> & P in [[COPIED]] : $any Base<T> & P to BaseInt
  let _ = baseTAndP_concrete as? BaseInt

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<T> & P to BaseInt
  let _ = baseTAndP_concrete as! BaseInt

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: checked_cast_br any Base<T> & P in [[COPIED]] : $any Base<T> & P to BaseTAndP
  let _ = baseTAndP_concrete as? BaseTAndP

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG9]] : $any Base<T> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<T> & P to BaseTAndP
  let _ = baseTAndP_concrete as! BaseTAndP

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG6]] : $BaseIntAndP
  // CHECK-NEXT: checked_cast_br BaseIntAndP in [[COPIED]] : $BaseIntAndP to any Derived & R
  let _ = baseIntAndP_archetype as? (Derived & R)

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG6]] : $BaseIntAndP
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $BaseIntAndP to any Derived & R
  let _ = baseIntAndP_archetype as! (Derived & R)

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: [[COPY:%.*]] = alloc_stack $any Base<Int> & P
  // CHECK-NEXT: store [[COPIED]] to [init] [[COPY]] : $*any Base<Int> & P
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $Optional<S>
  // CHECK-NEXT: [[PAYLOAD:%.*]] = init_enum_data_addr [[RESULT]] : $*Optional<S>, #Optional.some
  // CHECK-NEXT: checked_cast_addr_br take_always any Base<Int> & P in [[COPY]] : $*any Base<Int> & P to S in [[PAYLOAD]] : $*S
  let _ = baseIntAndP_concrete as? S

  // CHECK:      [[COPY:%.*]] = alloc_stack $any Base<Int> & P
  // CHECK-NEXT: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: store [[COPIED]] to [init] [[COPY]] : $*any Base<Int> & P
  // CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $S
  // CHECK-NEXT: unconditional_checked_cast_addr any Base<Int> & P in [[COPY]] : $*any Base<Int> & P to S in [[RESULT]] : $*S
  let _ = baseIntAndP_concrete as! S

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: checked_cast_br any Base<Int> & P in [[COPIED]] : $any Base<Int> & P to DerivedT
  let _ = baseIntAndP_concrete as? DerivedT

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<Int> & P to DerivedT
  let _ = baseIntAndP_concrete as! DerivedT

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: checked_cast_br any Base<Int> & P in [[COPIED]] : $any Base<Int> & P to BaseT
  let _ = baseIntAndP_concrete as? BaseT

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<Int> & P to BaseT
  let _ = baseIntAndP_concrete as! BaseT

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: checked_cast_br any Base<Int> & P in [[COPIED]] : $any Base<Int> & P to BaseInt
  let _ = baseIntAndP_concrete as? BaseInt

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<Int> & P to BaseInt
  let _ = baseIntAndP_concrete as! BaseInt

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: checked_cast_br any Base<Int> & P in [[COPIED]] : $any Base<Int> & P to BaseTAndP
  let _ = baseIntAndP_concrete as? BaseTAndP

  // CHECK: [[COPIED:%.*]] = copy_value [[ARG10]] : $any Base<Int> & P
  // CHECK-NEXT: unconditional_checked_cast [[COPIED]] : $any Base<Int> & P to BaseTAndP
  let _ = baseIntAndP_concrete as! BaseTAndP

  // CHECK:      return
  // CHECK-NEXT: }
}
