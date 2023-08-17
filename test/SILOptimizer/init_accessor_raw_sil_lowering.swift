// RUN: %target-swift-frontend -Xllvm -sil-print-after=definite-init -emit-sil -module-name assign_or_init_lowering %s -o /dev/null 2>&1 | %FileCheck %s

protocol Storage<T> {
  associatedtype T

  func getValue<V>(_: KeyPath<T, V>) -> V
  func setValue<V>(_: KeyPath<T, V>, _: V)
}

struct NoopStorage<T>: Storage {
  init() {}

  func getValue<V>(_: KeyPath<T, V>) -> V { fatalError() }
  func setValue<V>(_: KeyPath<T, V>, _: V) {}
}

final class TestIndirectionThroughStorage {
  var name: String = "item1" {
    @storageRestrictions(accesses: _storage)
    init {
      _storage.setValue(\.name, newValue)
    }
    get { _storage.getValue(\.name) }
    set { }
  }

  var age: Int? = nil {
    @storageRestrictions(accesses: _storage)
    init {
      _storage.setValue(\.age, newValue)
    }

    get { _storage.getValue(\.age) }
    set { }
  }

  private var _storage: any Storage<TestIndirectionThroughStorage> = NoopStorage()

  var storage: any Storage<TestIndirectionThroughStorage> {
    get { _storage }
    set { _storage = newValue }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering29TestIndirectionThroughStorageC4name3ageACSS_Sitcfc : $@convention(method) (@owned String, Int, @owned TestIndirectionThroughStorage) -> @owned TestIndirectionThroughStorage
  // CHECK: [[STORAGE_REF:%.*]] = ref_element_addr {{.*}} : $TestIndirectionThroughStorage, #TestIndirectionThroughStorage._storage
  // CHECK: [[STORAGE_INIT:%.*]] = function_ref @$s23assign_or_init_lowering29TestIndirectionThroughStorageC8_storage33_DE106275C2F16FB3D05881E72FBD87C8LLAA0H0_pAC1TAaFPRts_XPvpfi : $@convention(thin) () -> @out any Storage<TestIndirectionThroughStorage>
  // CHECK-NEXT: {{.*}} = apply [[STORAGE_INIT]]([[STORAGE_REF]]) : $@convention(thin) () -> @out any Storage<TestIndirectionThroughStorage>
  // Initialization:
  // CHECK: assign_or_init [set] #TestIndirectionThroughStorage.name, self %2 : $TestIndirectionThroughStorage, value {{.*}} : $String, init {{.*}} : $@convention(thin) (@owned String, @inout any Storage<TestIndirectionThroughStorage>) -> (), set {{.*}} : $@callee_guaranteed (@owned String) -> ()
  // CHECK: assign_or_init [set] #TestIndirectionThroughStorage.age, self %2 : $TestIndirectionThroughStorage, value {{.*}} : $Optional<Int>, init {{.*}} : $@convention(thin) (Optional<Int>, @inout any Storage<TestIndirectionThroughStorage>) -> (), set {{.*}} : $@callee_guaranteed (Optional<Int>) -> (
  // Explicit set:
  // CHECK: assign_or_init [set] #TestIndirectionThroughStorage.name, self %2 : $TestIndirectionThroughStorage, value {{.*}} : $String, init {{.*}} : $@convention(thin) (@owned String, @inout any Storage<TestIndirectionThroughStorage>) -> (), set {{.*}} : $@callee_guaranteed (@owned String) -> ()
  // CHECK: assign_or_init [set] #TestIndirectionThroughStorage.age, self %2 : $TestIndirectionThroughStorage, value {{.*}} : $Optional<Int>, init {{.*}} : $@convention(thin) (Optional<Int>, @inout any Storage<TestIndirectionThroughStorage>) -> (), set {{.*}} : $@callee_guaranteed (Optional<Int>) -> (
  init(name: String, age: Int) {
    self.name = name
    self.age = age
  }

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering29TestIndirectionThroughStorageC7storageAcA0H0_pAC1TAaEPRts_XP_tcfc : $@convention(method) (@in any Storage<TestIndirectionThroughStorage>, @owned TestIndirectionThroughStorage) -> @owned TestIndirectionThroughStorage
  // CHECK: [[STORAGE_REF:%.*]] = ref_element_addr {{.*}} : $TestIndirectionThroughStorage, #TestIndirectionThroughStorage._storage
  // CHECK: [[STORAGE_INIT:%.*]] = function_ref @$s23assign_or_init_lowering29TestIndirectionThroughStorageC8_storage33_DE106275C2F16FB3D05881E72FBD87C8LLAA0H0_pAC1TAaFPRts_XPvpfi : $@convention(thin) () -> @out any Storage<TestIndirectionThroughStorage>
  // CHECK-NEXT: {{.*}} = apply [[STORAGE_INIT]]([[STORAGE_REF]]) : $@convention(thin) () -> @out any Storage<TestIndirectionThroughStorage>
  // Initialization:
  // CHECK: assign_or_init [set] #TestIndirectionThroughStorage.name, self %1 : $TestIndirectionThroughStorage, value {{.*}} : $String, init {{.*}} : $@convention(thin) (@owned String, @inout any Storage<TestIndirectionThroughStorage>) -> (), set {{.*}} : $@callee_guaranteed (@owned String) -> ()
  // CHECK: assign_or_init [set] #TestIndirectionThroughStorage.age, self %1 : $TestIndirectionThroughStorage, value {{.*}} : $Optional<Int>, init {{.*}} : $@convention(thin) (Optional<Int>, @inout any Storage<TestIndirectionThroughStorage>) -> (), set {{.*}} : $@callee_guaranteed (Optional<Int>) -> ()
  // Explicit set:
  // CHECK: [[STORAGE_SETTER:%.*]] = function_ref @$s23assign_or_init_lowering29TestIndirectionThroughStorageC7storageAA0H0_pAC1TAaEPRts_XPvs : $@convention(method) (@in any Storage<TestIndirectionThroughStorage>, @guaranteed TestIndirectionThroughStorage) -> ()
  // CHECK-NEXT: {{.*}} = apply [[STORAGE_SETTER]]({{.*}}, %1) : $@convention(method) (@in any Storage<TestIndirectionThroughStorage>, @guaranteed TestIndirectionThroughStorage) -> ()
  init(storage: any Storage<TestIndirectionThroughStorage>) {
    self.storage = storage
  }
}

struct TestAccessOfOnePatternVars {
  var data: (Int, String) = (0, "a") {
    @storageRestrictions(accesses: x, y)
    init {
    }

    get { (x, y) }
    set {
      x = newValue.0
      y = newValue.1
    }
  }

