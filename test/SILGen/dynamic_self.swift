// RUN: %target-swift-emit-silgen -swift-version 4 %s -disable-objc-attr-requires-foundation-module -enable-objc-interop | %FileCheck %s
// RUN: %target-swift-emit-sil -swift-version 4 -O %s -disable-objc-attr-requires-foundation-module -enable-objc-interop
// RUN: %target-swift-emit-ir -swift-version 4 %s -disable-objc-attr-requires-foundation-module -enable-objc-interop

// RUN: %target-swift-emit-silgen -swift-version 5 %s -disable-objc-attr-requires-foundation-module -enable-objc-interop | %FileCheck %s
// RUN: %target-swift-emit-sil -swift-version 5 -O %s -disable-objc-attr-requires-foundation-module -enable-objc-interop
// RUN: %target-swift-emit-ir -swift-version 5 %s -disable-objc-attr-requires-foundation-module -enable-objc-interop

protocol P {
  func f() -> Self
  subscript() -> Self { get }
  var p: Self { get }
}

protocol CP : class {
  func f() -> Self
  subscript() -> Self { get }
  var p: Self { get }
}

class X : P, CP {
  required init(int i: Int) { }

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self1XC1f{{[_0-9a-zA-Z]*}}F : $@convention(method) (@guaranteed X) -> @owned
  func f() -> Self { return self }

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self1XCACXDycig : $@convention(method) (@guaranteed X) -> @owned X
  subscript() -> Self { self }

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self1XC1pACXDvg : $@convention(method) (@guaranteed X) -> @owned X
  var p: Self { self }

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self1XC7factory{{[_0-9a-zA-Z]*}}FZ : $@convention(method) (Int, @thick X.Type) -> @owned X
  // CHECK: bb0([[I:%[0-9]+]] : $Int, [[SELF:%[0-9]+]] : $@thick X.Type):
  // CHECK: [[DYNAMIC_SELF:%[0-9]+]] = unchecked_trivial_bit_cast [[SELF]] : $@thick X.Type to $@thick @dynamic_self X.Type
  // CHECK: [[STATIC_SELF:%[0-9]+]] = upcast [[DYNAMIC_SELF]] : $@thick @dynamic_self X.Type to $@thick X.Type
  // CHECK: [[CTOR:%[0-9]+]] = class_method [[STATIC_SELF]] : $@thick X.Type, #X.init!allocator : (X.Type) -> (Int) -> X, $@convention(method) (Int, @thick X.Type) -> @owned X
  // CHECK: apply [[CTOR]]([[I]], [[STATIC_SELF]]) : $@convention(method) (Int, @thick X.Type) -> @owned X
  class func factory(i: Int) -> Self { return self.init(int: i) }
}

class Y : X { 
  required init(int i: Int) {
    super.init(int: i)
  }
}

class GX<T> {
  func f() -> Self { return self }
}

