
// RUN: %target-swift-emit-silgen -module-name closures -parse-stdlib -parse-as-library %s | %FileCheck %s
// RUN: %target-swift-emit-silgen -module-name closures -parse-stdlib -parse-as-library  %s | %FileCheck %s --check-prefix=GUARANTEED

import Swift

var zero = 0

// <rdar://problem/15921334>
// CHECK-LABEL: sil hidden [ossa] @$s8closures46return_local_generic_function_without_captures{{[_0-9a-zA-Z]*}}F : $@convention(thin) <A, R> () -> @owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <A, R> {

func return_local_generic_function_without_captures<A, R>() -> (A) -> R {
  func f(_: A) -> R {
    Builtin.int_trap()
  }
  // CHECK:  [[FN:%.*]] = function_ref @$s8closures46return_local_generic_function_without_captures{{[_0-9a-zA-Z]*}} : $@convention(thin) <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1
  // CHECK:  [[FN_WITH_GENERIC_PARAMS:%.*]] = partial_apply [callee_guaranteed] [[FN]]<A, R>() : $@convention(thin) <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1
  // CHECK:  [[FN_CONV:%.*]] = convert_function [[FN_WITH_GENERIC_PARAMS]]
  // CHECK:  return [[FN_CONV]] :
  return f
}

func return_local_generic_function_with_captures<A, R>(_ a: A) -> (A) -> R {
  func f(_: A) -> R {
    _ = a
  }

  return f
}

// CHECK-LABEL: sil hidden [ossa] @$s8closures17read_only_captureyS2iF : $@convention(thin) (Int) -> Int {
func read_only_capture(_ x: Int) -> Int {
  var x = x
  // CHECK: bb0([[X:%[0-9]+]] : $Int):
  // CHECK:   [[XBOX:%[0-9]+]] = alloc_box ${ var Int }
  // SEMANTIC ARC TODO: This is incorrect. We need to do the project_box on the copy.
  // CHECK:   [[PROJECT:%.*]] = project_box [[XBOX]]
  // CHECK:   store [[X]] to [trivial] [[PROJECT]]

  func cap() -> Int {
    return x
  }

  return cap()
  // CHECK:   [[XBOX_BORROW:%.*]] = begin_borrow [[XBOX]]
  // SEMANTIC ARC TODO: See above. This needs to happen on the copy_valued box.
  // CHECK:   mark_function_escape [[PROJECT]]
  // CHECK:   [[CAP:%[0-9]+]] = function_ref @[[CAP_NAME:\$s8closures17read_only_capture.*]] : $@convention(thin) (@guaranteed { var Int }) -> Int
  // CHECK:   [[RET:%[0-9]+]] = apply [[CAP]]([[XBOX_BORROW]])
  // CHECK:   end_borrow [[XBOX_BORROW]]
  // CHECK:   destroy_value [[XBOX]]
  // CHECK:   return [[RET]]
}
// CHECK:   } // end sil function '$s8closures17read_only_captureyS2iF'

// CHECK: sil private [ossa] @[[CAP_NAME]]
// CHECK: bb0([[XBOX:%[0-9]+]] : @guaranteed ${ var Int }):
// CHECK: [[XADDR:%[0-9]+]] = project_box [[XBOX]]
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[XADDR]] : $*Int
// CHECK: [[X:%[0-9]+]] = load [trivial] [[ACCESS]]
// CHECK: return [[X]]
// } // end sil function '[[CAP_NAME]]'

// SEMANTIC ARC TODO: This is a place where we have again project_box too early.
// CHECK-LABEL: sil hidden [ossa] @$s8closures16write_to_captureyS2iF : $@convention(thin) (Int) -> Int {
func write_to_capture(_ x: Int) -> Int {
  var x = x
  // CHECK: bb0([[X:%[0-9]+]] : $Int):
  // CHECK:   [[XBOX:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK:   [[XBOX_PB:%.*]] = project_box [[XBOX]]
  // CHECK:   store [[X]] to [trivial] [[XBOX_PB]]
  // CHECK:   [[X2BOX:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK:   [[X2BOX_PB:%.*]] = project_box [[X2BOX]]
  // CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[XBOX_PB]] : $*Int
  // CHECK:   copy_addr [[ACCESS]] to [initialization] [[X2BOX_PB]]
  // CHECK:   [[X2BOX_BORROW:%.*]] = begin_borrow [[X2BOX]]
  // SEMANTIC ARC TODO: This next mark_function_escape should be on a projection from X2BOX_BORROW.
  // CHECK:   mark_function_escape [[X2BOX_PB]]
  var x2 = x

  func scribble() {
    x2 = zero
  }

  scribble()
  // CHECK:   [[SCRIB:%[0-9]+]] = function_ref @[[SCRIB_NAME:\$s8closures16write_to_capture.*]] : $@convention(thin) (@guaranteed { var Int }) -> ()
  // CHECK:   apply [[SCRIB]]([[X2BOX_BORROW]])
  // SEMANTIC ARC TODO: This should load from X2BOX_BORROW project. There is an
  // issue here where after a copy_value, we need to reassign a projection in
  // some way.
  // CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[X2BOX_PB]] : $*Int
  // CHECK:   [[RET:%[0-9]+]] = load [trivial] [[ACCESS]]
  // CHECK:   destroy_value [[X2BOX]]
  // CHECK:   destroy_value [[XBOX]]
  // CHECK:   return [[RET]]
  return x2
}
// CHECK:  } // end sil function '$s8closures16write_to_captureyS2iF'

// CHECK: sil private [ossa] @[[SCRIB_NAME]]
// CHECK: bb0([[XBOX:%[0-9]+]] : @guaranteed ${ var Int }):
// CHECK:   [[XADDR:%[0-9]+]] = project_box [[XBOX]]
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [unknown] [[XADDR]] : $*Int
// CHECK:   assign {{%[0-9]+}} to [[ACCESS]]
// CHECK:   return
// CHECK: } // end sil function '[[SCRIB_NAME]]'

// CHECK-LABEL: sil hidden [ossa] @$s8closures21multiple_closure_refs{{[_0-9a-zA-Z]*}}F
func multiple_closure_refs(_ x: Int) -> (() -> Int, () -> Int) {
  var x = x
  func cap() -> Int {
    return x
  }

  return (cap, cap)
  // CHECK: [[CAP:%[0-9]+]] = function_ref @[[CAP_NAME:\$s8closures21multiple_closure_refs.*]] : $@convention(thin) (@guaranteed { var Int }) -> Int
  // CHECK: [[CAP_CLOSURE_1:%[0-9]+]] = partial_apply [callee_guaranteed] [[CAP]]
  // CHECK: [[CAP:%[0-9]+]] = function_ref @[[CAP_NAME:\$s8closures21multiple_closure_refs.*]] : $@convention(thin) (@guaranteed { var Int }) -> Int
  // CHECK: [[CAP_CLOSURE_2:%[0-9]+]] = partial_apply [callee_guaranteed] [[CAP]]
  // CHECK: [[RET:%[0-9]+]] = tuple ([[CAP_CLOSURE_1]] : {{.*}}, [[CAP_CLOSURE_2]] : {{.*}})
  // CHECK: return [[RET]]
}

