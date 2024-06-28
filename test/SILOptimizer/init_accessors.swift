// RUN: %target-swift-frontend -primary-file %s -Onone -emit-sil \
// RUN:   -Xllvm -sil-print-after=raw-sil-inst-lowering \
// RUN:   -o /dev/null -module-name init_accessors 2>&1 | %FileCheck %s

class NSObject {}

struct TestInit {
  var x: Int
  var y: Int
  var full: (Int, Int)

  var point: (Int, Int) {
    // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors8TestInitV5pointSi_Sitvi : $@convention(thin) (Int, Int, @inout Int, @thin TestInit.Type) -> (@out Int, @out (Int, Int))
    // CHECK: bb0([[Y_REF:%.*]] : $*Int, [[FULL_REF:%.*]] : $*(Int, Int), [[X_VAL:%.*]] : $Int, [[Y_VAL:%.*]] : $Int, [[X_REF:%.*]] : $*Int, [[METATYPE:%.*]] : $@thin TestInit.Type):
    //
    // CHECK: [[INITIAL_VALUE:%.*]] = tuple ([[X_VAL]] : $Int, [[Y_VAL]] : $Int)
    // CHECK: ([[X_VAL:%.*]], [[Y_VAL:%.*]]) = destructure_tuple [[INITIAL_VALUE]] : $(Int, Int)
    // CHECK: [[Y_ACCESS:%.*]] = begin_access [modify] [static] [[Y_REF]] : $*Int
    // CHECK-NEXT: store [[Y_VAL]] to [trivial] [[Y_ACCESS]] : $*Int
    // CHECK-NEXT: end_access [[Y_ACCESS]] : $*Int
    //
    // CHECK-NEXT: [[X_ACCESS:%.*]] = begin_access [read] [static] [[X_REF]] : $*Int
    // CHECK-NEXT: [[X_VAL:%.*]] = load [trivial] [[X_ACCESS]] : $*Int
    // CHECK-NEXT: end_access [[X_ACCESS]] : $*Int
    //
    // CHECK-NEXT: [[Y_ACCESS:%.*]] = begin_access [read] [static] [[Y_REF]] : $*Int
    // CHECK-NEXT: [[Y_VAL:%.*]] = load [trivial] [[Y_ACCESS]] : $*Int
    // CHECK-NEXT: end_access [[Y_ACCESS]] : $*Int
    //
    // CHECK-NEXT: [[FULL_ACCESS:%.*]] = begin_access [modify] [static] [[FULL_REF]] : $*(Int, Int)
    // CHECK-NEXT: tuple_addr_constructor [init] [[FULL_ACCESS]] : $*(Int, Int) with ([[X_VAL]] : $Int, [[Y_VAL]] : $Int)
    // CHECK-NEXT: end_access [[FULL_ACCESS]] : $*(Int, Int)
    @storageRestrictions(initializes: y, full, accesses: x)
    init(initialValue) {
      self.y = initialValue.1
      self.full = (self.x, self.y)
    }

    get { full }
    set { full = newValue }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors8TestInitV1x1yACSi_SitcfC : $@convention(method) (Int, Int, @thin TestInit.Type) -> TestInit
  // CHECK: // function_ref TestInit.point.init
  // CHECK-NEXT: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors8TestInitV5pointSi_Sitvi : $@convention(thin) (Int, Int, @inout Int, @thin TestInit.Type) -> (@out Int, @out (Int, Int))
  // CHECK-NEXT: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]](%2) : $@convention(thin) (Int, Int, @inout Int, @thin TestInit.Type) -> (@out Int, @out (Int, Int))
  // CHECK: [[SELF_VALUE:%.*]] = begin_access [modify] [dynamic] {{.*}} : $*TestInit
  // CHECK: [[Y_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestInit, #TestInit.y
  // CHECK-NEXT: [[FULL_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestInit, #TestInit.full
  // CHECK-NEXT: ([[X_VAL:%.*]], [[Y_VAL:%.*]]) = destructure_tuple {{.*}} : $(Int, Int)
  // CHECK-NEXT: [[X_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestInit, #TestInit.x
  // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR]]([[Y_REF]], [[FULL_REF]], [[X_VAL]], [[Y_VAL]], [[X_REF]]) : $@noescape @callee_guaranteed (Int, Int, @inout Int) -> (@out Int, @out (Int, Int))
  // CHECK-NEXT: end_access [[SELF_VALUE]] : $*TestInit
  init(x: Int, y: Int) {
    self.x = x
    self.point = (x, y)
  }
}

struct TestSetter {
  var x: Int
  var y: Int

