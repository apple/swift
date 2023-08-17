// RUN: %target-swift-emit-silgen -module-name switch %s | %FileCheck %s

//////////////////
// Declarations //
//////////////////

func markUsed<T>(_ t: T) {}

// TODO: Implement tuple equality in the library.
// BLOCKED: <rdar://problem/13822406>
func ~= (x: (Int, Int), y: (Int, Int)) -> Bool {
  return x.0 == y.0 && x.1 == y.1
}

// Some fake predicates for pattern guards.
func runced() -> Bool { return true }
func funged() -> Bool { return true }
func ansed() -> Bool { return true }

func foo() -> Int { return 0 }
func bar() -> Int { return 0 }
func foobar() -> (Int, Int) { return (0, 0) }

func a() {}
func b() {}
func c() {}
func d() {}
func e() {}
func f() {}
func g() {}

func a(_ k: Klass) {}
func b(_ k: Klass) {}
func c(_ k: Klass) {}
func d(_ k: Klass) {}
func e(_ k: Klass) {}
func f(_ k: Klass) {}
func g(_ k: Klass) {}

class Klass {
  var isTrue: Bool { return true }
  var isFalse: Bool { return false }
}

enum TrivialSingleCaseEnum {
case a
}

enum NonTrivialSingleCaseEnum {
case a(Klass)
}

enum MultipleNonTrivialCaseEnum {
case a(Klass)
case b(Klass)
case c(Klass)
}

enum MultipleAddressOnlyCaseEnum<T : BinaryInteger> {
case a(T)
case b(T)
case c(T)
}

///////////
// Tests //
///////////

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test1yyF
func test1() {
  switch foo() {
  // CHECK:   function_ref @$s6switch3fooSiyF
  case _:
  // CHECK:   function_ref @$s6switch1ayyF
    a()
  }
  // CHECK:   function_ref @$s6switch1byyF
  b()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test2yyF
func test2() {
  switch foo() {
  // CHECK:   function_ref @$s6switch3fooSiyF
  case _:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()
  case _: // The second case is unreachable.
    b()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1cyyF
  c()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test3yyF
func test3() {
  switch foo() {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch6runcedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[CASE2:bb[0-9]+]]

  case _ where runced():
  // CHECK: [[CASE1]]:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  case _:
  // CHECK: [[CASE2]]:
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1cyyF
  c()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test4yyF
func test4() {
  switch (foo(), bar()) {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch3barSiyF
  case _:
  // CHECK:   function_ref @$s6switch1ayyF
    a()
  }
  // CHECK:   function_ref @$s6switch1byyF
  b()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test5yyF
func test5() {
  switch (foo(), bar()) {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   function_ref @$s6switch6runcedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NOT_CASE1:bb[0-9]+]]
  case _ where runced():
  // CHECK: [[CASE1]]:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[NOT_CASE1]]:
  // CHECK:   function_ref @$s6switch6fungedSbyF
  // CHECK:   cond_br {{%.*}}, [[YES_CASE2:bb[0-9]+]], [[NOT_CASE2:bb[0-9]+]]
  // CHECK: [[YES_CASE2]]:
  case (_, _) where funged():
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()

  case _:
  // CHECK: [[NOT_CASE2]]:
  // CHECK:   function_ref @$s6switch1cyyF
    c()
  }
  // CHECK:   function_ref @$s6switch1dyyF
  d()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test6yyF
func test6() {
  switch (foo(), bar()) {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch3barSiyF
  case (_, _):
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()
  case (_, _): // The second case is unreachable.
    b()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1cyyF
  c()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test7yyF
func test7() {
  switch (foo(), bar()) {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   function_ref @$s6switch6runcedSbyF
  // CHECK:   cond_br {{%.*}}, [[YES_CASE1:bb[0-9]+]], [[NOT_CASE1:bb[0-9]+]]
  // CHECK: [[YES_CASE1]]:
  case (_, _) where runced():
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  case (_, _):
  // CHECK: [[NOT_CASE1]]:
  // CHECK:   function_ref @$s6switch1byyF
    b()
  }
  c()
  // CHECK:   function_ref @$s6switch1cyyF
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test8yyF
func test8() {
  switch (foo(), bar()) {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   function_ref @$s6switch6foobarSi_SityF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NOT_CASE1:bb[0-9]+]]
  case foobar():
  // CHECK: [[CASE1]]:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[NOT_CASE1]]:
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NOT_CASE2:bb[0-9]+]]
  case (foo(), _):
  // CHECK: [[CASE2]]:
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()
  // CHECK: [[NOT_CASE2]]:
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE3_GUARD:bb[0-9]+]], [[NOT_CASE3:bb[0-9]+]]
  // CHECK: [[CASE3_GUARD]]:
  // CHECK:   function_ref @$s6switch6runcedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE3:bb[0-9]+]], [[NOT_CASE3_GUARD:bb[0-9]+]]
  case (_, bar()) where runced():
  // CHECK: [[CASE3]]:
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   br [[CONT]]
    c()
  // CHECK: [[NOT_CASE3_GUARD]]:
  // CHECK: [[NOT_CASE3]]:
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE4_GUARD_1:bb[0-9]+]], [[NOT_CASE4_1:bb[0-9]+]]
  // CHECK: [[CASE4_GUARD_1]]:
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE4_GUARD_2:bb[0-9]+]], [[NOT_CASE4_2:bb[0-9]+]]
  // CHECK: [[CASE4_GUARD_2]]:
  // CHECK:   function_ref @$s6switch6fungedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE4:bb[0-9]+]], [[NOT_CASE4_3:bb[0-9]+]]
  case (foo(), bar()) where funged():
  // CHECK: [[CASE4]]:
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   br [[CONT]]
    d()
  // CHECK: [[NOT_CASE4_3]]:
  // CHECK: [[NOT_CASE4_2]]:
  // CHECK: [[NOT_CASE4_1]]:
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE5_GUARD_1:bb[0-9]+]], [[NOT_CASE5:bb[0-9]+]]
  // CHECK: [[CASE5_GUARD_1]]:
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   cond_br {{%.*}}, [[YES_CASE5:bb[0-9]+]], [[NOT_CASE5:bb[0-9]+]]
  // CHECK: [[YES_CASE5]]:
  case (foo(), bar()):
  // CHECK:   function_ref @$s6switch1eyyF
  // CHECK:   br [[CONT]]
    e()
  // CHECK: [[NOT_CASE5]]:
  // CHECK:   br [[CASE6:bb[0-9]+]]
  case _:
  // CHECK: [[CASE6]]:
  // CHECK:   function_ref @$s6switch1fyyF
  // CHECK:   br [[CONT]]
    f()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1gyyF
  g()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch5test9yyF
func test9() {
  switch (foo(), bar()) {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   cond_br {{%.*}}, [[YES_CASE1:bb[0-9]+]], [[NOT_CASE1:bb[0-9]+]]
  // CHECK: [[YES_CASE1]]:
  case (foo(), _):
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[NOT_CASE1]]:
  // CHECK:   function_ref @$s6switch6foobarSi_SityF
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NOT_CASE2:bb[0-9]+]]
  case foobar():
  // CHECK: [[CASE2]]:
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()

  case _:
  // CHECK: [[NOT_CASE2]]:
  // CHECK:   function_ref @$s6switch1cyyF
    c()
  }
  // CHECK:   function_ref @$s6switch1dyyF
  d()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch6test10yyF
func test10() {
  switch (foo(), bar()) {
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   cond_br {{%.*}}, [[YES_CASE1:bb[0-9]+]], [[NOT_CASE1:bb[0-9]+]]
  // CHECK: [[YES_CASE1]]:
  case (foo()...bar(), _):
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  case _:
  // CHECK: [[NOT_CASE1]]:
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1cyyF
  c()
}

protocol P { func p() }

struct X : P { func p() {} }
struct Y : P { func p() {} }
struct Z : P { func p() {} }

// CHECK-LABEL: sil hidden [ossa] @$s6switch10test_isa_11pyAA1P_p_tF
func test_isa_1(p: P) {
  // CHECK: [[PTMPBUF:%[0-9]+]] = alloc_stack $any P
  // CHECK-NEXT: copy_addr %0 to [init] [[PTMPBUF]] : $*any P
  switch p {
    // CHECK: [[TMPBUF:%[0-9]+]] = alloc_stack $X
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[P:%.*]] : $*any P to X in [[TMPBUF]] : $*X, [[IS_X:bb[0-9]+]], [[IS_NOT_X:bb[0-9]+]]

  case is X:
  // CHECK: [[IS_X]]:
  // CHECK-NEXT: load [trivial] [[TMPBUF]]
  // CHECK-NEXT: // function_ref
  // CHECK-NEXT: [[FUNC:%.*]] = function_ref @$s6switch1ayyF
  // CHECK-NEXT: apply [[FUNC]]()
  // CHECK-NEXT: dealloc_stack [[TMPBUF]]
  // CHECK-NEXT: destroy_addr [[PTMPBUF]]
  // CHECK-NEXT: dealloc_stack [[PTMPBUF]]
    a()
    // CHECK:   br [[CONT:bb[0-9]+]]

  // CHECK: [[IS_NOT_X]]:
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[P]] : $*any P to Y in {{%.*}} : $*Y, [[IS_Y:bb[0-9]+]], [[IS_NOT_Y:bb[0-9]+]]

// CHECK: [[IS_Y]]:
  case is Y:
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[Y_CONT:bb[0-9]+]]
    b()

  // CHECK: [[IS_NOT_Y]]:
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[P]] : $*any P to Z in {{%.*}} : $*Z, [[IS_Z:bb[0-9]+]], [[IS_NOT_Z:bb[0-9]+]]

  // CHECK: [[IS_Z]]:
  case is Z:
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   br [[Z_CONT:bb[0-9]+]]
    c()

  // CHECK: [[IS_NOT_Z]]:
  case _:
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   br [[CONT]]
    d()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1eyyF
  e()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch10test_isa_21pyAA1P_p_tF