// CHECK-LABEL: sil hidden [ossa] @$s8closures18capture_local_funcySiycycSiF : $@convention(thin) (Int) -> @owned @callee_guaranteed () -> @owned @callee_guaranteed () -> Int {
func capture_local_func(_ x: Int) -> () -> () -> Int {
  // CHECK: bb0([[ARG:%.*]] : $Int):
  var x = x
  // CHECK:   [[XBOX:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK:   [[XBOX_PB:%.*]] = project_box [[XBOX]]
  // CHECK:   store [[ARG]] to [trivial] [[XBOX_PB]]

  func aleph() -> Int { return x }

  func beth() -> () -> Int { return aleph }
  // CHECK: [[BETH_REF:%.*]] = function_ref @[[BETH_NAME:\$s8closures18capture_local_funcySiycycSiF4bethL_SiycyF]] : $@convention(thin) (@guaranteed { var Int }) -> @owned @callee_guaranteed () -> Int
  // CHECK: [[XBOX_COPY:%.*]] = copy_value [[XBOX]]
  // SEMANTIC ARC TODO: This is incorrect. This should be a project_box from XBOX_COPY.
  // CHECK: mark_function_escape [[XBOX_PB]]
  // CHECK: [[BETH_CLOSURE:%[0-9]+]] = partial_apply [callee_guaranteed] [[BETH_REF]]([[XBOX_COPY]])

  return beth
  // CHECK: destroy_value [[XBOX]]
  // CHECK: return [[BETH_CLOSURE]]
}
// CHECK: } // end sil function '$s8closures18capture_local_funcySiycycSiF'

// CHECK: sil private [ossa] @[[ALEPH_NAME:\$s8closures18capture_local_funcySiycycSiF5alephL_SiyF]] : $@convention(thin) (@guaranteed { var Int }) -> Int {
// CHECK: bb0([[XBOX:%[0-9]+]] : @guaranteed ${ var Int }):

// CHECK: sil private [ossa] @[[BETH_NAME]] : $@convention(thin) (@guaranteed { var Int }) -> @owned @callee_guaranteed () -> Int {
// CHECK: bb0([[XBOX:%[0-9]+]] : @guaranteed ${ var Int }):
// CHECK:   [[XBOX_PB:%.*]] = project_box [[XBOX]]
// CHECK:   [[ALEPH_REF:%[0-9]+]] = function_ref @[[ALEPH_NAME]] : $@convention(thin) (@guaranteed { var Int }) -> Int
// CHECK:   [[XBOX_COPY:%.*]] = copy_value [[XBOX]]
// SEMANTIC ARC TODO: This should be on a PB from XBOX_COPY.
// CHECK:   mark_function_escape [[XBOX_PB]]
// CHECK:   [[ALEPH_CLOSURE:%[0-9]+]] = partial_apply [callee_guaranteed] [[ALEPH_REF]]([[XBOX_COPY]])
// CHECK:   return [[ALEPH_CLOSURE]]
// CHECK: } // end sil function '[[BETH_NAME]]'

// CHECK-LABEL: sil hidden [ossa] @$s8closures22anon_read_only_capture{{[_0-9a-zA-Z]*}}F
func anon_read_only_capture(_ x: Int) -> Int {
  var x = x
  // CHECK: bb0([[X:%[0-9]+]] : $Int):
  // CHECK: [[XBOX:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK: [[PB:%.*]] = project_box [[XBOX]]

  return ({ x })()
  // -- func expression
  // CHECK: [[ANON:%[0-9]+]] = function_ref @[[CLOSURE_NAME:\$s8closures22anon_read_only_capture[_0-9a-zA-Z]*]] : $@convention(thin) (@inout_aliasable Int) -> Int
  // -- apply expression
  // CHECK: [[RET:%[0-9]+]] = apply [[ANON]]([[PB]])
  // -- cleanup
  // CHECK: destroy_value [[XBOX]]
  // CHECK: return [[RET]]
}
// CHECK: sil private [ossa] @[[CLOSURE_NAME]]
// CHECK: bb0([[XADDR:%[0-9]+]] : $*Int):
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[XADDR]] : $*Int
// CHECK: [[X:%[0-9]+]] = load [trivial] [[ACCESS]]
// CHECK: return [[X]]

// CHECK-LABEL: sil hidden [ossa] @$s8closures21small_closure_capture{{[_0-9a-zA-Z]*}}F
func small_closure_capture(_ x: Int) -> Int {
  var x = x
  // CHECK: bb0([[X:%[0-9]+]] : $Int):
  // CHECK: [[XBOX:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK: [[PB:%.*]] = project_box [[XBOX]]

  return { x }()
  // -- func expression
  // CHECK: [[ANON:%[0-9]+]] = function_ref @[[CLOSURE_NAME:\$s8closures21small_closure_capture[_0-9a-zA-Z]*]] : $@convention(thin) (@inout_aliasable Int) -> Int
  // -- apply expression
  // CHECK: [[RET:%[0-9]+]] = apply [[ANON]]([[PB]])
  // -- cleanup
  // CHECK: destroy_value [[XBOX]]
  // CHECK: return [[RET]]
}
// CHECK: sil private [ossa] @[[CLOSURE_NAME]]
// CHECK: bb0([[XADDR:%[0-9]+]] : $*Int):
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[XADDR]] : $*Int
// CHECK: [[X:%[0-9]+]] = load [trivial] [[ACCESS]]
// CHECK: return [[X]]


