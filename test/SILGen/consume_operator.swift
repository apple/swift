// RUN: %target-swift-emit-silgen -enable-copy-propagation=requested-passes-only -module-name consume %s -disable-objc-attr-requires-foundation-module | %FileCheck %s

class Klass {}

protocol P {
  static var value: Self { get }
}

// CHECK-LABEL: sil hidden [ossa] @$s7consume15testLoadableLetyyF : $@convention(thin) () -> () {
// CHECK: [[ORIG_VALUE:%.*]] = apply {{%.*}}({{%.*}}) : $@convention(method) (@thick Klass.Type) -> @owned Klass
// CHECK: [[BORROWED_VALUE:%.*]] = begin_borrow [lexical] [[ORIG_VALUE]]
// CHECK: [[COPY:%.*]] = copy_value [[BORROWED_VALUE:%.*]]
// CHECK: move_value [allows_diagnostics] [[COPY]]
// CHECK: } // end sil function '$s7consume15testLoadableLetyyF'
func testLoadableLet() {
  let k = Klass()
  let _ = consume k
}

// CHECK-LABEL: sil hidden [ossa] @$s7consume15testLoadableVaryyF : $@convention(thin) () -> () {
// CHECK: [[BOX:%.*]] = alloc_box $
// CHECK: [[BORROW_BOX:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK: [[PROJECT:%.*]] = project_box [[BORROW_BOX]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [modify] [unknown] [[PROJECT]]
// CHECK: assign {{%.*}} to [[ACCESS]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [modify] [unknown] [[PROJECT]]
// CHECK: [[STACK:%.*]] = alloc_stack $Klass
// CHECK: mark_unresolved_move_addr [[ACCESS]] to [[STACK]]
// CHECK: [[VALUE:%.*]] = load [take] [[STACK]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: } // end sil function '$s7consume15testLoadableVaryyF'
func testLoadableVar() {
  var k = Klass()
  k = Klass()
  let _ = consume k
}

// CHECK-LABEL: sil hidden [ossa] @$s7consume18testAddressOnlyLetyyxmAA1PRzlF : $@convention(thin) <T where T : P> (@thick T.Type) -> () {
// CHECK: [[BOX:%.*]] = alloc_stack [lexical] $T
//
// CHECK: [[STACK:%.*]] = alloc_stack $T
// CHECK: mark_unresolved_move_addr [[BOX]] to [[STACK]]
//
// CHECK: } // end sil function '$s7consume18testAddressOnlyLetyyxmAA1PRzlF'
func testAddressOnlyLet<T : P>(_ t: T.Type) {
  let k = T.value
  let _ = consume k
}

// CHECK-LABEL: sil hidden [ossa] @$s7consume18testAddressOnlyVaryyxmAA1PRzlF : $@convention(thin) <T where T : P> (@thick T.Type) -> () {
// CHECK: [[BOX:%.*]] = alloc_box $
// CHECK: [[BORROW_BOX:%.*]] = begin_borrow [lexical] [[BOX]]
// CHECK: [[PROJECT:%.*]] = project_box [[BORROW_BOX]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [modify] [unknown] [[PROJECT]]
// CHECK: copy_addr [take] {{%.*}} to [[ACCESS]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: [[ACCESS:%.*]] = begin_access [modify] [unknown] [[PROJECT]]
// CHECK: [[STACK:%.*]] = alloc_stack $
// CHECK: mark_unresolved_move_addr [[ACCESS]] to [[STACK]]
// CHECK: end_access [[ACCESS]]
//
// CHECK: } // end sil function '$s7consume18testAddressOnlyVaryyxmAA1PRzlF'
func testAddressOnlyVar<T : P>(_ t: T.Type) {
  var k = T.value
  k = T.value
  let _ = consume k
}
