// RUN: %target-swift-emit-silgen -enable-experimental-feature NoImplicitCopy %s | %FileCheck %s

final class Klass {
  var k: Klass? = nil
}
struct ContainKlass {
  var k = Klass()

  consuming func consumeFunc() {}
  mutating func mutatingFunc() {}
  func borrowingFunc() {}

  var computedK: Klass {
    get {
      k
    }
  }

  var consumingComputedK: Klass {
    __consuming get {
      k
    }
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr22testCopyLoadableRValueyyF : $@convention(thin) () -> () {
// CHECK: [[X:%.*]] = apply {{%.*}}({{%.*}}) : $@convention(method) (@thin ContainKlass.Type) -> @owned ContainKlass
// CHECK: [[BORROW:%.*]] = begin_borrow [lexical] [[X]]
// CHECK: [[COPY_BORROW:%.*]] = copy_value [[BORROW]]
// CHECK: explicit_copy_value [[COPY_BORROW]]
// CHECK: } // end sil function '$s9copy_expr22testCopyLoadableRValueyyF'
func testCopyLoadableRValue() {
  let x = ContainKlass()
  let _ = copy x
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr25testCopyLoadableVarLValueyyF : $@convention(thin) () -> () {
// CHECK:   [[BOX:%.*]] = alloc_box ${ var ContainKlass }, var, name "x"
// CHECK:   [[BORROW_BOX:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT_BOX:%.*]] = project_box [[BORROW_BOX]]
//
// CHECK:   [[FINAL_ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT_BOX]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[FINAL_ACCESS]]
// CHECK:   explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[FINAL_ACCESS]]
// CHECK: } // end sil function '$s9copy_expr25testCopyLoadableVarLValueyyF'
func testCopyLoadableVarLValue() {
  var x = ContainKlass()
  x = ContainKlass()
  let _ = copy x
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr25testCopyLoadableVarLValueyyAA12ContainKlassVzF : $@convention(thin) (@inout ContainKlass) -> () {
// CHECK: bb0([[ARG:%.*]] : $*ContainKlass):
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   explicit_copy_value [[LOAD_BORROW]]
// CHECK: } // end sil function '$s9copy_expr25testCopyLoadableVarLValueyyAA12ContainKlassVzF'
func testCopyLoadableVarLValue(_ x: inout ContainKlass) {
  let _ = copy x
}

protocol P {
  static var value: Self { get }

  consuming func consumeFunc()
  mutating func mutatingFunc()
  func borrowingFunc()

  var computedK: Klass {
    get
  }

  var consumingComputedK: Klass {
    __consuming get
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr25testCopyAddressOnlyRValueyyxmAA1PRzlF : $@convention(thin) <T where T : P> (@thick T.Type) -> () {
// CHECK:   [[BOX:%.*]] = alloc_stack [lexical] $T, let, name "x"
// CHECK:   [[TMP:%.*]] = alloc_stack $T
// CHECK:   explicit_copy_addr [[BOX]] to [init] [[TMP]]
// CHECK: } // end sil function '$s9copy_expr25testCopyAddressOnlyRValueyyxmAA1PRzlF'
func testCopyAddressOnlyRValue<T : P>(_ t: T.Type) {
  let x = T.value
  let _ = copy x
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr25testCopyAddressOnlyLValueyyxmAA1PRzlF : $@convention(thin) <T where T : P> (@thick T.Type) -> () {
// CHECK:   [[BOX:%.*]] = alloc_box $<τ_0_0 where τ_0_0 : P> { var τ_0_0 } <T>, var, name "x"
// CHECK:   [[BORROW_BOX:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT_BOX:%.*]] = project_box [[BORROW_BOX]]
//
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT_BOX]]
// CHECK:   [[TMP:%.*]] = alloc_stack $T
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TMP]]
// CHECK:   end_access [[ACCESS]]
// CHECK: } // end sil function '$s9copy_expr25testCopyAddressOnlyLValueyyxmAA1PRzlF'
func testCopyAddressOnlyLValue<T : P>(_ t: T.Type) {
  var x = T.value
  x = T.value
  let _ = copy x
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr28testCopyAddressOnlyLValueArgyyxzAA1PRzlF : $@convention(thin) <T where T : P> (@inout T) -> () {
// CHECK: bb0([[ARG:%.*]] : $*T):
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[TMP:%.*]] = alloc_stack $T
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TMP]]
// CHECK: } // end sil function '$s9copy_expr28testCopyAddressOnlyLValueArgyyxzAA1PRzlF'
func testCopyAddressOnlyLValueArg<T : P>(_ x: inout T) {
  let _ = copy x
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr31testCallMethodOnLoadableLetCopyyyF : $@convention(thin) () -> () {
// CHECK: [[ORIG_X:%.*]] = apply {{%.*}}({{%.*}}) : $@convention(method) (@thin ContainKlass.Type) -> @owned ContainKlass
// CHECK: [[X:%.*]] = begin_borrow [lexical] [[ORIG_X]]
//
// Calling consumeFunc.
// CHECK: [[COPY_X:%.*]] = copy_value [[X]]
// CHECK: [[EXPLICIT_COPY_X:%.*]] = explicit_copy_value [[COPY_X]]
// CHECK: [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV11consumeFuncyyF : $@convention(method) (@owned ContainKlass) -> ()
// CHECK: apply [[FUNC]]([[EXPLICIT_COPY_X]])
// CHECK: destroy_value [[COPY_X]]
//
// Calling borrowingFunc.
// CHECK: [[COPY_X:%.*]] = copy_value [[X]]
// CHECK: [[EXPLICIT_COPY_X:%.*]] = explicit_copy_value [[COPY_X]]
// CHECK: [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV13borrowingFuncyyF : $@convention(method) (@guaranteed ContainKlass) -> ()
// CHECK: apply [[FUNC]]([[EXPLICIT_COPY_X]])
// CHECK: destroy_value [[EXPLICIT_COPY_X]]
// CHECK: destroy_value [[COPY_X]]
//
// Calling computedK. It is borrowed.
// CHECK: [[COPY_X:%.*]] = copy_value [[X]]
// CHECK: [[EXPLICIT_COPY_X:%.*]] = explicit_copy_value [[COPY_X]]
// CHECK: [[BORROW_EXPLICIT_COPY_X:%.*]] = begin_borrow [[EXPLICIT_COPY_X]]
// CHECK: [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV9computedKAA0D0Cvg : $@convention(method) (@guaranteed ContainKlass) -> @owned Klass
// CHECK: apply [[FUNC]]([[BORROW_EXPLICIT_COPY_X]])
// CHECK: end_borrow [[BORROW_EXPLICIT_COPY_X]]
// CHECK: destroy_value [[EXPLICIT_COPY_X]]
// CHECK: destroy_value [[COPY_X]]
//
// Calling computed getter.
// CHECK: [[COPY_X:%.*]] = copy_value [[X]]
// CHECK: [[EXPLICIT_COPY_X:%.*]] = explicit_copy_value [[COPY_X]]
// CHECK: [[BORROW_EXPLICIT_COPY_X:%.*]] = begin_borrow [[EXPLICIT_COPY_X]]
// CHECK: [[COPY_BORROW_EXPLICIT_COPY_X:%.*]] = copy_value [[BORROW_EXPLICIT_COPY_X]]
// CHECK: [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV18consumingComputedKAA0D0Cvg : $@convention(method) (@owned ContainKlass) -> @owned Klass
// CHECK: apply [[FUNC]]([[COPY_BORROW_EXPLICIT_COPY_X]])
// CHECK: end_borrow [[BORROW_EXPLICIT_COPY_X]]
// CHECK: destroy_value [[EXPLICIT_COPY_X]]
// CHECK: destroy_value [[COPY_X]]
// CHECK: } // end sil function '$s9copy_expr31testCallMethodOnLoadableLetCopyyyF'
func testCallMethodOnLoadableLetCopy() {
  let x = ContainKlass()
  (copy x).consumeFunc()
  (copy x).borrowingFunc()
  _ = (copy x).computedK
  _ = (copy x).consumingComputedK
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr31testCallMethodOnLoadableVarCopyyyF : $@convention(thin) () -> () {
// CHECK:   [[BOX:%.*]] = alloc_box ${ var ContainKlass }
// CHECK:   [[BORROW:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT:%.*]] = project_box [[BORROW]]
//
// Calling consumeFunc.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV11consumeFuncyyF : $@convention(method) (@owned ContainKlass) -> ()
// CHECK:   apply [[FUNC]]([[LOAD]])
//
// Calling borrowing func.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV13borrowingFuncyyF : $@convention(method) (@guaranteed ContainKlass) -> ()
// CHECK:   apply [[FUNC]]([[LOAD]])
// CHECK:   destroy_value [[LOAD]]
//
// Calling borrowing computed getter
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[BORROW_LOAD:%.*]] = begin_borrow [[LOAD]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV9computedKAA0D0Cvg : $@convention(method) (@guaranteed ContainKlass) -> @owned Klass
// CHECK:   apply [[FUNC]]([[BORROW_LOAD]])
// CHECK:   end_borrow [[BORROW_LOAD]]
// CHECK:   destroy_value [[LOAD]]
//
// Consuming computed getter.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[LOAD]]
// CHECK:   [[BORROW_COPY:%.*]] = copy_value [[BORROW]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV18consumingComputedKAA0D0Cvg : $@convention(method) (@owned ContainKlass) -> @owned Klass
// CHECK:   apply [[FUNC]]([[BORROW_COPY]])
// CHECK: } // end sil function '$s9copy_expr31testCallMethodOnLoadableVarCopyyyF'
func testCallMethodOnLoadableVarCopy() {
  var x = ContainKlass()
  x = ContainKlass()
  (copy x).consumeFunc()
  (copy x).borrowingFunc()
  _ = (copy x).computedK
  _ = (copy x).consumingComputedK
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr33testCallMethodOnLoadableInOutCopyyyAA12ContainKlassVzF : $@convention(thin) (@inout ContainKlass) -> () {
// CHECK: bb0([[ARG:%.*]] : $*ContainKlass):
//
// Calling consumeFunc.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV11consumeFuncyyF : $@convention(method) (@owned ContainKlass) -> ()
// CHECK:   apply [[FUNC]]([[LOAD]])
//
// Calling borrowing func.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV13borrowingFuncyyF : $@convention(method) (@guaranteed ContainKlass) -> ()
// CHECK:   apply [[FUNC]]([[LOAD]])
// CHECK:   destroy_value [[LOAD]]
//
// Calling borrowing computed getter
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[BORROW_LOAD:%.*]] = begin_borrow [[LOAD]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV9computedKAA0D0Cvg : $@convention(method) (@guaranteed ContainKlass) -> @owned Klass
// CHECK:   apply [[FUNC]]([[BORROW_LOAD]])
// CHECK:   end_borrow [[BORROW_LOAD]]
// CHECK:   destroy_value [[LOAD]]
//
// Consuming computed getter.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[LOAD]]
// CHECK:   [[BORROW_COPY:%.*]] = copy_value [[BORROW]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV18consumingComputedKAA0D0Cvg : $@convention(method) (@owned ContainKlass) -> @owned Klass
// CHECK:   apply [[FUNC]]([[BORROW_COPY]])
// CHECK: } // end sil function '$s9copy_expr33testCallMethodOnLoadableInOutCopyyyAA12ContainKlassVzF'
func testCallMethodOnLoadableInOutCopy(_ x: inout ContainKlass) {
  (copy x).consumeFunc()
  (copy x).borrowingFunc()
  _ = (copy x).computedK
  _ = (copy x).consumingComputedK
}

var containKlassGlobal: ContainKlass

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr30testCallMethodOnLoadableGlobalyyF : $@convention(thin) () -> () {
// CHECK:   [[ADDR:%.*]] = global_addr @$s9copy_expr18containKlassGlobalAA07ContainD0Vvp
//
// Calling consumeFunc.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [dynamic] [[ADDR]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV11consumeFuncyyF : $@convention(method) (@owned ContainKlass) -> ()
// CHECK:   apply [[FUNC]]([[LOAD]])
//
// Calling borrowing func.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [dynamic] [[ADDR]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV13borrowingFuncyyF : $@convention(method) (@guaranteed ContainKlass) -> ()
// CHECK:   apply [[FUNC]]([[LOAD]])
// CHECK:   destroy_value [[LOAD]]
//
// Calling borrowing computed getter
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [dynamic] [[ADDR]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[BORROW_LOAD:%.*]] = begin_borrow [[LOAD]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV9computedKAA0D0Cvg : $@convention(method) (@guaranteed ContainKlass) -> @owned Klass
// CHECK:   apply [[FUNC]]([[BORROW_LOAD]])
// CHECK:   end_borrow [[BORROW_LOAD]]
// CHECK:   destroy_value [[LOAD]]
//
// Consuming computed getter.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [dynamic] [[ADDR]]
// CHECK:   [[LOAD_BORROW:%.*]] = load_borrow [[ACCESS]]
// CHECK:   [[LOAD:%.*]] = explicit_copy_value [[LOAD_BORROW]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[LOAD]]
// CHECK:   [[BORROW_COPY:%.*]] = copy_value [[BORROW]]
// CHECK:   [[FUNC:%.*]] = function_ref @$s9copy_expr12ContainKlassV18consumingComputedKAA0D0Cvg : $@convention(method) (@owned ContainKlass) -> @owned Klass
// CHECK:   apply [[FUNC]]([[BORROW_COPY]])
// CHECK: } // end sil function '$s9copy_expr30testCallMethodOnLoadableGlobalyyF'
func testCallMethodOnLoadableGlobal() {
  (copy containKlassGlobal).consumeFunc()
  (copy containKlassGlobal).borrowingFunc()
  _ = (copy containKlassGlobal).computedK
  _ = (copy containKlassGlobal).consumingComputedK
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr34testCallMethodOnAddressOnlyLetCopyyyxmAA1PRzlF : $@convention(thin) <T where T : P> (@thick T.Type) -> () {
// CHECK: [[X:%.*]] = alloc_stack [lexical] $T, let, name "x"
//
// Calling consumeFunc.
// CHECK: [[TEMP:%.*]] = alloc_stack $T
// CHECK: explicit_copy_addr [[X]] to [init] [[TEMP]]
// CHECK: [[FUNC:%.*]] = witness_method $T, #P.consumeFunc : <Self where Self : P> (consuming Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> ()
// CHECK: apply [[FUNC]]<(T)>([[TEMP]])
//
// Calling borrowingFunc.
// CHECK: [[TEMP:%.*]] = alloc_stack $T
// CHECK: explicit_copy_addr [[X]] to [init] [[TEMP]]
// CHECK: [[FUNC:%.*]] = witness_method $T, #P.borrowingFunc : <Self where Self : P> (Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
// CHECK: apply [[FUNC]]<(T)>([[TEMP]])
//
// Calling computedK. It is borrowed.
// CHECK: [[TEMP:%.*]] = alloc_stack $T
// CHECK: explicit_copy_addr [[X]] to [init] [[TEMP]]
// CHECK: [[TEMP2:%.*]] = alloc_stack $T
// CHECK: copy_addr [[TEMP]] to [init] [[TEMP2]]
// CHECK: [[FUNC:%.*]] = witness_method $T, #P.computedK!getter : <Self where Self : P> (Self) -> () -> Klass : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @owned Klass
// CHECK: apply [[FUNC]]<(T)>([[TEMP2]])
//
// Calling computed consuming getter.
// CHECK: [[TEMP:%.*]] = alloc_stack $T
// CHECK: explicit_copy_addr [[X]] to [init] [[TEMP]]
// CHECK: [[TEMP2:%.*]] = alloc_stack $T
// CHECK: copy_addr [[TEMP]] to [init] [[TEMP2]]
// CHECK: [[FUNC:%.*]] = witness_method $T, #P.consumingComputedK!getter : <Self where Self : P> (__owned Self) -> () -> Klass : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> @owned Klass
// CHECK: apply [[FUNC]]<(T)>([[TEMP2]])
// CHECK: } // end sil function '$s9copy_expr34testCallMethodOnAddressOnlyLetCopyyyxmAA1PRzlF'
func testCallMethodOnAddressOnlyLetCopy<T : P>(_ t: T.Type) {
  let x = T.value
  (copy x).consumeFunc()
  (copy x).borrowingFunc()
  _ = (copy x).computedK
  _ = (copy x).consumingComputedK
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr34testCallMethodOnAddressOnlyVarCopyyyxmAA1PRzlF : $@convention(thin) <T where T : P> (@thick T.Type) -> () {
// CHECK:   [[BOX:%.*]] = alloc_box $
// CHECK:   [[BORROW:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK:   [[PROJECT:%.*]] = project_box [[BORROW]]
//
// Calling consumeFunc.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.consumeFunc : <Self where Self : P> (consuming Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> ()
// CHECK:   apply [[FUNC]]<(T)>([[TEMP]])
//
// Calling borrowing func.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.borrowingFunc : <Self where Self : P> (Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
// CHECK:   apply [[FUNC]]<(T)>([[TEMP]])
//
// Calling borrowing computed getter
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   [[TEMP2:%.*]] = alloc_stack $
// CHECK:   copy_addr [[TEMP]] to [init] [[TEMP2]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.computedK!getter : <Self where Self : P> (Self) -> () -> Klass : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @owned Klass
// CHECK:   apply [[FUNC]]<(T)>([[TEMP2]])
//
// Consuming computed getter.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJECT]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   [[TEMP2:%.*]] = alloc_stack $
// CHECK:   copy_addr [[TEMP]] to [init] [[TEMP2]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.consumingComputedK!getter : <Self where Self : P> (__owned Self) -> () -> Klass : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> @owned Klass
// CHECK:   apply [[FUNC]]<(T)>([[TEMP2]])
// CHECK: } // end sil function '$s9copy_expr34testCallMethodOnAddressOnlyVarCopyyyxmAA1PRzlF'
func testCallMethodOnAddressOnlyVarCopy<T : P>(_ t: T.Type) {
  var x = T.value
  x = T.value
  (copy x).consumeFunc()
  (copy x).borrowingFunc()
  _ = (copy x).computedK
  _ = (copy x).consumingComputedK
}

// CHECK-LABEL: sil hidden [ossa] @$s9copy_expr36testCallMethodOnAddressOnlyInOutCopyyyxzAA1PRzlF : $@convention(thin) <T where T : P> (@inout T) -> () {
// CHECK:  bb0([[ARG:%.*]] : $*
//
// Calling consumeFunc.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.consumeFunc : <Self where Self : P> (consuming Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> ()
// CHECK:   apply [[FUNC]]<(T)>([[TEMP]])
//
// Calling borrowing func.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.borrowingFunc : <Self where Self : P> (Self) -> () -> () : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> ()
// CHECK:   apply [[FUNC]]<(T)>([[TEMP]])
//
// Calling borrowing computed getter
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   [[TEMP2:%.*]] = alloc_stack $
// CHECK:   copy_addr [[TEMP]] to [init] [[TEMP2]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.computedK!getter : <Self where Self : P> (Self) -> () -> Klass : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @owned Klass
// CHECK:   apply [[FUNC]]<(T)>([[TEMP2]])
//
// Consuming computed getter.
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[ARG]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $
// CHECK:   explicit_copy_addr [[ACCESS]] to [init] [[TEMP]]
// CHECK:   [[TEMP2:%.*]] = alloc_stack $
// CHECK:   copy_addr [[TEMP]] to [init] [[TEMP2]]
// CHECK:   [[FUNC:%.*]] = witness_method $T, #P.consumingComputedK!getter : <Self where Self : P> (__owned Self) -> () -> Klass : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> @owned Klass
// CHECK:   apply [[FUNC]]<(T)>([[TEMP2]])
// CHECK: } // end sil function '$s9copy_expr36testCallMethodOnAddressOnlyInOutCopyyyxzAA1PRzlF'
func testCallMethodOnAddressOnlyInOutCopy<T : P>(_ x: inout T) {
  (copy x).consumeFunc()
  (copy x).borrowingFunc()
  _ = (copy x).computedK
  _ = (copy x).consumingComputedK
}