func test_isa_2(p: P) {
  switch (p, foo()) {
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[P:%.*]] : $*any P to X in {{%.*}} : $*X, [[IS_X:bb[0-9]+]], [[IS_NOT_X:bb[0-9]+]]

  // CHECK: [[IS_X]]:
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NOT_CASE1:bb[0-9]+]]
  case (is X, foo()):
  // CHECK: [[CASE1]]:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[IS_NOT_X]]:
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[P]] : $*any P to Y in {{%.*}} : $*Y, [[IS_Y:bb[0-9]+]], [[IS_NOT_Y:bb[0-9]+]]

  // CHECK: [[IS_Y]]:
  // CHECK:   function_ref @$s6switch3fooSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NOT_CASE2:bb[0-9]+]]
  case (is Y, foo()):
  // CHECK: [[CASE2]]:
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()

  // CHECK: [[NOT_CASE2]]:
  // CHECK: [[IS_NOT_Y]]:

  // CHECK:   checked_cast_addr_br copy_on_success any P in [[P:%.*]] : $*any P to X in {{%.*}} : $*X, [[CASE3:bb[0-9]+]], [[IS_NOT_X:bb[0-9]+]]

  case (is X, _):
  // CHECK: [[CASE3]]:
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   br [[CONT]]
    c()

  // -- case (is Y, foo()):
  // CHECK: [[IS_NOT_X]]:
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[P]] : $*any P to Y in {{%.*}} : $*Y, [[IS_Y:bb[0-9]+]], [[IS_NOT_Y:bb[0-9]+]]
  // CHECK: [[IS_Y]]:
  // CHECK:   function_ref @$s6switch3barSiyF
  // CHECK:   cond_br {{%.*}}, [[CASE4:bb[0-9]+]], [[NOT_CASE4:bb[0-9]+]]
  case (is Y, bar()):
  // CHECK: [[CASE4]]:
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   br [[CONT]]
    d()

  // CHECK: [[NOT_CASE4]]:
  // CHECK:   br [[CASE5:bb[0-9]+]]
  // CHECK: [[IS_NOT_Y]]:
  // CHECK:   br [[CASE5]]
  case _:
  // CHECK: [[CASE5]]:
  // CHECK:   function_ref @$s6switch1eyyF
  // CHECK:   br [[CONT]]
    e()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1fyyF
  f()
}

class B {}
class C : B {}
class D1 : C {}
class D2 : D1 {}
class E : C {}

// CHECK-LABEL: sil hidden [ossa] @$s6switch16test_isa_class_11xyAA1BC_tF : $@convention(thin) (@guaranteed B) -> () {
func test_isa_class_1(x: B) {
  // CHECK: bb0([[X:%.*]] : @guaranteed $B):
  // CHECK:   checked_cast_br B in [[X]] : $B to D1, [[IS_D1:bb[0-9]+]], [[IS_NOT_D1:bb[0-9]+]]
  switch x {

  // CHECK: [[IS_D1]]([[CAST_D1:%.*]] : @guaranteed $D1):
  // CHECK:   [[CAST_D1_COPY:%.*]] = copy_value [[CAST_D1]]
  // CHECK:   function_ref @$s6switch6runcedSbyF : $@convention(thin) () -> Bool
  // CHECK:   cond_br {{%.*}}, [[YES_CASE1:bb[0-9]+]], [[NO_CASE1:bb[0-9]+]]

  // CHECK: [[YES_CASE1]]:
  case is D1 where runced():
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   destroy_value [[CAST_D1_COPY]]
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[NO_CASE1]]:
  // CHECK-NEXT:   destroy_value [[CAST_D1_COPY]]
  // CHECK:   br [[NEXT_CASE:bb[0-9]+]]

  // CHECK: [[IS_NOT_D1]]([[CASTFAIL_D1:%.*]] : @guaranteed $B):
  // CHECK-NEXT:   br [[NEXT_CASE]]

  // CHECK: [[NEXT_CASE]]:
  // CHECK:   checked_cast_br B in [[X]] : $B to D2, [[IS_D2:bb[0-9]+]], [[IS_NOT_D2:bb[0-9]+]]
  case is D2:
  // CHECK: [[IS_D2]]([[CAST_D2:%.*]] : @guaranteed $D2):
  // CHECK:   [[CAST_D2_COPY:%.*]] = copy_value [[CAST_D2]]
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   destroy_value [[CAST_D2_COPY]]
  // CHECK:   br [[CONT]]
    b()

  // CHECK: [[IS_NOT_D2]]([[CASTFAIL_D2:%.*]] : @guaranteed $B):
  // CHECK:   checked_cast_br B in [[X]] : $B to E, [[IS_E:bb[0-9]+]], [[IS_NOT_E:bb[0-9]+]]
  case is E where funged():
  // CHECK: [[IS_E]]([[CAST_E:%.*]] : @guaranteed $E):
  // CHECK:   [[CAST_E_COPY:%.*]] = copy_value [[CAST_E]]
  // CHECK:   function_ref @$s6switch6fungedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE3:bb[0-9]+]], [[NO_CASE3:bb[0-9]+]]

  // CHECK: [[CASE3]]:
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   destroy_value [[CAST_E_COPY]]
  // CHECK:   br [[CONT]]
    c()

  // CHECK: [[NO_CASE3]]:
  // CHECK-NEXT:   destroy_value [[CAST_E_COPY]]
  // CHECK:   br [[NEXT_CASE:bb[0-9]+]]

  // CHECK: [[IS_NOT_E]]([[NOTCAST_E:%.*]] : @guaranteed $B):
  // CHECK:   br [[NEXT_CASE]]

  // CHECK: [[NEXT_CASE]]:
  // CHECK:   checked_cast_br B in [[X]] : $B to C, [[IS_C:bb[0-9]+]], [[IS_NOT_C:bb[0-9]+]]

  case is C:
  // CHECK: [[IS_C]]([[CAST_C:%.*]] : @guaranteed $C):
  // CHECK:   [[CAST_C_COPY:%.*]] = copy_value [[CAST_C]]
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK-NEXT: apply
  // CHECK:   destroy_value [[CAST_C_COPY]]
  // CHECK:   br [[CONT]]
    d()

  // CHECK: [[IS_NOT_C]]([[NOCAST_C:%.*]] : @guaranteed $B):
  default:
  // CHECK:    function_ref @$s6switch1eyyF
  // CHECK:    br [[CONT]]
    e()
  }
  // CHECK: [[CONT]]:
  // CHECK:   [[F_FUNC:%.*]] = function_ref @$s6switch1fyyF : $@convention(thin) () -> ()
  // CHECK:   apply [[F_FUNC]]()
  f()
}
// CHECK: } // end sil function '$s6switch16test_isa_class_11xyAA1BC_tF'