// CHECK-LABEL: sil hidden [ossa] @$s8closures35small_closure_capture_with_argument{{[_0-9a-zA-Z]*}}F
func small_closure_capture_with_argument(_ x: Int) -> (_ y: Int) -> Int {
  var x = x
  // CHECK: [[XBOX:%[0-9]+]] = alloc_box ${ var Int }

  return { x + $0 }
  // -- func expression
  // CHECK: [[ANON:%[0-9]+]] = function_ref @[[CLOSURE_NAME:\$s8closures35small_closure_capture_with_argument.*]] : $@convention(thin) (Int, @guaranteed { var Int }) -> Int
  // CHECK: [[XBOX_COPY:%.*]] = copy_value [[XBOX]]
  // CHECK: [[ANON_CLOSURE_APP:%[0-9]+]] = partial_apply [callee_guaranteed] [[ANON]]([[XBOX_COPY]])
  // -- return
  // CHECK: destroy_value [[XBOX]]
  // CHECK: return [[ANON_CLOSURE_APP]]
}
// CHECK: sil private [ossa] @[[CLOSURE_NAME]] : $@convention(thin) (Int, @guaranteed { var Int }) -> Int
// CHECK: bb0([[DOLLAR0:%[0-9]+]] : $Int, [[XBOX:%[0-9]+]] : @guaranteed ${ var Int }):
// CHECK: [[XADDR:%[0-9]+]] = project_box [[XBOX]]
// CHECK: [[INTTYPE:%[0-9]+]] = metatype $@thin Int.Type
// CHECK: [[XACCESS:%[0-9]+]] = begin_access [read] [unknown] [[XADDR]] : $*Int
// CHECK: [[LHS:%[0-9]+]] = load [trivial] [[XACCESS]]
// CHECK: end_access [[XACCESS]] : $*Int
// CHECK: [[PLUS:%[0-9]+]] = function_ref @$sSi1poiyS2i_SitFZ{{.*}}
// CHECK: [[RET:%[0-9]+]] = apply [[PLUS]]([[LHS]], [[DOLLAR0]], [[INTTYPE]])
// CHECK: return [[RET]]

// CHECK-LABEL: sil hidden [ossa] @$s8closures24small_closure_no_capture{{[_0-9a-zA-Z]*}}F
func small_closure_no_capture() -> (_ y: Int) -> Int {
  // CHECK:   [[ANON:%[0-9]+]] = function_ref @[[CLOSURE_NAME:\$s8closures24small_closure_no_captureS2icyFS2icfU_]] : $@convention(thin) (Int) -> Int
  // CHECK:   [[ANON_THICK:%[0-9]+]] = thin_to_thick_function [[ANON]] : ${{.*}} to $@callee_guaranteed (Int) -> Int
  // CHECK:   return [[ANON_THICK]]
  return { $0 }
}
// CHECK: sil private [ossa] @[[CLOSURE_NAME]] : $@convention(thin) (Int) -> Int
// CHECK: bb0([[YARG:%[0-9]+]] : $Int):

// CHECK-LABEL: sil hidden [ossa] @$s8closures17uncaptured_locals{{[_0-9a-zA-Z]*}}F :
func uncaptured_locals(_ x: Int) -> (Int, Int) {
  var x = x
  // -- locals without captures are stack-allocated
  // CHECK: bb0([[XARG:%[0-9]+]] : $Int):
  // CHECK:   [[XADDR:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK:   [[PB:%.*]] = project_box [[XADDR]]
  // CHECK:   store [[XARG]] to [trivial] [[PB]]

  var y = zero
  // CHECK:   [[YADDR:%[0-9]+]] = alloc_box ${ var Int }
  return (x, y)

}

class SomeClass {
  var x : Int = zero

  init() {
    x = { self.x }()   // Closing over self.
  }
}

// Closures within destructors <rdar://problem/15883734>
class SomeGenericClass<T> {
  deinit {
    var i: Int = zero
    // CHECK: [[C1REF:%[0-9]+]] = function_ref @$s8closures16SomeGenericClassCfdSiyXEfU_ : $@convention(thin) (@inout_aliasable Int) -> Int
    // CHECK: apply [[C1REF]]([[IBOX:%[0-9]+]]) : $@convention(thin) (@inout_aliasable Int) -> Int
    var x = { i + zero } ()

    // CHECK: [[C2REF:%[0-9]+]] = function_ref @$s8closures16SomeGenericClassCfdSiyXEfU0_ : $@convention(thin) () -> Int
    // CHECK: apply [[C2REF]]() : $@convention(thin) () -> Int
    var y = { zero } ()

    // CHECK: [[C3REF:%[0-9]+]] = function_ref @$s8closures16SomeGenericClassCfdyyXEfU1_ : $@convention(thin) <τ_0_0> () -> ()
    // CHECK: apply [[C3REF]]<T>() : $@convention(thin) <τ_0_0> () -> ()
    var z = { _ = T.self } ()
  }

  // CHECK-LABEL: sil private [ossa] @$s8closures16SomeGenericClassCfdSiyXEfU_ : $@convention(thin) (@inout_aliasable Int) -> Int

  // CHECK-LABEL: sil private [ossa] @$s8closures16SomeGenericClassCfdSiyXEfU0_ : $@convention(thin) () -> Int

  // CHECK-LABEL: sil private [ossa] @$s8closures16SomeGenericClassCfdyyXEfU1_ : $@convention(thin) <T> () -> ()
}

// This is basically testing that the constraint system ranking
// function conversions as worse than others, and therefore performs
// the conversion within closures when possible.
class SomeSpecificClass : SomeClass {}
func takesSomeClassGenerator(_ fn : () -> SomeClass) {}
func generateWithConstant(_ x : SomeSpecificClass) {
  takesSomeClassGenerator({ x })
}

// CHECK-LABEL: sil private [ossa] @$s8closures20generateWithConstantyyAA17SomeSpecificClassCFAA0eG0CyXEfU_ : $@convention(thin) (@guaranteed SomeSpecificClass) -> @owned SomeClass {
// CHECK: bb0([[T0:%.*]] : @guaranteed $SomeSpecificClass):
// CHECK:   debug_value [[T0]] : $SomeSpecificClass, let, name "x", argno 1
// CHECK:   [[T0_COPY:%.*]] = copy_value [[T0]]
// CHECK:   [[T0_COPY_CASTED:%.*]] = upcast [[T0_COPY]] : $SomeSpecificClass to $SomeClass
// CHECK:   return [[T0_COPY_CASTED]]
// CHECK: } // end sil function '$s8closures20generateWithConstantyyAA17SomeSpecificClassCFAA0eG0CyXEfU_'


// Check the annoying case of capturing 'self' in a derived class 'init'
// method. We allocate a mutable box to deal with 'self' potentially being
// rebound by super.init, but 'self' is formally immutable and so is captured
// by value. <rdar://problem/15599464>
class Base {}

class SelfCapturedInInit : Base {
  var foo : () -> SelfCapturedInInit

