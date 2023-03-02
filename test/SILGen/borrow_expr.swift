// RUN: %target-swift-frontend -enable-experimental-move-only -o - -emit-silgen %s | %FileCheck %s

final class Klass {
    var k = Klass()
    func useKlass() {}

    func doSomething() {}
    func doSomething(_ k: Klass) {}

    var computedK : Klass {
        return self
    }

    var computedK2 : Klass {
        _read {
            yield k
        }
        set {}
    }
}

func useKlass(_ k: Klass) {}

struct Struct {
    var k = Klass()

    func doSomething() {}
    func doSomething(_ k: Klass) {}

    var computedK: Klass {
        Klass()
    }
    var computedK2: Klass {
        get {
            Klass()
        }
        set {}
    }

    var computedK3: Klass {
        _read {
            yield k
        }
    }

    var computedK4: Klass {
        _read {
            yield k
        }
        set {}
    }
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

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr27simpleTestArgComputedField1yyF : $@convention(thin) () -> () {
// CHECK: bb0:
// CHECK:   [[BOX:%.*]] = alloc_box
// CHECK:   [[BORROW:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT:%.*]] = project_box [[BORROW]]
//
// useKlass((_borrow s).computedK)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[VALUE2:%.*]] = apply {{%.*}}([[VALUE]])
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[VALUE2]])
// CHECK: destroy_value [[VALUE2]]
//
// useKlass(_borrow s.computedK)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[VALUE2:%.*]] = apply {{%.*}}([[VALUE]])
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[VALUE2]])
// CHECK: destroy_value [[VALUE2]]
//
// useKlass(_borrow (_borrow s).computedK2)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[VALUE2:%.*]] = apply {{%.*}}([[VALUE]])
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[VALUE2]])
// CHECK: destroy_value [[VALUE2]]
// CHECK: } // end sil function '$s11borrow_expr27simpleTestArgComputedField1yyF'
func simpleTestArgComputedField1() {
    var s = Struct()
    s = Struct()

    // computedK is a get without a setter, so we do need a copy and also do not
    // nest our calls
    useKlass((_borrow s).computedK)
    useKlass(_borrow s.computedK)
    useKlass(_borrow (_borrow s).computedK)
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr27simpleTestArgComputedField2yyF : $@convention(thin) () -> () {
// CHECK:   [[BOX:%.*]] = alloc_box
// CHECK:   [[BORROW:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT:%.*]] = project_box [[BORROW]]
//
// computedK2 is a get with a setter, so we do need a copy but can nest our
// calls
//
// useKlass((_borrow s).computedK2)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[VALUE2:%.*]] = apply {{%.*}}([[VALUE]])
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[VALUE2]])
// CHECK: destroy_value [[VALUE2]]
//
// useKlass(_borrow s.computedK2)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[VALUE2:%.*]] = apply {{%.*}}([[VALUE]])
// CHECK: end_borrow [[VALUE]]
// CHECK: [[BORROW_VALUE2:%.*]] = begin_borrow [[VALUE2]]
// CHECK: apply {{%.*}}([[BORROW_VALUE2]])
// CHECK: end_borrow [[BORROW_VALUE2]]
// CHECK: end_access [[ACCESS]]
// CHECK: destroy_value [[VALUE2]]
//
// useKlass(_borrow (_borrow s).computedK2)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[VALUE2:%.*]] = apply {{%.*}}([[VALUE]])
// CHECK: end_borrow [[VALUE]]
// CHECK: [[BORROW_VALUE2:%.*]] = begin_borrow [[VALUE2]]
// CHECK: apply {{%.*}}([[BORROW_VALUE2]])
// CHECK: end_borrow [[BORROW_VALUE2]]
// CHECK: end_access [[ACCESS]]
// CHECK: destroy_value [[VALUE2]]
//
// CHECK: } // end sil function '$s11borrow_expr27simpleTestArgComputedField2yyF'
func simpleTestArgComputedField2() {
    var s = Struct()
    s = Struct()

    // Since computedK2 is a _read, we can avoid any intermediate owned value.
    useKlass((_borrow s).computedK2)
    useKlass(_borrow s.computedK2)
    useKlass(_borrow (_borrow s).computedK2)
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr27simpleTestArgComputedField3yyF : $@convention(thin) () -> () {
// CHECK: bb0:
// CHECK:   [[BOX:%.*]] = alloc_box
// CHECK:   [[BORROW:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT:%.*]] = project_box [[BORROW]]
//
// useKlass((_borrow s).computedK3)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: ([[VALUE2:%.*]], [[TOKEN:%.*]]) = begin_apply {{%.*}}([[VALUE]])
// CHECK: [[COPY_VALUE2:%.*]] = copy_value [[VALUE2]]
// CHECK: end_apply [[TOKEN]]
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[COPY_VALUE2]])
// CHECK: destroy_value [[COPY_VALUE2]]
//
// useKlass(_borrow s.computedK3)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: ([[VALUE2:%.*]], [[TOKEN:%.*]]) = begin_apply {{%.*}}([[VALUE]])
// CHECK: [[COPY_VALUE2:%.*]] = copy_value [[VALUE2]]
// CHECK: end_apply [[TOKEN]]
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[COPY_VALUE2]])
// CHECK: destroy_value [[COPY_VALUE2]]
//
// useKlass(_borrow (_borrow s).computedK3)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: ([[VALUE2:%.*]], [[TOKEN:%.*]]) = begin_apply {{%.*}}([[VALUE]])
// CHECK: [[COPY_VALUE2:%.*]] = copy_value [[VALUE2]]
// CHECK: end_apply [[TOKEN]]
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[COPY_VALUE2]])
// CHECK: destroy_value [[COPY_VALUE2]]
// CHECK: } // end sil function '$s11borrow_expr27simpleTestArgComputedField3yyF'
func simpleTestArgComputedField3() {
    var s = Struct()
    s = Struct()

    useKlass((_borrow s).computedK3)
    useKlass(_borrow s.computedK3)
    useKlass(_borrow (_borrow s).computedK3)
}

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr27simpleTestArgComputedField4yyF : $@convention(thin) () -> () {
// CHECK:   [[BOX:%.*]] = alloc_box
// CHECK:   [[BORROW:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT:%.*]] = project_box [[BORROW]]
//
// computedK2 is a get with a setter, so we do need a copy but can nest our
// calls
//
// useKlass((_borrow s).computedK4)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: ([[VALUE2:%.*]], [[TOKEN:%.*]]) = begin_apply {{%.*}}([[VALUE]])
// CHECK: [[COPY_VALUE2:%.*]] = copy_value [[VALUE2]]
// CHECK: end_apply [[TOKEN]]
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
// CHECK: apply {{%.*}}([[COPY_VALUE2]])
// CHECK: destroy_value [[COPY_VALUE2]]
//
// useKlass(_borrow s.computedK4)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: ([[VALUE2:%.*]], [[TOKEN:%.*]]) = begin_apply {{%.*}}([[VALUE]])
// CHECK: apply {{%.*}}([[VALUE2]])
// CHECK: end_apply [[TOKEN]]
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
//
// useKlass(_borrow (_borrow s).computedK4)
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK: [[VALUE:%.*]] = load_borrow [[ACCESS]]
// CHECK: ([[VALUE2:%.*]], [[TOKEN:%.*]]) = begin_apply {{%.*}}([[VALUE]])
// CHECK: apply {{%.*}}([[VALUE2]])
// CHECK: end_apply [[TOKEN]]
// CHECK: end_borrow [[VALUE]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: } // end sil function '$s11borrow_expr27simpleTestArgComputedField4yyF'
func simpleTestArgComputedField4() {
    var s = Struct()
    s = Struct()

    useKlass((_borrow s).computedK4)
    useKlass(_borrow s.computedK4)
    useKlass(_borrow (_borrow s).computedK4)
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

////////////////////////
// Address Only Tests //
////////////////////////

protocol Q {
}

protocol P {
  var q: Q { get /*_read*/ }
  func doSomething()
  func doSomething(_ k: Klass)

  mutating func mutatingExtensionPointCallMethodOnSelf()
  mutating func mutatingExtensionPointPassSelfAsArg()
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

/////////////
// Globals //
/////////////

var globalKlass = Klass()

// CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr0A6GlobalyyF : $@convention(thin) () -> () {
// CHECK: [[ADDR:%.*]] = global_addr @$s11borrow_expr11globalKlassAA0D0Cvp
// CHECK: [[ACCESS:%.*]] = begin_access [read] [dynamic] [[ADDR]]
// CHECK: [[VAL:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr5KlassC11doSomethingyyF : $@convention(method) (@guaranteed Klass) -> ()
// CHECK: apply [[FUNC]]([[VAL]]
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
// CHECK: [[ACCESS:%.*]] = begin_access [read] [dynamic] [[ADDR]]
// CHECK: [[VAL:%.*]] = load_borrow [[ACCESS]]
// CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr8useKlassyyAA0D0CF : $@convention(thin) (@guaranteed Klass) -> ()
// CHECK: apply [[FUNC]]([[VAL]])
// CHECK: end_borrow [[VAL]]
// CHECK: end_access [[ACCESS]]
// CHECK: } // end sil function '$s11borrow_expr0A6GlobalyyF'
func borrowGlobal() {
  (_borrow globalKlass).doSomething()
  useKlass(_borrow globalKlass)
}

/////////////
// Methods //
/////////////

struct MethodTestingStruct {
  var k = Klass()

  func doSomething() {}

  // CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr19MethodTestingStructV012mutatingCallC6OnSelfyyF : $@convention(method) (@inout MethodTestingStruct) -> () {
  // CHECK: bb0([[SELF:%.*]] :
  //
  // Without borrow
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[SELF]]
  // CHECK: [[VAL:%.*]] = load [copy] [[ACCESS]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr19MethodTestingStructV11doSomethingyyF : $@convention(method) (@guaranteed MethodTestingStruct) -> ()
  // CHECK: apply [[FUNC]]([[VAL]])
  // CHECK: destroy_value [[VAL]]
  //
  // With borrow
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[SELF]]
  // CHECK: [[VAL:%.*]] = load_borrow [[ACCESS]]
  // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr19MethodTestingStructV11doSomethingyyF : $@convention(method) (@guaranteed MethodTestingStruct) -> ()
  // CHECK: apply [[FUNC]]([[VAL]])
  // CHECK: end_borrow [[VAL]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: } // end sil function '$s11borrow_expr19MethodTestingStructV012mutatingCallC6OnSelfyyF'
  mutating func mutatingCallMethodOnSelf() {
    // Without borrow
    doSomething()
    // With borrow
    (_borrow self).doSomething()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr19MethodTestingStructV26mutatingPassSelfAsArgumentyyF : $@convention(method) (@inout MethodTestingStruct) -> () {
  // CHECK: bb0([[SELF:%.*]] :
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[SELF]]
  // CHECK: [[VAL:%.*]] = load [copy] [[ACCESS]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr22useMethodTestingStructyyAA0deF0VF : $@convention(thin) (@guaranteed MethodTestingStruct) -> ()
  // CHECK: apply [[FUNC]]([[VAL]])
  // CHECK: destroy_value [[VAL]]
  //
  // With borrow
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[SELF]]
  // CHECK: [[VAL:%.*]] = load_borrow [[ACCESS]]
    // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr22useMethodTestingStructyyAA0deF0VF : $@convention(thin) (@guaranteed MethodTestingStruct) -> ()
  // CHECK: apply [[FUNC]]([[VAL]])
  // CHECK: end_borrow [[VAL]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: } // end sil function '$s11borrow_expr19MethodTestingStructV26mutatingPassSelfAsArgumentyyF'
  mutating func mutatingPassSelfAsArgument() {
    // Without borrow
    useMethodTestingStruct(self)
    // With borrow
    useMethodTestingStruct(_borrow self)
  }
}

func useMethodTestingStruct(_ x: MethodTestingStruct) {}

extension P {
  // CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr1PPAAE27mutatingCallingMethodOnSelfyyF : $@convention(method) <Self where Self : P> (@inout Self) -> () {
  // CHECK: bb0([[ADDR:%.*]] :
  //
  // First without borrow.
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[STACK:%.*]] = alloc_stack $Self
  // CHECK: copy_addr [[ACCESS]] to [init] [[STACK]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: [[FUNC:%.*]] = witness_method $Self, #P.doSomething : <Self where Self : P> (Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<Self>([[STACK]])
  // CHECK: destroy_addr [[STACK]]
  // CHECK: dealloc_stack [[STACK]]
  //
  // Now with borrow
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[FUNC:%.*]] = witness_method $Self, #P.doSomething : <Self where Self : P> (Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<(Self)>([[ACCESS]])
  // CHECK: end_access [[ACCESS]]
  // CHECK: } // end sil function '$s11borrow_expr1PPAAE27mutatingCallingMethodOnSelfyyF'
  mutating func mutatingCallingMethodOnSelf() {
    // Without borrow
    doSomething()

    // With borrow
    (_borrow self).doSomething()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr1PPAAE26mutatingPassSelfAsArgumentyyF : $@convention(method) <Self where Self : P> (@inout Self) -> () {
  // CHECK: bb0([[ADDR:%.*]] :
  //
  // First without borrow.
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[STACK:%.*]] = alloc_stack $Self
  // CHECK: copy_addr [[ACCESS]] to [init] [[STACK]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr11usePGenericyyxAA1PRzlF : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<Self>([[STACK]])
  // CHECK: destroy_addr [[STACK]]
  // CHECK: dealloc_stack [[STACK]]
  //
  // Now with borrow
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr11usePGenericyyxAA1PRzlF : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<Self>([[ACCESS]])
  // CHECK: end_access [[ACCESS]]
  // CHECK: } // end sil function '$s11borrow_expr1PPAAE26mutatingPassSelfAsArgumentyyF'
  mutating func mutatingPassSelfAsArgument() {
    // Without borrow
    usePGeneric(self)

    // With borrow
    usePGeneric(_borrow self)
  }

  // CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr1PPAAE38mutatingExtensionPointCallMethodOnSelfyyF : $@convention(method) <Self where Self : P> (@inout Self) -> () {
  // CHECK: bb0([[ADDR:%.*]] :
  //
  // First without borrow.
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[STACK:%.*]] = alloc_stack $Self
  // CHECK: copy_addr [[ACCESS]] to [init] [[STACK]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: [[FUNC:%.*]] = witness_method $Self, #P.doSomething : <Self where Self : P> (Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<Self>([[STACK]])
  // CHECK: destroy_addr [[STACK]]
  // CHECK: dealloc_stack [[STACK]]
  //
  // Now with borrow
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[FUNC:%.*]] = witness_method $Self, #P.doSomething : <Self where Self : P> (Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<(Self)>([[ACCESS]])
  // CHECK: end_access [[ACCESS]]
  // CHECK: } // end sil function '$s11borrow_expr1PPAAE38mutatingExtensionPointCallMethodOnSelfyyF'
  mutating func mutatingExtensionPointCallMethodOnSelf() {
    // Without borrow
    doSomething()

    // With borrow
    (_borrow self).doSomething()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s11borrow_expr1PPAAE35mutatingExtensionPointPassSelfAsArgyyF : $@convention(method) <Self where Self : P> (@inout Self) -> () {
  // CHECK: bb0([[ADDR:%.*]] :
  //
  // First without borrow.
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[STACK:%.*]] = alloc_stack $Self
  // CHECK: copy_addr [[ACCESS]] to [init] [[STACK]]
  // CHECK: end_access [[ACCESS]]
  // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr11usePGenericyyxAA1PRzlF : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<Self>([[STACK]])
  // CHECK: destroy_addr [[STACK]]
  // CHECK: dealloc_stack [[STACK]]
  //
  // Now with borrow
  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[ADDR]]
  // CHECK: [[FUNC:%.*]] = function_ref @$s11borrow_expr11usePGenericyyxAA1PRzlF : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
  // CHECK: apply [[FUNC]]<Self>([[ACCESS]])
  // CHECK: end_access [[ACCESS]]
  // CHECK: } // end sil function '$s11borrow_expr1PPAAE35mutatingExtensionPointPassSelfAsArgyyF'
  mutating func mutatingExtensionPointPassSelfAsArg() {
    // Without borrow
    usePGeneric(self)

    // With borrow
    usePGeneric(_borrow self)
  }
}
