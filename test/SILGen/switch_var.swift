// RUN: %target-swift-emit-silgen -module-name switch_var %s | %FileCheck %s

// TODO: Implement tuple equality in the library.
// BLOCKED: <rdar://problem/13822406>
func ~= (x: (Int, Int), y: (Int, Int)) -> Bool {
  return x.0 == y.0 && x.1 == y.1
}

// Some fake predicates for pattern guards.
func runced() -> Bool { return true }
func funged() -> Bool { return true }
func ansed() -> Bool { return true }

func runced(x x: Int) -> Bool { return true }
func funged(x x: Int) -> Bool { return true }
func ansed(x x: Int) -> Bool { return true }

func foo() -> Int { return 0 }
func bar() -> Int { return 0 }
func foobar() -> (Int, Int) { return (0, 0) }

func foos() -> String { return "" }
func bars() -> String { return "" }

func a() {}
func b() {}
func c() {}
func d() {}
func e() {}
func f() {}
func g() {}

func a(x x: Int) {}
func b(x x: Int) {}
func c(x x: Int) {}
func d(x x: Int) {}

func a(x x: String) {}
func b(x x: String) {}

func aa(x x: (Int, Int)) {}
func bb(x x: (Int, Int)) {}
func cc(x x: (Int, Int)) {}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var05test_B2_1yyF
func test_var_1() {
  // CHECK:   function_ref @$s10switch_var3fooSiyF
  switch foo() {
  // CHECK:   [[XADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[X:%.*]] = project_box [[XADDR]]
  // CHECK-NOT: br bb
  case var x:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[X]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1a1xySi_tF
  // CHECK:   destroy_value [[XADDR]]
    a(x: x)
  }
  // CHECK:   function_ref @$s10switch_var1byyF
  b()
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var05test_B2_2yyF
func test_var_2() {
  // CHECK:   function_ref @$s10switch_var3fooSiyF
  switch foo() {
  // CHECK:   [[XADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[X:%.*]] = project_box [[XADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[X]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var6runced1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NO_CASE1:bb[0-9]+]]
  // -- TODO: Clean up these empty waypoint bbs.
  case var x where runced(x: x):
  // CHECK: [[CASE1]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[X]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1a1xySi_tF
  // CHECK:   destroy_value [[XADDR]]
  // CHECK:   br [[CONT:bb[0-9]+]]
    a(x: x)
  // CHECK: [[NO_CASE1]]:
  // CHECK:   [[YADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[Y:%.*]] = project_box [[YADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Y]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var6funged1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NO_CASE2:bb[0-9]+]]
  case var y where funged(x: y):
  // CHECK: [[CASE2]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Y]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1b1xySi_tF
  // CHECK:   destroy_value [[YADDR]]
  // CHECK:   br [[CONT]]
    b(x: y)
  case var z:
  // CHECK: [[NO_CASE2]]:
  // CHECK:   [[ZADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[Z:%.*]] = project_box [[ZADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Z]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1c1xySi_tF
  // CHECK:   destroy_value [[ZADDR]]
  // CHECK:   br [[CONT]]
    c(x: z)
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s10switch_var1dyyF
  d()
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var05test_B2_3yyF
func test_var_3() {
  // CHECK:   function_ref @$s10switch_var3fooSiyF
  // CHECK:   function_ref @$s10switch_var3barSiyF
  switch (foo(), bar()) {
  // CHECK:   [[XADDR:%.*]] = alloc_box ${ var (Int, Int) }
  // CHECK:   [[X:%.*]] = project_box [[XADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[X]]
  // CHECK:   tuple_element_addr [[READ]] : {{.*}}, 0
  // CHECK:   function_ref @$s10switch_var6runced1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NO_CASE1:bb[0-9]+]]
  case var x where runced(x: x.0):
  // CHECK: [[CASE1]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[X]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var2aa1xySi_Sit_tF
  // CHECK:   destroy_value [[XADDR]]
  // CHECK:   br [[CONT:bb[0-9]+]]
    aa(x: x)

  // CHECK: [[NO_CASE1]]:
  // CHECK:   [[YADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[Y:%.*]] = project_box [[YADDR]]
  // CHECK:   [[ZADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[Z:%.*]] = project_box [[ZADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Y]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var6funged1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NO_CASE2:bb[0-9]+]]
  case (var y, var z) where funged(x: y):
  // CHECK: [[CASE2]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Y]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1a1xySi_tF
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Z]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1b1xySi_tF
  // CHECK:   destroy_value [[ZADDR]]
  // CHECK:   destroy_value [[YADDR]]
  // CHECK:   br [[CONT]]
    a(x: y)
    b(x: z)
  // CHECK: [[NO_CASE2]]:
  // CHECK:   [[WADDR:%.*]] = alloc_box ${ var (Int, Int) }
  // CHECK:   [[W:%.*]] = project_box [[WADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[W]]
  // CHECK:   tuple_element_addr [[READ]] : {{.*}}, 0
  // CHECK:   function_ref @$s10switch_var5ansed1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE3:bb[0-9]+]], [[NO_CASE3:bb[0-9]+]]
  case var w where ansed(x: w.0):
  // CHECK: [[CASE3]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[W]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var2bb1xySi_Sit_tF
  // CHECK:   br [[CONT]]
    bb(x: w)
  // CHECK: [[NO_CASE3]]:
  // CHECK:   destroy_value [[WADDR]]
  case var v:
  // CHECK:   [[VADDR:%.*]] = alloc_box ${ var (Int, Int) } 
  // CHECK:   [[V:%.*]] = project_box [[VADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[V]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var2cc1xySi_Sit_tF
  // CHECK:   destroy_value [[VADDR]]
  // CHECK:   br [[CONT]]
    cc(x: v)
  }
  // CHECK: [[CONT]]:
  // CHECK:   function_ref @$s10switch_var1dyyF
  d()
}

protocol P { func p() }

struct X : P { func p() {} }
struct Y : P { func p() {} }
struct Z : P { func p() {} }

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var05test_B2_41pyAA1P_p_tF : $@convention(thin) (@in_guaranteed any P) -> () {
func test_var_4(p p: P) {
  // CHECK:   function_ref @$s10switch_var3fooSiyF
  switch (p, foo()) {
  // CHECK:   [[PAIR:%.*]] = alloc_stack $(any P, Int)
  // CHECK:   store
  // CHECK:   [[PAIR_0:%.*]] = tuple_element_addr [[PAIR]] : $*(any P, Int), 0
  // CHECK:   [[T0:%.*]] = tuple_element_addr [[PAIR]] : $*(any P, Int), 1
  // CHECK:   [[PAIR_1:%.*]] = load [trivial] [[T0]] : $*Int
  // CHECK:   [[TMP:%.*]] = alloc_stack $X
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[PAIR_0]] : $*any P to X in [[TMP]] : $*X, [[IS_X:bb[0-9]+]], [[IS_NOT_X:bb[0-9]+]]

  // CHECK: [[IS_X]]:
  // CHECK:   [[T0:%.*]] = load [trivial] [[TMP]] : $*X
  // CHECK:   [[XADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[X:%.*]] = project_box [[XADDR]]
  // CHECK:   store [[PAIR_1]] to [trivial] [[X]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[X]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var6runced1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NO_CASE1:bb[0-9]+]]
  case (is X, var x) where runced(x: x):
  // CHECK: [[CASE1]]:
  // CHECK:   function_ref @$s10switch_var1a1xySi_tF
  // CHECK:   destroy_value [[XADDR]]
  // CHECK:   dealloc_stack [[TMP]]
  // CHECK:   destroy_addr [[PAIR_0]] : $*any P
  // CHECK:   dealloc_stack [[PAIR]]
  // CHECK:   br [[CONT:bb[0-9]+]]
    a(x: x)

  // CHECK: [[NO_CASE1]]:
  // CHECK:   destroy_value [[XADDR]]
  // CHECK:   dealloc_stack [[TMP]]
  // CHECK:   br [[NEXT:bb[0-9]+]]
  // CHECK: [[IS_NOT_X]]:
  // CHECK:   dealloc_stack [[TMP]]
  // CHECK:   br [[NEXT]]

  // CHECK: [[NEXT]]:
  // CHECK:   [[TMP:%.*]] = alloc_stack $Y
  // CHECK:   checked_cast_addr_br copy_on_success any P in [[PAIR_0]] : $*any P to Y in [[TMP]] : $*Y, [[IS_Y:bb[0-9]+]], [[IS_NOT_Y:bb[0-9]+]]

  // CHECK: [[IS_Y]]:
  // CHECK:   [[T0:%.*]] = load [trivial] [[TMP]] : $*Y
  // CHECK:   [[YADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[Y:%.*]] = project_box [[YADDR]]
  // CHECK:   store [[PAIR_1]] to [trivial] [[Y]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Y]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var6funged1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NO_CASE2:bb[0-9]+]]

  case (is Y, var y) where funged(x: y):
  // CHECK: [[CASE2]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Y]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1b1xySi_tF
  // CHECK:   destroy_value [[YADDR]]
  // CHECK:   dealloc_stack [[TMP]]
  // CHECK:   destroy_addr [[PAIR_0]] : $*any P
  // CHECK:   dealloc_stack [[PAIR]]
  // CHECK:   br [[CONT]]
    b(x: y)

  // CHECK: [[NO_CASE2]]:
  // CHECK:   destroy_value [[YADDR]]
  // CHECK:   dealloc_stack [[TMP]]
  // CHECK:   br [[NEXT:bb[0-9]+]]
  // CHECK: [[IS_NOT_Y]]:
  // CHECK:   dealloc_stack [[TMP]]
  // CHECK:   br [[NEXT]]

  // CHECK: [[NEXT]]:
  // CHECK:   [[ZADDR:%.*]] = alloc_box ${ var (any P, Int) }
  // CHECK:   [[ZLIFETIME:%.+]] = begin_borrow [lexical] [[ZADDR]]
  // CHECK:   [[Z:%.*]] = project_box [[ZLIFETIME]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Z]]
  // CHECK:   tuple_element_addr [[READ]] : {{.*}}, 1
  // CHECK:   function_ref @$s10switch_var5ansed1xSbSi_tF
  // CHECK:   cond_br {{%.*}}, [[CASE3:bb[0-9]+]], [[DFLT_NO_CASE3:bb[0-9]+]]
  case var z where ansed(x: z.1):
  // CHECK: [[CASE3]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[Z]]
  // CHECK:   tuple_element_addr [[READ]] : {{.*}}, 1
  // CHECK:   function_ref @$s10switch_var1c1xySi_tF
  // CHECK:   end_borrow [[ZLIFETIME]]
  // CHECK:   destroy_value [[ZADDR]]
  // CHECK-NEXT: destroy_addr [[PAIR]]
  // CHECK-NEXT: dealloc_stack [[PAIR]]
  // CHECK:   br [[CONT]]
    c(x: z.1)

  // CHECK: [[DFLT_NO_CASE3]]:
  // CHECK-NEXT:   end_borrow [[ZLIFETIME]]
  // CHECK-NEXT:   destroy_value [[ZADDR]]
  // CHECK-NOT: destroy_addr
  case (_, var w):
  // CHECK:   [[PAIR_0:%.*]] = tuple_element_addr [[PAIR]] : $*(any P, Int), 0
  // CHECK:   [[WADDR:%.*]] = alloc_box ${ var Int }
  // CHECK:   [[W:%.*]] = project_box [[WADDR]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[W]]
  // CHECK:   load [trivial] [[READ]]
  // CHECK:   function_ref @$s10switch_var1d1xySi_tF
  // CHECK:   destroy_value [[WADDR]]
  // CHECK-NEXT:   destroy_addr [[PAIR_0]] : $*any P
  // CHECK-NEXT:   dealloc_stack [[PAIR]]
  // CHECK-NEXT:   dealloc_stack
  // CHECK-NEXT:   br [[CONT]]
    d(x: w)
  }
  e()
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var05test_B2_5yyF : $@convention(thin) () -> () {
func test_var_5() {
  // CHECK:   function_ref @$s10switch_var3fooSiyF
  // CHECK:   function_ref @$s10switch_var3barSiyF
  switch (foo(), bar()) {
  // CHECK:   [[XADDR:%.*]] = alloc_box ${ var (Int, Int) }
  // CHECK:   [[X:%.*]] = project_box [[XADDR]]
  // CHECK:   cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NO_CASE1:bb[0-9]+]]
  case var x where runced(x: x.0):
  // CHECK: [[CASE1]]:
  // CHECK:   br [[CONT:bb[0-9]+]]
    a()
  // CHECK: [[NO_CASE1]]:
  // CHECK:   [[YADDR:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK:   [[Y:%[0-9]+]] = project_box [[YADDR]]
  // CHECK:   [[ZADDR:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK:   [[Z:%[0-9]+]] = project_box [[ZADDR]]
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NO_CASE2:bb[0-9]+]]
  case (var y, var z) where funged(x: y):
  // CHECK: [[CASE2]]:
  // CHECK:   destroy_value [[ZADDR]]
  // CHECK:   destroy_value [[YADDR]]
  // CHECK:   br [[CONT]]
    b()
  // CHECK: [[NO_CASE2]]:
  // CHECK:   destroy_value [[ZADDR]]
  // CHECK:   destroy_value [[YADDR]]
  // CHECK:   cond_br {{%.*}}, [[CASE3:bb[0-9]+]], [[NO_CASE3:bb[0-9]+]]
  case (_, _) where runced():
  // CHECK: [[CASE3]]:
  // CHECK:   br [[CONT]]
    c()
  // CHECK: [[NO_CASE3]]:
  // CHECK:   br [[CASE4:bb[0-9]+]]
  case _:
  // CHECK: [[CASE4]]:
    d()
  }
  e()
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var05test_B7_returnyyF : $@convention(thin) () -> () {
func test_var_return() {
  switch (foo(), bar()) {
  case var x where runced():
    // CHECK: [[XADDR:%[0-9]+]] = alloc_box ${ var (Int, Int) }
    // CHECK: [[X:%[0-9]+]] = project_box [[XADDR]]
    // CHECK: function_ref @$s10switch_var1ayyF
    // CHECK: destroy_value [[XADDR]]
    // CHECK: br [[EPILOG:bb[0-9]+]]
    a()
    return
  // CHECK: [[YADDR:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK: [[Y:%[0-9]+]] = project_box [[YADDR]]
  // CHECK: [[ZADDR:%[0-9]+]] = alloc_box ${ var Int }
  // CHECK: [[Z:%[0-9]+]] = project_box [[ZADDR]]
  case (var y, var z) where funged():
    // CHECK: function_ref @$s10switch_var1byyF
    // CHECK: destroy_value [[ZADDR]]
    // CHECK: destroy_value [[YADDR]]
    // CHECK: br [[EPILOG]]
    b()
    return
  case var w where ansed():
    // CHECK: [[WADDR:%[0-9]+]] = alloc_box ${ var (Int, Int) }
    // CHECK: [[W:%[0-9]+]] = project_box [[WADDR]]
    // CHECK: function_ref @$s10switch_var1cyyF
    // CHECK-NOT: destroy_value [[ZADDR]]
    // CHECK-NOT: destroy_value [[YADDR]]
    // CHECK: destroy_value [[WADDR]]
    // CHECK: br [[EPILOG]]
    c()
    return
  case var v:
    // CHECK: [[VADDR:%[0-9]+]] = alloc_box ${ var (Int, Int) }
    // CHECK: [[V:%[0-9]+]] = project_box [[VADDR]]
    // CHECK: function_ref @$s10switch_var1dyyF
    // CHECK-NOT: destroy_value [[ZADDR]]
    // CHECK-NOT: destroy_value [[YADDR]]
    // CHECK: destroy_value [[VADDR]]
    // CHECK: br [[EPILOG]]
    d()
    return
  }
}

// When all of the bindings in a column are immutable, don't emit a mutable
// box. <rdar://problem/15873365>
// CHECK-LABEL: sil hidden [ossa] @$s10switch_var8test_letyyF : $@convention(thin) () -> () {
func test_let() {
  // CHECK: [[FOOS:%.*]] = function_ref @$s10switch_var4foosSSyF
  // CHECK: [[VAL:%.*]] = apply [[FOOS]]()
  // CHECK: [[BORROWED_VAL:%.*]] = begin_borrow [[VAL]]
  // CHECK: [[VAL_COPY:%.*]] = copy_value [[BORROWED_VAL]]
  // CHECK: function_ref @$s10switch_var6runcedSbyF
  // CHECK: cond_br {{%.*}}, [[CASE1:bb[0-9]+]], [[NO_CASE1:bb[0-9]+]]
  switch foos() {
  case let x where runced():
  // CHECK: [[CASE1]]:
  // CHECK:   [[BORROWED_VAL_COPY:%.*]] = begin_borrow [[VAL_COPY]]
  // CHECK:   [[A:%.*]] = function_ref @$s10switch_var1a1xySS_tF
  // CHECK:   apply [[A]]([[BORROWED_VAL_COPY]])
  // CHECK:   destroy_value [[VAL_COPY]]
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT:bb[0-9]+]]
    a(x: x)
  // CHECK: [[NO_CASE1]]:
  // CHECK:   destroy_value [[VAL_COPY]]
  // CHECK:   [[BORROWED_VAL_2:%.*]] = begin_borrow [[VAL]]
  // CHECK:   [[VAL_COPY_2:%.*]] = copy_value [[BORROWED_VAL_2]]
  // CHECK:   function_ref @$s10switch_var6fungedSbyF
  // CHECK:   cond_br {{%.*}}, [[CASE2:bb[0-9]+]], [[NO_CASE2:bb[0-9]+]]
  case let y where funged():
  // CHECK: [[CASE2]]:
  // CHECK:   [[BORROWED_VAL_COPY_2:%.*]] = begin_borrow [[VAL_COPY_2]]
  // CHECK:   [[B:%.*]] = function_ref @$s10switch_var1b1xySS_tF
  // CHECK:   apply [[B]]([[BORROWED_VAL_COPY_2]])
  // CHECK:   destroy_value [[VAL_COPY_2]]
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT]]
    b(x: y)
  // CHECK: [[NO_CASE2]]:
  // CHECK:   destroy_value [[VAL_COPY_2]]
  // CHECK:   [[BORROWED_VAL_3:%.*]] = begin_borrow [[VAL]]
  // CHECK:   [[VAL_COPY_3:%.*]] = copy_value [[BORROWED_VAL_3]]
  // CHECK:   function_ref @$s10switch_var4barsSSyF
  // CHECK:   [[BORROWED_VAL_COPY_3:%.*]] = begin_borrow [[VAL_COPY_3]]
  // CHECK:   [[SB:%.*]] = store_borrow [[BORROWED_VAL_COPY_3]] to [[IN_ARG:%.*]] :
  // CHECK:   apply {{%.*}}<String>({{.*}}, [[SB]])
  // CHECK:   cond_br {{%.*}}, [[YES_CASE3:bb[0-9]+]], [[NO_CASE3:bb[0-9]+]]
  // ExprPatterns implicitly contain a 'let' binding.
  case bars():
  // CHECK: [[YES_CASE3]]:
  // CHECK:   destroy_value [[VAL_COPY_3]]
  // CHECK:   [[FUNC:%.*]] = function_ref @$s10switch_var1cyyF
  // CHECK-NEXT: apply [[FUNC]](
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT]]
    c()

  case _:
    // CHECK: [[NO_CASE3]]:
    // CHECK:   destroy_value [[VAL_COPY_3]]
    // CHECK:   function_ref @$s10switch_var1dyyF
    // CHECK:   destroy_value [[VAL]]
    // CHECK:   br [[CONT]]
    d()
  }
  // CHECK: [[CONT]]:
  // CHECK:   return
}
// CHECK: } // end sil function '$s10switch_var8test_letyyF'

