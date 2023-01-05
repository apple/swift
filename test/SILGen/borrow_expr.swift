// RUN: %target-swift-frontend -enable-experimental-move-only -o - -emit-silgen %s | %FileCheck %s

final class Klass {
    func useKlass() {}

    func doSomething() {}
    func doSomething(_ k: Klass) {}
}

func useKlass(_ k: Klass) {}

struct Struct {
    var k = Klass()
    var computedK : Klass { Klass() }

    func doSomething() {}
    func doSomething(_ k: Klass) {}
}

func useStruct(_ s: Struct) {}

/////////////////////////
// Concrete Type Tests //
/////////////////////////

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr13simpleTestArgyyF : $@convention(thin) () -> () {
// CHECK: [[ADDR:%.*]] = project_box
//
// First check without the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[VAL:%.*]] = load [copy] [[ACCESS]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr9useStructyyAA0D0VF : $@convention(thin) (@guaranteed Struct) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: destroy_value [[VAL]]
//
// Now with the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[VAL:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr9useStructyyAA0D0VF : $@convention(thin) (@guaranteed Struct) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
// CHECK: } // end sil function '$s11borrow_expr13simpleTestArgyyF'
func simpleTestArg() {
    var s = Struct()
    s = Struct()
    // Without borrow.
    useStruct(s)
    // With borrow.
    useStruct(_borrow s)
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr18simpleTestArgFieldyyF : $@convention(thin) () -> () {
// CHECK: [[ADDR:%.*]] = project_box
//
// First check without the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[GEP:%.*]] = struct_element_addr [[ACCESS]] : $*Struct, #Struct.k
// CHECK: [[VAL:%.*]] = load [copy] [[GEP]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr8useKlassyyAA0D0CF : $@convention(thin) (@guaranteed Klass) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: destroy_value [[VAL]]
//
// Now with the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[GEP:%.*]] = struct_element_addr [[ACCESS]] : $*Struct, #Struct.k
// CHECK: [[VAL:%.*]] = load_borrow [[GEP]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr8useKlassyyAA0D0CF : $@convention(thin) (@guaranteed Klass) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
// CHECK: } // end sil function '$s11borrow_expr18simpleTestArgFieldyyF'
func simpleTestArgField() {
    var s = Struct()
    s = Struct()
    // Without borrow.
    useKlass(s.k)
    // With borrow.
    useKlass(_borrow s.k)
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr14simpleTestSelfyyF : $@convention(thin) () -> () {
// CHECK: [[ADDR:%.*]] = project_box
//
// First check without the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[VAL:%.*]] = load [copy] [[ACCESS]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr6StructV11doSomethingyyF : $@convention(method) (@guaranteed Struct) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: destroy_value [[VAL]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[VAL:%.*]] = load [copy] [[ACCESS]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr6StructV11doSomethingyyAA5KlassCF : $@convention(method) (@guaranteed Klass, @guaranteed Struct) -> ()
// CHECK: apply [[FUNC]]({{%.*}}, [[VAL]])
// CHECK: destroy_value [[VAL]]
//
// Now with the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[VAL:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr6StructV11doSomethingyyF : $@convention(method) (@guaranteed Struct) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[VAL:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr6StructV11doSomethingyyAA5KlassCF : $@convention(method) (@guaranteed Klass, @guaranteed Struct) -> ()
// CHECK: apply [[FUNC]]({{%.*}}, [[VAL]])
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: } // end sil function '$s11borrow_expr14simpleTestSelfyyF'
func simpleTestSelf() {
    var s = Struct()
    s = Struct()
    // Without borrow.
    s.doSomething()
    s.doSomething(Klass())

    // With borrow.
    (_borrow s).doSomething()
    (_borrow s).doSomething(Klass())
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr19simpleTestSelfFieldyyF : $@convention(thin) () -> () {
// CHECK: [[ADDR:%.*]] = project_box
//
// First check without the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[GEP:%.*]] = struct_element_addr [[ACCESS]] : $*Struct, #Struct.k
// CHECK: [[VAL:%.*]] = load [copy] [[GEP]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr5KlassC11doSomethingyyF : $@convention(method) (@guaranteed Klass) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: destroy_value [[VAL]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[GEP:%.*]] = struct_element_addr [[ACCESS]] : $*Struct, #Struct.k
// CHECK: [[VAL:%.*]] = load [copy] [[GEP]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr5KlassC11doSomethingyyACF : $@convention(method) (@guaranteed Klass, @guaranteed Klass) -> ()
// CHECK: apply [[FUNC]]({{%.*}}, [[VAL]])
// CHECK: destroy_value [[VAL]]
//
// Now with the borrow:
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[GEP:%.*]] = struct_element_addr [[ACCESS]] : $*Struct, #Struct.k
// CHECK: [[VAL:%.*]] = load_borrow [[GEP]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr5KlassC11doSomethingyyF : $@convention(method) (@guaranteed Klass) -> (
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[GEP:%.*]] = struct_element_addr [[ACCESS]] : $*Struct, #Struct.k
// CHECK: [[VAL:%.*]] = load_borrow [[GEP]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr5KlassC11doSomethingyyACF : $@convention(method) (@guaranteed Klass, @guaranteed Klass) -> ()
// CHECK: apply [[FUNC]]({{%.*}}, [[VAL]])
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: } // end sil function '$s11borrow_expr19simpleTestSelfFieldyyF'
func simpleTestSelfField() {
    var s = Struct()
    s = Struct()
    // Without borrow.
    s.k.doSomething()
    s.k.doSomething(Klass())

    // With borrow.
    (_borrow s.k).doSomething()
    (_borrow s.k).doSomething(Klass())
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr27simpleTestSelfComputedFieldyyF : $@convention(thin) () -> () {
// CHECK: [[ADDR:%.*]] = project_box
//
// Without borrow 1
// XHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// XHECK: [[VAL:%.*]] = load [copy] [[ACCESS]]
// XHECK: end_access [[ACCESS]]
// XHECK: [[BORROW_VAL:%.*]] = begin_borrow [[VAL]]
// XHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr6StructV9computedKAA5KlassCvg : $@convention(method) (@guaranteed Struct) -> @owned Klass
// XHECK: [[OWNED_VAL:%.*]] = apply [[FUNC]]([[BORROW_VAL]])
// XHECK: end_borrow [[BORROW_VAL]]
// XHECK: destroy_value [[VAL]]
// XHECK: [[FUNC2:%.*]] = function_ref @$s11borrow_expr5KlassC11doSomethingyyF : $@convention(method) (@guaranteed Klass) -> ()
// XHECK: apply [[FUNC2]]([[OWNED_VAL]])
// XHECK: destroy_value [[OWNED_VAL]]
//
// Without borrow 2
// XHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// XHECK: [[VAL:%.*]] = load [copy] [[ACCESS]]
// XHECK: end_access [[ACCESS]]
// XHECK: [[BORROW_VAL:%.*]] = begin_borrow [[VAL]]
// XHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr6StructV9computedKAA5KlassCvg : $@convention(method) (@guaranteed Struct) -> @owned Klass
// XHECK: [[OWNED_VAL:%.*]] = apply [[FUNC]]([[BORROW_VAL]])
// XHECK: end_borrow [[BORROW_VAL]]
// XHECK: destroy_value [[VAL]]
// XHECK: [[FUNC2:%.*]] = function_ref @$s11borrow_expr5KlassC11doSomethingyyACF : $@convention(method) (@guaranteed Klass, @guaranteed Klass) -> ()
// XHECK: apply [[FUNC2]]({{%.*}}, [[OWNED_VAL]])
// XHECK: destroy_value [[OWNED_VAL]]
//
// CHECK: } // end sil function '$s11borrow_expr27simpleTestSelfComputedFieldyyF'
func simpleTestSelfComputedField() {
    var s = Struct()
    s = Struct()

    // Without borrow.
    //s.computedK.doSomething()
    //s.computedK.doSomething(Klass())

    // With borrow.
    (_borrow s).computedK.doSomething()
    (_borrow s).computedK.doSomething(Klass())
}

////////////////////////
// Address Only Tests //
////////////////////////

protocol Q {
}

protocol P {
  var q: Q { get /*_read*/ }
  func doSomething()
  func doSomething(_ k: Klass)
}

func usePExistential(_ p: P) {}
func usePGeneric<T : P>(_ p: T) {}
func useQExistential<T : Q>(_ q: T) {}
func useQGeneric<T : Q>(_ q: T) {}

//--------------------------------------------------------------------------------
// Generics
//

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr20simpleTestGenericArgyyxAA1PRzlF : $@convention(thin) <T where T : P> (@in_guaranteed T) -> () {
// CHECK: [[ADDR:%.*]] = project_box
//
// First without borrow.
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[STACK:%.*]] = alloc_stack $T
// CHECK: copy_addr [[ACCESS]] to [init] [[STACK]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr11usePGenericyyxAA1PRzlF : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
// CHECK: apply [[FUNC]]<T>([[STACK]])
// CHECK: destroy_addr [[STACK]]
// CHECK: dealloc_stack [[STACK]]
//
// Now with borrow
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr11usePGenericyyxAA1PRzlF : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
// CHECK: apply [[FUNC]]<T>([[ACCESS]])
// CHECK: end_access [[ACCESS]]
// CHECK: } // end sil function '$s11borrow_expr20simpleTestGenericArgyyxAA1PRzlF'
func simpleTestGenericArg<T : P>(_ pArg: T) {
    var p = pArg
    p = pArg

    // Without borrow.
    usePGeneric(p)

    // With borrow.
    usePGeneric(_borrow p)
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr25simpleTestGenericArgFieldyyxAA1PRzlF : $@convention(thin) <T where T : P> (@in_guaranteed T) -> () {
// CHECK: } // end sil function '$s11borrow_expr25simpleTestGenericArgFieldyyxAA1PRzlF'
func simpleTestGenericArgField<T : P>(_ pArg: T) {
    var p = pArg
    p = pArg

    // Without borrow.
    useQGeneric(p.q)
    
    // With borrow.
    //
    // TODO: This doesn't work now. We should support this potentially for
    // _read. But protocols seem to not support _read at this time.
    // useQGeneric(_borrow p.q)
}

//--------------------------------------------------------------------------------
// Exisentials
//

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr24simpleTestExistentialArgyyAA1P_pF : $@convention(thin) (@in_guaranteed any P) -> () {
// CHECK: [[ADDR:%.*]] = project_box {{%.*}} : ${ var any P }, 0
//
// First without the borrow.
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[STACK:%.*]] = alloc_stack $any P
// CHECK: copy_addr [[ACCESS]] to [init] [[STACK]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr15usePExistentialyyAA1P_pF : $@convention(thin) (@in_guaranteed any P) -> ()
// CHECK: apply [[FUNC]]([[STACK]])
// CHECK: destroy_addr [[STACK]]
// CHECK: dealloc_stack [[STACK]]
//
// Now with the borrow.
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr15usePExistentialyyAA1P_pF : $@convention(thin) (@in_guaranteed any P) -> ()
// CHECK: apply [[FUNC]]([[ACCESS]])
// CHECK: end_access [[ACCESS]]
// CHECK: } // end sil function '$s11borrow_expr24simpleTestExistentialArgyyAA1P_pF'
func simpleTestExistentialArg(_ pArg: P) {
    var p = pArg
    p = pArg

    // Without borrow.
    usePExistential(p)

    // With borrow.
    usePExistential(_borrow p)
}


func simpleTestExistentialArgField(_ pArg: P) {
    var p = pArg
    p = pArg

    // Without borrow.
    useQGeneric(p.q)
    
    // With borrow.
    //
    // TODO: This doesn't work now. We should support this potentially for
    // _read. But protocols seem to not support _read at this time.
    // useQGeneric(_borrow p.q)
}