  var other: Bool = false {
    @storageRestrictions(accesses: x)
    init {}
    get { x != 0 }
    set {}
  }

  var x: Int = 1, y: String = ""

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering26TestAccessOfOnePatternVarsV1x1yACSi_SStcfC : $@convention(method) (Int, @owned String, @thin TestAccessOfOnePatternVars.Type) -> @owned TestAccessOfOnePatternVars
  // CHECK: [[X_REF:%.*]] = struct_element_addr {{.*}} : $*TestAccessOfOnePatternVars, #TestAccessOfOnePatternVars.x
  // CHECK: [[X_INIT:%.*]] = function_ref @$s23assign_or_init_lowering26TestAccessOfOnePatternVarsV1xSivpfi : $@convention(thin) () -> Int
  // CHECK-NEXT: {{.*}} = apply [[X_INIT]]() : $@convention(thin) () -> Int
  // CHECK: [[Y_REF:%.*]] = struct_element_addr {{.*}} : $*TestAccessOfOnePatternVars, #TestAccessOfOnePatternVars.y
  // CHECK: [[Y_INIT:%.*]] = function_ref @$s23assign_or_init_lowering26TestAccessOfOnePatternVarsV1ySSvpfi : $@convention(thin) () -> @owned String
  // CHECK-NEXT: {{.*}} = apply [[Y_INIT]]() : $@convention(thin) () -> @owned String
  // CHECK-NOT: [[X_REF:%.*]] = struct_element_addr %3 : $*TestAccessOfOnePatternVars, #TestAccessOfOnePatternVars.x
  // CHECK-NOT: [[Y_REF:%.*]] = struct_element_addr {{.*}} : $*TestAccessOfOnePatternVars, #TestAccessOfOnePatternVars.y
  // CHECK: assign_or_init [set] #TestAccessOfOnePatternVars.data, self {{.*}} : $*TestAccessOfOnePatternVars, value {{.*}} : $(Int, String), init {{.*}} : $@convention(thin) (Int, @owned String, @inout Int, @inout String) -> (), set {{.*}} : $@callee_guaranteed (Int, @owned String) -> ()
  // CHECK: assign_or_init [set] #TestAccessOfOnePatternVars.other, self {{.*}} : $*TestAccessOfOnePatternVars, value {{.*}} : $Bool, init {{.*}} : $@convention(thin) (Bool, @inout Int) -> (), set {{.*}} : $@callee_guaranteed (Bool) -> ()
  init(x: Int, y: String) {
    self.x = x
    self.y = y
  }
}

struct Test1 {
  var _a: Int
  var _b: String