// CHECK-LABEL: sil hidden [ossa] @$s6switch16test_isa_class_21xyXlAA1BC_tF : $@convention(thin)
func test_isa_class_2(x: B) -> AnyObject {
  // CHECK: bb0([[X:%.*]] : @guaranteed $B):
  switch x {

  // CHECK:   checked_cast_br B in [[X]] : $B to D1, [[IS_D1:bb[0-9]+]], [[IS_NOT_D1:bb[0-9]+]]
  case let y as D1 where runced():
  // CHECK: [[IS_D1]]([[CAST_D1:%.*]] : @guaranteed $D1):
  // CHECK:   [[CAST_D1_COPY:%.*]] = copy_value [[CAST_D1]]
  // CHECK:   [[BORROWED_CAST_D1_COPY:%.*]] = begin_borrow [lexical] [[CAST_D1_COPY]]
  // CHECK:   function_ref @$s6switch6runcedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NO_CASE1:bb[0-9]+]]

  // CHECK: [[CASE1]]:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   [[CAST_D1_COPY_COPY:%.*]] = copy_value [[BORROWED_CAST_D1_COPY]]
  // CHECK:   [[RET:%.*]] = init_existential_ref [[CAST_D1_COPY_COPY]]
  // CHECK:   end_borrow [[BORROWED_CAST_D1_COPY]]
  // CHECK:   destroy_value [[CAST_D1_COPY]]
  // CHECK:   br [[CONT:bb[0-9]+]]([[RET]] : $AnyObject)
    a()
    return y

  // CHECK: [[NO_CASE1]]:
  // CHECK:   destroy_value [[CAST_D1_COPY]]
  // CHECK:   br [[NEXT_CASE:bb5]]

  // CHECK: [[IS_NOT_D1]]([[NOCAST_D1:%.*]] : @guaranteed $B):
  // CHECK:   br [[NEXT_CASE]]

  // CHECK: [[NEXT_CASE]]:
  // CHECK:   checked_cast_br B in [[X]] : $B to D2, [[CASE2:bb[0-9]+]], [[IS_NOT_D2:bb[0-9]+]]
  case let y as D2:
  // CHECK: [[CASE2]]([[CAST_D2:%.*]] : @guaranteed $D2):
  // CHECK:   [[CAST_D2_COPY:%.*]] = copy_value [[CAST_D2]]
  // CHECK:   [[BORROWED_CAST_D2_COPY:%.*]] = begin_borrow [lexical] [[CAST_D2_COPY]]
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   [[CAST_D2_COPY_COPY:%.*]] = copy_value [[BORROWED_CAST_D2_COPY]]
  // CHECK:   [[RET:%.*]] = init_existential_ref [[CAST_D2_COPY_COPY]]
  // CHECK:   end_borrow [[BORROWED_CAST_D2_COPY]]
  // CHECK:   destroy_value [[CAST_D2_COPY]]
  // CHECK:   br [[CONT]]([[RET]] : $AnyObject)
    b()
    return y

  // CHECK: [[IS_NOT_D2]]([[NOCAST_D2:%.*]] : @guaranteed $B):
  // CHECK:   checked_cast_br B in [[X]] : $B to E, [[IS_E:bb[0-9]+]], [[IS_NOT_E:bb[0-9]+]]
  case let y as E where funged():
  // CHECK: [[IS_E]]([[CAST_E:%.*]] : @guaranteed $E):
  // CHECK:   [[CAST_E_COPY:%.*]] = copy_value [[CAST_E]]
  // CHECK:   [[BORROWED_CAST_E_COPY:%.*]] = begin_borrow [lexical] [[CAST_E_COPY]]
  // CHECK:   function_ref @$s6switch6fungedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE3:bb[0-9]+]], [[NO_CASE3:bb[0-9]+]]

  // CHECK: [[CASE3]]:
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   [[CAST_E_COPY_COPY:%.*]] = copy_value [[BORROWED_CAST_E_COPY]]
  // CHECK:   [[RET:%.*]] = init_existential_ref [[CAST_E_COPY_COPY]]
  // CHECK:   end_borrow [[BORROWED_CAST_E_COPY]]
  // CHECK:   destroy_value [[CAST_E_COPY]]
  // CHECK:   br [[CONT]]([[RET]] : $AnyObject)
    c()
    return y

  // CHECK: [[NO_CASE3]]:
  // CHECK:    destroy_value [[CAST_E_COPY]]
  // CHECK:   br [[NEXT_CASE:bb[0-9]+]]

  // CHECK: [[IS_NOT_E]]([[NOCAST_E:%.*]] : @guaranteed $B):
  // CHECK:   br [[NEXT_CASE]]

  // CHECK: [[NEXT_CASE]]
  // CHECK:   checked_cast_br B in [[X]] : $B to C, [[CASE4:bb[0-9]+]], [[IS_NOT_C:bb[0-9]+]]
  case let y as C:
  // CHECK: [[CASE4]]([[CAST_C:%.*]] : @guaranteed $C):
  // CHECK:   [[CAST_C_COPY:%.*]] = copy_value [[CAST_C]]
  // CHECK:   [[BORROWED_CAST_C_COPY:%.*]] = begin_borrow [lexical] [[CAST_C_COPY]]
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   [[CAST_C_COPY_COPY:%.*]] = copy_value [[BORROWED_CAST_C_COPY]]
  // CHECK:   [[RET:%.*]] = init_existential_ref [[CAST_C_COPY_COPY]]
  // CHECK:   end_borrow [[BORROWED_CAST_C_COPY]]
  // CHECK:   destroy_value [[CAST_C_COPY]]
  // CHECK:   br [[CONT]]([[RET]] : $AnyObject)
    d()
    return y

  // CHECK: [[IS_NOT_C]]([[NOCAST_C:%.*]] : @guaranteed $B):
  default:
  // CHECK:   function_ref @$s6switch1eyyF
  // CHECK:   [[X_COPY_2:%.*]] = copy_value [[X]]
  // CHECK:   [[RET:%.*]] = init_existential_ref [[X_COPY_2]]
  // CHECK:   br [[CONT]]([[RET]] : $AnyObject)
    e()
    return x
  }

  // CHECK: [[CONT]]([[T0:%.*]] : @owned $AnyObject):
  // CHECK:   return [[T0]]
}
// CHECK: } // end sil function '$s6switch16test_isa_class_21xyXlAA1BC_tF'

// https://github.com/apple/swift/issues/56139

// CHECK-LABEL: sil hidden [ossa] @$s6switch31test_isa_pattern_array_downcast2psySayAA1P_pG_tF : $@convention(thin) (@guaranteed Array<any P>) -> () {
func test_isa_pattern_array_downcast(ps: Array<P>) {
  // CHECK: bb0(%0 : @guaranteed $Array<any P>):
  switch ps {
  // CHECK: checked_cast_addr_br copy_on_success Array<any P> in [[P:%[0-9]+]] : $*Array<any P> to Array<X> in {{%[0-9]+}} : $*Array<X>, [[IS_X:bb[0-9]+]], [[IS_NOT_X:bb[0-9]+]]
  case is [X]:
    // CHECK: [[IS_X]]:
    // CHECK: function_ref @$s6switch1ayyF
    a()
    // CHECK: br [[CONT:bb[0-9]+]]

  // CHECK: [[IS_NOT_X]]:
  // CHECK: [[DEST:%[0-9]+]] = alloc_stack $Array<Y>
  // CHECK-NEXT: checked_cast_addr_br copy_on_success Array<any P> in [[P:%[0-9]+]] : $*Array<any P> to Array<Y> in [[DEST]] : $*Array<Y>, [[IS_Y:bb[0-9]+]], [[IS_NOT_Y:bb[0-9]+]]
  case let _ as [Y]:
    // CHECK: [[IS_Y]]:
    // CHECK-NEXT: load [take] [[DEST]] : $*Array<Y>
    // CHECK: function_ref @$s6switch1byyF
    b()
    // CHECK: br [[CONT]]
  default:
    // CHECK: [[IS_NOT_Y]]:
    // CHECK: function_ref @$s6switch1cyyF
    c()
    // CHECK: br [[CONT]]
  }
  // CHECK: [[CONT]]:
  // CHECK: function_ref @$s6switch1dyyF
  d()
}
// CHECK: } // end sil function '$s6switch31test_isa_pattern_array_downcast2psySayAA1P_pG_tF'