  var point: (Int, Int) {
    @storageRestrictions(accesses: x, y)
    init(initialValue) {
    }

    get { (x, y) }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors10TestSetterV1x1yACSi_SitcfC : $@convention(method) (Int, Int, @thin TestSetter.Type) -> TestSetter
  // CHECK: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors10TestSetterV5pointSi_Sitvi : $@convention(thin) (Int, Int, @inout Int, @inout Int, @thin TestSetter.Type) -> ()
  // CHECK: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]](%2) : $@convention(thin) (Int, Int, @inout Int, @inout Int, @thin TestSetter.Type) -> ()
  // CHECK: [[SELF:%.*]] = begin_access [modify] [dynamic] %14 : $*TestSetter
  // CHECK-NEXT: ([[X:%.*]], [[Y:%.*]]) = destructure_tuple {{.*}} : $(Int, Int)
  // CHECK-NEXT: [[X_REF:%.*]] = struct_element_addr [[SELF]] : $*TestSetter, #TestSetter.x
  // CHECK-NEXT: [[Y_REF:%.*]] = struct_element_addr [[SELF]] : $*TestSetter, #TestSetter.y
  // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR]]([[X]], [[Y]], [[X_REF]], [[Y_REF]]) : $@noescape @callee_guaranteed (Int, Int, @inout Int, @inout Int) -> ()
  init(x: Int, y: Int) {
    self.x = x
    self.y = y
    self.point = (x, y)
  }
}

struct TestInitThenSetter {
  var x: Int
  var y: Int

  var point: (Int, Int) {
    @storageRestrictions(initializes: x, y)
    init(initialValue) {
      self.x = initialValue.0
      self.y = initialValue.1
    }

    get { (x, y) }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors18TestInitThenSetterV1x1yACSi_SitcfC : $@convention(method) (Int, Int, @thin TestInitThenSetter.Type) -> TestInitThenSetter
  // CHECK: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors18TestInitThenSetterV5pointSi_Sitvi : $@convention(thin) (Int, Int, @thin TestInitThenSetter.Type) -> (@out Int, @out Int)
  // CHECK: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]](%2) : $@convention(thin) (Int, Int, @thin TestInitThenSetter.Type) -> (@out Int, @out Int)
  // CHECK: [[X_REF:%.*]] = struct_element_addr {{.*}} : $*TestInitThenSetter, #TestInitThenSetter.x
  // CHECK-NEXT: [[Y_REF:%.*]] = struct_element_addr {{.*}} : $*TestInitThenSetter, #TestInitThenSetter.y
  // CHECK: {{.*}} = apply [[INIT_ACCESSOR]]([[X_REF]], [[Y_REF]], {{.*}}) : $@noescape @callee_guaranteed (Int, Int) -> (@out Int, @out Int)
  //
  // CHECK: [[SETTER_REF:%.*]] = function_ref @$s14init_accessors18TestInitThenSetterV5pointSi_Sitvs : $@convention(method) (Int, Int, @inout TestInitThenSetter) -> ()
  // CHECK-NEXT: [[SETTER_CLOSURE:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[SETTER_REF]]([[SELF_VALUE:%.*]]) : $@convention(method) (Int, Int, @inout TestInitThenSetter) -> ()
  // CHECK: ([[ZERO_X:%.*]], [[ZERO_Y:%.*]]) = destructure_tuple {{.*}} : $(Int, Int)
  // CHECK: {{.*}} = apply [[SETTER_CLOSURE]]([[ZERO_X]], [[ZERO_Y]]) : $@noescape @callee_guaranteed (Int, Int) -> ()
  init(x: Int, y: Int) {
    self.point = (x, y)

    if x == 1 {
      self.point = (0, 0)
    }
  }
}

struct TestPartialInt {
  var x: Int
  var y: Int

  var pointX: Int {
    @storageRestrictions(initializes: x)
    init(newValue) {
      self.x = newValue
    }

    get { x }
    set { self.x = newValue }
  }

