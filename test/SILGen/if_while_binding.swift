
// RUN: %target-swift-emit-silgen -module-name if_while_binding -Xllvm -sil-full-demangle %s | %FileCheck %s

func foo() -> String? { return "" }
func bar() -> String? { return "" }

func a(_ x: String) {}
func b(_ x: String) {}
func c(_ x: String) {}

func marker_1() {}
func marker_2() {}
func marker_3() {}


// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0A8_no_else{{[_0-9a-zA-Z]*}}F
func if_no_else() {
  // CHECK:   [[FOO:%.*]] = function_ref @$s16if_while_binding3fooSSSgyF
  // CHECK:   [[OPT_RES:%.*]] = apply [[FOO]]()
  // CHECK:   switch_enum [[OPT_RES]] : $Optional<String>, case #Optional.some!enumelt: [[YES:bb[0-9]+]], case #Optional.none!enumelt: [[NO:bb[0-9]+]]
  //
  // CHECK: [[NO]]:
  // CHECK:  br [[CONT:bb[0-9]+]]
  if let x = foo() {
  // CHECK: [[YES]]([[VAL:%[0-9]+]] : @owned $String):
  // CHECK:   [[BORROWED_VAL:%.*]] = begin_borrow [[VAL]]
  // CHECK:   [[A:%.*]] = function_ref @$s16if_while_binding1a
  // CHECK:   apply [[A]]([[BORROWED_VAL]])
  // CHECK:   end_borrow [[BORROWED_VAL]]
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT]]
    a(x)
  }
  // CHECK: [[CONT]]:
  // CHECK-NEXT:   tuple ()
}
// CHECK: } // end sil function '$s16if_while_binding0A8_no_else{{[_0-9a-zA-Z]*}}F'

// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0A11_else_chainyyF : $@convention(thin) () -> () {
func if_else_chain() {
  // CHECK:   [[FOO:%.*]] = function_ref @$s16if_while_binding3foo{{[_0-9a-zA-Z]*}}F
  // CHECK-NEXT:   [[OPT_RES:%.*]] = apply [[FOO]]()
  // CHECK-NEXT:   switch_enum [[OPT_RES]] : $Optional<String>, case #Optional.some!enumelt: [[YESX:bb[0-9]+]], case #Optional.none!enumelt: [[NOX:bb[0-9]+]]
  if let x = foo() {
  // CHECK: [[YESX]]([[VAL:%[0-9]+]] : @owned $String):
  // CHECK:   debug_value [[VAL]] : $String, let, name "x"
  // CHECK:   [[BORROWED_VAL:%.*]] = begin_borrow [[VAL]]
  // CHECK:   [[A:%.*]] = function_ref @$s16if_while_binding1a
  // CHECK:   apply [[A]]([[BORROWED_VAL]])
  // CHECK:   end_borrow [[BORROWED_VAL]]
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT_X:bb[0-9]+]]
    a(x)
  //
  // CHECK: [[NOX]]:
  // CHECK:   alloc_box ${ var String }, var, name "y"
  // CHECK:   switch_enum {{.*}} : $Optional<String>, case #Optional.some!enumelt: [[YESY:bb[0-9]+]], case #Optional.none!enumelt: [[ELSE:bb[0-9]+]]
  } else if var y = bar() {
  // CHECK: [[YESY]]([[VAL:%[0-9]+]] : @owned $String):
  // CHECK:   br [[CONT_Y:bb[0-9]+]]
    b(y)
  } else {
    // CHECK: [[ELSE]]:
    // CHECK:   dealloc_box {{.*}} ${ var String }
    // CHECK:   function_ref if_while_binding.c
    c("")
    // CHECK:   br [[CONT_Y]]
  }

  // CHECK: [[CONT_Y]]:
  //   br [[CONT_X]]
  // CHECK: [[CONT_X]]:
}

// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0B5_loopyyF : $@convention(thin) () -> () {
func while_loop() {
  // CHECK:   br [[LOOP_ENTRY:bb[0-9]+]]
  //
  // CHECK: [[LOOP_ENTRY]]:
  // CHECK:   switch_enum {{.*}} : $Optional<String>, case #Optional.some!enumelt: [[LOOP_BODY:bb[0-9]+]], case #Optional.none!enumelt: [[NO_TRAMPOLINE:bb[0-9]+]]
  //
  // CHECK: [[NO_TRAMPOLINE]]:
  // CHECK:   br [[LOOP_EXIT:bb[0-9]+]]
  while let x = foo() {
  // CHECK: [[LOOP_BODY]]([[X:%[0-9]+]] : @owned $String):
  // CHECK:   switch_enum {{.*}} : $Optional<String>, case #Optional.some!enumelt: [[YES:bb[0-9]+]], case #Optional.none!enumelt: [[FAILURE_DEST_2:bb[0-9]+]]
    if let y = bar() {
  // CHECK: [[YES]]([[Y:%[0-9]+]] : @owned $String):
      a(y)
      break
      // CHECK: destroy_value [[Y]]
      // CHECK: destroy_value [[X]]
      // CHECK:     br [[LOOP_EXIT]]
    }
  // CHECK: [[FAILURE_DEST_2]]:
  // CHECK:   destroy_value [[X]]
  // CHECK:   br [[LOOP_ENTRY]]
  }
  // CHECK: [[LOOP_EXIT]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// Don't leak alloc_stacks for address-only conditional bindings in 'while'.
// <rdar://problem/16202294>
// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0B13_loop_generic{{[_0-9a-zA-Z]*}}F
// CHECK:         br [[COND:bb[0-9]+]]
// CHECK:       [[COND]]:
// CHECK:         [[X:%.*]] = alloc_stack [lexical] $T, let, name "x"
// CHECK:         [[OPTBUF:%[0-9]+]] = alloc_stack $Optional<T>
// CHECK:         switch_enum_addr {{.*}}, case #Optional.some!enumelt: [[LOOPBODY:bb.*]], case #Optional.none!enumelt: [[OUT:bb[0-9]+]]
// CHECK:       [[LOOPBODY]]:
// CHECK:         [[ENUMVAL:%.*]] = unchecked_take_enum_data_addr
// CHECK:         copy_addr [take] [[ENUMVAL]] to [init] [[X]]
// CHECK:         destroy_addr [[X]]
// CHECK:         dealloc_stack [[X]]
// CHECK:         br [[COND]]
// CHECK:       [[OUT]]:
// CHECK:         dealloc_stack [[OPTBUF]]
// CHECK:         dealloc_stack [[X]]
// CHECK:         return
// CHECK: } // end sil function '$s16if_while_binding0B13_loop_generic{{[_0-9a-zA-Z]*}}F'
func while_loop_generic<T>(_ source: () -> T?) {
  while let x = source() {
  }
}

// <rdar://problem/19382942> Improve 'if let' to avoid optional pyramid of doom
// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0B11_loop_multiyyF
func while_loop_multi() {
  // CHECK:   br [[LOOP_ENTRY:bb[0-9]+]]
  // CHECK: [[LOOP_ENTRY]]:
  // CHECK:         switch_enum {{.*}}, case #Optional.some!enumelt: [[CHECKBUF2:bb.*]], case #Optional.none!enumelt: [[NONE_TRAMPOLINE:bb[0-9]+]]
  //
  // CHECK: [[NONE_TRAMPOLINE]]:
  // CHECK:   br [[LOOP_EXIT0:bb[0-9]+]]

  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : @owned $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"

  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt: [[LOOP_BODY:bb.*]], case #Optional.none!enumelt: [[LOOP_EXIT2a:bb[0-9]+]]

  // CHECK: [[LOOP_EXIT2a]]:
  // CHECK: destroy_value [[A]]
  // CHECK: br [[LOOP_EXIT0]]

  // CHECK: [[LOOP_BODY]]([[B:%[0-9]+]] : @owned $String):
    while let a = foo(), let b = bar() {
    // CHECK:   debug_value [[B]] : $String, let, name "b"
    // CHECK:   [[BORROWED_A:%.*]] = begin_borrow [[A]]
    // CHECK:   [[A_COPY:%.*]] = copy_value [[BORROWED_A]]
    // CHECK:   debug_value [[A_COPY]] : $String, let, name "c"
    // CHECK:   destroy_value [[A_COPY]]
    // CHECK:   destroy_value [[B]]
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[LOOP_ENTRY]]
    let c = a
  }
  // CHECK: [[LOOP_EXIT0]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0A6_multiyyF
func if_multi() {
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt: [[CHECKBUF2:bb.*]], case #Optional.none!enumelt: [[NONE_TRAMPOLINE:bb[0-9]+]]
  //
  // CHECK: [[NONE_TRAMPOLINE]]:
  // CHECK:   br [[IF_DONE:bb[0-9]+]]

  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : @owned $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"
  // CHECK:   [[B:%[0-9]+]] = alloc_box ${ var String }, var, name "b"
  // CHECK:   [[PB:%[0-9]+]] = project_box [[B]]
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt: [[IF_BODY:bb.*]], case #Optional.none!enumelt: [[IF_EXIT1a:bb[0-9]+]]

  // CHECK: [[IF_EXIT1a]]:
  // CHECK:   dealloc_box {{.*}} ${ var String }
  // CHECK:   destroy_value [[A]]
  // CHECK:   br [[IF_DONE]]

  // CHECK: [[IF_BODY]]([[BVAL:%[0-9]+]] : @owned $String):
  if let a = foo(), var b = bar() {
    // CHECK:   store [[BVAL]] to [init] [[PB]] : $*String
    // CHECK:   debug_value {{.*}} : $String, let, name "c"
    // CHECK:   destroy_value [[B]]
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[IF_DONE]]
    let c = a
  }
  // CHECK: [[IF_DONE]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0A11_multi_elseyyF
func if_multi_else() {
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt: [[CHECKBUF2:bb.*]], case #Optional.none!enumelt: [[NONE_TRAMPOLINE:bb[0-9]+]]
  //
  // CHECK: [[NONE_TRAMPOLINE]]:
  // CHECK:   br [[ELSE:bb[0-9]+]]
  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : @owned $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"
  // CHECK:   [[B:%[0-9]+]] = alloc_box ${ var String }, var, name "b"
  // CHECK:   [[PB:%[0-9]+]] = project_box [[B]]
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt: [[IF_BODY:bb.*]], case #Optional.none!enumelt: [[IF_EXIT1a:bb[0-9]+]]
  
    // CHECK: [[IF_EXIT1a]]:
    // CHECK:   dealloc_box {{.*}} ${ var String }
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[ELSE]]

  // CHECK: [[IF_BODY]]([[BVAL:%[0-9]+]] : @owned $String):
  if let a = foo(), var b = bar() {
    // CHECK:   store [[BVAL]] to [init] [[PB]] : $*String
    // CHECK:   debug_value {{.*}} : $String, let, name "c"
    // CHECK:   destroy_value [[B]]
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[IF_DONE:bb[0-9]+]]
    let c = a
  } else {
    let d = 0
    // CHECK: [[ELSE]]:
    // CHECK:   debug_value {{.*}} : $Int, let, name "d"
    // CHECK:   br [[IF_DONE]]
 }
  // CHECK: [[IF_DONE]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0A12_multi_whereyyF