  // CHECK-LABEL: sil hidden [ossa] @$s8closures18SelfCapturedInInitC{{[_0-9a-zA-Z]*}}fc : $@convention(method) (@owned SelfCapturedInInit) -> @owned SelfCapturedInInit {
  // CHECK: bb0([[SELF:%.*]] : @owned $SelfCapturedInInit):
  //
  // First create our initial value for self.
  // CHECK:   [[SELF_BOX:%.*]] = alloc_box ${ var SelfCapturedInInit }, let, name "self"
  // CHECK:   [[UNINIT_SELF:%.*]] = mark_uninitialized [derivedself] [[SELF_BOX]]
  // CHECK:   [[PB_SELF_BOX:%.*]] = project_box [[UNINIT_SELF]]
  // CHECK:   store [[SELF]] to [init] [[PB_SELF_BOX]]
  //
  // Then perform the super init sequence.
  // CHECK:   [[TAKEN_SELF:%.*]] = load [take] [[PB_SELF_BOX]]
  // CHECK:   [[UPCAST_TAKEN_SELF:%.*]] = upcast [[TAKEN_SELF]]
  // CHECK:   [[NEW_SELF:%.*]] = apply {{.*}}([[UPCAST_TAKEN_SELF]]) : $@convention(method) (@owned Base) -> @owned Base
  // CHECK:   [[DOWNCAST_NEW_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]] : $Base to $SelfCapturedInInit
  // CHECK:   store [[DOWNCAST_NEW_SELF]] to [init] [[PB_SELF_BOX]]
  //
  // Finally put self in the closure.
  // CHECK:   [[BORROWED_SELF:%.*]] = load_borrow [[PB_SELF_BOX]]
  // CHECK:   [[COPIED_SELF:%.*]] = load [copy] [[PB_SELF_BOX]]
  // CHECK:   [[FOO_VALUE:%.*]] = partial_apply [callee_guaranteed] {{%.*}}([[COPIED_SELF]]) : $@convention(thin) (@guaranteed SelfCapturedInInit) -> @owned SelfCapturedInInit
  // CHECK:   [[FOO_LOCATION:%.*]] = ref_element_addr [[BORROWED_SELF]]
  // CHECK:   [[ACCESS:%.*]] = begin_access [modify] [dynamic] [[FOO_LOCATION]] : $*@callee_guaranteed () -> @owned SelfCapturedInInit
  // CHECK:   assign [[FOO_VALUE]] to [[ACCESS]]
  override init() {
    super.init()
    foo = { self }
  }
}

func takeClosure(_ fn: () -> Int) -> Int { return fn() }

class TestCaptureList {
  var x = zero

  func testUnowned() {
    let aLet = self
    takeClosure { aLet.x }
    takeClosure { [unowned aLet] in aLet.x }
    takeClosure { [weak aLet] in aLet!.x }

    var aVar = self
    takeClosure { aVar.x }
    takeClosure { [unowned aVar] in aVar.x }
    takeClosure { [weak aVar] in aVar!.x }

    takeClosure { self.x }
    takeClosure { [unowned self] in self.x }
    takeClosure { [weak self] in self!.x }

    takeClosure { [unowned newVal = TestCaptureList()] in newVal.x }
    takeClosure { [weak newVal = TestCaptureList()] in newVal!.x }
  }
}

class ClassWithIntProperty { final var x = 42 }

func closeOverLetLValue() {
  let a : ClassWithIntProperty
  a = ClassWithIntProperty()
  
  takeClosure { a.x }
}

// The let property needs to be captured into a temporary stack slot so that it
// is loadable even though we capture the value.
// CHECK-LABEL: sil private [ossa] @$s8closures18closeOverLetLValueyyFSiyXEfU_ : $@convention(thin) (@guaranteed ClassWithIntProperty) -> Int {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $ClassWithIntProperty):
// CHECK:   [[TMP_CLASS_ADDR:%.*]] = alloc_stack $ClassWithIntProperty, let, name "a", argno 1
// CHECK:   [[COPY_ARG:%.*]] = copy_value [[ARG]]
// CHECK:   store [[COPY_ARG]] to [init] [[TMP_CLASS_ADDR]] : $*ClassWithIntProperty
// CHECK:   [[BORROWED_LOADED_CLASS:%.*]] = load_borrow [[TMP_CLASS_ADDR]] : $*ClassWithIntProperty
// CHECK:   [[INT_IN_CLASS_ADDR:%.*]] = ref_element_addr [[BORROWED_LOADED_CLASS]] : $ClassWithIntProperty, #ClassWithIntProperty.x
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [dynamic] [[INT_IN_CLASS_ADDR]] : $*Int
// CHECK:   [[INT_IN_CLASS:%.*]] = load [trivial] [[ACCESS]] : $*Int
// CHECK:   end_borrow [[BORROWED_LOADED_CLASS]]
// CHECK:   destroy_addr [[TMP_CLASS_ADDR]] : $*ClassWithIntProperty
// CHECK:   dealloc_stack %1 : $*ClassWithIntProperty
// CHECK:   return [[INT_IN_CLASS]]
// CHECK: } // end sil function '$s8closures18closeOverLetLValueyyFSiyXEfU_'

// GUARANTEED-LABEL: sil private [ossa] @$s8closures18closeOverLetLValueyyFSiyXEfU_ : $@convention(thin) (@guaranteed ClassWithIntProperty) -> Int {
// GUARANTEED: bb0(%0 : @guaranteed $ClassWithIntProperty):
// GUARANTEED:   [[TMP:%.*]] = alloc_stack $ClassWithIntProperty
// GUARANTEED:   [[COPY:%.*]] = copy_value %0 : $ClassWithIntProperty
// GUARANTEED:   store [[COPY]] to [init] [[TMP]] : $*ClassWithIntProperty
// GUARANTEED:   [[BORROWED:%.*]] = load_borrow [[TMP]]
// GUARANTEED:   end_borrow [[BORROWED]]
// GUARANTEED:   destroy_addr [[TMP]]
// GUARANTEED: } // end sil function '$s8closures18closeOverLetLValueyyFSiyXEfU_'


// Use an external function so inout deshadowing cannot see its body.
@_silgen_name("takesNoEscapeClosure")
func takesNoEscapeClosure(fn : () -> Int)

struct StructWithMutatingMethod {
  var x = 42

  mutating func mutatingMethod() {
    // This should not capture the refcount of the self shadow.
    takesNoEscapeClosure { x = 42; return x }
  }
}

// Check that the address of self is passed in, but not the refcount pointer.

// CHECK-LABEL: sil hidden [ossa] @$s8closures24StructWithMutatingMethodV08mutatingE0{{[_0-9a-zA-Z]*}}F
// CHECK: bb0(%0 : $*StructWithMutatingMethod):
// CHECK: [[CLOSURE:%[0-9]+]] = function_ref @$s8closures24StructWithMutatingMethodV08mutatingE0{{.*}} : $@convention(thin) (@inout_aliasable StructWithMutatingMethod) -> Int
// CHECK: partial_apply [callee_guaranteed] [[CLOSURE]](%0) : $@convention(thin) (@inout_aliasable StructWithMutatingMethod) -> Int

// Check that the closure body only takes the pointer.
// CHECK-LABEL: sil private [ossa] @$s8closures24StructWithMutatingMethodV08mutatingE0{{.*}} : $@convention(thin) (@inout_aliasable StructWithMutatingMethod) -> Int {
// CHECK:       bb0(%0 : $*StructWithMutatingMethod):