  var pointY: Int {
    @storageRestrictions(initializes: y)
    init(newValue) {
      self.y = newValue
    }

    get { y }
    set { self.y = newValue }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors14TestPartialIntV1x1yACSi_SitcfC : $@convention(method) (Int, Int, @thin TestPartialInt.Type) -> TestPartialInt
  //
  // CHECK: [[INIT_REF_FN:%.*]] = function_ref @$s14init_accessors14TestPartialIntV6pointXSivi : $@convention(thin) (Int, @thin TestPartialInt.Type) -> @out Int
  // CHECK: [[INIT_REF:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_REF_FN]](%2) : $@convention(thin) (Int, @thin TestPartialInt.Type) -> @out Int
  // CHECK: [[X_REF:%.*]] = struct_element_addr {{.*}} : $*TestPartialInt, #TestPartialInt.x
  // CHECK-NEXT: {{.*}} = apply [[INIT_REF]]([[X_REF]], %0) : $@noescape @callee_guaranteed (Int) -> @out Int
  //
  // CHECK: [[INIT_REF_FN:%.*]] = function_ref @$s14init_accessors14TestPartialIntV6pointYSivi : $@convention(thin) (Int, @thin TestPartialInt.Type) -> @out Int
  // CHECK: [[INIT_REF:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_REF_FN]](%2) : $@convention(thin) (Int, @thin TestPartialInt.Type) -> @out Int
  // CHECK: [[Y_REF:%.*]] = struct_element_addr {{.*}} : $*TestPartialInt, #TestPartialInt.y
  // CHECK-NEXT: {{.*}} = apply [[INIT_REF]]([[Y_REF]], %1) : $@noescape @callee_guaranteed (Int) -> @out Int
  //
  // CHECK: [[BUILTIN_ONE:%.*]] = integer_literal $Builtin.IntLiteral, 1
  // CHECK: [[SETTER_REF:%.*]] = function_ref @$s14init_accessors14TestPartialIntV6pointXSivs : $@convention(method) (Int, @inout TestPartialInt) -> ()
  // CHECK-NEXT: [[SETTER_CLOSURE:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[SETTER_REF]]({{.*}}) : $@convention(method) (Int, @inout TestPartialInt) -> ()
  // CHECK-NEXT: {{.*}} = apply [[SETTER_CLOSURE]]({{.*}}) : $@noescape @callee_guaranteed (Int) -> ()
  //
  // CHECK: [[BUILTIN_TWO:%.*]] = integer_literal $Builtin.IntLiteral, 2
  // CHECK: [[SETTER_REF:%.*]] = function_ref @$s14init_accessors14TestPartialIntV6pointYSivs : $@convention(method) (Int, @inout TestPartialInt) -> ()
  // CHECK-NEXT: [[SETTER_CLOSURE:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[SETTER_REF]]({{.*}}) : $@convention(method) (Int, @inout TestPartialInt) -> ()
  // CHECK-NEXT: {{.*}} = apply [[SETTER_CLOSURE]]({{.*}}) : $@noescape @callee_guaranteed (Int) -> ()
  init(x: Int, y: Int) {
    // Init
    self.pointX = x
    // Init
    self.pointY = y

    // Setter
    self.pointX = 1
    // Setter
    self.pointY = 2

  }
}

struct TestNoInitAndInit {
  var x: Int
  var y: Int

  var pointX: Int {
    @storageRestrictions(accesses: x)
    init(initalValue) {
    }

    get { x }
    set { }
  }

  var pointY: Int {
    @storageRestrictions(initializes: y)
    init(initialValue) {
      self.y = initialValue
    }

    get { y }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors013TestNoInitAndE0V1x1yACSi_SitcfC : $@convention(method) (Int, Int, @thin TestNoInitAndInit.Type) -> TestNoInitAndInit
  //
  // CHECK: [[INIT_REF_FN:%.*]] = function_ref @$s14init_accessors013TestNoInitAndE0V6pointXSivi : $@convention(thin) (Int, @inout Int, @thin TestNoInitAndInit.Type) -> ()
  // CHECK: [[INIT_REF:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_REF_FN]](%2) : $@convention(thin) (Int, @inout Int, @thin TestNoInitAndInit.Type) -> ()
  // CHECK: [[SELF_REF:%.*]] = begin_access [modify] [dynamic] {{.*}} : $*TestNoInitAndInit
  // CHECK-NEXT: [[X_REF:%.*]] = struct_element_addr [[SELF_REF]] : $*TestNoInitAndInit, #TestNoInitAndInit.x
  // CHECK-NEXT: {{.*}} = apply [[INIT_REF]](%0, [[X_REF]]) : $@noescape @callee_guaranteed (Int, @inout Int) -> ()
  // CHECK-NEXT: end_access [[SELF_REF]] : $*TestNoInitAndInit
  //
  // CHECK: [[INIT_REF_FN:%.*]] = function_ref @$s14init_accessors013TestNoInitAndE0V6pointYSivi : $@convention(thin) (Int, @thin TestNoInitAndInit.Type) -> @out Int
  // CHECK: [[INIT_REF:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_REF_FN]](%2) : $@convention(thin) (Int, @thin TestNoInitAndInit.Type) -> @out Int
  // CHECK: [[SELF_REF:%.*]] = begin_access [modify] [dynamic] {{.*}} : $*TestNoInitAndInit
  // CHECK-NEXT: [[Y_REF:%.*]] = struct_element_addr [[SELF_REF]] : $*TestNoInitAndInit, #TestNoInitAndInit.y
  // CHECK-NEXT: {{.*}} = apply [[INIT_REF]]([[Y_REF]], %1) : $@noescape @callee_guaranteed (Int) -> @out Int
  // CHECK-NEXT: end_access [[SELF_REF]] : $*TestNoInitAndInit
  init(x: Int, y: Int) {
    self.x = x
    self.pointX = x
    self.pointY = y
    print("Point(x: \(self.x), y: \(self.y)")
  }
}

class TestClass {
  var x: Int
  var y: (Int, [String])

