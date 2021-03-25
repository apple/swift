
// RUN: %target-swift-emit-silgen -module-name statements -Xllvm -sil-full-demangle -parse-as-library -verify %s | %FileCheck %s

class MyClass { 
  func foo() { }
}

func markUsed<T>(_ t: T) {}

func marker_1() {}
func marker_2() {}
func marker_3() {}

class BaseClass {}
class DerivedClass : BaseClass {}

var global_cond: Bool = false

func bar(_ x: Int) {}
func foo(_ x: Int, _ y: Bool) {}

func abort() -> Never { abort() }



func assignment(_ x: Int, y: Int) {
  var x = x
  var y = y
  x = 42
  y = 57
  _ = x
  _ = y
  (x, y) = (1,2)
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}assignment
// CHECK: integer_literal $Builtin.IntLiteral, 42
// CHECK: assign
// CHECK: integer_literal $Builtin.IntLiteral, 57
// CHECK: assign

func if_test(_ x: Int, y: Bool) {
  if (y) {
   bar(x);
  }
  bar(x);
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements7if_test{{[_0-9a-zA-Z]*}}F

func if_else(_ x: Int, y: Bool) {
  if (y) {
   bar(x);
  } else {
   foo(x, y);
  }
  bar(x);
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements7if_else{{[_0-9a-zA-Z]*}}F

func nested_if(_ x: Int, y: Bool, z: Bool) {
  if (y) {
    if (z) {
      bar(x);
    }
  } else {
    if (z) {
      foo(x, y);
    }
  }
  bar(x);
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements9nested_if{{[_0-9a-zA-Z]*}}F

func nested_if_merge_noret(_ x: Int, y: Bool, z: Bool) {
  if (y) {
    if (z) {
      bar(x);
    }
  } else {
    if (z) {
      foo(x, y);
    }
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements21nested_if_merge_noret{{[_0-9a-zA-Z]*}}F

func nested_if_merge_ret(_ x: Int, y: Bool, z: Bool) -> Int {
  if (y) {
    if (z) {
      bar(x);
    }
    return 1
  } else {
    if (z) {
      foo(x, y);
    }
  }
  return 2
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements19nested_if_merge_ret{{[_0-9a-zA-Z]*}}F

func else_break(_ x: Int, y: Bool, z: Bool) {
  while z {
    if y {
    } else {
      break
    }
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements10else_break{{[_0-9a-zA-Z]*}}F

func loop_with_break(_ x: Int, _ y: Bool, _ z: Bool) -> Int {
  while (x > 2) {
   if (y) {
     bar(x);
     break
   }
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements15loop_with_break{{[_0-9a-zA-Z]*}}F

func loop_with_continue(_ x: Int, y: Bool, z: Bool) -> Int {
  while (x > 2) {
    if (y) {
     bar(x);
     continue
    }
    _ = loop_with_break(x, y, z);
  }
  bar(x);
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements18loop_with_continue{{[_0-9a-zA-Z]*}}F

func do_loop_with_continue(_ x: Int, y: Bool, z: Bool) -> Int {
  repeat {
    if (x < 42) {
     bar(x);
     continue
    }
    _ = loop_with_break(x, y, z);
  }
  while (x > 2);
  bar(x);
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements21do_loop_with_continue{{[_0-9a-zA-Z]*}}F 


// CHECK-LABEL: sil hidden [ossa] @{{.*}}for_loops1
func for_loops1(_ x: Int, c: Bool) {
  for i in 1..<100 {
    markUsed(i)
  }

}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}for_loops2
func for_loops2() {
  // rdar://problem/19316670
  // CHECK: alloc_stack $Optional<MyClass>
  // CHECK-NEXT: [[WRITE:%.*]] = begin_access [modify] [unknown]
  // CHECK: [[NEXT:%[0-9]+]] = witness_method $IndexingIterator<Array<MyClass>>, #IteratorProtocol.next : <Self where Self : IteratorProtocol> (inout Self) -> () -> Self.Element? : $@convention(witness_method: IteratorProtocol) <τ_0_0 where τ_0_0 : IteratorProtocol> (@inout τ_0_0) -> @out Optional<τ_0_0.Element>
  // CHECK-NEXT: apply [[NEXT]]<IndexingIterator<Array<MyClass>>>
  // CHECK: class_method [[OBJ:%[0-9]+]] : $MyClass, #MyClass.foo :
  let objects = [MyClass(), MyClass() ]
  for obj in objects {
    obj.foo()
  }

  return 
}

func void_return() {
  let b:Bool
  if b {
    return
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s10statements11void_return{{[_0-9a-zA-Z]*}}F
// CHECK: cond_br {{%[0-9]+}}, [[BB1:bb[0-9]+]], [[BB2:bb[0-9]+]]
// CHECK: [[BB1]]:
// CHECK:   br [[EPILOG:bb[0-9]+]]
// CHECK: [[BB2]]:
// CHECK:   br [[EPILOG]]
// CHECK: [[EPILOG]]:
// CHECK:   [[R:%[0-9]+]] = tuple ()
// CHECK:   return [[R]]

func foo() {}

// <rdar://problem/13549626>
// CHECK-LABEL: sil hidden [ossa] @$s10statements14return_from_if{{[_0-9a-zA-Z]*}}F
func return_from_if(_ a: Bool) -> Int {
  // CHECK: bb0(%0 : $Bool):
  // CHECK: cond_br {{.*}}, [[THEN:bb[0-9]+]], [[ELSE:bb[0-9]+]]
  if a {
    // CHECK: [[THEN]]:
    // CHECK: br [[EPILOG:bb[0-9]+]]({{%.*}})
    return 1
  } else {
    // CHECK: [[ELSE]]:
    // CHECK: br [[EPILOG]]({{%.*}})
    return 0
  }
  // CHECK-NOT: function_ref @foo
  // CHECK: [[EPILOG]]([[RET:%.*]] : $Int):
  // CHECK:   return [[RET]]
  foo()  // expected-warning {{will never be executed}}
}

class C {}

func use(_ c: C) {}

func for_each_loop(_ x: [C]) {
  for i in x {
    use(i)
  }
  _ = 0
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}test_break
func test_break(_ i : Int) {
  switch i {
  case (let x) where x != 17: 
    if x == 42 { break } 
    markUsed(x)
  default:
    break
  }
}


// <rdar://problem/19150249> Allow labeled "break" from an "if" statement

// CHECK-LABEL: sil hidden [ossa] @$s10statements13test_if_breakyyAA1CCSgF : $@convention(thin) (@guaranteed Optional<C>) -> () {
func test_if_break(_ c : C?) {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $Optional<C>):
label1:
  // CHECK: [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK: switch_enum [[ARG_COPY]] : $Optional<C>, case #Optional.some!enumelt: [[TRUE:bb[0-9]+]], case #Optional.none!enumelt: [[FALSE:bb[0-9]+]]
  if let x = c {
// CHECK: [[TRUE]]({{.*}} : @owned $C):

    // CHECK: apply
    foo()

    // CHECK: destroy_value
    // CHECK: br [[FALSE:bb[0-9]+]]
    break label1
    use(x)  // expected-warning {{will never be executed}}
  }

  // CHECK: [[FALSE]]:
  // CHECK: return
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements18test_if_else_breakyyAA1CCSgF : $@convention(thin) (@guaranteed Optional<C>) -> () {
func test_if_else_break(_ c : C?) {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $Optional<C>):
label2:
  // CHECK: [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK: switch_enum [[ARG_COPY]] : $Optional<C>, case #Optional.some!enumelt: [[TRUE:bb[0-9]+]], case #Optional.none!enumelt: [[FALSE:bb[0-9]+]]

  if let x = c {
    // CHECK: [[TRUE]]({{.*}} : @owned $C):
    use(x)
    // CHECK: apply
    // CHECK:   br [[EPILOG:bb[0-9]+]]
  } else {
    // CHECK: [[FALSE]]:
    // CHECK: apply
    // CHECK:   br [[EPILOG:bb[0-9]+]]
    foo()
    break label2
    foo() // expected-warning {{will never be executed}}
  }
  // CHECK: [[EPILOG]]:
  // CHECK: return
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements23test_if_else_then_breakyySb_AA1CCSgtF
func test_if_else_then_break(_ a : Bool, _ c : C?) {
label3:
  // CHECK: bb0({{.*}}, [[ARG2:%.*]] : @guaranteed $Optional<C>):
  // CHECK: [[ARG2_COPY:%.*]] = copy_value [[ARG2]]
  // CHECK: switch_enum [[ARG2_COPY]] : $Optional<C>, case #Optional.some!enumelt: [[TRUE:bb[0-9]+]], case #Optional.none!enumelt: [[FALSE:bb[0-9]+]]

  if let x = c {
    // CHECK: [[TRUE]]({{.*}} : @owned $C):
    use(x)
    // CHECK:   br [[EPILOG_BB:bb[0-9]+]]
  } else if a {
    // CHECK: [[FALSE]]:
    // CHECK:   cond_br {{.*}}, [[TRUE2:bb[0-9]+]], [[FALSE2:bb[0-9]+]]
    //
    // CHECK: [[TRUE2]]:
    // CHECK:   apply
    // CHECK:   br [[EPILOG_BB]]
    foo()
    break label3
    foo()    // expected-warning {{will never be executed}}
  }
  // CHECK: [[FALSE2]]:
  // CHECK: br [[EPILOG_BB]]

  // CHECK: [[EPILOG_BB]]:
  // CHECK: return


}


// CHECK-LABEL: sil hidden [ossa] @$s10statements13test_if_breakyySbF
func test_if_break(_ a : Bool) {
  // CHECK: br [[LOOP:bb[0-9]+]]
  // CHECK: [[LOOP]]:
  // CHECK-NEXT: struct_extract {{.*}}
  // CHECK-NEXT: cond_br {{.*}}, [[LOOPTRUE:bb[0-9]+]], [[EXIT:bb[0-9]+]]
  while a {
    if a {
      foo()
      break  // breaks out of while, not if.
    }
    foo()
  }

  // CHECK: [[LOOPTRUE]]:
  // CHECK-NEXT: struct_extract {{.*}}
  // CHECK-NEXT: cond_br {{.*}}, [[IFTRUE:bb[0-9]+]], [[IFFALSE:bb[0-9]+]]

  // [[IFTRUE]]:
  // CHECK: function_ref statements.foo
  // CHECK: br [[OUT:bb[0-9]+]]

  // CHECK: [[IFFALSE]]:
  // CHECK: function_ref statements.foo
  // CHECK: br [[LOOP]]

  // CHECK: [[EXIT]]:
  // CHECK: br [[OUT]]

  // CHECK: [[OUT]]:
  // CHECK:   return
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements7test_doyyF
func test_do() {
  // CHECK: integer_literal $Builtin.IntLiteral, 0
  // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
  // CHECK: apply [[BAR]](
  bar(0)
  // CHECK-NOT: br bb
  do {
    // CHECK: [[CTOR:%.*]] = function_ref @$s10statements7MyClassC{{[_0-9a-zA-Z]*}}fC
    // CHECK: [[OBJ:%.*]] = apply [[CTOR]](
    let obj = MyClass()
    _ = obj
    
    // CHECK: integer_literal $Builtin.IntLiteral, 1
    // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
    // CHECK: apply [[BAR]](
    bar(1)

    // CHECK-NOT: br bb
    // CHECK: destroy_value [[OBJ]]
    // CHECK-NOT: br bb
  }

  // CHECK: integer_literal $Builtin.IntLiteral, 2
  // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
  // CHECK: apply [[BAR]](
  bar(2)
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements15test_do_labeledyyF
func test_do_labeled() {
  // CHECK: integer_literal $Builtin.IntLiteral, 0
  // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
  // CHECK: apply [[BAR]](
  bar(0)
  // CHECK: br bb1
  // CHECK: bb1:
  lbl: do {
    // CHECK: [[CTOR:%.*]] = function_ref @$s10statements7MyClassC{{[_0-9a-zA-Z]*}}fC
    // CHECK: [[OBJ:%.*]] = apply [[CTOR]](
    let obj = MyClass()
    _ = obj

    // CHECK: integer_literal $Builtin.IntLiteral, 1
    // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
    // CHECK: apply [[BAR]](
    bar(1)

    // CHECK: [[GLOBAL:%.*]] = function_ref @$s10statements11global_condSbvau
    // CHECK: cond_br {{%.*}}, bb2, bb3
    if (global_cond) {
      // CHECK: bb2:
      // CHECK: destroy_value [[OBJ]]
      // CHECK: br bb1
      continue lbl
    }

    // CHECK: bb3:
    // CHECK: integer_literal $Builtin.IntLiteral, 2
    // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
    // CHECK: apply [[BAR]](
    bar(2)

    // CHECK: [[GLOBAL:%.*]] = function_ref @$s10statements11global_condSbvau
    // CHECK: cond_br {{%.*}}, bb4, bb5
    if (global_cond) {
      // CHECK: bb4:
      // CHECK: destroy_value [[OBJ]]
      // CHECK: br bb6
      break lbl
    }

    // CHECK: bb5:
    // CHECK: integer_literal $Builtin.IntLiteral, 3
    // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
    // CHECK: apply [[BAR]](
    bar(3)

    // CHECK: destroy_value [[OBJ]]
    // CHECK: br bb6
  }

  // CHECK: integer_literal $Builtin.IntLiteral, 4
  // CHECK: [[BAR:%.*]] = function_ref @$s10statements3baryySiF
  // CHECK: apply [[BAR]](
  bar(4)
}


func callee1() {}
func callee2() {}
func callee3() {}

// CHECK-LABEL: sil hidden [ossa] @$s10statements11defer_test1yyF
func defer_test1() {
  defer { callee1() }
  defer { callee2() }
  callee3()
  
  // CHECK: [[C3:%.*]] = function_ref @$s10statements7callee3yyF
  // CHECK: apply [[C3]]
  // CHECK: [[C2:%.*]] = function_ref @$s10statements11defer_test1yyF6
  // CHECK: apply [[C2]]
  // CHECK: [[C1:%.*]] = function_ref @$s10statements11defer_test1yyF6
  // CHECK: apply [[C1]]
}
// CHECK: sil private [ossa] @$s10statements11defer_test1yyF6
// CHECK: function_ref @{{.*}}callee1yyF

// CHECK: sil private [ossa] @$s10statements11defer_test1yyF6
// CHECK: function_ref @{{.*}}callee2yyF

// CHECK-LABEL: sil hidden [ossa] @$s10statements11defer_test2yySbF
func defer_test2(_ cond : Bool) {
  // CHECK: [[C3:%.*]] = function_ref @{{.*}}callee3yyF
  // CHECK: apply [[C3]]
  callee3()
  
// test the condition.
// CHECK:  [[CONDTRUE:%.*]] = struct_extract {{.*}}
// CHECK: cond_br [[CONDTRUE]], [[BODY:bb[0-9]+]], [[EXIT:bb[0-9]+]]
  while cond {
// CHECK: [[BODY]]:
  // CHECK: [[C2:%.*]] = function_ref @{{.*}}callee2yyF
  // CHECK: apply [[C2]]

  // CHECK: [[C1:%.*]] = function_ref @$s10statements11defer_test2yySbF6
  // CHECK: apply [[C1]]
  // CHECK: br [[RETURN:bb[0-9]+]]
    defer { callee1() }
    callee2()
    break
  }
  
// CHECK: [[EXIT]]:
// CHECK: br [[RETURN]]

// CHECK: [[RETURN]]:
// CHECK: [[C3:%.*]] = function_ref @{{.*}}callee3yyF
// CHECK: apply [[C3]]

  callee3()
}

func generic_callee_1<T>(_: T) {}
func generic_callee_2<T>(_: T) {}
func generic_callee_3<T>(_: T) {}

// CHECK-LABEL: sil hidden [ossa] @$s10statements16defer_in_generic{{[_0-9a-zA-Z]*}}F
func defer_in_generic<T>(_ x: T) {
  // CHECK: [[C3:%.*]] = function_ref @$s10statements16generic_callee_3{{[_0-9a-zA-Z]*}}F
  // CHECK: apply [[C3]]<T>
  // CHECK: [[C2:%.*]] = function_ref @$s10statements16defer_in_genericyyxlF6
  // CHECK: apply [[C2]]<T>
  // CHECK: [[C1:%.*]] = function_ref @$s10statements16defer_in_genericyyxlF6
  // CHECK: apply [[C1]]<T>
  defer { generic_callee_1(x) }
  defer { generic_callee_2(x) }
  generic_callee_3(x)
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements017defer_in_closure_C8_genericyyxlF : $@convention(thin) <T> (@in_guaranteed T) -> ()
func defer_in_closure_in_generic<T>(_ x: T) {
  // CHECK-LABEL: sil private [ossa] @$s10statements017defer_in_closure_C8_genericyyxlFyycfU_ : $@convention(thin) <T> () -> ()
  _ = {
    // CHECK-LABEL: sil private [ossa] @$s10statements017defer_in_closure_C8_genericyyxlFyycfU_6$deferL_yylF : $@convention(thin) <T> () -> ()
    defer { generic_callee_1(T.self) } // expected-warning {{'defer' statement at end of scope always executes immediately}}{{5-10=do}}
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements13defer_mutableyySiF
func defer_mutable(_ x: Int) {
  var x = x
  // CHECK: [[BOX:%.*]] = alloc_box ${ var Int }
  // CHECK-NEXT: project_box [[BOX]]
  // CHECK-NOT: [[BOX]]
  // CHECK: function_ref @$s10statements13defer_mutableyySiF6$deferL_yyF : $@convention(thin) (@inout_aliasable Int) -> ()
  // CHECK-NOT: [[BOX]]
  // CHECK: destroy_value [[BOX]]
  defer { _ = x } // expected-warning {{'defer' statement at end of scope always executes immediately}}{{3-8=do}}
}

protocol StaticFooProtocol { static func foo() }

func testDeferOpenExistential(_ b: Bool, type: StaticFooProtocol.Type) {
  defer { type.foo() }
  if b { return }
  return
}




// CHECK-LABEL: sil hidden [ossa] @$s10statements22testRequireExprPatternyySiF

func testRequireExprPattern(_ a : Int) {
  marker_1()
  // CHECK: [[M1:%[0-9]+]] = function_ref @$s10statements8marker_1yyF : $@convention(thin) () -> ()
  // CHECK-NEXT: apply [[M1]]() : $@convention(thin) () -> ()

  // CHECK: function_ref Swift.~= infix<A where A: Swift.Equatable>(A, A) -> Swift.Bool
  // CHECK: cond_br {{.*}}, bb1, bb2
  guard case 4 = a else { marker_2(); return }

  // Fall through case comes first.

  // CHECK: bb1:
  // CHECK: [[M3:%[0-9]+]] = function_ref @$s10statements8marker_3yyF : $@convention(thin) () -> ()
  // CHECK-NEXT: apply [[M3]]() : $@convention(thin) () -> ()
  // CHECK-NEXT: br bb3
  marker_3()

  // CHECK: bb2:
  // CHECK: [[M2:%[0-9]+]] = function_ref @$s10statements8marker_2yyF : $@convention(thin) () -> ()
  // CHECK-NEXT: apply [[M2]]() : $@convention(thin) () -> ()
  // CHECK-NEXT: br bb3

  // CHECK: bb3:
  // CHECK-NEXT: tuple ()
  // CHECK-NEXT: return
}


// CHECK-LABEL: sil hidden [ossa] @$s10statements20testRequireOptional1yS2iSgF
// CHECK: bb0([[ARG:%.*]] : $Optional<Int>):
// CHECK-NEXT:   debug_value [[ARG]] : $Optional<Int>, let, name "a"
// CHECK-NEXT:   switch_enum [[ARG]] : $Optional<Int>, case #Optional.some!enumelt: [[SOME:bb[0-9]+]], case #Optional.none!enumelt: [[NONE:bb[0-9]+]]
func testRequireOptional1(_ a : Int?) -> Int {

  // CHECK: [[SOME]]([[PAYLOAD:%.*]] : $Int):
  // CHECK-NEXT:   debug_value [[PAYLOAD]] : $Int, let, name "t"
  // CHECK-NEXT:   return [[PAYLOAD]] : $Int
  guard let t = a else { abort() }

  // CHECK: [[NONE]]:
  // CHECK-NEXT:    // function_ref statements.abort() -> Swift.Never
  // CHECK-NEXT:    [[FUNC_REF:%.*]] = function_ref @$s10statements5aborts5NeverOyF
  // CHECK-NEXT:    apply [[FUNC_REF]]() : $@convention(thin) () -> Never
  // CHECK-NEXT:    unreachable
  return t
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements20testRequireOptional2yS2SSgF
// CHECK: bb0([[ARG:%.*]] : @guaranteed $Optional<String>):
// CHECK-NEXT:   debug_value [[ARG]] : $Optional<String>, let, name "a"
// CHECK-NEXT:   [[ARG_COPY:%.*]] = copy_value [[ARG]] : $Optional<String>
// CHECK-NEXT:   switch_enum [[ARG_COPY]] : $Optional<String>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]
func testRequireOptional2(_ a : String?) -> String {
  guard let t = a else { abort() }

  // CHECK:  [[SOME_BB]]([[STR:%.*]] : @owned $String):
  // CHECK-NEXT:   debug_value [[STR]] : $String, let, name "t"
  // CHECK-NEXT:   [[BORROWED_STR:%.*]] = begin_borrow [[STR]]
  // CHECK-NEXT:   [[RETURN:%.*]] = copy_value [[BORROWED_STR]]
  // CHECK-NEXT:   end_borrow [[BORROWED_STR]]
  // CHECK-NEXT:   destroy_value [[STR]] : $String
  // CHECK-NEXT:   return [[RETURN]] : $String

  // CHECK: [[NONE_BB]]:
  // CHECK-NEXT:   // function_ref statements.abort() -> Swift.Never
  // CHECK-NEXT:   [[ABORT_FUNC:%.*]] = function_ref @$s10statements5aborts5NeverOyF
  // CHECK-NEXT:   [[NEVER:%.*]] = apply [[ABORT_FUNC]]()
  // CHECK-NEXT:   unreachable
  return t
}


// CHECK-LABEL: sil hidden [ossa] @$s10statements19testCleanupEmission{{[_0-9a-zA-Z]*}}F
// <rdar://problem/20563234> let-else problem: cleanups for bound patterns shouldn't be run in the else block
protocol MyProtocol {}
func testCleanupEmission<T>(_ x: T) {
  // SILGen shouldn't crash/verify abort on this example.
  guard let x2 = x as? MyProtocol else { return }
  _ = x2
}


// CHECK-LABEL: sil hidden [ossa] @$s10statements15test_is_patternyyAA9BaseClassCF
func test_is_pattern(_ y : BaseClass) {
  // checked_cast_br %0 : $BaseClass to DerivedClass
  guard case is DerivedClass = y else { marker_1(); return }

  marker_2()
}

// CHECK-LABEL: sil hidden [ossa] @$s10statements15test_as_patternyAA12DerivedClassCAA04BaseF0CF
func test_as_pattern(_ y : BaseClass) -> DerivedClass {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $BaseClass):
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK:   checked_cast_br [[ARG_COPY]] : $BaseClass to DerivedClass
  guard case let result as DerivedClass = y else {  }
  // CHECK: bb{{.*}}({{.*}} : @owned $DerivedClass):


  // CHECK: bb{{.*}}([[PTR:%[0-9]+]] : @owned $DerivedClass):
  // CHECK-NEXT: debug_value [[PTR]] : $DerivedClass, let, name "result"
  // CHECK-NEXT: [[BORROWED_PTR:%.*]] = begin_borrow [[PTR]]
  // CHECK-NEXT: [[RESULT:%.*]] = copy_value [[BORROWED_PTR]]
  // CHECK-NEXT: end_borrow [[BORROWED_PTR]]
  // CHECK-NEXT: destroy_value [[PTR]] : $DerivedClass
  // CHECK-NEXT: return [[RESULT]] : $DerivedClass
  return result
}
// CHECK-LABEL: sil hidden [ossa] @$s10statements22let_else_tuple_bindingyS2i_SitSgF
func let_else_tuple_binding(_ a : (Int, Int)?) -> Int {

  // CHECK: bb0([[ARG:%.*]] : $Optional<(Int, Int)>):
  // CHECK-NEXT:   debug_value [[ARG]] : $Optional<(Int, Int)>, let, name "a"
  // CHECK-NEXT:   switch_enum [[ARG]] : $Optional<(Int, Int)>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]

  guard let (x, y) = a else { }
  _ = y
  return x

  // CHECK: [[SOME_BB]]([[PAYLOAD:%.*]] : $(Int, Int)):
  // CHECK-NEXT:   ([[PAYLOAD_1:%.*]], [[PAYLOAD_2:%.*]]) = destructure_tuple [[PAYLOAD]]
  // CHECK-NEXT:   debug_value [[PAYLOAD_1]] : $Int, let, name "x"
  // CHECK-NEXT:   debug_value [[PAYLOAD_2]] : $Int, let, name "y"
  // CHECK-NEXT:   return [[PAYLOAD_1]] : $Int
}