class GY<T> : GX<[T]> { }

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self23testDynamicSelfDispatch{{[_0-9a-zA-Z]*}}F : $@convention(thin) (@guaranteed Y) -> ()
// CHECK: bb0([[Y:%[0-9]+]] : @guaranteed $Y):
// CHECK:   [[Y_AS_X:%[0-9]+]] = upcast [[Y]] : $Y to $X
// CHECK:   [[X_F:%[0-9]+]] = class_method [[Y_AS_X]] : $X, #X.f : (X) -> () -> @dynamic_self X, $@convention(method) (@guaranteed X) -> @owned X
// CHECK:   [[X_RESULT:%[0-9]+]] = apply [[X_F]]([[Y_AS_X]]) : $@convention(method) (@guaranteed X) -> @owned X
// CHECK:   [[Y_RESULT:%[0-9]+]] = unchecked_ref_cast [[X_RESULT]] : $X to $Y
// CHECK:   destroy_value [[Y_RESULT]] : $Y
func testDynamicSelfDispatch(y: Y) {
  _ = y.f()
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self30testDynamicSelfDispatchGeneric{{[_0-9a-zA-Z]*}}F : $@convention(thin) (@guaranteed GY<Int>) -> ()
func testDynamicSelfDispatchGeneric(gy: GY<Int>) {
  // CHECK: bb0([[GY:%[0-9]+]] : @guaranteed $GY<Int>):
  // CHECK:   [[GY_AS_GX:%[0-9]+]] = upcast [[GY]] : $GY<Int> to $GX<Array<Int>>
  // CHECK:   [[GX_F:%[0-9]+]] = class_method [[GY_AS_GX]] : $GX<Array<Int>>, #GX.f : <T> (GX<T>) -> () -> @dynamic_self GX<T>, $@convention(method) <τ_0_0> (@guaranteed GX<τ_0_0>) -> @owned GX<τ_0_0>
  // CHECK:   [[GX_RESULT:%[0-9]+]] = apply [[GX_F]]<[Int]>([[GY_AS_GX]]) : $@convention(method) <τ_0_0> (@guaranteed GX<τ_0_0>) -> @owned GX<τ_0_0>
  // CHECK:   [[GY_RESULT:%[0-9]+]] = unchecked_ref_cast [[GX_RESULT]] : $GX<Array<Int>> to $GY<Int>
  // CHECK:   destroy_value [[GY_RESULT]] : $GY<Int>
  _ = gy.f()
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self21testArchetypeDispatch{{[_0-9a-zA-Z]*}}F : $@convention(thin) <T where T : P> (@in_guaranteed T) -> ()
func testArchetypeDispatch<T: P>(t: T) {
  // CHECK: bb0([[T:%[0-9]+]] : $*T):
  // CHECK:   [[T_RESULT:%[0-9]+]] = alloc_stack $T
  // CHECK:   [[ARCHETYPE_F:%[0-9]+]] = witness_method $T, #P.f : {{.*}} : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
  // CHECK:   [[SELF_RESULT:%[0-9]+]] = apply [[ARCHETYPE_F]]<T>([[T_RESULT]], [[T]]) : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
  _ = t.f()
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self23testExistentialDispatch{{[_0-9a-zA-Z]*}}F
func testExistentialDispatch(p: P) {
// CHECK: bb0([[P:%[0-9]+]] : $*any P):
// CHECK:   [[PCOPY_ADDR:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P to $*@opened([[N:".*"]], any P) Self
// CHECK:   [[P_RESULT:%[0-9]+]] = alloc_stack $any P
// CHECK:   [[P_F_METHOD:%[0-9]+]] = witness_method $@opened([[N]], any P) Self, #P.f : {{.*}}, [[PCOPY_ADDR]]{{.*}} : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
// CHECK:   [[P_RESULT_ADDR:%[0-9]+]] = init_existential_addr [[P_RESULT]] : $*any P, $@opened([[N]], any P) Self
// CHECK:   apply [[P_F_METHOD]]<@opened([[N]], any P) Self>([[P_RESULT_ADDR]], [[PCOPY_ADDR]]) : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
// CHECK:   destroy_addr [[P_RESULT]] : $*any P
// CHECK:   dealloc_stack [[P_RESULT]] : $*any P
  _ = p.f()

// CHECK:   [[PCOPY_ADDR:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P to $*@opened([[N:".*"]], any P) Self
// CHECK:   [[P_RESULT:%[0-9]+]] = alloc_stack $any P
// CHECK:   [[P_P_GETTER:%[0-9]+]] = witness_method $@opened([[N]], any P) Self, #P.p!getter : {{.*}}, [[PCOPY_ADDR]]{{.*}} : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
// CHECK:   [[P_RESULT_ADDR2:%[0-9]+]] = init_existential_addr [[P_RESULT]] : $*any P, $@opened([[N]], any P) Self
// CHECK:   apply [[P_P_GETTER]]<@opened([[N]], any P) Self>([[P_RESULT_ADDR2]], [[PCOPY_ADDR]]) : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
// CHECK:   destroy_addr [[P_RESULT]] : $*any P
// CHECK:   dealloc_stack [[P_RESULT]] : $*any P
  _ = p.p

// CHECK:   [[PCOPY_ADDR:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P to $*@opened([[N:".*"]], any P) Self
// CHECK:   [[P_RESULT:%[0-9]+]] = alloc_stack $any P
// CHECK:   [[P_SUBSCRIPT_GETTER:%[0-9]+]] = witness_method $@opened([[N]], any P) Self, #P.subscript!getter : {{.*}}, [[PCOPY_ADDR]]{{.*}} : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
// CHECK:   [[P_RESULT_ADDR:%[0-9]+]] = init_existential_addr [[P_RESULT]] : $*any P, $@opened([[N]], any P) Self
// CHECK:   apply [[P_SUBSCRIPT_GETTER]]<@opened([[N]], any P) Self>([[P_RESULT_ADDR]], [[PCOPY_ADDR]]) : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out τ_0_0
// CHECK:   destroy_addr [[P_RESULT]] : $*any P
// CHECK:   dealloc_stack [[P_RESULT]] : $*any P
  _ = p[]
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self28testExistentialDispatchClass{{[_0-9a-zA-Z]*}}F : $@convention(thin) (@guaranteed any CP) -> ()
func testExistentialDispatchClass(cp: CP) {
// CHECK: bb0([[CP:%[0-9]+]] : @guaranteed $any CP):
// CHECK:   [[CP_ADDR:%[0-9]+]] = open_existential_ref [[CP]] : $any CP to $@opened([[N:".*"]], any CP) Self
// CHECK:   [[CP_F:%[0-9]+]] = witness_method $@opened([[N]], any CP) Self, #CP.f : {{.*}}, [[CP_ADDR]]{{.*}} : $@convention(witness_method: CP) <τ_0_0 where τ_0_0 : CP> (@guaranteed τ_0_0) -> @owned τ_0_0
// CHECK:   [[CP_F_RESULT:%[0-9]+]] = apply [[CP_F]]<@opened([[N]], any CP) Self>([[CP_ADDR]]) : $@convention(witness_method: CP) <τ_0_0 where τ_0_0 : CP> (@guaranteed τ_0_0) -> @owned τ_0_0
// CHECK:   [[RESULT_EXISTENTIAL:%[0-9]+]] = init_existential_ref [[CP_F_RESULT]] : $@opened([[N]], any CP) Self : $@opened([[N]], any CP) Self, $any CP
// CHECK:   destroy_value [[RESULT_EXISTENTIAL]]
  _ = cp.f()

// CHECK: [[CP_ADDR:%[0-9]+]] = open_existential_ref [[CP]] : $any CP to $@opened([[N:".*"]], any CP) Self
// CHECK: [[CP_ADDR_1:%[0-9]+]] = copy_value [[CP_ADDR]] : $@opened([[N]], any CP) Self
// CHECK: [[CP_BORROWED:%[0-9]+]] = begin_borrow [[CP_ADDR_1]] : $@opened([[N]], any CP) Self
// CHECK: [[CP_P_GETTER:%[0-9]+]] = witness_method $@opened([[N]], any CP) Self, #CP.p!getter : {{.*}}, [[CP_ADDR]]{{.*}} : $@convention(witness_method: CP) <τ_0_0 where τ_0_0 : CP> (@guaranteed τ_0_0) -> @owned τ_0_0
// CHECK: [[APPLY_RESULT:%[0-9]+]] = apply [[CP_P_GETTER]]<@opened([[N]], any CP) Self>([[CP_BORROWED]]) : $@convention(witness_method: CP) <τ_0_0 where τ_0_0 : CP> (@guaranteed τ_0_0) -> @owned τ_0_0
// CHECK: end_borrow [[CP_BORROWED]] : $@opened([[N]], any CP) Self
// CHECK: destroy_value [[CP_ADDR_1]] : $@opened([[N]], any CP) Self
// CHECK: [[RESULT_EXISTENTIAL:%[0-9]+]] = init_existential_ref [[APPLY_RESULT]] : $@opened([[N]], any CP) Self : $@opened([[N]], any CP) Self, $any CP
// CHECK: destroy_value [[RESULT_EXISTENTIAL]] : $any CP
  _ = cp.p

// CHECK: [[CP_ADDR:%[0-9]+]] = open_existential_ref [[CP]] : $any CP to $@opened([[N:".*"]], any CP) Self
// CHECK: [[CP_ADDR_1:%[0-9]+]] = copy_value [[CP_ADDR]] : $@opened([[N]], any CP) Self
// CHECK: [[CP_BORROWED:%[0-9]+]] = begin_borrow [[CP_ADDR_1]] : $@opened([[N]], any CP) Self
// CHECK: [[CP_SUBSCRIPT_GETTER:%[0-9]+]] = witness_method $@opened([[N]], any CP) Self, #CP.subscript!getter : {{.*}}, [[CP_ADDR]]{{.*}} : $@convention(witness_method: CP) <τ_0_0 where τ_0_0 : CP> (@guaranteed τ_0_0) -> @owned τ_0_0
// CHECK: [[APPLY_RESULT:%[0-9]+]] = apply [[CP_SUBSCRIPT_GETTER]]<@opened([[N]], any CP) Self>([[CP_BORROWED]]) : $@convention(witness_method: CP) <τ_0_0 where τ_0_0 : CP> (@guaranteed τ_0_0) -> @owned τ_0_0
// CHECK: end_borrow [[CP_BORROWED]] : $@opened([[N]], any CP) Self
// CHECK: destroy_value [[CP_ADDR_1]] : $@opened([[N]], any CP) Self
// CHECK: [[RESULT_EXISTENTIAL:%[0-9]+]] = init_existential_ref [[APPLY_RESULT]] : $@opened([[N]], any CP) Self : $@opened([[N]], any CP) Self, $any CP
// CHECK: destroy_value [[RESULT_EXISTENTIAL]] : $any CP
  _ = cp[]
}

@objc class ObjC {
  @objc func method() -> Self { return self }
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self21testAnyObjectDispatch1oyyXl_tF : $@convention(thin) (@guaranteed AnyObject) -> () {
func testAnyObjectDispatch(o: AnyObject) {
  // CHECK: dynamic_method_br [[O_OBJ:%[0-9]+]] : $@opened({{.*}}, AnyObject) Self, #ObjC.method!foreign, bb1, bb2

  // CHECK: bb1([[METHOD:%[0-9]+]] : {{.*}}):
  // CHECK:   [[O_OBJ_COPY:%.*]] = copy_value [[O_OBJ]]
  // CHECK:   [[VAR_9:%[0-9]+]] = partial_apply [callee_guaranteed] [[METHOD]]([[O_OBJ_COPY]]) :
  var _ = o.method
}
// CHECK: } // end sil function '$s12dynamic_self21testAnyObjectDispatch1oyyXl_tF'


// <rdar://problem/16270889> Dispatch through ObjC metatypes.
class ObjCInit {
  @objc dynamic required init() { }
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self12testObjCInit4metayAA0dE0Cm_tF : $@convention(thin) (@thick ObjCInit.Type) -> ()
func testObjCInit(meta: ObjCInit.Type) {
// CHECK: bb0([[THICK_META:%[0-9]+]] : $@thick ObjCInit.Type):
// CHECK:   [[INIT:%[0-9]+]] = function_ref @$s12dynamic_self8ObjCInitCACycfC : $@convention(method) (@thick ObjCInit.Type) -> @owned ObjCInit
// CHECK:   [[RESULT_OBJ:%[0-9]+]] = apply [[INIT]]([[THICK_META]]) : $@convention(method) (@thick ObjCInit.Type) -> @owned ObjCInit
// CHECK:   [[RESULT:%[0-9]+]] = tuple ()
// CHECK:   return [[RESULT]] : $()
  _ = meta.init()
}

class OptionalResult {
  func foo() -> Self? { return self }
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self14OptionalResultC3fooACXDSgyF : $@convention(method) (@guaranteed OptionalResult) -> @owned Optional<OptionalResult> {
// CHECK: bb0([[SELF:%.*]] : @guaranteed $OptionalResult):
// CHECK-NEXT: debug_value [[SELF]] : $OptionalResult
// CHECK-NEXT: [[SELF_COPY:%.*]] = copy_value [[SELF]]
// CHECK-NEXT: [[T0:%.*]] = enum $Optional<OptionalResult>, #Optional.some!enumelt, [[SELF_COPY]] : $OptionalResult
// CHECK-NEXT: return [[T0]] : $Optional<OptionalResult>
// CHECK: } // end sil function '$s12dynamic_self14OptionalResultC3fooACXDSgyF'

class OptionalResultInheritor : OptionalResult {
  func bar() {}
}

func testOptionalResult(v : OptionalResultInheritor) {
  v.foo()?.bar()
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self18testOptionalResult1vyAA0dE9InheritorC_tF : $@convention(thin) (@guaranteed OptionalResultInheritor) -> () {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $OptionalResultInheritor):
// CHECK:      [[CAST_ARG:%.*]] = upcast [[ARG]]
// CHECK:      [[T0:%.*]] = class_method [[CAST_ARG]] : $OptionalResult, #OptionalResult.foo : (OptionalResult) -> () -> @dynamic_self OptionalResult?, $@convention(method) (@guaranteed OptionalResult) -> @owned Optional<OptionalResult>
// CHECK-NEXT: [[RES:%.*]] = apply [[T0]]([[CAST_ARG]])
// CHECK-NEXT: [[T4:%.*]] = unchecked_ref_cast [[RES]] : $Optional<OptionalResult> to $Optional<OptionalResultInheritor>

func id<T>(_ t: T) -> T { return t }

class Z {

  required init() {}

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self1ZC23testDynamicSelfCaptures1xACXDSi_tF : $@convention(method) (Int, @guaranteed Z) -> @owned Z {
  func testDynamicSelfCaptures(x: Int) -> Self {
    // CHECK: bb0({{.*}}, [[SELF:%.*]] : @guaranteed $Z):

    // Single capture of 'self' type

    // CHECK:      [[FN:%.*]] = function_ref @$s12dynamic_self1ZC23testDynamicSelfCaptures1xACXDSi_tFyycfU_ : $@convention(thin) (@guaranteed Z) -> ()
    // CHECK-NEXT: [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Z
    // CHECK-NEXT: partial_apply [callee_guaranteed] [[FN]]([[SELF_COPY]])
    let fn1 = { _ = self }
    fn1()

    // Capturing 'self', but it's not the last capture. Make sure it ends
    // up at the end of the list anyway

    // CHECK:      [[FN:%.*]] = function_ref @$s12dynamic_self1ZC23testDynamicSelfCaptures1xACXDSi_tFyycfU0_ : $@convention(thin) (Int, @guaranteed Z) -> ()
    // CHECK-NEXT: [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Z
    // CHECK-NEXT: partial_apply [callee_guaranteed] [[FN]]({{.*}}, [[SELF_COPY]])
    let fn2 = {
      _ = self
      _ = x
    }
    fn2()

    // Capturing 'self' weak, so we have to pass in a metatype explicitly
    // so that IRGen can recover metadata.

    // CHECK:      [[WEAK_SELF:%.*]] = alloc_box ${ var @sil_weak Optional<Z> }
    // CHECK:      [[WEAK_SELF_LIFETIME:%.*]] = begin_borrow [lexical] [var_decl] [[WEAK_SELF]]
    // CHECK:      [[FN:%.*]] = function_ref @$s12dynamic_self1ZC23testDynamicSelfCaptures1xACXDSi_tFyycfU1_ : $@convention(thin) (@guaranteed { var @sil_weak Optional<Z> }, @thick @dynamic_self Z.Type) -> ()
    // CHECK:      [[WEAK_SELF_COPY:%.*]] = copy_value [[WEAK_SELF_LIFETIME]] : ${ var @sil_weak Optional<Z> }
    // CHECK-NEXT: [[DYNAMIC_SELF:%.*]] = metatype $@thick @dynamic_self Z.Type
    // CHECK:      partial_apply [callee_guaranteed] [[FN]]([[WEAK_SELF_COPY]], [[DYNAMIC_SELF]]) : $@convention(thin) (@guaranteed { var @sil_weak Optional<Z> }, @thick @dynamic_self Z.Type) -> ()
    let fn3 = {
      [weak self] in
      _ = self
    }
    fn3()

    // Capturing a value with a complex type involving self

    // CHECK:      [[FN:%.*]] = function_ref @$s12dynamic_self1ZC23testDynamicSelfCaptures1xACXDSi_tFyycfU2_ : $@convention(thin) (@guaranteed (Z, Z), @thick @dynamic_self Z.Type) -> ()
    let xx = (self, self)
    let fn4 = {
      _ = xx
    }
    fn4()

    return self
  }

  // Capturing metatype of dynamic self
  static func testStaticMethodDynamicSelfCaptures() -> Self {
    let fn0 = { _ = self; _ = { _ = self } }
    fn0()

    let x = self
    let fn1 = { _ = x; _ = { _ = x } }
    fn1()

    let xx = (self, self)
    let fn2 = { _ = xx; _ = { _ = xx } }
    fn2()

    return self.init()
  }

  static func testStaticMethodMutableDynamicSelfCaptures() -> Self {
    let fn0 = { _ = self; _ = { _ = self } }
    fn0()

    var x = self
    let fn1 = { _ = x; _ = { _ = x } }
    fn1()

    var xx = (self, self)
    let fn2 = { _ = xx; _ = { _ = xx } }
    fn2()

    return self.init()
  }

  // Make sure the actual self value has the same lowered type as the
  // substituted result of a generic function call
  func testDynamicSelfSubstitution(_ b: Bool) -> Self {
    return b ? self : id(self)
  }

  // Same for metatype of self
  static func testStaticMethodDynamicSelfSubstitution(_ b: Bool) -> Self {
    _ = (b ? self : id(self))
    return self.init()
  }
}

// Unbound reference to a method returning Self.

class Factory {
  required init() {}

  func newInstance() -> Self { return self }
  class func classNewInstance() -> Self { return self.init() }
  static func staticNewInstance() -> Self { return self.init() }
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self22partialApplySelfReturn1c1tyAA7FactoryC_AFmtF : $@convention(thin) (@guaranteed Factory, @thick Factory.Type) -> ()
func partialApplySelfReturn(c: Factory, t: Factory.Type) {
  _ = c.newInstance
  _ = Factory.newInstance

  _ = t.newInstance
  _ = type(of: c).newInstance

  _ = t.classNewInstance
  _ = type(of: c).classNewInstance
  _ = Factory.classNewInstance

  _ = t.staticNewInstance
  _ = type(of: c).staticNewInstance
  _ = Factory.staticNewInstance
}

class FactoryFactory {

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self07FactoryC0C11newInstanceACXDyFZ : $@convention(method) (@thick FactoryFactory.Type) -> @owned FactoryFactory
  static func newInstance() -> Self {
    // CHECK: bb0(%0 : $@thick FactoryFactory.Type):

    // CHECK: [[DYNAMIC_SELF:%.*]] = unchecked_trivial_bit_cast %0 : $@thick FactoryFactory.Type to $@thick @dynamic_self FactoryFactory.Type
    // CHECK: [[METATYPE:%.*]] = value_metatype $@thick @dynamic_self FactoryFactory.Type.Type, [[DYNAMIC_SELF]] : $@thick @dynamic_self FactoryFactory.Type
    // CHECK: [[ANY:%.*]] = init_existential_metatype [[METATYPE]] : $@thick @dynamic_self FactoryFactory.Type.Type, $@thick any Any.Type
    let _: Any.Type = type(of: self)

    while true {}
  }
}

// Test that we downcast to the correct type when invoking 'Self'-returning class members on 'super'.
class Base {
  required init() {}

  func method() -> Self { self }
  var property: Self { self }
  subscript() -> Self { self }

  static func staticMethod() -> Self { self.init() }
  static var staticProperty: Self { self.init() }
  static subscript() -> Self { self.init() }
}

class Derived : Base {
  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self7DerivedC9superCallyyF : $@convention(method) (@guaranteed Derived) -> ()
  // CHECK: convert_function %{{[0-9]+}} : $@callee_guaranteed () -> @owned Base to $@callee_guaranteed () -> @owned Derived
  // CHECK-COUNT-3: unchecked_ref_cast %{{[0-9]+}} : $Base to $Derived
  // CHECK-NOT: unchecked_ref_cast
  // CHECK: end sil function '$s12dynamic_self7DerivedC9superCallyyF'
  func superCall() {
    _ = super.method
    _ = super.method()
    _ = super.property
    _ = super[]
  }

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self7DerivedC15superCallStaticyyFZ : $@convention(method) (@thick Derived.Type) -> ()
  // CHECK: convert_function %{{[0-9]+}} : $@callee_guaranteed () -> @owned Base to $@callee_guaranteed () -> @owned Derived
  // CHECK-COUNT-3: unchecked_ref_cast %{{[0-9]+}} : $Base to $Derived
  // CHECK-NOT: unchecked_ref_cast
  // CHECK: end sil function '$s12dynamic_self7DerivedC15superCallStaticyyFZ'
  static func superCallStatic() {
    _ = super.staticMethod
    _ = super.staticMethod()
    _ = super.staticProperty
    _ = super[]
  }

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self7DerivedC32superCallFromMethodReturningSelfACXDyF : $@convention(method) (@guaranteed Derived) -> @owned Derived
  // CHECK: convert_function %{{[0-9]+}} : $@callee_guaranteed () -> @owned Base to $@callee_guaranteed () -> @owned Derived
  // CHECK-COUNT-3: unchecked_ref_cast %{{[0-9]+}} : $Base to $Derived
  // CHECK-NOT: unchecked_ref_cast
  // CHECK: end sil function '$s12dynamic_self7DerivedC32superCallFromMethodReturningSelfACXDyF'
  func superCallFromMethodReturningSelf() -> Self {
    _ = super.method
    _ = super.method()
    _ = super[]
    return super.property
  }

  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self7DerivedC38superCallFromMethodReturningSelfStaticACXDyFZ : $@convention(method) (@thick Derived.Type) -> @owned Derived
  // CHECK: convert_function %{{[0-9]+}} : $@callee_guaranteed () -> @owned Base to $@callee_guaranteed () -> @owned Derived
  // CHECK-COUNT-3: unchecked_ref_cast %{{[0-9]+}} : $Base to $Derived
  // CHECK-NOT: unchecked_ref_cast
  // CHECK: end sil function '$s12dynamic_self7DerivedC38superCallFromMethodReturningSelfStaticACXDyFZ'
  static func superCallFromMethodReturningSelfStatic() -> Self {
    _ = super.staticMethod
    _ = super.staticMethod()
    _ = super[]
    return super.staticProperty
  }
}

class Generic<T> {
  // Examples where we have to add a special argument to capture Self's metadata
  func t1() -> Self {
    // CHECK-LABEL: sil private [ossa] @$s12dynamic_self7GenericC2t1ACyxGXDyFAEXDSgycfU_ : $@convention(thin) <T> (@guaranteed <τ_0_0> { var @sil_weak Optional<Generic<τ_0_0>> } <T>, @thick @dynamic_self Generic<T>.Type) -> @owned Optional<Generic<T>>
    _ = {[weak self] in self }
    return self
  }

  func t2() -> Self {
    // CHECK-LABEL: sil private [ossa] @$s12dynamic_self7GenericC2t2ACyxGXDyFAEXD_AEXDtycfU_ : $@convention(thin) <T> (@guaranteed (Generic<T>, Generic<T>), @thick @dynamic_self Generic<T>.Type) -> (@owned Generic<T>, @owned Generic<T>)
    let selves = (self, self)
    _ = { selves }
    return self
  }

  func t3() -> Self {
    // CHECK-LABEL: sil private [ossa] @$s12dynamic_self7GenericC2t3ACyxGXDyFAEXDycfU_ : $@convention(thin) <T> (@guaranteed @sil_unowned Generic<T>, @thick @dynamic_self Generic<T>.Type) -> @owned Generic<T> 
    _ = {[unowned self] in self }
    return self
  }
}

protocol SelfReplaceable {
  init<T>(t: T)
}

extension SelfReplaceable {
  init(with fn: (Self.Type) -> Self) {
    self = fn(Self.self)
  }

  init<T>(with genericFn: (Self.Type) -> T) {
    self.init(t: genericFn(Self.self))
  }
}

class SelfReplaceClass : SelfReplaceable {
  let t: Any

  required init<T>(t: T) {
    self.t = t
  }

  convenience init(y: Int) {
    self.init(with: { type in type.init(t: y) })
  }

  convenience init(z: Int) {
    self.init(with: { type in z })
  }
}

public protocol EmptyProtocol { }

public extension EmptyProtocol {
  func run(_: (Self) -> ()) { }
}

func doEscaping<T>(_: @escaping (T) -> ()) {}

public class FunctionConversionTest : EmptyProtocol {
  // CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self22FunctionConversionTestC07convertC0yACXDyypXEF :
  func convertFunction(_ fn: (Any) -> ()) -> Self {
    // The reabstraction thunk captures a self metatype here:
    // CHECK: function_ref @$s12dynamic_self22FunctionConversionTestCIgg_ACIegn_ACXMTTy : $@convention(thin) ({{.*}}, @thick FunctionConversionTest.Type) -> ()
    run(fn)
    return self
  }

  func convertWithoutEscaping(_ fn: (Any.Type) -> ()) -> Self {
    func takesNoEscapeWithSelf(_ _fn: (Self.Type) -> ()) {
      withoutActuallyEscaping(_fn) { __fn in
        doEscaping(__fn)
      }
    }

    takesNoEscapeWithSelf(fn)

    return self
  }
}

public class CaptureTwoValuesTest {
  public required init() {}

  // CHECK-LABEL: sil [ossa] @$s12dynamic_self20CaptureTwoValuesTestC08capturesdE0yyFZ : $@convention(method) (@thick CaptureTwoValuesTest.Type) -> () {
  public static func capturesTwoValues() {
    let a = Self()
    let b = Self()

    // CHECK: function_ref @$s12dynamic_self20CaptureTwoValuesTestC08capturesdE0yyFZyycfU_ : $@convention(thin) (@guaranteed CaptureTwoValuesTest, @guaranteed CaptureTwoValuesTest) -> ()
    _ = {
      _ = a
      _ = b
      _ = Self.self
    }

    // CHECK-LABEL: sil private [ossa] @$s12dynamic_self20CaptureTwoValuesTestC08capturesdE0yyFZyycfU_ : $@convention(thin) (@guaranteed CaptureTwoValuesTest, @guaranteed CaptureTwoValuesTest) -> () {
  }
}


final class Final {
  static func useSelf(_ body: (Self) -> ()) {}
}

// CHECK-LABEL: sil hidden [ossa] @$s12dynamic_self13testNoErasureyyyAA5FinalCXEF :
func testNoErasure(_ body: (Final) -> ()) {
  // CHECK: function_ref @$s12dynamic_self5FinalC7useSelfyyyACXDXEFZ :
  return Final.useSelf(body)
}

// CHECK-LABEL: sil_witness_table hidden X: P module dynamic_self {
// CHECK: method #P.f: {{.*}} : @$s12dynamic_self1XCAA1PA2aDP1f{{[_0-9a-zA-Z]*}}FTW

// CHECK-LABEL: sil_witness_table hidden X: CP module dynamic_self {
// CHECK: method #CP.f: {{.*}} : @$s12dynamic_self1XCAA2CPA2aDP1f{{[_0-9a-zA-Z]*}}FTW