func if_multi_where() {
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt: [[CHECKBUF2:bb.*]], case #Optional.none!enumelt: [[NONE_TRAMPOLINE:bb[0-9]+]]
  //
  // CHECK: [[NONE_TRAMPOLINE]]:
  // CHECK:   br [[DONE:bb[0-9]+]]
  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : @owned $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"
  // CHECK:   [[BBOX:%[0-9]+]] = alloc_box ${ var String }, var, name "b"
  // CHECK:   [[PB:%[0-9]+]] = project_box [[BBOX]]
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt: [[CHECK_WHERE:bb.*]], case #Optional.none!enumelt: [[IF_EXIT1a:bb[0-9]+]]
  // CHECK: [[IF_EXIT1a]]:
  // CHECK:   dealloc_box {{.*}} ${ var String }
  // CHECK:   destroy_value [[A]]
  // CHECK:   br [[DONE]]

  // CHECK: [[CHECK_WHERE]]([[B:%[0-9]+]] : @owned $String):
  // CHECK:   struct_extract {{.*}}
  // CHECK:   cond_br {{.*}}, [[IF_BODY:bb[0-9]+]], [[IF_EXIT3:bb[0-9]+]]
  if let a = foo(), var b = bar(), a == b {
    // CHECK: [[IF_BODY]]:
    // CHECK:   destroy_value [[BBOX]]
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[DONE]]
    let c = a
  }
  // CHECK: [[IF_EXIT3]]:
  // CHECK:   destroy_value [[BBOX]]
  // CHECK:   destroy_value [[A]]
  // CHECK:   br [[DONE]]
  // CHECK: [[DONE]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}