  var a: Int {
    @storageRestrictions(initializes: _a)
    init(initialValue) {
      _a = initialValue
    }

    get { _a }
    set { }
  }

  var b: String {
    @storageRestrictions(initializes: _a, _b)
    init(initialValue) {
      _a = 0
      _b = initialValue
    }

    get { _b }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering5Test1V1aACSi_tcfC : $@convention(method) (Int, @thin Test1.Type) -> @owned Test1
  init(a: Int) {
    // CHECK: [[INIT_REF:%.*]] = function_ref @$s23assign_or_init_lowering5Test1V1aSivi : $@convention(thin) (Int) -> @out Int
    // CHECK: assign_or_init [init] #Test1.a, self {{.*}}, value [[VALUE:%.*]] : $Int, init [[INIT_REF]] : $@convention(thin) (Int) -> @out Int, set {{.*}} : $@callee_guaranteed (Int) -> ()
    self.a = a
    // CHECK: [[INIT_REF:%.*]] = function_ref @$s23assign_or_init_lowering5Test1V1bSSvi : $@convention(thin) (@owned String) -> (@out Int, @out String)
    // CHECK: assign_or_init [init] [assign=0] #Test1.b, self {{.*}}, value [[VALUE:%.*]] : $String, init [[INIT_REF]] : $@convention(thin) (@owned String) -> (@out Int, @out String), set {{.*}} : $@callee_guaranteed (@owned String) -> ()
    self.a = -1
    self.b = ""
    // CHECK: [[INIT_REF:%.*]] = function_ref @$s23assign_or_init_lowering5Test1V1aSivi : $@convention(thin) (Int) -> @out Int
    // CHECK: assign_or_init [set] #Test1.a, self {{.*}}, value [[VALUE:%.*]] : $Int, init [[INIT_REF]] : $@convention(thin) (Int) -> @out Int, set {{.*}} : $@callee_guaranteed (Int) -> ()
    self.a = a
  }
}

struct Test2<T> {
  var _a: Int
  var _b: T
  var _c: String