  var data: (Int, (Int, [String])) {
    // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors9TestClassC4dataSi_Si_SaySSGttvi : $@convention(thin) (Int, Int, @owned Array<String>, @thick TestClass.Type) -> (@out Int, @out (Int, Array<String>))
    // CHECK: bb0([[X_REF:%.*]] : $*Int, [[Y_REF:%.*]] : $*(Int, Array<String>), [[X_VAL:%.*]] : $Int, [[Y_VAL_0:%.*]] : $Int, [[Y_VAL_1:%.*]] : @owned $Array<String>, [[METATYPE:%.*]] : $@thick TestClass.Type):
    //
    // CHECK: ([[X_VAL:%.*]], [[Y_VAL:%.*]]) = destructure_tuple {{.*}} : $(Int, (Int, Array<String>))
    // CHECK: [[X_ACCESS:%.*]] = begin_access [modify] [static] [[X_REF]] : $*Int
    // CHECK-NEXT: store [[X_VAL]] to [trivial] [[X_ACCESS]] : $*Int
    // CHECK-NEXT: end_access [[X_ACCESS]] : $*Int
    //
    // CHECK: ([[X_VAL:%.*]], [[Y_VAL:%.*]]) = destructure_tuple {{.*}} : $(Int, (Int, Array<String>))
    // CHECK: ([[Y_VAL_0:%.*]], [[Y_VAL_1:%.*]]) = destructure_tuple {{.*}} : $(Int, Array<String>)
    // CHECK: [[Y_ACCESS:%.*]] = begin_access [modify] [static] [[Y_REF]] : $*(Int, Array<String>)
    // CHECK-NEXT: tuple_addr_constructor [init] [[Y_ACCESS]] : $*(Int, Array<String>) with ([[Y_VAL_0]] : $Int, [[Y_VAL_1]] :
    // CHECK-NEXT: end_access [[Y_ACCESS]] : $*(Int, Array<String>)
    @storageRestrictions(initializes: x, y)
    init(initialValue) {
      x = initialValue.0
      y = initialValue.1
    }

    get { (x, y) }
    set {
      x = newValue.0
      y = newValue.1
    }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors9TestClassC1x1yACSi_Si_SaySSGttcfc : $@convention(method) (Int, Int, @owned Array<String>, @owned TestClass) -> @owned TestClass
  // CHECK: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors9TestClassC4dataSi_Si_SaySSGttvi : $@convention(thin) (Int, Int, @owned Array<String>, @thick TestClass.Type) -> (@out Int, @out (Int, Array<String>))
  // CHECK: [[METATYPE:%.*]] = value_metatype $@thick TestClass.Type, {{%.*}}
  // CHECK: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]]([[METATYPE]]) : $@convention(thin) (Int, Int, @owned Array<String>, @thick TestClass.Type) -> (@out Int, @out (Int, Array<String>))
  // CHECK: [[SELF_REF:%.*]] = begin_borrow [[SELF_VALUE:%.*]] : $TestClass
  // CHECK: [[X_REF:%.*]] = ref_element_addr [[SELF_REF]] : $TestClass, #TestClass.x
  // CHECK-NEXT: [[Y_REF:%.*]] = ref_element_addr [[SELF_REF]] : $TestClass, #TestClass.y
  //
  // CHECK-NEXT: ([[X_VAL:%.*]], [[Y_VAL:%.*]]) = destructure_tuple {{.*}} : $(Int, (Int, Array<String>))
  // CHECK-NEXT: ([[Y_VAL_0:%.*]], [[Y_VAL_1:%.*]]) = destructure_tuple [[Y_VAL]] : $(Int, Array<String>)
  // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR]]([[X_REF]], [[Y_REF]], [[X_VAL]], [[Y_VAL_0]], [[Y_VAL_1]]) : $@noescape @callee_guaranteed (Int, Int, @owned Array<String>) -> (@out Int, @out (Int, Array<String>))
  init(x: Int, y: (Int, [String])) {
    self.data = (x, y)
  }
}

struct TestGeneric<T, U> {
  var a: T
  var b: T
  var c: U

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors11TestGenericV4datax_xtvi : $@convention(thin) <T, U> (@in T, @in T, @inout U, @thin TestGeneric<T, U>.Type) -> (@out T, @out T)
  //
  // CHECK: bb0([[A_REF:%.*]] : $*T, [[B_REF:%.*]] : $*T, [[A_VALUE:%.*]] : $*T, [[B_VALUE:%.*]] : $*T, [[C_REF:%.*]] : $*U, [[METATYPE:%.*]] : $@thin TestGeneric<T, U>.Type):
  //
  // CHECK: [[A_ACCESS:%.*]] = begin_access [modify] [static] [[A_REF]] : $*T
  // CHECK-NEXT: copy_addr [take] {{.*}} to [init] [[A_ACCESS]] : $*T
  // CHECK-NEXT: end_access [[A_ACCESS]] : $*T
  //
  // CHECK: [[B_ACCESS:%.*]] = begin_access [modify] [static] [[B_REF]] : $*T
  // CHECK-NEXT: copy_addr [take] {{.*}} to [init] [[B_ACCESS]] : $*T
  // CHECK-NEXT: end_access [[B_ACCESS]] : $*T
  //
  // CHECK: [[C_ACCESS:%.*]] = begin_access [read] [static] [[C_REF]] : $*U
  // CHECK-NEXT: [[C_AS_ANY:%.*]] = init_existential_addr {{.*}} : $*Any, $U
  // CHECK-NEXT: copy_addr [[C_ACCESS]] to [init] [[C_AS_ANY]] : $*U
  // CHECK-NEXT: end_access [[C_ACCESS]] : $*U
  var data: (T, T) {
    @storageRestrictions(initializes: a, b, accesses: c)
    init(initialValue) {
      a = initialValue.0
      b = initialValue.1
      print(c)
    }

    get { (a, b) }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors11TestGenericV1a1b1cACyxq_Gx_xq_tcfC : $@convention(method) <T, U> (@in T, @in T, @in U, @thin TestGeneric<T, U>.Type) -> @out TestGeneric<T, U>
  //
  // CHECK: [[INIT_ACCESSOR:%.*]] = function_ref @$s14init_accessors11TestGenericV4datax_xtvi : $@convention(thin) <τ_0_0, τ_0_1> (@in τ_0_0, @in τ_0_0, @inout τ_0_1, @thin TestGeneric<τ_0_0, τ_0_1>.Type) -> (@out τ_0_0, @out τ_0_0)
  // CHECK-NEXT: [[SUBST_INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR]]<T, U>(%4) : $@convention(thin) <τ_0_0, τ_0_1> (@in τ_0_0, @in τ_0_0, @inout τ_0_1, @thin TestGeneric<τ_0_0, τ_0_1>.Type) -> (@out τ_0_0, @out τ_0_0)
  // CHECK: {{.*}} = apply [[SUBST_INIT_ACCESSOR]]({{.*}}) : $@noescape @callee_guaranteed (@in T, @in T, @inout U) -> (@out T, @out T)
  //
  // CHECK: [[SETTER:%.*]] = function_ref @$s14init_accessors11TestGenericV4datax_xtvs : $@convention(method) <τ_0_0, τ_0_1> (@in τ_0_0, @in τ_0_0, @inout TestGeneric<τ_0_0, τ_0_1>) -> ()
  // CHECK-NEXT: [[SETTER_CLOSURE:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[SETTER]]<T, U>([[SELF_VALUE:%.*]]) : $@convention(method) <τ_0_0, τ_0_1> (@in τ_0_0, @in τ_0_0, @inout TestGeneric<τ_0_0, τ_0_1>) -> ()
  // CHECK: {{.*}} = apply [[SETTER_CLOSURE]]({{.*}}) : $@noescape @callee_guaranteed (@in T, @in T) -> ()
  // CHECK-NEXT: end_access [[SELF_VALUE]] : $*TestGeneric<T, U>
  init(a: T, b: T, c: U) {
    self.c = c
    self.data = (a, b)
    self.data = (b, a)
  }
}

struct TestGenericTuple<T, U> {
  var a: T
  var b: (T, U)