// If one of the bindings is a "var", allocate a box for the column.
// CHECK-LABEL: sil hidden [ossa] @$s10switch_var015test_mixed_let_B0yyF : $@convention(thin) () -> () {
func test_mixed_let_var() {
  // CHECK: bb0:
  // CHECK:   [[FOOS:%.*]] = function_ref @$s10switch_var4foosSSyF
  // CHECK:   [[VAL:%.*]] = apply [[FOOS]]()
  // CHECK:   [[BORROWED_VAL:%.*]] = begin_borrow [[VAL]]
  switch foos() {

  // First pattern.
  // CHECK:   [[BOX:%.*]] = alloc_box ${ var String }, var, name "x"
  // CHECK:   [[PBOX:%.*]] = project_box [[BOX]]
  // CHECK:   [[VAL_COPY:%.*]] = copy_value [[BORROWED_VAL]]
  // CHECK:   store [[VAL_COPY]] to [init] [[PBOX]]
  // CHECK:   cond_br {{.*}}, [[CASE1:bb[0-9]+]], [[NOCASE1:bb[0-9]+]]
  case var x where runced():
  // CHECK: [[CASE1]]:
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[PBOX]]
  // CHECK:   [[X:%.*]] = load [copy] [[READ]]
  // CHECK:   [[A:%.*]] = function_ref @$s10switch_var1a1xySS_tF
  // CHECK:   apply [[A]]([[X]])
  // CHECK:   destroy_value [[BOX]]
  // CHECK:   br [[CONT:bb[0-9]+]]
    a(x: x)

  // CHECK: [[NOCASE1]]:
  // CHECK:   destroy_value [[BOX]]
  // CHECK:   [[BORROWED_VAL:%.*]] = begin_borrow [[VAL]]
  // CHECK:   [[VAL_COPY:%.*]] = copy_value [[BORROWED_VAL]]
  // CHECK:   cond_br {{.*}}, [[CASE2:bb[0-9]+]], [[NOCASE2:bb[0-9]+]]
  case let y where funged():

  // CHECK: [[CASE2]]:
  // CHECK:   [[BORROWED_VAL_COPY:%.*]] = begin_borrow [[VAL_COPY]]
  // CHECK:   [[B:%.*]] = function_ref @$s10switch_var1b1xySS_tF
  // CHECK:   apply [[B]]([[BORROWED_VAL_COPY]])
  // CHECK:   end_borrow [[BORROWED_VAL_COPY]]
  // CHECK:   destroy_value [[VAL_COPY]]
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT]]  
  b(x: y)

  // CHECK: [[NOCASE2]]:
  // CHECK:   destroy_value [[VAL_COPY]]

  // CHECK:   [[BORROWED_VAL:%.*]] = begin_borrow [[VAL]]
  // CHECK:   [[VAL_COPY:%.*]] = copy_value [[BORROWED_VAL]]
  // CHECK:   [[BORROWED_VAL_COPY:%.*]] = begin_borrow [[VAL_COPY]]
  // CHECK:   [[SB:%.*]] = store_borrow [[BORROWED_VAL_COPY]] to [[TMP_VAL_COPY_ADDR:%.*]] :
  // CHECK:   apply {{.*}}<String>({{.*}}, [[SB]])
  // CHECK:   cond_br {{.*}}, [[CASE3:bb[0-9]+]], [[NOCASE3:bb[0-9]+]]
  case bars():
  // CHECK: [[CASE3]]:
  // CHECK:   destroy_value [[VAL_COPY]]
  // CHECK:   [[FUNC:%.*]] = function_ref @$s10switch_var1cyyF : $@convention(thin) () -> ()
  // CHECK:   apply [[FUNC]]()
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT]]
    c()

  // CHECK: [[NOCASE3]]:
  // CHECK:   destroy_value [[VAL_COPY]]
  // CHECK:   [[D_FUNC:%.*]] = function_ref @$s10switch_var1dyyF : $@convention(thin) () -> ()
  // CHECK:   apply [[D_FUNC]]()
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT]]
  case _:
    d()
  }

  // CHECK: [[CONT]]:
  // CHECK:   return
}
// CHECK: } // end sil function '$s10switch_var015test_mixed_let_B0yyF'

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var23test_multiple_patterns1yyF : $@convention(thin) () -> () {
func test_multiple_patterns1() {
  // CHECK:   function_ref @$s10switch_var6foobarSi_SityF
  switch foobar() {
  // CHECK-NOT: br bb
  case (0, let x), (let x, 0):
    // CHECK:   cond_br {{%.*}}, [[FIRST_MATCH_CASE:bb[0-9]+]], [[FIRST_FAIL:bb[0-9]+]]
    // CHECK:   [[FIRST_MATCH_CASE]]:
    // CHECK:     br [[CASE_BODY:bb[0-9]+]]([[FIRST_X:%.*]] : $Int)
    // CHECK:   [[FIRST_FAIL]]:
    // CHECK:     cond_br {{%.*}}, [[SECOND_MATCH_CASE:bb[0-9]+]], [[SECOND_FAIL:bb[0-9]+]]
    // CHECK:   [[SECOND_MATCH_CASE]]:
    // CHECK:     br [[CASE_BODY]]([[SECOND_X:%.*]] : $Int)
    // CHECK:   [[CASE_BODY]]([[BODY_VAR:%.*]] : $Int):
    // CHECK:     debug_value [[BODY_VAR]] : $Int, let, name "x"
    // CHECK:     [[A:%.*]] = function_ref @$s10switch_var1a1xySi_tF
    // CHECK:     apply [[A]]([[BODY_VAR]])
    a(x: x)
  default:
    // CHECK:   [[SECOND_FAIL]]:
    // CHECK:     function_ref @$s10switch_var1byyF
    b()
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var23test_multiple_patterns2yyF : $@convention(thin) () -> () {
func test_multiple_patterns2() {
  let t1 = 2
  let t2 = 4
  // CHECK:   debug_value [[T1:%.+]] :
  // CHECK:   debug_value [[T2:%.+]] :
  switch (0,0) {
    // CHECK-NOT: br bb
  case (_, let x) where x > t1, (let x, _) where x > t2:
    // CHECK:   ([[FIRST:%[0-9]+]], [[SECOND:%[0-9]+]]) = destructure_tuple {{%.+}} : $(Int, Int)
    // CHECK:   apply {{%.+}}([[SECOND]], [[T1]], {{%.+}})
    // CHECK:   cond_br {{%.*}}, [[FIRST_MATCH_CASE:bb[0-9]+]], [[FIRST_FAIL:bb[0-9]+]]
    // CHECK:   [[FIRST_MATCH_CASE]]:
    // CHECK:     br [[CASE_BODY:bb[0-9]+]]([[SECOND]] : $Int)
    // CHECK:   [[FIRST_FAIL]]:
    // CHECK:     apply {{%.*}}([[FIRST]], [[T2]], {{%.+}})
    // CHECK:     cond_br {{%.*}}, [[SECOND_MATCH_CASE:bb[0-9]+]], [[SECOND_FAIL:bb[0-9]+]]
    // CHECK:   [[SECOND_MATCH_CASE]]:
    // CHECK:     br [[CASE_BODY]]([[FIRST]] : $Int)
    // CHECK:   [[CASE_BODY]]([[BODY_VAR:%.*]] : $Int):
    // CHECK:     [[A:%.*]] = function_ref @$s10switch_var1a1xySi_tF
    // CHECK:     apply [[A]]([[BODY_VAR]])
    a(x: x)
  default:
    // CHECK:   [[SECOND_FAIL]]:
    // CHECK:     function_ref @$s10switch_var1byyF
    b()
  }
}

enum Foo {
  case A(Int, Double)
  case B(Double, Int)
  case C(Int, Int, Double)
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var23test_multiple_patterns3yyF : $@convention(thin) () -> () {
func test_multiple_patterns3() {
  let f = Foo.C(0, 1, 2.0)
  switch f {
    // CHECK:   switch_enum {{%.*}} : $Foo, case #Foo.A!enumelt: [[A:bb[0-9]+]], case #Foo.B!enumelt: [[B:bb[0-9]+]], case #Foo.C!enumelt: [[C:bb[0-9]+]]
  case .A(let x, let n), .B(let n, let x), .C(_, let x, let n):
    // CHECK:   [[A]]([[A_TUP:%.*]] : $(Int, Double)):
    // CHECK:     ([[A_X:%.*]], [[A_N:%.*]]) = destructure_tuple [[A_TUP]]
    // CHECK:     br [[CASE_BODY:bb[0-9]+]]([[A_X]] : $Int, [[A_N]] : $Double)
    
    // CHECK:   [[B]]([[B_TUP:%.*]] : $(Double, Int)):
    // CHECK:     ([[B_N:%.*]], [[B_X:%.*]]) = destructure_tuple [[B_TUP]]
    // CHECK:     br [[CASE_BODY]]([[B_X]] : $Int, [[B_N]] : $Double)

    // CHECK:   [[C]]([[C_TUP:%.*]] : $(Int, Int, Double)):
    // CHECK:     ([[C__:%.*]], [[C_X:%.*]], [[C_N:%.*]]) = destructure_tuple [[C_TUP]]
    // CHECK:     br [[CASE_BODY]]([[C_X]] : $Int, [[C_N]] : $Double)

    // CHECK:   [[CASE_BODY]]([[BODY_X:%.*]] : $Int, [[BODY_N:%.*]] : $Double):
    // CHECK:     [[FUNC_A:%.*]] = function_ref @$s10switch_var1a1xySi_tF
    // CHECK:     apply [[FUNC_A]]([[BODY_X]])
    a(x: x)
  }
}

enum Bar {
  case Y(Foo, Int)
  case Z(Int, Foo)
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var23test_multiple_patterns4yyF : $@convention(thin) () -> () {
func test_multiple_patterns4() {
  let b = Bar.Y(.C(0, 1, 2.0), 3)
  switch b {
    // CHECK:   switch_enum {{%.*}} : $Bar, case #Bar.Y!enumelt: [[Y:bb[0-9]+]], case #Bar.Z!enumelt: [[Z:bb[0-9]+]]
  case .Y(.A(let x, _), _), .Y(.B(_, let x), _), .Y(.C, let x), .Z(let x, _):
    // CHECK:   [[Y]]([[Y_TUP:%.*]] : $(Foo, Int)):
    // CHECK:     ([[Y_F:%.*]], [[Y_X:%.*]]) = destructure_tuple [[Y_TUP]]
    // CHECK:     switch_enum [[Y_F]] : $Foo, case #Foo.A!enumelt: [[A:bb[0-9]+]], case #Foo.B!enumelt: [[B:bb[0-9]+]], case #Foo.C!enumelt: [[C:bb[0-9]+]]
    
    // CHECK:   [[A]]([[A_TUP:%.*]] : $(Int, Double)):
    // CHECK:     ([[A_X:%.*]], [[A_N:%.*]]) = destructure_tuple [[A_TUP]]
    // CHECK:     br [[CASE_BODY:bb[0-9]+]]([[A_X]] : $Int)
    
    // CHECK:   [[B]]([[B_TUP:%.*]] : $(Double, Int)):
    // CHECK:     ([[B_N:%.*]], [[B_X:%.*]]) = destructure_tuple [[B_TUP]]
    // CHECK:     br [[CASE_BODY]]([[B_X]] : $Int)
    
    // CHECK:   [[C]]({{%.*}} : $(Int, Int, Double)):
    // CHECK:     br [[CASE_BODY]]([[Y_X]] : $Int)

    // CHECK:   [[Z]]([[Z_TUP:%.*]] : $(Int, Foo)):
    // CHECK:     ([[Z_X:%.*]], [[Z_F:%.*]]) = destructure_tuple [[Z_TUP]]
    // CHECK:     br [[CASE_BODY]]([[Z_X]] : $Int)

    // CHECK:   [[CASE_BODY]]([[BODY_X:%.*]] : $Int):
    // CHECK:     [[FUNC_A:%.*]] = function_ref @$s10switch_var1a1xySi_tF
    // CHECK:     apply [[FUNC_A]]([[BODY_X]])
    a(x: x)
  }
}

func aaa(x x: inout Int) {}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_var23test_multiple_patterns5yyF : $@convention(thin) () -> () {
func test_multiple_patterns5() {
  let b = Bar.Y(.C(0, 1, 2.0), 3)
  switch b {
    // CHECK:   switch_enum {{%.*}} : $Bar, case #Bar.Y!enumelt: [[Y:bb[0-9]+]], case #Bar.Z!enumelt: [[Z:bb[0-9]+]]
  case .Y(.A(var x, _), _), .Y(.B(_, var x), _), .Y(.C, var x), .Z(var x, _):
    // CHECK:   [[Y]]([[Y_TUP:%.*]] : $(Foo, Int)):
    // CHECK:     ([[Y_F:%.*]], [[Y_X:%.*]]) = destructure_tuple [[Y_TUP]]
    // CHECK:     switch_enum [[Y_F]] : $Foo, case #Foo.A!enumelt: [[A:bb[0-9]+]], case #Foo.B!enumelt: [[B:bb[0-9]+]], case #Foo.C!enumelt: [[C:bb[0-9]+]]
    
    // CHECK:   [[A]]([[A_TUP:%.*]] : $(Int, Double)):
    // CHECK:     ([[A_X:%.*]], [[A_N:%.*]]) = destructure_tuple [[A_TUP]]
    // CHECK:     br [[CASE_BODY:bb[0-9]+]]([[A_X]] : $Int)
    
    // CHECK:   [[B]]([[B_TUP:%.*]] : $(Double, Int)):
    // CHECK:     ([[B_N:%.*]], [[B_X:%.*]]) = destructure_tuple [[B_TUP]]
    // CHECK:     br [[CASE_BODY]]([[B_X]] : $Int)
    
    // CHECK:   [[C]]({{%.*}} : $(Int, Int, Double)):
    // CHECK:     br [[CASE_BODY]]([[Y_X]] : $Int)
    
    // CHECK:   [[Z]]([[Z_TUP:%.*]] : $(Int, Foo)):
    // CHECK:     ([[Z_X:%.*]], [[Z_F:%.*]]) = destructure_tuple [[Z_TUP]]
    // CHECK:     br [[CASE_BODY]]([[Z_X]] : $Int)
    
    // CHECK:   [[CASE_BODY]]([[BODY_X:%.*]] : $Int):
    // CHECK:     store [[BODY_X]] to [trivial] [[BOX_X:%.*]] : $*Int
    // CHECK:     [[WRITE:%.*]] = begin_access [modify] [unknown] [[BOX_X]]
    // CHECK:     [[FUNC_AAA:%.*]] = function_ref @$s10switch_var3aaa1xySiz_tF
    // CHECK:     apply [[FUNC_AAA]]([[WRITE]])
    aaa(x: &x)
  }
}

// rdar://problem/29252758 -- local decls must not be reemitted.
func test_against_reemission(x: Bar) {
  switch x {
  case .Y(let a, _), .Z(_, let a):
    let b = a
  }
}

class C    {}
class D: C {}
func f(_: D) -> Bool { return true }

// CHECK-LABEL: sil hidden [ossa] @{{.*}}test_multiple_patterns_value_semantics
func test_multiple_patterns_value_semantics(_ y: C) {
  switch y {
    // CHECK:   checked_cast_br C in {{%.*}} : $C to D, [[AS_D:bb[0-9]+]], [[NOT_AS_D:bb[0-9]+]]
    // CHECK: [[AS_D]]({{.*}}):
    // CHECK:   [[ORIG_BORROW:%.*]] = begin_borrow [lexical] [[ORIG:%.*]] :
    // CHECK:   cond_br {{%.*}}, [[F_TRUE:bb[0-9]+]], [[F_FALSE:bb[0-9]+]]
    // CHECK: [[F_TRUE]]:
    // CHECK:   [[BINDING:%.*]] = copy_value [[ORIG_BORROW]] :
    // CHECK:   destroy_value [[ORIG]]
    // CHECK:   br {{bb[0-9]+}}([[BINDING]]
    case let x as D where f(x), let x as D: break
    default: break
  }
}