  var pair: (Int, T) {
    @storageRestrictions(initializes: _a, _b)
    init(initialValue) {
      _a = initialValue.0
      _b = initialValue.1
    }

    get { (_a, _b) }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering5Test2V1a1bACyxGSi_xtcfC : $@convention(method) <T> (Int, @in T, @thin Test2<T>.Type) -> @out Test2<T>
  init(a: Int, b: T) {
    // CHECK: [[INIT_REF:%.*]] = function_ref @$s23assign_or_init_lowering5Test2V4pairSi_xtvi : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0)
    // CHECK-NEXT: [[SUBST_INIT_REF:%.*]] = partial_apply [callee_guaranteed] [[INIT_REF]]<T>() : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0)
    // CHECK: assign_or_init [init] #Test2.pair, self {{.*}}, value [[VALUE:%.*]] : $*(Int, T), init [[SUBST_INIT_REF]] : $@callee_guaranteed (Int, @in T) -> (@out Int, @out T), set {{.*}} : $@callee_guaranteed (Int, @in T) -> ()
    self.pair = (a, b)
    // CHECK: [[INIT_REF:%.*]] = function_ref @$s23assign_or_init_lowering5Test2V4pairSi_xtvi : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0)
    // CHECK-NEXT: [[SUBST_INIT_REF:%.*]] = partial_apply [callee_guaranteed] [[INIT_REF]]<T>() : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0)
    // CHECK: assign_or_init [init] [assign=0] [assign=1] #Test2.pair, self {{.*}}, value [[VALUE:%.*]] : $*(Int, T), init [[SUBST_INIT_REF]] : $@callee_guaranteed (Int, @in T) -> (@out Int, @out T), set {{.*}} : $@callee_guaranteed (Int, @in T) -> ()
    self.pair = (0, b)
    self._c = ""
    // CHECK: [[INIT_REF:%.*]] = function_ref @$s23assign_or_init_lowering5Test2V4pairSi_xtvi : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0)
    // CHECK-NEXT: [[SUBST_INIT_REF:%.*]] = partial_apply [callee_guaranteed] [[INIT_REF]]<T>() : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0)
    // CHECK: assign_or_init [set] #Test2.pair, self {{.*}}, value [[VALUE:%.*]] : $*(Int, T), init [[SUBST_INIT_REF]] : $@callee_guaranteed (Int, @in T) -> (@out Int, @out T), set {{.*}} : $@callee_guaranteed (Int, @in T) -> ()
    self.pair = (1, b)
  }
}

struct Test {
  var test: Int {
    init {
    }

    get { 42 }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering4TestV1vACSi_tcfC : $@convention(method) (Int, @thin Test.Type) -> Test
  init(v: Int) {
    // CHECK: [[INIT_REF:%.*]] = function_ref @$s23assign_or_init_lowering4TestV4testSivi : $@convention(thin) (Int) -> ()
    // CHECK: assign_or_init [set] #Test.test, self {{.*}}, value %0 : $Int, init [[INIT_REF]] : $@convention(thin) (Int) -> (), set {{.*}} : $@callee_guaranteed (Int) -> ()
    self.test = v
  }
}

protocol Initializable {
  static var initialValue: Self { get }
}

func test_default_inits() {
  struct Test1 {
    var _x: Int

    var x: Int = 0 {
      @storageRestrictions(initializes: _x)
      init {
        _x = newValue
      }
      get { _x }
      set { _x = newValue }
    }

    // CHECK-LABEL: sil private [ossa] @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_VADycfC : $@convention(method) (@thin Test1.Type) -> Test1
    // CHECK: [[DEFAULT:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_V1xSivpfi : $@convention(thin) () -> Int
    // CHECK-NEXT: [[INIT_VAL:%.*]] = apply [[DEFAULT]]() : $@convention(thin) () -> Int
    // CHECK-NEXT: [[SELF_REF:%.*]] = begin_access [modify] [static] %1 : $*Test1
    // CHECK: [[INIT_ACCESSOR:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_V1xSivi : $@convention(thin) (Int) -> @out Int
    // CHECK: [[SETTER_REF:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_V1xSivs : $@convention(method) (Int, @inout Test1) -> ()
    // CHECK-NEXT: [[SETTER:%.*]] = partial_apply [callee_guaranteed] [[SETTER_REF]]([[SELF_REF]]) : $@convention(method) (Int, @inout Test1) -> ()
    // CHECK-NEXT: assign_or_init [init] #<abstract function>Test1.x, self [[SELF_REF]] : $*Test1, value [[INIT_VAL]] : $Int, init [[INIT_ACCESSOR]] : $@convention(thin) (Int) -> @out Int, set [[SETTER]] : $@callee_guaranteed (Int) -> ()
    // CHECK-NEXT: end_access [[SELF_REF]] : $*Test1
    // CHECK-NEXT: destroy_value [[SETTER]] : $@callee_guaranteed (Int) -> ()
    init() {
    }

    // CHECK-LABEL: sil private [ossa] @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_V1xADSi_tcfC : $@convention(method) (Int, @thin Test1.Type) -> Test1
    // CHECK: [[DEFAULT:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_V1xSivpfi : $@convention(thin) () -> Int
    // CHECK-NEXT: [[INIT_VAL:%.*]] = apply [[DEFAULT]]() : $@convention(thin) () -> Int
    // CHECK-NEXT: [[SELF_REF:%.*]] = begin_access [modify] [static] %2 : $*Test1
    // CHECK: [[INIT_ACCESSOR:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_V1xSivi : $@convention(thin) (Int) -> @out Int
    // CHECK: [[SETTER_REF:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test1L_V1xSivs : $@convention(method) (Int, @inout Test1) -> ()
    // CHECK-NEXT: [[SETTER:%.*]] = partial_apply [callee_guaranteed] [[SETTER_REF]]([[SELF_REF]]) : $@convention(method) (Int, @inout Test1) -> ()
    // CHECK-NEXT: assign_or_init [init] #<abstract function>Test1.x, self [[SELF_REF]] : $*Test1, value [[INIT_VAL]] : $Int, init [[INIT_ACCESSOR]] : $@convention(thin) (Int) -> @out Int, set [[SETTER]] : $@callee_guaranteed (Int) -> ()
    // CHECK-NEXT: end_access [[SELF_REF]] : $*Test1
    // CHECK-NEXT: destroy_value [[SETTER]] : $@callee_guaranteed (Int) -> ()
    //
    // CHECK: assign_or_init [set] #<abstract function>Test1.x, self {{.*}} : $*Test1, value %0 : $Int, init {{.*}} : $@convention(thin) (Int) -> @out Int, set {{.*}} : $@callee_guaranteed (Int) -> ()
    init(x: Int) {
      self.x = x
    }
  }

  class Test2<T: Initializable> {
    var _x: (T, String)

    var x: (T, String) = (T.initialValue, "test") {
      @storageRestrictions(initializes: _x)
      init {
        _x = newValue
      }
      get { _x }
      set { _x = newValue }
    }

    var y: Int

    // CHECK-LABEL: sil private [ossa] @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_CADyxGycfc : $@convention(method) <T where T : Initializable> (@owned Test2<T>) -> @owned Test2<T>
    // CHECK: [[DEFAULT:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_C1xx_SStvpfi : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> () -> (@out τ_0_0, @owned String)
    // CHECK-NEXT: [[T:%.*]] = alloc_stack $T
    // CHECK-NEXT: [[STR:%.*]] = apply [[DEFAULT]]<T>([[T]]) : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> () -> (@out τ_0_0, @owned String)
    // CHECK-NEXT: [[NEW_VALUE:%.*]] = alloc_stack $(T, String)
    // CHECK-NEXT: [[NEW_VALUE_0:%.*]] = tuple_element_addr [[NEW_VALUE]] : $*(T, String), 0
    // CHECK-NEXT: [[NEW_VALUE_1:%.*]] = tuple_element_addr [[NEW_VALUE]] : $*(T, String), 1
    // CHECK-NEXT: copy_addr [take] [[T]] to [init] [[NEW_VALUE_0]] : $*T
    // CHECK-NEXT: store [[STR]] to [init] [[NEW_VALUE_1]] : $*String
    // CHECK: [[INIT_ACCESSOR_REF:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_C1xx_SStvi : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String) -> @out (τ_0_0, String)
    // CHECK-NEXT: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [[INIT_ACCESSOR_REF]]<T>() : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String) -> @out (τ_0_0, String)
    // CHECK: [[SETTER_REF:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_C1xx_SStvs : $@convention(method) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String, @guaranteed Test2<τ_0_0>) -> ()
    // CHECK-NEXT: [[SELF:%.*]] = copy_value %0 : $Test2<T>
    // CHECK-NEXT: [[SETTER:%.*]] = partial_apply [callee_guaranteed] [[SETTER_REF]]<T>([[SELF]]) : $@convention(method) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String, @guaranteed Test2<τ_0_0>) -> ()
    // CHECK-NEXT: assign_or_init [init] #<abstract function>Test2.x, self %0 : $Test2<T>, value [[NEW_VALUE]] : $*(T, String), init [[INIT_ACCESSOR]] : $@callee_guaranteed (@in T, @owned String) -> @out (T, String), set [[SETTER]] : $@callee_guaranteed (@in T, @owned String) -> ()
    // CHECK-NEXT: destroy_value [[SETTER]] : $@callee_guaranteed (@in T, @owned String) -> ()
    // CHECK-NEXT: destroy_value [[INIT_ACCESSOR]] : $@callee_guaranteed (@in T, @owned String) -> @out (T, String)
    // CHECK-NEXT: dealloc_stack [[NEW_VALUE]] : $*(T, String)
    // CHECK-NEXT: dealloc_stack [[T]] : $*T
    init() {
      y = 10
    }

    // CHECK-LABEL: sil private [ossa] @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_C1x1yADyxGx_SSt_Sitcfc : $@convention(method) <T where T : Initializable> (@in T, @owned String, Int, @owned Test2<T>) -> @owned Test2<T>
    // CHECK: [[DEFAULT:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_C1xx_SStvpfi : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> () -> (@out τ_0_0, @owned String)
    // CHECK-NEXT: [[T:%.*]] = alloc_stack $T
    // CHECK-NEXT: [[STR:%.*]] = apply [[DEFAULT]]<T>([[T]]) : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> () -> (@out τ_0_0, @owned String)
    // CHECK-NEXT: [[NEW_VALUE:%.*]] = alloc_stack $(T, String)
    // CHECK-NEXT: [[NEW_VALUE_0:%.*]] = tuple_element_addr [[NEW_VALUE]] : $*(T, String), 0
    // CHECK-NEXT: [[NEW_VALUE_1:%.*]] = tuple_element_addr [[NEW_VALUE]] : $*(T, String), 1
    // CHECK-NEXT: copy_addr [take] [[T]] to [init] [[NEW_VALUE_0]] : $*T
    // CHECK-NEXT: store [[STR]] to [init] [[NEW_VALUE_1]] : $*String
    // CHECK: [[INIT_ACCESSOR_REF:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_C1xx_SStvi : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String) -> @out (τ_0_0, String)
    // CHECK-NEXT: [[INIT_ACCESSOR:%.*]] = partial_apply [callee_guaranteed] [[INIT_ACCESSOR_REF]]<T>() : $@convention(thin) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String) -> @out (τ_0_0, String)
    // CHECK: [[SETTER_REF:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test2L_C1xx_SStvs : $@convention(method) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String, @guaranteed Test2<τ_0_0>) -> ()
    // CHECK-NEXT: [[SELF:%.*]] = copy_value %3 : $Test2<T>
    // CHECK-NEXT: [[SETTER:%.*]] = partial_apply [callee_guaranteed] [[SETTER_REF]]<T>([[SELF]]) : $@convention(method) <τ_0_0 where τ_0_0 : Initializable> (@in τ_0_0, @owned String, @guaranteed Test2<τ_0_0>) -> ()
    // CHECK-NEXT: assign_or_init [init] #<abstract function>Test2.x, self %3 : $Test2<T>, value [[NEW_VALUE]] : $*(T, String), init [[INIT_ACCESSOR]] : $@callee_guaranteed (@in T, @owned String) -> @out (T, String), set [[SETTER]] : $@callee_guaranteed (@in T, @owned String) -> ()
    // CHECK-NEXT: destroy_value [[SETTER]] : $@callee_guaranteed (@in T, @owned String) -> ()
    // CHECK-NEXT: destroy_value [[INIT_ACCESSOR]] : $@callee_guaranteed (@in T, @owned String) -> @out (T, String)
    // CHECK-NEXT: dealloc_stack [[NEW_VALUE]] : $*(T, String)
    // CHECK-NEXT: dealloc_stack [[T]] : $*T
    //
    // CHECK: assign_or_init [init] [assign=0] #<abstract function>Test2.x, self %3 : $Test2<T>, value {{.*}} : $*(T, String), init {{.*}} : $@callee_guaranteed (@in T, @owned String) -> @out (T, String), set {{.*}} : $@callee_guaranteed (@in T, @owned String) -> ()
    // CHECK: assign %2 to [init] [[Y_REF:%.*]] : $*Int
    // CHECK: assign_or_init [set] #<abstract function>Test2.x, self %3 : $Test2<T>, value {{.*}} : $*(T, String), init {{.*}} : $@callee_guaranteed (@in T, @owned String) -> @out (T, String), set {{.*}} : $@callee_guaranteed (@in T, @owned String) -> ()
    init(x: (T, String), y: Int) {
      self.x = x
      self.y = y
      self.x = x
    }
  }

  class Test3 {
    var _x: Int = 42
    var x: Int = 42 {
      @storageRestrictions(initializes: _x)
      init {
        _x = newValue
      }
      get { _x }
    }

    var _y: String = ""
    var y: String = "" {
      @storageRestrictions(initializes: _y)
      init {
        _y = newValue
      }
      get { _y }
    }

    // CHECK-LABEL: sil private [ossa] @$s23assign_or_init_lowering18test_default_initsyyF5Test3L_CADycfc : $@convention(method) (@owned Test3) -> @owned Test3
    // CHECK: function_ref variable initialization expression of x in Test3 #1 in test_default_inits()
    // CHECK-NOT: function_ref variable initialization expression of _x in Test3 #1 in test_default_inits()
    //
    // CHECK: function_ref variable initialization expression of y in Test3 #1 in test_default_inits()
    // CHECK-NOT: function_ref variable initialization expression of _y in Test3 #1 in test_default_inits()
  }

  class Test4 {
    var _x: Int

    var x: Int = 42 {
      @storageRestrictions(initializes: _x)
      init {
        _x = newValue
      }
      get { _x }
    }

    var _y: String

    var y: String = "" {
      @storageRestrictions(initializes: _y)
      init {
        _y = newValue
      }
      get { _y }
    }

    // CHECK-LABEL: sil private [ossa] @$s23assign_or_init_lowering18test_default_initsyyF5Test4L_CADycfc : $@convention(method) (@owned Test4) -> @owned Test4
    // CHECK: [[X_DEFAULT:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test4L_C1xSivpfi : $@convention(thin) () -> Int
    // CHECK-NEXT: [[X_VALUE:%.*]] = apply [[X_DEFAULT]]() : $@convention(thin) () -> Int
    // CHECK: assign_or_init [init] #<abstract function>Test4.x, self %0 : $Test4, value [[X_VALUE]] : $Int, init {{.*}} : $@convention(thin) (Int) -> @out Int, set undef : $@convention(thin) (Int) -> @out Int
    // CHECK: [[Y_DEFAULT:%.*]] = function_ref @$s23assign_or_init_lowering18test_default_initsyyF5Test4L_C1ySSvpfi : $@convention(thin) () -> @owned String
    // CHECK-NEXT: [[Y_VALUE:%.*]] = apply [[Y_DEFAULT]]() : $@convention(thin) () -> @owned String
    // CHECK: assign_or_init [init] #<abstract function>Test4.y, self %0 : $Test4, value [[Y_VALUE]] : $String, init {{.*}} : $@convention(thin) (@owned String) -> @out String, set undef : $@convention(thin) (@owned String) -> @out String
  }
}

func test_handling_of_superclass_properties() {
  class Entity {
    var _age: Int
    var age: Int = 0 {
      @storageRestrictions(initializes: _age)
      init { _age = newValue }
      get { _age }
      set { _age = newValue }
    }
  }

  class Person : Entity {
    // CHECK-LABEL: sil private [ossa] @$s23assign_or_init_lowering38test_handling_of_superclass_propertiesyyF6PersonL_C3ageADSi_tcfc : $@convention(method) (Int, @owned Person) -> @owned Person
    // CHECK: [[SELF:%.*]] = load [copy] %2 : $*Person
    // CHECK-NEXT: [[SELF_AS_ENTITY:%.*]] = upcast [[SELF]] : $Person to $Entity
    // CHECK-NEXT: [[AGE_SETTER:%.*]] = class_method [[SELF_AS_ENTITY]] : $Entity, #<abstract function>Entity.age!setter : (Entity) -> (Int) -> (), $@convention(method) (Int, @guaranteed Entity) -> ()
    // CHECK-NEXT: {{.*}} = apply [[AGE_SETTER]](%0, [[SELF_AS_ENTITY]]) : $@convention(method) (Int, @guaranteed Entity) -> ()
    init(age: Int) {
      super.init()
      self.age = age
    }
  }
}