  // CHECK-LABEL: sil hidden [ossa] @$s14init_accessors16TestGenericTupleV4datax_x_q_ttvi : $@convention(thin) <T, U> (@in T, @in T, @in U, @thin TestGenericTuple<T, U>.Type) -> (@out T, @out (T, U)) {
  //
  // CHECK: bb0([[A_REF:%.*]] : $*T, [[B_REF:%.*]] : $*(T, U), [[A_VALUE:%.*]] : $*T, [[B_VALUE:%.*]] : $*T, [[C_VALUE:%.*]] : $*U, [[METATYPE:%.*]] : $@thin TestGenericTuple<T, U>.Type):
  //
  // CHECK: [[INIT_VALUE_1:%.*]] = alloc_stack $(T, U)
  // CHECK-NEXT: [[INIT_VALUE_1_0:%.*]] = tuple_element_addr [[INIT_VALUE_1]] : $*(T, U), 0
  // CHECK-NEXT: copy_addr [take] [[B_VALUE]] to [init] [[INIT_VALUE_1_0]]
  // CHECK-NEXT: [[INIT_VALUE_1_1:%.*]] = tuple_element_addr [[INIT_VALUE_1]] : $*(T, U), 1
  // CHECK-NEXT: copy_addr [take] [[C_VALUE]] to [init] [[INIT_VALUE_1_1]]