// CHECK-LABEL: sil hidden [ossa] @$s6switch39test_isa_pattern_array_downcast_closureyyF : $@convention(thin) () -> () {
// CHECK: function_ref @$s6switch39test_isa_pattern_array_downcast_closureyyFySayAA1P_pGcfU_
// CHECK: } // end sil function '$s6switch39test_isa_pattern_array_downcast_closureyyF'
func test_isa_pattern_array_downcast_closure() {
// CHECK-LABEL: sil private [ossa] @$s6switch39test_isa_pattern_array_downcast_closureyyFySayAA1P_pGcfU_ : $@convention(thin) (@guaranteed Array<any P>) -> () {
  let _ = { (ps: [P]) -> Void in
    // CHECK: bb0(%0 : @guaranteed $Array<any P>):
    switch ps {
    // CHECK: [[DEST:%[0-9]+]] = alloc_stack $Array<X>
    // CHECK-NEXT: checked_cast_addr_br copy_on_success Array<any P> in [[P:%[0-9]+]] : $*Array<any P> to Array<X> in [[DEST]] : $*Array<X>, [[IS_X:bb[0-9]+]], [[IS_NOT_X:bb[0-9]+]]
    case let _ as [X]:
      // CHECK: [[IS_X]]:
      // CHECK-NEXT: load [take] [[DEST]] : $*Array<X>
      // CHECK: function_ref @$s6switch1ayyF
      a()
      // CHECK: br [[CONT:bb[0-9]+]]

    // CHECK: [[IS_NOT_X]]:
    // CHECK: br [[DEF:bb[0-9]+]]
    default:
      // CHECK: [[DEF]]:
      // CHECK: function_ref @$s6switch1byyF
      b()
      // CHECK: br [[CONT]]
    }
  }
  // CHECK: } // end sil function '$s6switch39test_isa_pattern_array_downcast_closureyyFySayAA1P_pGcfU_'
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch30test_isa_pattern_dict_downcast2psySDySSAA1P_pG_tF : $@convention(thin) (@guaranteed Dictionary<String, any P>) -> () {
func test_isa_pattern_dict_downcast(ps: Dictionary<String, P>) {
  // CHECK: bb0(%0 : @guaranteed $Dictionary<String, any P>):
  switch ps {
  // CHECK: checked_cast_addr_br copy_on_success Dictionary<String, any P> in [[P:%[0-9]+]] : $*Dictionary<String, any P> to Dictionary<String, X> in {{%[0-9]+}} : $*Dictionary<String, X>, [[IS_X:bb[0-9]+]], [[IS_NOT_X:bb[0-9]+]]
  case is [String : X]:
    // CHECK: [[IS_X]]:
    // CHECK: function_ref @$s6switch1ayyF
    a()
    // CHECK: br [[CONT:bb[0-9]+]]

  // CHECK: [[IS_NOT_X]]:
  // CHECK: [[DEST:%[0-9]+]] = alloc_stack $Dictionary<String, Y>
  // CHECK-NEXT: checked_cast_addr_br copy_on_success Dictionary<String, any P> in [[P:%[0-9]+]] : $*Dictionary<String, any P> to Dictionary<String, Y> in [[DEST]] : $*Dictionary<String, Y>, [[IS_Y:bb[0-9]+]], [[IS_NOT_Y:bb[0-9]+]]
  case let _ as [String : Y]:
    // CHECK: [[IS_Y]]:
    // CHECK-NEXT: load [take] [[DEST]] : $*Dictionary<String, Y>
    // CHECK: function_ref @$s6switch1byyF
    b()
    // CHECK: br [[CONT]]
  default:
    // CHECK: [[IS_NOT_Y]]:
    // CHECK: function_ref @$s6switch1cyyF
    c()
    // CHECK: br [[CONT]]
  }
  // CHECK: [[CONT]]:
  // CHECK: function_ref @$s6switch1dyyF
  d()
}
// CHECK-LABEL: } // end sil function '$s6switch30test_isa_pattern_dict_downcast2psySDySSAA1P_pG_tF'

// CHECK-LABEL: sil hidden [ossa] @$s6switch29test_isa_pattern_set_downcast2psyShyxG_tSHRzlF : $@convention(thin) <T where T : Hashable> (@guaranteed Set<T>) -> () {
func test_isa_pattern_set_downcast<T: Hashable>(ps: Set<T>) {
  // CHECK: bb0(%0 : @guaranteed $Set<T>):
  switch ps {
  // CHECK: checked_cast_addr_br copy_on_success Set<T> in [[P:%[0-9]+]] : $*Set<T> to Set<Int> in {{%[0-9]+}} : $*Set<Int>, [[IS_INT:bb[0-9]+]], [[IS_NOT_INT:bb[0-9]+]]
  case is Set<Int>:
    // CHECK: [[IS_INT]]:
    // CHECK: function_ref @$s6switch1ayyF
    a()
    // CHECK: br [[CONT:bb[0-9]+]]

  // CHECK: [[IS_NOT_INT]]:
  // CHECK: [[DEST:%[0-9]+]] = alloc_stack $Set<Bool>
  // CHECK-NEXT: checked_cast_addr_br copy_on_success Set<T> in [[P:%[0-9]+]] : $*Set<T> to Set<Bool> in [[DEST]] : $*Set<Bool>, [[IS_BOOL:bb[0-9]+]], [[IS_NOT_BOOL:bb[0-9]+]]
  case let _ as Set<Bool>:
    // CHECK: [[IS_BOOL]]:
    // CHECK-NEXT: load [take] [[DEST]] : $*Set<Bool>
    // CHECK: function_ref @$s6switch1byyF
    b()
    // CHECK: br [[CONT]]
  default:
    // CHECK: [[IS_NOT_BOOL]]:
    // CHECK: function_ref @$s6switch1cyyF
    c()
    // CHECK: br [[CONT]]
  }
  // CHECK: [[CONT]]:
  // CHECK: function_ref @$s6switch1dyyF
  d()
}
// CHECK: } // end sil function '$s6switch29test_isa_pattern_set_downcast2psyShyxG_tSHRzlF'

enum MaybePair {
  case Neither
  case Left(Int)
  case Right(String)
  case Both(Int, String)
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch12test_union_11uyAA9MaybePairO_tF
func test_union_1(u: MaybePair) {
  switch u {
  // CHECK: switch_enum [[SUBJECT:%.*]] : $MaybePair,
  // CHECK:   case #MaybePair.Neither!enumelt: [[IS_NEITHER:bb[0-9]+]],
  // CHECK:   case #MaybePair.Left!enumelt: [[IS_LEFT:bb[0-9]+]],
  // CHECK:   case #MaybePair.Right!enumelt: [[IS_RIGHT:bb[0-9]+]],
  // CHECK:   case #MaybePair.Both!enumelt: [[IS_BOTH:bb[0-9]+]]

  // CHECK: [[IS_NEITHER]]:
  // CHECK-NOT: destroy_value
  case .Neither:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[IS_LEFT]]({{%.*}}):
  // CHECK-NOT: destroy_value
  case (.Left):
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()

  // CHECK: [[IS_RIGHT]]([[STR:%.*]] : @guaranteed $String):
  case var .Right:
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   br [[CONT]]
    c()

  // CHECK: [[IS_BOTH]]([[TUP:%.*]] : @guaranteed $(Int, String)):
  case .Both:
  // CHECK:   ({{%.*}}, [[TUP_STR:%.*]]) = destructure_tuple [[TUP]]
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   br [[CONT]]
    d()
  }

  // CHECK: [[CONT]]:
  // CHECK-NOT: switch_enum [[SUBJECT]]
  // CHECK:   function_ref @$s6switch1eyyF
  e()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch12test_union_31uyAA9MaybePairO_tF : $@convention(thin) (@guaranteed MaybePair) -> () {
func test_union_3(u: MaybePair) {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $MaybePair):
  // CHECK:   switch_enum [[ARG]] : $MaybePair,
  // CHECK:     case #MaybePair.Neither!enumelt: [[IS_NEITHER:bb[0-9]+]],
  // CHECK:     case #MaybePair.Left!enumelt: [[IS_LEFT:bb[0-9]+]],
  // CHECK:     case #MaybePair.Right!enumelt: [[IS_RIGHT:bb[0-9]+]],
  // CHECK:     default [[DEFAULT:bb[0-9]+]]
  switch u {
  // CHECK: [[IS_NEITHER]]:
  case .Neither:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[IS_LEFT]]({{%.*}}):
  case .Left:
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()

  // CHECK: [[IS_RIGHT]]([[STR:%.*]] : @guaranteed $String):
  case .Right:
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   br [[CONT]]
    c()

  // CHECK: [[DEFAULT]](
  // -- Ensure the fully-opaque value is destroyed in the default case.
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   br [[CONT]]

  default:
    d()
  }

  // CHECK: [[CONT]]:
  // CHECK-NOT: switch_enum [[ARG]]
  // CHECK:   function_ref @$s6switch1eyyF
  e()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch12test_union_41uyAA9MaybePairO_tF
func test_union_4(u: MaybePair) {
  switch u {
  // CHECK: switch_enum {{%.*}} : $MaybePair,
  // CHECK:   case #MaybePair.Neither!enumelt: [[IS_NEITHER:bb[0-9]+]],
  // CHECK:   case #MaybePair.Left!enumelt: [[IS_LEFT:bb[0-9]+]],
  // CHECK:   case #MaybePair.Right!enumelt: [[IS_RIGHT:bb[0-9]+]],
  // CHECK:   case #MaybePair.Both!enumelt: [[IS_BOTH:bb[0-9]+]]

  // CHECK: [[IS_NEITHER]]:
  case .Neither:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[IS_LEFT]]({{%.*}}):
  case .Left(_):
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()

  // CHECK: [[IS_RIGHT]]({{%.*}}):
  case .Right(_):
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   br [[CONT]]
    c()

  // CHECK: [[IS_BOTH]]({{%.*}}):
  case .Both(_):
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   br [[CONT]]
    d()
  }

  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1eyyF
  e()
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch12test_union_51uyAA9MaybePairO_tF
func test_union_5(u: MaybePair) {
  switch u {
  // CHECK: switch_enum {{%.*}} : $MaybePair,
  // CHECK:   case #MaybePair.Neither!enumelt: [[IS_NEITHER:bb[0-9]+]],
  // CHECK:   case #MaybePair.Left!enumelt: [[IS_LEFT:bb[0-9]+]],
  // CHECK:   case #MaybePair.Right!enumelt: [[IS_RIGHT:bb[0-9]+]],
  // CHECK:   case #MaybePair.Both!enumelt: [[IS_BOTH:bb[0-9]+]]

  // CHECK: [[IS_NEITHER]]:
  case .Neither:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[IS_LEFT]]({{%.*}}):
  case .Left(_):
  // CHECK:   function_ref @$s6switch1byyF
  // CHECK:   br [[CONT]]
    b()

  // CHECK: [[IS_RIGHT]]({{%.*}}):
  case .Right(_):
  // CHECK:   function_ref @$s6switch1cyyF
  // CHECK:   br [[CONT]]
    c()

  // CHECK: [[IS_BOTH]]({{%.*}}):
  case .Both(_, _):
  // CHECK:   function_ref @$s6switch1dyyF
  // CHECK:   br [[CONT]]
    d()
  }

  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1eyyF
  e()
}

enum MaybeAddressOnlyPair {
  case Neither
  case Left(P)
  case Right(String)
  case Both(P, String)
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch22test_union_addr_only_11uyAA20MaybeAddressOnlyPairO_tF
func test_union_addr_only_1(u: MaybeAddressOnlyPair) {
  switch u {
  // CHECK: switch_enum_addr [[ENUM_ADDR:%.*]] : $*MaybeAddressOnlyPair,
  // CHECK:   case #MaybeAddressOnlyPair.Neither!enumelt: [[IS_NEITHER:bb[0-9]+]],
  // CHECK:   case #MaybeAddressOnlyPair.Left!enumelt: [[IS_LEFT:bb[0-9]+]],
  // CHECK:   case #MaybeAddressOnlyPair.Right!enumelt: [[IS_RIGHT:bb[0-9]+]],
  // CHECK:   case #MaybeAddressOnlyPair.Both!enumelt: [[IS_BOTH:bb[0-9]+]]

  // CHECK: [[IS_NEITHER]]:
  case .Neither:
  // CHECK:   function_ref @$s6switch1ayyF
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()

  // CHECK: [[IS_LEFT]]:
  // CHECK:   [[P:%.*]] = unchecked_take_enum_data_addr [[ENUM_ADDR]] : $*MaybeAddressOnlyPair, #MaybeAddressOnlyPair.Left!enumelt
  case .Left(_):
  // CHECK:   [[FUNC:%.*]] = function_ref @$s6switch1byyF
  // CHECK-NEXT: apply [[FUNC]](
  // CHECK:   destroy_addr [[P]]
  // CHECK:   br [[CONT]]
    b()

  // CHECK: [[IS_RIGHT]]:
  // CHECK:   [[STR_ADDR:%.*]] = unchecked_take_enum_data_addr [[ENUM_ADDR]] : $*MaybeAddressOnlyPair, #MaybeAddressOnlyPair.Right!enumelt
  // CHECK:   [[STR:%.*]] = load [take] [[STR_ADDR]]
  case .Right(_):
  // CHECK:   [[FUNC:%.*]] = function_ref @$s6switch1cyyF
  // CHECK:   apply [[FUNC]](
  // CHECK:   destroy_value [[STR]] : $String
  // CHECK:   br [[CONT]]
    c()

  // CHECK: [[IS_BOTH]]:
  // CHECK:   [[P_STR_TUPLE:%.*]] = unchecked_take_enum_data_addr [[ENUM_ADDR]] : $*MaybeAddressOnlyPair, #MaybeAddressOnlyPair.Both!enumelt
  case .Both(_):
  // CHECK:   [[FUNC:%.*]] = function_ref @$s6switch1dyyF
  // CHECK-NEXT: apply [[FUNC]](
  // CHECK:   destroy_addr [[P_STR_TUPLE]]
  // CHECK:   br [[CONT]]
    d()
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s6switch1eyyF
  e()
}

enum Generic<T, U> {
  case Foo(T)
  case Bar(U)
}

// Check that switching over a generic instance generates verified SIL.
func test_union_generic_instance(u: Generic<Int, String>) {
  switch u {
  case .Foo(var x):
    a()
  case .Bar(var y):
    b()
  }
  c()
}

enum Foo { case A, B }

// CHECK-LABEL: sil hidden [ossa] @$s6switch05test_A11_two_unions1x1yyAA3FooO_AFtF
func test_switch_two_unions(x: Foo, y: Foo) {
  // CHECK:   [[T0:%.*]] = tuple (%0 : $Foo, %1 : $Foo)
  // CHECK:   ([[X:%.*]], [[Y:%.*]]) = destructure_tuple [[T0]]
  // CHECK:   switch_enum [[Y]] : $Foo, case #Foo.A!enumelt: [[IS_CASE1:bb[0-9]+]], default [[IS_NOT_CASE1:bb[0-9]+]]

  switch (x, y) {
  // CHECK: [[IS_CASE1]]:
  case (_, Foo.A):
  // CHECK:   function_ref @$s6switch1ayyF
    a()

  // CHECK: [[IS_NOT_CASE1]]:
  // CHECK:   switch_enum [[X]] : $Foo, case #Foo.B!enumelt: [[IS_CASE2:bb[0-9]+]], default [[IS_NOT_CASE2:bb[0-9]+]]
  // CHECK: [[IS_CASE2]]:
  case (Foo.B, _):
  // CHECK:   function_ref @$s6switch1byyF
    b()

  // CHECK: [[IS_NOT_CASE2]]:
  // CHECK:   switch_enum [[Y]] : $Foo, case #Foo.B!enumelt: [[IS_CASE3:bb[0-9]+]], default [[UNREACHABLE:bb[0-9]+]]
  // CHECK: [[IS_CASE3]]:
  case (_, Foo.B):
  // CHECK:   function_ref @$s6switch1cyyF
    c()

  // CHECK: [[UNREACHABLE]]:
  // CHECK:   unreachable
  }
}


// <rdar://problem/14826416>
func rdar14826416<T, U>(t: T, u: U) {
  switch t {
  case is Int: markUsed("Int")
  case is U: markUsed("U")
  case _: markUsed("other")
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s6switch12rdar14826416{{[_0-9a-zA-Z]*}}F
// CHECK:   checked_cast_addr_br copy_on_success T in {{%.*}} : $*T to Int in {{%.*}} : $*Int, [[IS_INT:bb[0-9]+]], [[ISNT_INT:bb[0-9]+]]
// CHECK: [[ISNT_INT]]:
// CHECK:   checked_cast_addr_br copy_on_success T in {{%.*}} : $*T to U in {{%.*}} : $*U, [[ISNT_INT_IS_U:bb[0-9]+]], [[ISNT_INT_ISNT_U:bb[0-9]+]]

// <rdar://problem/14835992>
class Rdar14835992 {}
class SubRdar14835992 : Rdar14835992 {}

// CHECK-LABEL: sil hidden [ossa] @$s6switch12rdar14835992{{[_0-9a-zA-Z]*}}F
func rdar14835992<T, U>(t: Rdar14835992, tt: T, uu: U) {
  switch t {
  case is SubRdar14835992: markUsed("Sub")
  case is T: markUsed("T")
  case is U: markUsed("U")
  case _: markUsed("other")
  }
}




// <rdar://problem/17272985>
enum ABC { case A, B, C }

// CHECK-LABEL: sil hidden [ossa] @$s6switch18testTupleWildcardsyyAA3ABCO_ADtF
// CHECK:         ([[X:%.*]], [[Y:%.*]]) = destructure_tuple {{%.*}} : $(ABC, ABC)
// CHECK:         switch_enum [[X]] : $ABC, case #ABC.A!enumelt: [[X_A:bb[0-9]+]], default [[X_NOT_A:bb[0-9]+]]
// CHECK:       [[X_A]]:
// CHECK:         function_ref @$s6switch1ayyF
// CHECK:       [[X_NOT_A]](
// CHECK:         switch_enum [[Y]] : $ABC, case #ABC.A!enumelt: [[Y_A:bb[0-9]+]], case #ABC.B!enumelt: [[Y_B:bb[0-9]+]], case #ABC.C!enumelt: [[Y_C:bb[0-9]+]]
// CHECK-NOT: default
// CHECK:       [[Y_A]]:
// CHECK:         function_ref @$s6switch1byyF
// CHECK:       [[Y_B]]:
// CHECK:         function_ref @$s6switch1cyyF
// CHECK:       [[Y_C]]:
// CHECK:         switch_enum [[X]] : $ABC, case #ABC.C!enumelt: [[X_C:bb[0-9]+]], default [[X_NOT_C:bb[0-9]+]]
// CHECK:       [[X_C]]:
// CHECK:         function_ref @$s6switch1dyyF
// CHECK:       [[X_NOT_C]](
// CHECK:         function_ref @$s6switch1eyyF
func testTupleWildcards(_ x: ABC, _ y: ABC) {
  switch (x, y) {
  case (.A, _):
    a()
  case (_, .A):
    b()
  case (_, .B):
    c()
  case (.C, .C):
    d()
  default:
    e()
  }
}

enum LabeledScalarPayload {
  case Payload(name: Int)
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch24testLabeledScalarPayloadyypAA0cdE0OF
func testLabeledScalarPayload(_ lsp: LabeledScalarPayload) -> Any {
  // CHECK: switch_enum {{%.*}}, case #LabeledScalarPayload.Payload!enumelt: bb1
  switch lsp {
  // CHECK: bb1([[TUPLE:%.*]] : $(name: Int)):
  // CHECK:   [[X:%.*]] = destructure_tuple [[TUPLE]]
  // CHECK:   [[ANY_X_ADDR:%.*]] = init_existential_addr {{%.*}}, $Int
  // CHECK:   store [[X]] to [trivial] [[ANY_X_ADDR]]
  case let .Payload(x):
    return x
  }
}

// There should be no unreachable generated.
// CHECK-LABEL: sil hidden [ossa] @$s6switch19testOptionalPatternyySiSgF
func testOptionalPattern(_ value : Int?) {
  // CHECK: switch_enum %0 : $Optional<Int>, case #Optional.some!enumelt: bb1, case #Optional.none!enumelt: [[NILBB:bb[0-9]+]]
  switch value {
  case 1?: a()
  case 2?: b()
  case nil: d()
  default: e()
  }
}


// x? and .none should both be considered "similar" and thus handled in the same
// switch on the enum kind.  There should be no unreachable generated.
// CHECK-LABEL: sil hidden [ossa] @$s6switch19testOptionalEnumMixyS2iSgF
func testOptionalEnumMix(_ a : Int?) -> Int {
  // CHECK: debug_value %0 : $Optional<Int>, let, name "a"
  // CHECK-NEXT: switch_enum %0 : $Optional<Int>, case #Optional.some!enumelt: [[SOMEBB:bb[0-9]+]], case #Optional.none!enumelt: [[NILBB:bb[0-9]+]]
  switch a {
  case let x?:
    return 0

  // CHECK: [[SOMEBB]]([[X:%.*]] : $Int):
  // CHECK-NEXT: debug_value [[X]] : $Int, let, name "x"
  // CHECK: integer_literal $Builtin.IntLiteral, 0

  case .none:
    return 42

  // CHECK: [[NILBB]]:
  // CHECK: integer_literal $Builtin.IntLiteral, 42
  }
}

// x? and nil should both be considered "similar" and thus handled in the same
// switch on the enum kind.  There should be no unreachable generated.
// CHECK-LABEL: sil hidden [ossa] @$s6switch26testOptionalEnumMixWithNilyS2iSgF
func testOptionalEnumMixWithNil(_ a : Int?) -> Int {
  // CHECK: debug_value %0 : $Optional<Int>, let, name "a"
  // CHECK-NEXT: switch_enum %0 : $Optional<Int>, case #Optional.some!enumelt: [[SOMEBB:bb[0-9]+]], case #Optional.none!enumelt: [[NILBB:bb[0-9]+]]
  switch a {
  case let x?:
    return 0

  // CHECK: [[SOMEBB]]([[X:%.*]] : $Int):
  // CHECK-NEXT: debug_value [[X]] : $Int, let, name "x"
  // CHECK: integer_literal $Builtin.IntLiteral, 0

  case nil:
    return 42

  // CHECK: [[NILBB]]:
  // CHECK: integer_literal $Builtin.IntLiteral, 42
  }
}

// https://github.com/apple/swift/issues/46106
//
// CHECK-LABEL: sil hidden [ossa] @$s6switch43testMultiPatternsWithOuterScopeSameNamedVar4base6filterySiSg_AEtF
func testMultiPatternsWithOuterScopeSameNamedVar(base: Int?, filter: Int?) {
  switch(base, filter) {

  case (.some(let base), .some(let filter)):
    // CHECK: bb2(%10 : $Int):
    // CHECK-NEXT: debug_value %8 : $Int, let, name "base"
    // CHECK-NEXT: debug_value %10 : $Int, let, name "filter"
    print("both: \(base), \(filter)")
  case (.some(let base), .none), (.none, .some(let base)):
    // CHECK: bb3:
    // CHECK-NEXT: br bb6(%8 : $Int)

    // CHECK: bb5([[OTHER_BASE:%.*]] : $Int)
    // CHECK-NEXT: br bb6([[OTHER_BASE]] : $Int)

    // CHECK: bb6([[ARG:%.*]] : $Int):
    // CHECK-NEXT: debug_value [[ARG]] : $Int, let, name "base"
    print("single: \(base)")
  default:
    print("default")
  }
}

// All cases are unreachable, either structurally (tuples involving Never) or
// nominally (empty enums).  We fold all of these to 'unreachable'.
enum MyNever {}
func ~= (_ : MyNever, _ : MyNever) -> Bool { return true }
func myFatalError() -> MyNever { fatalError("asdf") }

func testUninhabitedSwitchScrutinee() {
  func test1(x : MyNever) {
    // CHECK: bb0(%0 : $MyNever):
    // CHECK-NEXT: debug_value %0 : $MyNever, let, name "x"
    // CHECK-NEXT: unreachable
    switch x {
    case myFatalError(): break
    case myFatalError(): break
    case myFatalError(): break
    }
  }
  func test2(x : Never) {
    // CHECK: bb0(%0 : $Never):
    // CHECK-NEXT: debug_value %0 : $Never, let, name "x"
    // CHECK-NEXT: unreachable
    switch (x, x) {}
  }
  func test3(x : Never) {
    // CHECK: unreachable
    // CHECK-NEXT: } // end sil function '$s6switch30testUninhabitedSwitchScrutineeyyF5test3L_1xys5NeverO_tF'
    switch (x, 5, x) {}
  }
  func test4(x : Never) {
    // CHECK: unreachable
    // CHECK-NEXT: } // end sil function '$s6switch30testUninhabitedSwitchScrutineeyyF5test4L_1xys5NeverO_tF'
    switch ((8, 6, 7), (5, 3, (0, x))) {}
  }
  func test5() {
    // CHECK: %0 = function_ref @$s6switch12myFatalErrorAA7MyNeverOyF : $@convention(thin) () -> MyNever
    // CHECK-NEXT: %1 = apply %0() : $@convention(thin) () -> MyNever
    // CHECK-NEXT: unreachable
    switch myFatalError() {}
  }
}

// Make sure that we properly can handle address only tuples with loadable
// subtypes.

// CHECK-LABEL: sil hidden [ossa] @$s6switch33address_only_with_trivial_subtypeyyAA21TrivialSingleCaseEnumO_yptF : $@convention(thin) (TrivialSingleCaseEnum, @in_guaranteed Any) -> () {
// CHECK: [[MEM:%.*]] = alloc_stack $(TrivialSingleCaseEnum, Any)
// CHECK: [[INIT_TUP_0:%.*]] = tuple_element_addr [[MEM]] : $*(TrivialSingleCaseEnum, Any), 0
// CHECK: [[INIT_TUP_1:%.*]] = tuple_element_addr [[MEM]] : $*(TrivialSingleCaseEnum, Any), 1
// CHECK: store {{%.*}} to [trivial] [[INIT_TUP_0]]
// CHECK: copy_addr [take] {{%.*}} to [init] [[INIT_TUP_1]]
// CHECK: [[TUP_0:%.*]] = tuple_element_addr [[MEM]] : $*(TrivialSingleCaseEnum, Any), 0
// CHECK: [[TUP_0_VAL:%.*]] = load [trivial] [[TUP_0]]
// CHECK: [[TUP_1:%.*]] = tuple_element_addr [[MEM]] : $*(TrivialSingleCaseEnum, Any), 1
// CHECK: switch_enum [[TUP_0_VAL]]
//
// CHECK: } // end sil function '$s6switch33address_only_with_trivial_subtypeyyAA21TrivialSingleCaseEnumO_yptF'
func address_only_with_trivial_subtype(_ a: TrivialSingleCaseEnum, _ value: Any) {
  switch (a, value) {
  case (.a, _):
    break
  default:
    break
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch36address_only_with_nontrivial_subtypeyyAA24NonTrivialSingleCaseEnumO_yptF : $@convention(thin) (@guaranteed NonTrivialSingleCaseEnum, @in_guaranteed Any) -> () {
// CHECK: [[MEM:%.*]] = alloc_stack $(NonTrivialSingleCaseEnum, Any)
// CHECK: [[INIT_TUP_0:%.*]] = tuple_element_addr [[MEM]] : $*(NonTrivialSingleCaseEnum, Any), 0
// CHECK: [[INIT_TUP_1:%.*]] = tuple_element_addr [[MEM]] : $*(NonTrivialSingleCaseEnum, Any), 1
// CHECK: store {{%.*}} to [init] [[INIT_TUP_0]]
// CHECK: copy_addr [take] {{%.*}} to [init] [[INIT_TUP_1]]
// CHECK: [[TUP_0:%.*]] = tuple_element_addr [[MEM]] : $*(NonTrivialSingleCaseEnum, Any), 0
// CHECK: [[TUP_0_VAL:%.*]] = load_borrow [[TUP_0]]
// CHECK: [[TUP_1:%.*]] = tuple_element_addr [[MEM]] : $*(NonTrivialSingleCaseEnum, Any), 1
// CHECK: switch_enum [[TUP_0_VAL]]
//
// CHECK: bb1([[CASE_VAL:%.*]] :
// CHECK-NEXT:   destroy_addr [[TUP_1]]
// CHECK-NEXT:   end_borrow [[TUP_0_VAL]]
// CHECK-NEXT:   destroy_addr [[TUP_0]]
// CHECK-NEXT:   dealloc_stack [[MEM]]
//
// CHECK: bb2:
// CHECK-NEXT:   destroy_addr [[MEM]]
// CHECK-NEXT:   dealloc_stack [[MEM]]
// CHECK: } // end sil function '$s6switch36address_only_with_nontrivial_subtypeyyAA24NonTrivialSingleCaseEnumO_yptF'
func address_only_with_nontrivial_subtype(_ a: NonTrivialSingleCaseEnum, _ value: Any) {
  switch (a, value) {
  case (.a, _):
    break
  default:
    break
  }
}

// This test makes sure that when we have a tuple that is partly address only
// and partially an object that even though we access the object at +0 via a
// load_borrow, we do not lose the +1 from the original tuple formation.
// CHECK-LABEL: sil hidden [ossa] @$s6switch35partial_address_only_tuple_dispatchyyAA5KlassC_ypSgtF : $@convention(thin) (@guaranteed Klass, @in_guaranteed Optional<Any>) -> () {
// CHECK: bb0([[ARG0:%.*]] : @guaranteed $Klass, [[ARG1:%.*]] : $*Optional<Any>):
// CHECK:   [[ARG0_COPY:%.*]] = copy_value [[ARG0]]
// CHECK:   [[ARG1_COPY:%.*]] = alloc_stack $Optional<Any>
// CHECK:   copy_addr [[ARG1]] to [init] [[ARG1_COPY]]
// CHECK:   [[TUP:%.*]] = alloc_stack $(Klass, Optional<Any>)
// CHECK:   [[TUP_0:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 0
// CHECK:   [[TUP_1:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 1
// CHECK:   store [[ARG0_COPY]] to [init] [[TUP_0]]
// CHECK:   copy_addr [take] [[ARG1_COPY]] to [init] [[TUP_1]]
// CHECK:   [[TUP_0:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 0
// CHECK:   [[TUP_0_VAL:%.*]] = load_borrow [[TUP_0]]
// CHECK:   [[TUP_1:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 1
// CHECK:   destroy_addr [[TUP_1]]
// CHECK:   end_borrow [[TUP_0_VAL]]
// CHECK:   destroy_addr [[TUP_0]]
// CHECK:   dealloc_stack [[TUP]]
// CHECK:   br bb2
//
// CHECK: bb1:
// CHECK:   destroy_addr [[TUP]]
// CHECK:   dealloc_stack [[TUP]]
// CHECK: } // end sil function '$s6switch35partial_address_only_tuple_dispatchyyAA5KlassC_ypSgtF'
func partial_address_only_tuple_dispatch(_ name: Klass, _ value: Any?) {
  switch (name, value) {
  case (_, _):
    break
  default:
    break
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch50partial_address_only_tuple_dispatch_with_fail_caseyyAA5KlassC_ypSgtF : $@convention(thin) (@guaranteed Klass, @in_guaranteed Optional<Any>) -> () {
// CHECK: bb0([[ARG0:%.*]] : @guaranteed $Klass, [[ARG1:%.*]] : $*Optional<Any>):
// CHECK:   [[ARG0_COPY:%.*]] = copy_value [[ARG0]]
// CHECK:   [[ARG1_COPY:%.*]] = alloc_stack $Optional<Any>
// CHECK:   copy_addr [[ARG1]] to [init] [[ARG1_COPY]]
// CHECK:   [[TUP:%.*]] = alloc_stack $(Klass, Optional<Any>)
// CHECK:   [[TUP_0:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 0
// CHECK:   [[TUP_1:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 1
// CHECK:   store [[ARG0_COPY]] to [init] [[TUP_0]]
// CHECK:   copy_addr [take] [[ARG1_COPY]] to [init] [[TUP_1]]
// CHECK:   [[TUP_0:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 0
// CHECK:   [[TUP_0_VAL:%.*]] = load_borrow [[TUP_0]]
// CHECK:   [[TUP_1:%.*]] = tuple_element_addr [[TUP]] : $*(Klass, Optional<Any>), 1
// CHECK:   checked_cast_br Klass in [[TUP_0_VAL]] : $Klass to AnyObject, [[IS_ANYOBJECT_BB:bb[0-9]+]], [[ISNOT_ANYOBJECT_BB:bb[0-9]+]]
//
// CHECK: [[IS_ANYOBJECT_BB]]([[ANYOBJECT:%.*]] : @guaranteed $AnyObject):
// CHECK:   [[ANYOBJECT_COPY:%.*]] = copy_value [[ANYOBJECT]]
//          ... CASE BODY ...
// CHECK:   destroy_addr [[TUP_1]]
// CHECK:   end_borrow [[TUP_0_VAL]]
// CHECK:   destroy_addr [[TUP_0]]
// CHECK:   dealloc_stack [[TUP]]
// CHECK:   br [[EXIT_BB:bb[0-9]+]]
//
// CHECK: [[ISNOT_ANYOBJECT_BB]](
// CHECK:   switch_enum_addr [[TUP_1]] : $*Optional<Any>, case #Optional.some!enumelt: [[HAS_TUP_1_BB:bb[0-9]+]], default [[NO_TUP_1_BB:bb[0-9]+]]
//
// CHECK: [[HAS_TUP_1_BB]]:
// CHECK-NEXT: [[OPT_ANY_ADDR:%.*]] = alloc_stack $Optional<Any>
// CHECK-NEXT: copy_addr [[TUP_1]] to [init] [[OPT_ANY_ADDR]]
// CHECK-NEXT: [[SOME_ANY_ADDR:%.*]] = unchecked_take_enum_data_addr [[OPT_ANY_ADDR]]
// CHECK-NEXT: [[ANYOBJECT_ADDR:%.*]] = alloc_stack $AnyObject
// CHECK-NEXT: checked_cast_addr_br copy_on_success Any in {{%.*}} : $*Any to AnyObject in {{%.*}} : $*AnyObject, [[IS_ANY_BB:bb[0-9]+]], [[ISNOT_ANY_BB:bb[0-9]+]]
//
// Make sure that we clean up everything here. We are exiting here.
//
// CHECK: [[IS_ANY_BB]]:
// CHECK-NEXT:   [[ANYOBJECT:%.*]] = load [take] [[ANYOBJECT_ADDR]]
// CHECK-NEXT:   begin_borrow
// CHECK-NEXT:   debug_value
// CHECK-NEXT:   end_borrow
// CHECK-NEXT:   destroy_value [[ANYOBJECT]]
// CHECK-NEXT:   dealloc_stack [[ANYOBJECT_ADDR]]
// CHECK-NEXT:   destroy_addr [[SOME_ANY_ADDR]]
// CHECK-NEXT:   dealloc_stack [[OPT_ANY_ADDR]]
// CHECK-NEXT:   destroy_addr [[TUP_1]]
// CHECK-NEXT:   end_borrow [[TUP_0_VAL]]
// CHECK-NEXT:   destroy_addr [[TUP_0]]
// CHECK-NEXT:   dealloc_stack [[TUP]]
// CHECK-NEXT:   dealloc_stack
// CHECK-NEXT:   br [[EXIT_BB]]
//
// CHECK: [[ISNOT_ANY_BB]]:
// CHECK-NEXT:   dealloc_stack [[ANYOBJECT_ADDR]]
// CHECK-NEXT:   destroy_addr [[SOME_ANY_ADDR]]
// CHECK-NEXT:   dealloc_stack [[OPT_ANY_ADDR]]
// CHECK-NEXT:   end_borrow
// CHECK-NEXT:   br [[UNFORWARD_BB:bb[0-9]+]]
//
// CHECK: [[NO_TUP_1_BB]]:
// CHECK-NEXT: end_borrow
// CHECK-NEXT: br [[UNFORWARD_BB]]
//
// CHECK: [[UNFORWARD_BB]]:
// CHECK-NEXT:   destroy_addr [[TUP]]
// CHECK-NEXT:   dealloc_stack [[TUP]]
// CHECK:   br [[EXIT_BB]]
//
// CHECK: [[EXIT_BB]]:
// ...
// CHECK: } // end sil function '$s6switch50partial_address_only_tuple_dispatch_with_fail_caseyyAA5KlassC_ypSgtF'
func partial_address_only_tuple_dispatch_with_fail_case(_ name: Klass, _ value: Any?) {
  switch (name, value) {
  case let (x as AnyObject, _):
    break
  case let (_, y as AnyObject):
    break
  default:
    break
  }
}

// This was crashing the ownership verifier at some point and was reported in
// https://github.com/apple/swift/issues/49213. Just make sure that we still
// pass the ownership verifier.
//
// `indirect` is necessary; generic parameter is necessary.
indirect enum IndirectGenericEnum<Element> {
  // Tuple associated value is necessary; one element must be a function,
  // the other must be a non-function using the generic parameter.
  // (The original associated value was `(where: (Element) -> Bool, of: Element?)`,
  // to give you an idea of the variety of types.)
  case index((Int) -> Void, Element)

  // Function can be in an extension or not. Can have a return value or not. Can
  // have a parameter or not. Can be generic or not.
  func relative() {
    switch self {
    // Matching the case is necessary. You can capture or ignore the associated
    // values.
    case .index:
      // Body doesn't matter.
      break
    }
  }
}

// Make sure that we properly create switch_enum success arguments if we have an
// associated type that is a void type.
func testVoidType() {
  let x: Optional<()> = ()
  switch x {
  case .some(let x):
    break
  case .none:
    break
  }
}

////////////////////////////////////////////////
// Fallthrough Multiple Case Label Item Tests //
////////////////////////////////////////////////

// CHECK-LABEL: sil hidden [ossa] @$s6switch28addressOnlyFallthroughCalleeyyAA015MultipleAddressC8CaseEnumOyxGSzRzlF : $@convention(thin) <T where T : BinaryInteger> (@in_guaranteed MultipleAddressOnlyCaseEnum<T>) -> () {
// CHECK: bb0([[ARG:%.*]] :
// CHECK:   [[AB_PHI:%.*]] = alloc_stack $T, let, name "x"
// CHECK:   [[ABB_PHI:%.*]] = alloc_stack $T, let, name "x"
// CHECK:   [[ABBC_PHI:%.*]] = alloc_stack $T, let, name "x"
// CHECK:   [[SWITCH_ENUM_ARG:%.*]] = alloc_stack $MultipleAddressOnlyCaseEnum<T>
// CHECK:   copy_addr [[ARG]] to [init] [[SWITCH_ENUM_ARG]]
// CHECK:   switch_enum_addr [[SWITCH_ENUM_ARG]] : $*MultipleAddressOnlyCaseEnum<T>, case #MultipleAddressOnlyCaseEnum.a!enumelt: [[BB_A:bb[0-9]+]], case #MultipleAddressOnlyCaseEnum.b!enumelt: [[BB_B:bb[0-9]+]], case #MultipleAddressOnlyCaseEnum.c!enumelt: [[BB_C:bb[0-9]+]]
//
// CHECK: [[BB_A]]:
// CHECK:   [[SWITCH_ENUM_ARG_PROJ:%.*]] = unchecked_take_enum_data_addr [[SWITCH_ENUM_ARG]]
// CHECK:   [[CASE_BODY_VAR_A:%.*]] = alloc_stack [lexical] $T, let, name "x"
// CHECK:   copy_addr [take] [[SWITCH_ENUM_ARG_PROJ]] to [init] [[CASE_BODY_VAR_A]]
// CHECK:   copy_addr [[CASE_BODY_VAR_A]] to [init] [[AB_PHI]]
// CHECK:   destroy_addr [[CASE_BODY_VAR_A]]
// CHECK:   br [[BB_AB:bb[0-9]+]]
//
// CHECK: [[BB_B]]:
// CHECK:   [[SWITCH_ENUM_ARG_PROJ:%.*]] = unchecked_take_enum_data_addr [[SWITCH_ENUM_ARG]]
// CHECK:   [[CASE_BODY_VAR_B:%.*]] = alloc_stack [lexical] $T, let, name "x"
// CHECK:   copy_addr [[SWITCH_ENUM_ARG_PROJ]] to [init] [[CASE_BODY_VAR_B]]
// CHECK:   [[FUNC_CMP:%.*]] = function_ref @$sSzsE2eeoiySbx_qd__tSzRd__lFZ :
// CHECK:   [[GUARD_RESULT:%.*]] = apply [[FUNC_CMP]]<T, Int>([[CASE_BODY_VAR_B]], {{%.*}}, {{%.*}})
// CHECK:   [[GUARD_RESULT_EXT:%.*]] = struct_extract [[GUARD_RESULT]]
// CHECK:   cond_br [[GUARD_RESULT_EXT]], [[BB_B_GUARD_SUCC:bb[0-9]+]], [[BB_B_GUARD_FAIL:bb[0-9]+]]
//
// CHECK: [[BB_B_GUARD_SUCC]]:
// CHECK:   copy_addr [[CASE_BODY_VAR_B]] to [init] [[AB_PHI]]
// CHECK:   destroy_addr [[CASE_BODY_VAR_B]]
// CHECK:   destroy_addr [[SWITCH_ENUM_ARG_PROJ]]
// CHECK:   br [[BB_AB]]
//
// CHECK: [[BB_AB]]:
// CHECK:   copy_addr [[AB_PHI]] to [init] [[ABB_PHI]]
// CHECK:   destroy_addr [[AB_PHI]]
// CHECK:   br [[BB_AB_CONT:bb[0-9]+]]
//
// CHECK: [[BB_AB_CONT]]:
// CHECK:   copy_addr [[ABB_PHI]] to [init] [[ABBC_PHI]]
// CHECK:   destroy_addr [[ABB_PHI]]
// CHECK:   br [[BB_FINAL_CONT:bb[0-9]+]]
//
// CHECK: [[BB_B_GUARD_FAIL]]:
// CHECK:   destroy_addr [[CASE_BODY_VAR_B]]
// CHECK:   [[CASE_BODY_VAR_B_2:%.*]] = alloc_stack [lexical] $T, let, name "x"
// CHECK:   copy_addr [take] [[SWITCH_ENUM_ARG_PROJ]] to [init] [[CASE_BODY_VAR_B_2]]
// CHECK:   copy_addr [[CASE_BODY_VAR_B_2]] to [init] [[ABB_PHI]]
// CHECK:   br [[BB_AB_CONT]]
//
// CHECK: [[BB_C]]:
// CHECK:   [[SWITCH_ENUM_ARG_PROJ:%.*]] = unchecked_take_enum_data_addr [[SWITCH_ENUM_ARG]]
// CHECK:   [[CASE_BODY_VAR_C:%.*]] = alloc_stack [lexical] $T, let, name "x"
// CHECK:   copy_addr [take] [[SWITCH_ENUM_ARG_PROJ]] to [init] [[CASE_BODY_VAR_C]]
// CHECK:   copy_addr [[CASE_BODY_VAR_C]] to [init] [[ABBC_PHI]]
// CHECK:   destroy_addr [[CASE_BODY_VAR_C]]
// CHECK:   br [[BB_FINAL_CONT]]
//
// CHECK: [[BB_FINAL_CONT]]:
// CHECK:   destroy_addr [[ABBC_PHI]]
// CHECK:   return
// CHECK: } // end sil function '$s6switch28addressOnlyFallthroughCalleeyyAA015MultipleAddressC8CaseEnumOyxGSzRzlF'
func addressOnlyFallthroughCallee<T : BinaryInteger>(_ e : MultipleAddressOnlyCaseEnum<T>) {
  switch e {
  case .a(let x): fallthrough
  case .b(let x) where x == 2: fallthrough
  case .b(let x): fallthrough
  case .c(let x):
    print(x)
  }
}

func addressOnlyFallthroughCaller() {
  var myFoo : MultipleAddressOnlyCaseEnum = MultipleAddressOnlyCaseEnum.a(10)
  addressOnlyFallthroughCallee(myFoo)
}

// CHECK-LABEL: sil hidden [ossa] @$s6switch35nonTrivialLoadableFallthroughCalleeyyAA011MultipleNonC8CaseEnumOF : $@convention(thin) (@guaranteed MultipleNonTrivialCaseEnum) -> () {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $MultipleNonTrivialCaseEnum):
// CHECK:   switch_enum [[ARG]] : $MultipleNonTrivialCaseEnum, case #MultipleNonTrivialCaseEnum.a!enumelt: [[BB_A:bb[0-9]+]], case #MultipleNonTrivialCaseEnum.b!enumelt: [[BB_B:bb[0-9]+]], case #MultipleNonTrivialCaseEnum.c!enumelt: [[BB_C:bb[0-9]+]]
//
// CHECK: [[BB_A]]([[BB_A_ARG:%.*]] : @guaranteed
// CHECK:   [[BB_A_ARG_COPY:%.*]] = copy_value [[BB_A_ARG]]
// CHECK:   [[BB_A_ARG_COPY_BORROW:%.*]] = begin_borrow [lexical] [[BB_A_ARG_COPY]]
// CHECK:   apply {{%.*}}([[BB_A_ARG_COPY_BORROW]])
// CHECK:   [[RESULT:%.*]] = copy_value [[BB_A_ARG_COPY_BORROW]]
// CHECK:   br [[BB_AB:bb[0-9]+]]([[RESULT]] :
//
// CHECK: [[BB_B]]([[BB_B_ARG:%.*]] : @guaranteed
// CHECK:   [[BB_B_ARG_COPY:%.*]] = copy_value [[BB_B_ARG]]
// CHECK:   [[BB_B_ARG_COPY_BORROW:%.*]] = begin_borrow [lexical] [[BB_B_ARG_COPY]]
// CHECK:   [[RESULT:%.*]] = copy_value [[BB_B_ARG_COPY_BORROW]]
// CHECK:   br [[BB_AB:bb[0-9]+]]([[RESULT]] :
//
// CHECK: [[BB_AB:bb[0-9]+]]([[BB_AB_PHI:%.*]] : @owned
// CHECK:   [[BB_AB_PHI_BORROW:%.*]] = begin_borrow [[BB_AB_PHI]]
// CHECK:   apply {{%.*}}([[BB_AB_PHI_BORROW]])
// CHECK:   [[RESULT:%.*]] = copy_value [[BB_AB_PHI]]
// CHECK:   br [[BB_ABC:bb[0-9]+]]([[RESULT]] :
//
// CHECK: [[BB_C]]([[BB_C_ARG:%.*]] : @guaranteed
// CHECK:   [[BB_C_COPY:%.*]] = copy_value [[BB_C_ARG]]
// CHECK:   [[BB_C_BORROWED_COPY:%.*]] = begin_borrow [lexical] [[BB_C_COPY]]
// CHECK:   [[RESULT:%.*]] = copy_value [[BB_C_BORROWED_COPY]]
// CHECK:   br [[BB_ABC]]([[RESULT]] :
//
// CHECK: [[BB_ABC]]([[BB_ABC_ARG:%.*]] : @owned
// CHECK:   [[BB_ABC_ARG_BORROW:%.*]] = begin_borrow [[BB_ABC_ARG]]
// CHECK:   apply {{%.*}}([[BB_ABC_ARG_BORROW]])
// CHECK:   return
// CHECK: } // end sil function '$s6switch35nonTrivialLoadableFallthroughCalleeyyAA011MultipleNonC8CaseEnumOF'
func nonTrivialLoadableFallthroughCallee(_ e : MultipleNonTrivialCaseEnum) {
  switch e {
  case .a(let x):
    a(x)
    fallthrough
  case .b(let x):
    b(x)
    fallthrough
  case .c(let x):
    c(x)
  }
}

// Just make sure that we do not crash on this.
func nonTrivialLoadableFallthroughCalleeGuards(_ e : MultipleNonTrivialCaseEnum) {
  switch e {
  case .a(let x) where x.isFalse:
    a(x)
    fallthrough
  case .a(let x) where x.isTrue:
    a(x)
    fallthrough
  case .b(let x) where x.isTrue:
    b(x)
    fallthrough
  case .b(let x) where x.isFalse:
    b(x)
    fallthrough
  case .c(let x) where x.isTrue:
    c(x)
    fallthrough
  case .c(let x) where x.isFalse:
    c(x)
    break
  default:
    d()
  }
}

func nonTrivialLoadableFallthroughCallee2(_ e : MultipleNonTrivialCaseEnum) {
  switch e {
  case .a(let x):
    a(x)
    fallthrough
  case .b(let x):
    b(x)
    break
  default:
    break
  }
}

// Make sure that we do not crash while emitting this code.
//
// DISCUSSION: The original crash was due to us performing an assignment/lookup
// on the VarLocs DenseMap in the same statement. This was caught be an
// asanified compiler. This test is just to make sure we do not regress.
enum Storage {
  case empty
  case single(Int)
  case pair(Int, Int)
  case array([Int])

  subscript(range: [Int]) -> Storage {
    get {
      return .empty
    }
    set {
      switch self {
      case .empty:
        break
      case .single(let index):
        break
      case .pair(let first, let second):
        switch (range[0], range[1]) {
        case (0, 0):
          switch newValue {
          case .empty:
            break
          case .single(let other):
            break
          case .pair(let otherFirst, let otherSecond):
            break
          case .array(let other):
            break
          }
          break
        case (0, 1):
          switch newValue {
          case .empty:
            break
          case .single(let other):
            break
          case .pair(let otherFirst, let otherSecond):
            break
          case .array(let other):
            break
          }
          break
        case (0, 2):
          break
        case (1, 2):
          switch newValue {
          case .empty:
            break
          case .single(let other):
            break
          case .pair(let otherFirst, let otherSecond):
            break
          case .array(let other):
            self = .array([first] + other)
          }
          break
        case (2, 2):
          switch newValue {
          case .empty:
            break
          case .single(let other):
            break
          case .pair(let otherFirst, let otherSecond):
            break
          case .array(let other):
            self = .array([first, second] + other)
          }
          break
        default:
          let r = range
        }
      case .array(let indexes):
        break
      }
    }
  }
}

// Make sure that we do not leak tuple elements if we fail to match the first
// tuple element.
enum rdar49990484Enum1 {
  case case1(Klass)
  case case2(Klass, Int)
}

enum rdar49990484Enum2 {
  case case1(Klass)
  case case2(rdar49990484Enum1, Klass)
}

struct rdar49990484Struct {
  var value: rdar49990484Enum2

  func doSomethingIfLet() {
    if case let .case2(.case2(k, _), _) = value {
      return
    }
  }

  func doSomethingSwitch() {
    switch value {
    case let .case2(.case2(k, _), _):
      return
    default:
      return
    }
    return
  }

  func doSomethingGuardLet() {
    guard case let .case2(.case2(k, _), _) = value else {
      return
    }
  }
}