// <rdar://problem/19797158> Swift 1.2's "if" has 2 behaviors. They could be unified.
// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding0A16_leading_booleanyySiF
func if_leading_boolean(_ a : Int) {
  // Test the boolean condition.
  
  // CHECK: debug_value %0 : $Int, let, name "a"
  // CHECK: [[EQRESULT:%[0-9]+]] = apply {{.*}}(%0, %0{{.*}}) : $@convention({{.*}}) (Int, Int{{.*}}) -> Bool

  // CHECK: [[EQRESULTI1:%[0-9]+]] = struct_extract {{.*}} : $Bool, #Bool._value
  // CHECK-NEXT: cond_br [[EQRESULTI1]], [[CHECKFOO:bb[0-9]+]], [[ELSE:bb[0-9]+]]

  // Call Foo and test for the optional being present.
// CHECK: [[CHECKFOO]]:
  // CHECK: [[OPTRESULT:%[0-9]+]] = apply {{.*}}() : $@convention(thin) () -> @owned Optional<String>
  
  // CHECK:   switch_enum [[OPTRESULT]] : $Optional<String>, case #Optional.some!enumelt: [[SUCCESS:bb.*]], case #Optional.none!enumelt: [[IFDONE:bb[0-9]+]]

// CHECK: [[SUCCESS]]([[B:%[0-9]+]] : @owned $String):
  // CHECK:   debug_value [[B]] : $String, let, name "b"
  // CHECK:   [[BORROWED_B:%.*]] = begin_borrow [[B]]
  // CHECK:   [[B_COPY:%.*]] = copy_value [[BORROWED_B]]
  // CHECK:   debug_value [[B_COPY]] : $String, let, name "c"
  // CHECK:   destroy_value [[B_COPY]]
  // CHECK:   destroy_value [[B]]
  // CHECK:   br [[IFDONE:bb[0-9]+]]
  if a == a, let b = foo() {
    let c = b
  }
  // CHECK: [[ELSE]]:
  // CHECK:   br [[IFDONE]]
  // CHECK: [[IFDONE]]:
  // CHECK-NEXT: tuple ()

}


/// <rdar://problem/20364869> Assertion failure when using 'as' pattern in 'if let'
class BaseClass {}
class DerivedClass : BaseClass {}

// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding20testAsPatternInIfLetyyAA9BaseClassCSgF
func testAsPatternInIfLet(_ a : BaseClass?) {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $Optional<BaseClass>):
  // CHECK:   debug_value [[ARG]] : $Optional<BaseClass>, let, name "a"
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]] : $Optional<BaseClass>
  // CHECK:   switch_enum [[ARG_COPY]] : $Optional<BaseClass>, case #Optional.some!enumelt: [[OPTPRESENTBB:bb[0-9]+]], case #Optional.none!enumelt: [[NILBB:bb[0-9]+]]

  // CHECK: [[NILBB]]:
  // CHECK:   br [[EXITBB:bb[0-9]+]]

  // CHECK: [[OPTPRESENTBB]]([[CLS:%.*]] : @owned $BaseClass):
  // CHECK:   checked_cast_br BaseClass in [[CLS]] : $BaseClass to DerivedClass, [[ISDERIVEDBB:bb[0-9]+]], [[ISBASEBB:bb[0-9]+]]

  // CHECK: [[ISDERIVEDBB]]([[DERIVED_CLS:%.*]] : @owned $DerivedClass):
  // CHECK:   [[DERIVED_CLS_SOME:%.*]] = enum $Optional<DerivedClass>, #Optional.some!enumelt, [[DERIVED_CLS]] : $DerivedClass
  // CHECK:   br [[MERGE:bb[0-9]+]]([[DERIVED_CLS_SOME]] : $Optional<DerivedClass>)

  // CHECK: [[ISBASEBB]]([[BASECLASS:%.*]] : @owned $BaseClass):
  // CHECK:   destroy_value [[BASECLASS]] : $BaseClass
  // CHECK:   = enum $Optional<DerivedClass>, #Optional.none!enumelt
  // CHECK:   br [[MERGE]](

  // CHECK: [[MERGE]]([[OPTVAL:%[0-9]+]] : @owned $Optional<DerivedClass>):
  // CHECK:    switch_enum [[OPTVAL]] : $Optional<DerivedClass>, case #Optional.some!enumelt: [[ISDERIVEDBB:bb[0-9]+]], case #Optional.none!enumelt: [[NILBB:bb[0-9]+]]

  // CHECK: [[ISDERIVEDBB]]([[DERIVEDVAL:%[0-9]+]] : @owned $DerivedClass):
  // CHECK:   [[BORROWED_DERIVED_VAL:%.*]] = begin_borrow [lexical] [[DERIVEDVAL]]
  // CHECK:   debug_value [[BORROWED_DERIVED_VAL]] : $DerivedClass
  // => SEMANTIC SIL TODO: This is benign, but scoping wise, this end borrow should be after derived val.
  // CHECK:   destroy_value [[DERIVEDVAL]] : $DerivedClass
  // CHECK:   br [[EXITBB]]
  
  // CHECK: [[EXITBB]]:
  // CHECK:   tuple ()
  // CHECK:   return
  if case let b as DerivedClass = a {

  }
}

// <rdar://problem/22312114> if case crashes swift - bools not supported in let/else yet
// CHECK-LABEL: sil hidden [ossa] @$s16if_while_binding12testCaseBoolyySbSgF
func testCaseBool(_ value : Bool?) {
  // CHECK: bb0([[ARG:%.*]] : $Optional<Bool>):
  // CHECK: switch_enum [[ARG]] : $Optional<Bool>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_TRAMPOLINE:bb[0-9]+]]
  //
  // CHECK: [[NONE_TRAMPOLINE]]:
  // CHECK:   br [[CONT_BB:bb[0-9]+]]
  //
  // CHECK: [[SOME_BB]]([[PAYLOAD:%.*]] : $Bool):
  // CHECK:   [[ISTRUE:%[0-9]+]] = struct_extract [[PAYLOAD]] : $Bool, #Bool._value
  // CHECK:   cond_br [[ISTRUE]], [[TRUE_BB:bb[0-9]+]], [[FALSE_TRAMPOLINE:bb[0-9]+]]

  // CHECK: [[FALSE_TRAMPOLINE]]:
  // CHECK:   br [[CONT_BB]]

  // CHECK: [[TRUE_BB]]:
  // CHECK:   function_ref @$s16if_while_binding8marker_1yyF
  // CHECK:   br [[CONT_BB]]
  if case true? = value {
    marker_1()
  }

  // CHECK: [[CONT_BB]]:
  // CHECK:   switch_enum [[ARG]] : $Optional<Bool>, case #Optional.some!enumelt: [[SUCC_BB_2:bb[0-9]+]], case #Optional.none!enumelt: [[NO_TRAMPOLINE_2:bb[0-9]+]]

  // CHECK: [[NO_TRAMPOLINE_2]]:
  // CHECK:   br [[EPILOG_BB:bb[0-9]+]]

  // CHECK: [[SUCC_BB_2]]([[PAYLOAD2:%.*]] : $Bool):
  // CHECK:   [[ISTRUE:%[0-9]+]] = struct_extract [[PAYLOAD2]] : $Bool, #Bool._value
  // CHECK:   cond_br [[ISTRUE]], [[TRUE3_BB:bb[0-9]+]], [[FALSE2_BB:bb[0-9]+]]

  // CHECK: [[TRUE3_BB]]:
  // CHECK:   br [[EPILOG_BB:bb[0-9]+]]
  //
  // CHECK: [[FALSE2_BB]]:
  // CHECK:   function_ref @$s16if_while_binding8marker_2yyF
  // CHECK:   br [[EPILOG_BB]]

  // CHECK: [[EPILOG_BB]]:
  // CHECK:   return
  if case false? = value {
    marker_2()
  }
}