  // CHECK-NEXT: [[INIT_VALUE_2:%.*]] = alloc_stack [lexical] [var_decl] $(T, (T, U))
  // CHECK-NEXT: [[INIT_VALUE_2_0:%.*]] = tuple_element_addr [[INIT_VALUE_2]] : $*(T, (T, U)), 0
  // CHECK-NEXT: copy_addr [take] [[A_VALUE]] to [init] [[INIT_VALUE_2_0]]
  // CHECK-NEXT: [[INIT_VALUE_2_1:%.*]] = tuple_element_addr [[INIT_VALUE_2]] : $*(T, (T, U)), 1
  // CHECK-NEXT: copy_addr [take] [[INIT_VALUE_1]] to [init] [[INIT_VALUE_2_1]]

  var data: (T, (T, U)) {
    @storageRestrictions(initializes: a, b)
    init(initialValue) {
      a = initialValue.0
      b = initialValue.1
    }

    get { (a, b) }
    set { }
  }

  init(a: T, b: T, c: U) {
    self.data = (a, (b, c))
    self.data = (b, (a, c))
  }
}

func test_local_with_memberwise() {
  class MyValue {}

  struct TestMemberwiseConcrete {
    var a: Int
    var b: String

    var pair: (Int, String) {
      @storageRestrictions(initializes: a, b)
      init(initialValue) {
        a = initialValue.0
        b = initialValue.1
      }

      get { (a, b) }
      set { }
    }

    var c: [MyValue]

    // CHECK-LABEL: sil private [ossa] @$s14init_accessors26test_local_with_memberwiseyyF22TestMemberwiseConcreteL_V4pair1cADSi_SSt_SayAaByyF7MyValueL_CGtcfC : $@convention(method) (Int, @owned String, @owned Array<MyValue>, @thin TestMemberwiseConcrete.Type) -> @owned TestMemberwiseConcrete
    // CHECK:  [[SELF_VALUE:%.*]] = alloc_stack $TestMemberwiseConcrete
    // CHECK-NEXT: [[A_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestMemberwiseConcrete, #<abstract function>TestMemberwiseConcrete.a
    // CHECK-NEXT: [[B_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestMemberwiseConcrete, #<abstract function>TestMemberwiseConcrete.b
    // CHECK:  [[INIT_ACCESSOR_REF:%.*]] = function_ref @$s14init_accessors26test_local_with_memberwiseyyF22TestMemberwiseConcreteL_V4pairSi_SStvi : $@convention(thin) (Int, @owned String, @thin TestMemberwiseConcrete.Type) -> (@out Int, @out String)
    // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR_REF]]([[A_REF]], [[B_REF]], %0, %1, %3) : $@convention(thin) (Int, @owned String, @thin TestMemberwiseConcrete.Type) -> (@out Int, @out String)
    // CHECK-NEXT: [[C_REF:%.*]] = struct_element_addr %4 : $*TestMemberwiseConcrete, #<abstract function>TestMemberwiseConcrete.c
    // CHECK-NEXT: store %2 to [init] [[C_REF]] : $*Array<MyValue>
    // CHECK-NEXT: [[RESULT:%.*]] = load [take] [[SELF_VALUE]] : $*TestMemberwiseConcrete
    // CHECK-NEXT: dealloc_stack [[SELF_VALUE]] : $*TestMemberwiseConcrete
    // CHECK-NEXT: return [[RESULT]] : $TestMemberwiseConcrete
  }

  _ = TestMemberwiseConcrete(pair: (0, "a"), c: [])

  struct TestMemberwiseGeneric<T, C> where C: RangeReplaceableCollection, C.Element == T {
    var _a: T
    var _b: String
    var _c: C

    var a: T {
      @storageRestrictions(initializes: _a)
      init(initialValue) {
        _a = initialValue
      }

      get { _a }
      set { }
    }

    var pair: (String, C) {
      @storageRestrictions(initializes: _b, _c, accesses: _a)
      init(initialValue) {
        _b = initialValue.0
        _c = initialValue.1
        _c.append(_a)
      }

      get { (_b, _c) }
      set { }
    }

    // CHECK-LABEL: sil private [ossa] @$s14init_accessors26test_local_with_memberwiseyyF21TestMemberwiseGenericL_V1a4pairADyxq_Gx_SS_q_ttcfC : $@convention(method) <T, C where T == C.Element, C : RangeReplaceableCollection> (@in T, @owned String, @in C, @thin TestMemberwiseGeneric<T, C>.Type) -> @out TestMemberwiseGeneric<T, C>
    // CHECK: bb0([[SELF_VALUE:%.*]]  : $*TestMemberwiseGeneric<T, C>, [[A_VALUE:%*.]] : $*T, [[B_VALUE:%.*]] : @owned $String, [[C_VALUE:%.*]] : $*C, [[METATYPE:%.*]] : $@thin TestMemberwiseGeneric<T, C>.Type):
    // CHECK-NEXT: [[A_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestMemberwiseGeneric<T, C>, #<abstract function>TestMemberwiseGeneric._a
    // CHECK: [[INIT_ACCESSOR_REF:%.*]] = function_ref @$s14init_accessors26test_local_with_memberwiseyyF21TestMemberwiseGenericL_V1axvi : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_0 == τ_0_1.Element, τ_0_1 : RangeReplaceableCollection> (@in τ_0_0, @thin TestMemberwiseGeneric<τ_0_0, τ_0_1>.Type) -> @out τ_0_0 // user: %7
    // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR_REF]]<T, C>([[A_REF]], [[A_VALUE]], [[METATYPE]]) : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_0 == τ_0_1.Element, τ_0_1 : RangeReplaceableCollection> (@in τ_0_0, @thin TestMemberwiseGeneric<τ_0_0, τ_0_1>.Type) -> @out τ_0_0
    // CHECK-NEXT: [[B_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestMemberwiseGeneric<T, C>, #<abstract function>TestMemberwiseGeneric._b
    // CHECK-NEXT: [[C_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestMemberwiseGeneric<T, C>, #<abstract function>TestMemberwiseGeneric._c
    // CHECK-NEXT: [[A_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*TestMemberwiseGeneric<T, C>, #<abstract function>TestMemberwiseGeneric._a
    // CHECK: [[INIT_ACCESSOR_REF:%.*]] = function_ref @$s14init_accessors26test_local_with_memberwiseyyF21TestMemberwiseGenericL_V4pairSS_q_tvi : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_0 == τ_0_1.Element, τ_0_1 : RangeReplaceableCollection> (@owned String, @in τ_0_1, @inout τ_0_0, @thin TestMemberwiseGeneric<τ_0_0, τ_0_1>.Type) -> (@out String, @out τ_0_1)
    // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR_REF]]<T, C>([[B_REF]], [[C_REF]], [[B_VALUE]], [[C_VALUE]], [[A_REF]], [[METATYPE]]) : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_0 == τ_0_1.Element, τ_0_1 : RangeReplaceableCollection> (@owned String, @in τ_0_1, @inout τ_0_0, @thin TestMemberwiseGeneric<τ_0_0, τ_0_1>.Type) -> (@out String, @out τ_0_1)
    // CHECK-NEXT: [[VOID:%.*]] = tuple ()
    // CHECK-NEXT: return [[VOID]] : $()
  }

  _ = TestMemberwiseGeneric(a: 1, pair: ("a", [0]))
}

// CHECK-LABEL: sil private [ossa] @$s14init_accessors023test_type_lowering_for_A9_accessoryyF4TestL_V2fnADyxq_Gq_xc_tcfC : $@convention(method) <T, U> (@owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T, U>, @thin Test<T, U>.Type) -> @owned Test<T, U>
// CHECK: {{.*}} = function_ref @$s14init_accessors023test_type_lowering_for_A9_accessoryyF4TestL_V2fnyq_xcvi : $@convention(thin) <τ_0_0, τ_0_1> (@owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <τ_0_0, τ_0_1>, @thin Test<τ_0_0, τ_0_1>.Type) -> @out Optional<@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <τ_0_0, τ_0_1>>
func test_type_lowering_for_init_accessor() {
  struct Test<T, U> {
    var _fn: ((T) -> U)? = nil

    // CHECK-LABEL: sil private [ossa] @$s14init_accessors023test_type_lowering_for_A9_accessoryyF4TestL_V2fnyq_xcvi : $@convention(thin) <T, U> (@owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T, U>, @thin Test<T, U>.Type) -> @out Optional<@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <T, U>>
    var fn: (T) -> U {
      @storageRestrictions(initializes: _fn)
      init { _fn = newValue }
      get { _fn! }
      set { _fn = newValue }
    }

    init(fn: @escaping (T) -> U) {
      self.fn = fn
    }
  }

  _ = Test<Int, () -> Void> { _ in
    return {}
  } // Ok
}

func test_assignments() {
  struct Test {
    var _a: Int
    var _b: Int

    var a: Int {
      @storageRestrictions(initializes: _a)
      init(initialValue) {
        self._a = initialValue
      }
      get { _a }
      set { _a = newValue  }
    }

    var pair: (Int, Int) {
      @storageRestrictions(initializes: _a, _b)
      init(initialValue) {
        _a = initialValue.0
        _b = initialValue.1
      }

      get { (_a, _b) }
      set { }
    }

    // CHECK-LABEL: sil private [ossa] @$s14init_accessors16test_assignmentsyyF4TestL_V1aADSi_tcfC : $@convention(method) (Int, @thin Test.Type) -> Test
    // CHECK: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors16test_assignmentsyyF4TestL_V1aSivi : $@convention(thin) (Int, @thin Test.Type) -> @out Int
    // CHECK: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]](%1) : $@convention(thin) (Int, @thin Test.Type) -> @out Int
    // CHECK: [[A_REF:%.*]] = struct_element_addr {{.*}} : $*Test, #<abstract function>Test._a
    // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR]]([[A_REF]], %0) : $@noescape @callee_guaranteed (Int) -> @out Int
    // CHECK: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors16test_assignmentsyyF4TestL_V1aSivi : $@convention(thin) (Int, @thin Test.Type) -> @out Int
    // CHECK: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]](%1) : $@convention(thin) (Int, @thin Test.Type) -> @out Int
    // CHECK: [[A_REF:%.*]] = struct_element_addr {{.*}} : $*Test, #<abstract function>Test._a
    // CHECK-NEXT: destroy_addr [[A_REF]] : $*Int
    // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR]]([[A_REF]], %0) : $@noescape @callee_guaranteed (Int) -> @out Int
    // CHECK: [[B_REF:%.*]] = struct_element_addr {{.*}} : $*Test, #<abstract function>Test._b
    // CHECK-NEXT: store {{.*}} to [trivial] [[B_REF]] : $*Int
    // CHECK: [[SETTER_REF:%.*]] = function_ref @$s14init_accessors16test_assignmentsyyF4TestL_V1aSivs : $@convention(method) (Int, @inout Test) -> ()
    // CHECK-NEXT: [[SETTER_CLOSURE:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[SETTER_REF]]([[SELF_VALUE:%.*]]) : $@convention(method) (Int, @inout Test) -> ()
    // CHECK: {{.*}} = apply [[SETTER_CLOSURE]](%0) : $@noescape @callee_guaranteed (Int) -> ()
    init(a: Int) {
      self.a = a
      self.a = a
      self._b = 42
      self.a = a
    }

    // CHECK-LABEL: sil private [ossa] @$s14init_accessors16test_assignmentsyyF4TestL_V1a1bADSi_SitcfC : $@convention(method) (Int, Int, @thin Test.Type) -> Test
    // CHECK: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors16test_assignmentsyyF4TestL_V1aSivi : $@convention(thin) (Int, @thin Test.Type) -> @out Int
    // CHECK: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]](%2) : $@convention(thin) (Int, @thin Test.Type) -> @out Int
    // CHECK: [[A_REF:%.*]] = struct_element_addr {{.*}} : $*Test, #<abstract function>Test._a
    // CHECK-NEXT: {{.*}} = apply [[INIT_ACCESSOR]]([[A_REF]], %0) : $@noescape @callee_guaranteed (Int) -> @out Int
    // CHECK: [[INIT_ACCESSOR_FN:%.*]] = function_ref @$s14init_accessors16test_assignmentsyyF4TestL_V4pairSi_Sitvi : $@convention(thin) (Int, Int, @thin Test.Type) -> (@out Int, @out Int)
    // CHECK: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[INIT_ACCESSOR_FN]](%2) : $@convention(thin) (Int, Int, @thin Test.Type) -> (@out Int, @out Int)
    // CHECK: [[A_REF:%.*]] = struct_element_addr [[SELF_VALUE:%.*]] : $*Test, #<abstract function>Test._a
    // CHECK-NEXT: destroy_addr [[A_REF]] : $*Int
    // CHECK-NEXT: [[B_REF:%.*]] = struct_element_addr [[SELF_VALUE]] : $*Test, #<abstract function>Test._b
    // CHECK: {{.*}} = apply [[INIT_ACCESSOR]]([[A_REF]], [[B_REF]], {{.*}}) : $@noescape @callee_guaranteed (Int, Int) -> (@out Int, @out Int)
    init(a: Int, b: Int) {
      self.a = a
      self.pair = (0, b)
    }
  }
}

// rdar://112417250 (Crash with macro expansion on generic NSObject subclass)
// self is already borrowed within the initializer.
//
// CHECK-LABEL: sil private [ossa] @$s14init_accessors8testObjCyyF07GenericD9CSubclassL_CyADyxGxcfc : $@convention(method) <T> (@in T, @owned GenericObjCSubclass<T>) -> @owned GenericObjCSubclass<T> {
// CHECK: [[BORROW:%.*]] = load_borrow %{{.*}} : $*GenericObjCSubclass<T>
// CHECK: ref_element_addr [[BORROW]] : $GenericObjCSubclass<T>, #<abstract function>GenericObjCSubclass._value
// CHECK: apply
// CHECK: end_borrow [[BORROW]] : $GenericObjCSubclass<T>
// CHECK-NOT: end_borrow [[BORROW]] : $GenericObjCSubclass<T>
func testObjC() {
  class GenericObjCSubclass<T>: NSObject {
    var _value: T

    var value: T {
      @storageRestrictions(initializes: _value)
      init {
        self._value = newValue
      }
      get { _value }
      set { _value = newValue }
    }

    init(_ value: T) {
      self.value = value
    }
  }
}