class SuperBase {
  func boom() {}
}
class SuperSub : SuperBase {
  override func boom() {}

  // CHECK-LABEL: sil hidden [ossa] @$s8closures8SuperSubC1ayyF : $@convention(method) (@guaranteed SuperSub) -> () {
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SuperSub):
  // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_1:\$s8closures8SuperSubC1a[_0-9a-zA-Z]*]] : $@convention(thin) (@guaranteed SuperSub) -> ()
  // CHECK:   apply [[INNER]]([[SELF]])
  // CHECK: } // end sil function '$s8closures8SuperSubC1ayyF'
  func a() {
    // CHECK: sil private [ossa] @[[INNER_FUNC_1]] : $@convention(thin) (@guaranteed SuperSub) -> () {
    // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
    // CHECK:   [[CLASS_METHOD:%.*]] = class_method [[ARG]] : $SuperSub, #SuperSub.boom :
    // CHECK:   = apply [[CLASS_METHOD]]([[ARG]])
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
    // CHECK:   [[ARG_COPY_SUPER:%.*]] = upcast [[ARG_COPY]] : $SuperSub to $SuperBase
    // CHECK:   [[SUPER_METHOD:%[0-9]+]] = function_ref @$s8closures9SuperBaseC4boomyyF : $@convention(method) (@guaranteed SuperBase) -> ()
    // CHECK:   = apply [[SUPER_METHOD]]([[ARG_COPY_SUPER]])
    // CHECK:   destroy_value [[ARG_COPY_SUPER]]
    // CHECK: } // end sil function '[[INNER_FUNC_1]]'
    func a1() {
      self.boom()
      super.boom()
    }
    a1()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s8closures8SuperSubC1byyF : $@convention(method) (@guaranteed SuperSub) -> () {
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SuperSub):
  // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_1:\$s8closures8SuperSubC1b[_0-9a-zA-Z]*]] : $@convention(thin) (@guaranteed SuperSub) -> ()
  // CHECK:   = apply [[INNER]]([[SELF]])
  // CHECK: } // end sil function '$s8closures8SuperSubC1byyF'
  func b() {
    // CHECK: sil private [ossa] @[[INNER_FUNC_1]] : $@convention(thin) (@guaranteed SuperSub) -> () {
    // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
    // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_2:\$s8closures8SuperSubC1b.*]] : $@convention(thin) (@guaranteed SuperSub) -> ()
    // CHECK:   = apply [[INNER]]([[ARG]])
    // CHECK: } // end sil function '[[INNER_FUNC_1]]'
    func b1() {
      // CHECK: sil private [ossa] @[[INNER_FUNC_2]] : $@convention(thin) (@guaranteed SuperSub) -> () {
      // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
      // CHECK:   [[CLASS_METHOD:%.*]] = class_method [[ARG]] : $SuperSub, #SuperSub.boom :
      // CHECK:   = apply [[CLASS_METHOD]]([[ARG]]) : $@convention(method) (@guaranteed SuperSub) -> ()
      // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
      // CHECK:   [[ARG_COPY_SUPER:%.*]] = upcast [[ARG_COPY]] : $SuperSub to $SuperBase
      // CHECK:   [[SUPER_METHOD:%.*]] = function_ref @$s8closures9SuperBaseC4boomyyF : $@convention(method) (@guaranteed SuperBase) -> ()
      // CHECK:   = apply [[SUPER_METHOD]]([[ARG_COPY_SUPER]]) : $@convention(method) (@guaranteed SuperBase)
      // CHECK:   destroy_value [[ARG_COPY_SUPER]]
      // CHECK: } // end sil function '[[INNER_FUNC_2]]'
      func b2() {
        self.boom()
        super.boom()
      }
      b2()
    }
    b1()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s8closures8SuperSubC1cyyF : $@convention(method) (@guaranteed SuperSub) -> () {
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SuperSub):
  // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_1:\$s8closures8SuperSubC1c[_0-9a-zA-Z]*]] : $@convention(thin) (@guaranteed SuperSub) -> ()
  // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK:   [[PA:%.*]] = partial_apply [callee_guaranteed] [[INNER]]([[SELF_COPY]])
  // CHECK:   [[BORROWED_PA:%.*]] = begin_borrow [[PA]]
  // CHECK:   [[PA_COPY:%.*]] = copy_value [[BORROWED_PA]]
  // CHECK:   [[B:%.*]] = begin_borrow [[PA_COPY]]
  // CHECK:   apply [[B]]()
	// CHECK:   end_borrow [[B]]
  // CHECK:   destroy_value [[PA_COPY]]
  // CHECK:   end_borrow [[BORROWED_PA]]
  // CHECK:   destroy_value [[PA]]
  // CHECK: } // end sil function '$s8closures8SuperSubC1cyyF'
  func c() {
    // CHECK: sil private [ossa] @[[INNER_FUNC_1]] : $@convention(thin) (@guaranteed SuperSub) -> ()
    // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
    // CHECK:   [[CLASS_METHOD:%.*]] = class_method [[ARG]] : $SuperSub, #SuperSub.boom :
    // CHECK:   = apply [[CLASS_METHOD]]([[ARG]]) : $@convention(method) (@guaranteed SuperSub) -> ()
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
    // CHECK:   [[ARG_COPY_SUPER:%.*]] = upcast [[ARG_COPY]] : $SuperSub to $SuperBase
    // CHECK:   [[SUPER_METHOD:%[0-9]+]] = function_ref @$s8closures9SuperBaseC4boomyyF : $@convention(method) (@guaranteed SuperBase) -> ()
    // CHECK:   = apply [[SUPER_METHOD]]([[ARG_COPY_SUPER]])
    // CHECK:   destroy_value [[ARG_COPY_SUPER]]
    // CHECK: } // end sil function '[[INNER_FUNC_1]]'
    let c1 = { () -> Void in
      self.boom()
      super.boom()
    }
    c1()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s8closures8SuperSubC1dyyF : $@convention(method) (@guaranteed SuperSub) -> () {
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SuperSub):
  // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_1:\$s8closures8SuperSubC1d[_0-9a-zA-Z]*]] : $@convention(thin) (@guaranteed SuperSub) -> ()
  // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK:   [[PA:%.*]] = partial_apply [callee_guaranteed] [[INNER]]([[SELF_COPY]])
  // CHECK:   [[BORROWED_PA:%.*]] = begin_borrow [[PA]]
  // CHECK:   [[PA_COPY:%.*]] = copy_value [[BORROWED_PA]]
  // CHECK:   [[B:%.*]] = begin_borrow [[PA_COPY]]
  // CHECK:   apply [[B]]()
  // CHECK:   end_borrow [[B]]
  // CHECK:   destroy_value [[PA_COPY]]
  // CHECK:   end_borrow [[BORROWED_PA]]
  // CHECK:   destroy_value [[PA]]
  // CHECK: } // end sil function '$s8closures8SuperSubC1dyyF'
  func d() {
    // CHECK: sil private [ossa] @[[INNER_FUNC_1]] : $@convention(thin) (@guaranteed SuperSub) -> () {
    // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
    // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_2:\$s8closures8SuperSubC1d.*]] : $@convention(thin) (@guaranteed SuperSub) -> ()
    // CHECK:   = apply [[INNER]]([[ARG]])
    // CHECK: } // end sil function '[[INNER_FUNC_1]]'
    let d1 = { () -> Void in
      // CHECK: sil private [ossa] @[[INNER_FUNC_2]] : $@convention(thin) (@guaranteed SuperSub) -> () {
      // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
      // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
      // CHECK:   [[ARG_COPY_SUPER:%.*]] = upcast [[ARG_COPY]] : $SuperSub to $SuperBase
      // CHECK:   [[SUPER_METHOD:%.*]] = function_ref @$s8closures9SuperBaseC4boomyyF : $@convention(method) (@guaranteed SuperBase) -> ()
      // CHECK:   = apply [[SUPER_METHOD]]([[ARG_COPY_SUPER]])
      // CHECK:   destroy_value [[ARG_COPY_SUPER]]
      // CHECK: } // end sil function '[[INNER_FUNC_2]]'
      func d2() {
        super.boom()
      }
      d2()
    }
    d1()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s8closures8SuperSubC1eyyF : $@convention(method) (@guaranteed SuperSub) -> () {
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SuperSub):
  // CHECK: [[INNER:%.*]] = function_ref @[[INNER_FUNC_NAME1:\$s8closures8SuperSubC1e[_0-9a-zA-Z]*]] : $@convention(thin)
  // CHECK: = apply [[INNER]]([[SELF]])
  // CHECK: } // end sil function '$s8closures8SuperSubC1eyyF'
  func e() {
    // CHECK: sil private [ossa] @[[INNER_FUNC_NAME1]] : $@convention(thin)
    // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
    // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_NAME2:\$s8closures8SuperSubC1e.*]] : $@convention(thin)
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
    // CHECK:   [[PA:%.*]] = partial_apply [callee_guaranteed] [[INNER]]([[ARG_COPY]])
    // CHECK:   [[BORROWED_PA:%.*]] = begin_borrow [[PA]]
    // CHECK:   [[PA_COPY:%.*]] = copy_value [[BORROWED_PA]]
    // CHECK:   [[B:%.*]] = begin_borrow [[PA_COPY]]
    // CHECK:   apply [[B]]() : $@callee_guaranteed () -> ()
    // CHECK:   end_borrow [[B]]
    // CHECK:   destroy_value [[PA_COPY]]
    // CHECK:   end_borrow [[BORROWED_PA]]
    // CHECK:   destroy_value [[PA]]
    // CHECK: } // end sil function '[[INNER_FUNC_NAME1]]'
    func e1() {
      // CHECK: sil private [ossa] @[[INNER_FUNC_NAME2]] : $@convention(thin)
      // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
      // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
      // CHECK:   [[ARG_COPY_SUPERCAST:%.*]] = upcast [[ARG_COPY]] : $SuperSub to $SuperBase
      // CHECK:   [[SUPER_METHOD:%.*]] = function_ref @$s8closures9SuperBaseC4boomyyF : $@convention(method) (@guaranteed SuperBase) -> ()
      // CHECK:   = apply [[SUPER_METHOD]]([[ARG_COPY_SUPERCAST]])
      // CHECK:   destroy_value [[ARG_COPY_SUPERCAST]]
      // CHECK:   return
      // CHECK: } // end sil function '[[INNER_FUNC_NAME2]]'
      let e2 = {
        super.boom()
      }
      e2()
    }
    e1()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s8closures8SuperSubC1fyyF : $@convention(method) (@guaranteed SuperSub) -> () {
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SuperSub):
  // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_1:\$s8closures8SuperSubC1fyyFyycfU_]] : $@convention(thin) (@guaranteed SuperSub) -> ()
  // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK:   [[PA:%.*]] = partial_apply [callee_guaranteed] [[INNER]]([[SELF_COPY]])
  // CHECK:   destroy_value [[PA]]
  // CHECK: } // end sil function '$s8closures8SuperSubC1fyyF'
  func f() {
    // CHECK: sil private [ossa] @[[INNER_FUNC_1]] : $@convention(thin) (@guaranteed SuperSub) -> () {
    // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
    // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_2:\$s8closures8SuperSubC1fyyFyycfU_yyYaKXEfu_]] : $@convention(thin) @async (@guaranteed SuperSub) -> @error Error
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
    // CHECK:   [[PA:%.*]] = partial_apply [callee_guaranteed] [[INNER]]([[ARG_COPY]])
    // CHECK:   [[CVT:%.*]] = convert_escape_to_noescape [not_guaranteed] [[PA]]
    // CHECK:   [[REABSTRACT_PA:%.*]] = partial_apply [callee_guaranteed] {{.*}}([[CVT]])
    // CHECK:   [[REABSTRACT_CVF:%.*]] = convert_function [[REABSTRACT_PA]]
    // CHECK:   [[REABSTRACT_CVT:%.*]] = convert_escape_to_noescape [not_guaranteed] [[REABSTRACT_CVF]]
    // CHECK:   [[TRY_APPLY_AUTOCLOSURE:%.*]] = function_ref @$ss2qqoiyxxSg_xyYaKXKtYaKlF :
    // CHECK:   try_apply [noasync] [[TRY_APPLY_AUTOCLOSURE]]<()>({{.*}}, {{.*}}, [[REABSTRACT_CVT]]) : {{.*}}, normal [[NORMAL_BB:bb1]], error [[ERROR_BB:bb2]]
    // CHECK: [[NORMAL_BB]]{{.*}}
    // CHECK: } // end sil function '[[INNER_FUNC_1]]'
    let f1 = {
      // CHECK: sil private [transparent] [ossa] @[[INNER_FUNC_2]]
      // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
      // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
      // CHECK:   [[ARG_COPY_SUPER:%.*]] = upcast [[ARG_COPY]] : $SuperSub to $SuperBase
      // CHECK:   [[SUPER_METHOD:%.*]] = function_ref @$s8closures9SuperBaseC4boomyyF : $@convention(method) (@guaranteed SuperBase) -> ()
      // CHECK:   = apply [[SUPER_METHOD]]([[ARG_COPY_SUPER]]) : $@convention(method) (@guaranteed SuperBase) -> ()
      // CHECK:   destroy_value [[ARG_COPY_SUPER]]
      // CHECK: } // end sil function '[[INNER_FUNC_2]]'
      nil ?? super.boom()
    }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s8closures8SuperSubC1gyyF : $@convention(method) (@guaranteed SuperSub) -> () {
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SuperSub):
  // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_1:\$s8closures8SuperSubC1g[_0-9a-zA-Z]*]] : $@convention(thin) (@guaranteed SuperSub) -> ()
  // CHECK:   = apply [[INNER]]([[SELF]])
  // CHECK: } // end sil function '$s8closures8SuperSubC1gyyF'
  func g() {
    // CHECK: sil private [ossa] @[[INNER_FUNC_1]] : $@convention(thin) (@guaranteed SuperSub) -> ()
    // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
    // CHECK:   [[INNER:%.*]] = function_ref @[[INNER_FUNC_2:\$s8closures8SuperSubC1g.*]] : $@convention(thin) @async (@guaranteed SuperSub) -> @error Error
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
    // CHECK:   [[PA:%.*]] = partial_apply [callee_guaranteed] [[INNER]]([[ARG_COPY]])
    // CHECK:   [[CVT:%.*]] = convert_escape_to_noescape [not_guaranteed] [[PA]] : $@async @callee_guaranteed () -> @error Error to $@noescape @async @callee_guaranteed () -> @error Error
    // CHECK:   [[REABSTRACT_PA:%.*]] = partial_apply [callee_guaranteed] {{%.*}}([[CVT]]) : $@convention(thin) @async (@noescape @async @callee_guaranteed () -> @error Error) -> (@out (), @error Error)
    // CHECK:   [[REABSTRACT_CVF:%.*]] = convert_function [[REABSTRACT_PA]]
    // CHECK:   [[REABSTRACT_CVT:%.*]] = convert_escape_to_noescape [not_guaranteed] [[REABSTRACT_CVF]]
    // CHECK:   [[TRY_APPLY_FUNC:%.*]] = function_ref @$ss2qqoiyxxSg_xyYaKXKtYaKlF :
    // CHECK:   try_apply [noasync] [[TRY_APPLY_FUNC]]<()>({{.*}}, {{.*}}, [[REABSTRACT_CVT]]) : {{.*}}, normal [[NORMAL_BB:bb1]], error [[ERROR_BB:bb2]]
    // CHECK: [[NORMAL_BB]]{{.*}}
    // CHECK: } // end sil function '[[INNER_FUNC_1]]'
    func g1() {
      // CHECK: sil private [transparent] [ossa] @[[INNER_FUNC_2]] : $@convention(thin) @async (@guaranteed SuperSub) -> @error Error {
      // CHECK: bb0([[ARG:%.*]] : @guaranteed $SuperSub):
      // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
      // CHECK:   [[ARG_COPY_SUPER:%.*]] = upcast [[ARG_COPY]] : $SuperSub to $SuperBase
      // CHECK:   [[SUPER_METHOD:%.*]] = function_ref @$s8closures9SuperBaseC4boomyyF : $@convention(method) (@guaranteed SuperBase) -> ()
      // CHECK:   = apply [[SUPER_METHOD]]([[ARG_COPY_SUPER]])
      // CHECK:   destroy_value [[ARG_COPY_SUPER]]
      // CHECK: } // end sil function '[[INNER_FUNC_2]]'
      nil ?? super.boom()
    }
    g1()
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s8closures24UnownedSelfNestedCaptureC06nestedE0{{[_0-9a-zA-Z]*}}F : $@convention(method) (@guaranteed UnownedSelfNestedCapture) -> ()
// -- We enter with an assumed strong +1.
// CHECK:  bb0([[SELF:%.*]] : @guaranteed $UnownedSelfNestedCapture):
// CHECK:         [[OUTER_SELF_CAPTURE:%.*]] = alloc_box ${ var @sil_unowned UnownedSelfNestedCapture }
// CHECK:         [[PB:%.*]] = project_box [[OUTER_SELF_CAPTURE]]
// -- strong +2
// CHECK:         [[SELF_COPY:%.*]] = copy_value [[SELF]]
// CHECK:         [[UNOWNED_SELF:%.*]] = ref_to_unowned [[SELF_COPY]] :
// -- TODO: A lot of fussy r/r traffic and owned/unowned conversions here.
// -- strong +2, unowned +1
// CHECK:         [[UNOWNED_SELF_COPY:%.*]] = copy_value [[UNOWNED_SELF]]
// CHECK:         store [[UNOWNED_SELF_COPY]] to [init] [[PB]]
// SEMANTIC ARC TODO: This destroy_value should probably be /after/ the load from PB on the next line.
// CHECK:         destroy_value [[SELF_COPY]]
// CHECK:         [[UNOWNED_SELF:%.*]] = load_borrow [[PB]]
// -- strong +2, unowned +1
// CHECK:         [[SELF:%.*]] = strong_copy_unowned_value [[UNOWNED_SELF]]
// CHECK:         end_borrow [[UNOWNED_SELF]]
// CHECK:         [[UNOWNED_SELF2:%.*]] = ref_to_unowned [[SELF]]
// -- strong +2, unowned +2
// CHECK:         [[UNOWNED_SELF2_COPY:%.*]] = copy_value [[UNOWNED_SELF2]]
// -- strong +1, unowned +2
// CHECK:         destroy_value [[SELF]]
// -- closure takes unowned ownership
// CHECK:         [[OUTER_CLOSURE:%.*]] = partial_apply [callee_guaranteed] {{%.*}}([[UNOWNED_SELF2_COPY]])
// CHECK:         [[OUTER_CONVERT:%.*]] = convert_escape_to_noescape [not_guaranteed] [[OUTER_CLOSURE]]
// -- call consumes closure
// -- strong +1, unowned +1
// CHECK:         [[INNER_CLOSURE:%.*]] = apply [[OUTER_CONVERT]]
// CHECK:         [[B:%.*]] = begin_borrow [[INNER_CLOSURE]]
// CHECK:         [[CONSUMED_RESULT:%.*]] = apply [[B]]()
// CHECK:         destroy_value [[CONSUMED_RESULT]]
// CHECK:         destroy_value [[INNER_CLOSURE]]
// -- destroy_values unowned self in box
// -- strong +1, unowned +0
// CHECK:         destroy_value [[OUTER_SELF_CAPTURE]]
// -- strong +0, unowned +0
// CHECK: } // end sil function '$s8closures24UnownedSelfNestedCaptureC06nestedE0{{[_0-9a-zA-Z]*}}F'

// -- outer closure
// -- strong +0, unowned +1
// CHECK: sil private [ossa] @[[OUTER_CLOSURE_FUN:\$s8closures24UnownedSelfNestedCaptureC06nestedE0yyFACycyXEfU_]] : $@convention(thin) (@guaranteed @sil_unowned UnownedSelfNestedCapture) -> @owned @callee_guaranteed () -> @owned UnownedSelfNestedCapture {
// CHECK: bb0([[CAPTURED_SELF:%.*]] : @guaranteed $@sil_unowned UnownedSelfNestedCapture):
// -- strong +0, unowned +2
// CHECK:         [[CAPTURED_SELF_COPY:%.*]] = copy_value [[CAPTURED_SELF]] :
// -- closure takes ownership of unowned ref
// CHECK:         [[INNER_CLOSURE:%.*]] = partial_apply [callee_guaranteed] {{%.*}}([[CAPTURED_SELF_COPY]])
// -- strong +0, unowned +1 (claimed by closure)
// CHECK:         return [[INNER_CLOSURE]]
// CHECK: } // end sil function '[[OUTER_CLOSURE_FUN]]'

// -- inner closure
// -- strong +0, unowned +1
// CHECK: sil private [ossa] @[[INNER_CLOSURE_FUN:\$s8closures24UnownedSelfNestedCaptureC06nestedE0yyFACycyXEfU_ACycfU_]] : $@convention(thin) (@guaranteed @sil_unowned UnownedSelfNestedCapture) -> @owned UnownedSelfNestedCapture {
// CHECK: bb0([[CAPTURED_SELF:%.*]] : @guaranteed $@sil_unowned UnownedSelfNestedCapture):
// -- strong +1, unowned +1
// CHECK:         [[SELF:%.*]] = strong_copy_unowned_value [[CAPTURED_SELF:%.*]] :
// -- strong +1, unowned +0 (claimed by return)
// CHECK:         return [[SELF]]
// CHECK: } // end sil function '[[INNER_CLOSURE_FUN]]'
class UnownedSelfNestedCapture {
  func nestedCapture() {
    {[unowned self] in { self } }()()
  }
}

// Check that capturing 'self' via a 'super' call also captures the generic
// signature if the base class is concrete and the derived class is generic

class ConcreteBase {
  func swim() {}
}

// CHECK-LABEL: sil private [ossa] @$s8closures14GenericDerivedC4swimyyFyyXEfU_ : $@convention(thin) <Ocean> (@guaranteed GenericDerived<Ocean>) -> ()
// CHECK: bb0([[ARG:%.*]] : @guaranteed $GenericDerived<Ocean>):
// CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
// CHECK:   [[ARG_COPY_SUPER:%.*]] = upcast [[ARG_COPY]] : $GenericDerived<Ocean> to $ConcreteBase
// CHECK:   [[METHOD:%.*]] = function_ref @$s8closures12ConcreteBaseC4swimyyF
// CHECK:   apply [[METHOD]]([[ARG_COPY_SUPER]]) : $@convention(method) (@guaranteed ConcreteBase) -> ()
// CHECK:   destroy_value [[ARG_COPY_SUPER]]
// CHECK: } // end sil function '$s8closures14GenericDerivedC4swimyyFyyXEfU_'

class GenericDerived<Ocean> : ConcreteBase {
  override func swim() {
    withFlotationAid {
      super.swim()
    }
  }

  func withFlotationAid(_ fn: () -> ()) {}
}

// Don't crash on this
func r25993258_helper(_ fn: (inout Int, Int) -> ()) {}
func r25993258() {
  r25993258_helper { _, _ in () }
}

// rdar://29810997
//
// Using a let from a closure in an init was causing the type-checker
// to produce invalid AST: 'self.fn' was an l-value, but 'self' was already
// loaded to make an r-value.  This was ultimately because CSApply was
// building the member reference correctly in the context of the closure,
// where 'fn' is not settable, but CSGen / CSSimplify was processing it
// in the general DC of the constraint system, i.e. the init, where
// 'fn' *is* settable.
func r29810997_helper(_ fn: (Int) -> Int) -> Int { return fn(0) }
struct r29810997 {
    private let fn: (Int) -> Int
    private var x: Int

    init(function: @escaping (Int) -> Int) {
        fn = function
        x = r29810997_helper { fn($0) }
    }
}

//   DI will turn this into a direct capture of the specific stored property.
// CHECK-LABEL: sil hidden [ossa] @$s8closures16r29810997_helperyS3iXEF : $@convention(thin) (@noescape @callee_guaranteed (Int) -> Int) -> Int

// rdar://problem/37790062

protocol P_37790062 {
  associatedtype T
  var elt: T { get }
}

func rdar37790062() {
  struct S<T> {
    init(_ a: () -> T, _ b: () -> T) {}
  }

  class C1 : P_37790062 {
    typealias T = Int
    var elt: T { return 42 }
  }

  class C2 : P_37790062 {
    typealias T = (String, Int, Void)
    var elt: T { return ("question", 42, ()) }
  }

  func foo() -> Int { return 42 }
  func bar() -> Void {}
  func baz() -> (String, Int) { return ("question", 42) }
  func bzz<T>(_ a: T) -> T { return a }
  func faz<T: P_37790062>(_ a: T) -> T.T { return a.elt }

  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU_
  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU0_
  // CHECK: function_ref @$s8closures12rdar37790062yyF1SL_VyADyxGxyXE_xyXEtcfC
  _ = S({ foo() }, { bar() })

  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU1_
  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU2_
  // CHECK: function_ref @$s8closures12rdar37790062yyF1SL_VyADyxGxyXE_xyXEtcfC
  _ = S({ baz() }, { bar() })

  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU3_
  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU4_
  // CHECK: function_ref @$s8closures12rdar37790062yyF1SL_VyADyxGxyXE_xyXEtcfC
  _ = S({ bzz(("question", 42)) }, { bar() })

  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU5_
  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU6_
  // CHECK: function_ref @$s8closures12rdar37790062yyF1SL_VyADyxGxyXE_xyXEtcfC
  _ = S({ bzz(String.self) }, { bar() })

  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU7_
  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU8_
  // CHECK: function_ref @$s8closures12rdar37790062yyF1SL_VyADyxGxyXE_xyXEtcfC
  _ = S({ bzz(((), (()))) }, { bar() })

  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU9_
  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU10_
  // CHECK: function_ref @$s8closures12rdar37790062yyF1SL_VyADyxGxyXE_xyXEtcfC
  _ = S({ bzz(C1()) }, { bar() })

  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU11_
  // CHECK: function_ref @$s8closures12rdar37790062yyFyyXEfU12_
  // CHECK: function_ref @$s8closures12rdar37790062yyF1SL_VyADyxGxyXE_xyXEtcfC
  _ = S({ faz(C2()) }, { bar() })
}

// Make sure that if we capture a trivial value, we do not try to copy it. This
// violates SIL invariants and the SILVerifier will explode as such.
func closeOverStructDontCopyTrivial() {
  let a : StructWithMutatingMethod
  a = StructWithMutatingMethod()
  takeClosure { a.x }
}
