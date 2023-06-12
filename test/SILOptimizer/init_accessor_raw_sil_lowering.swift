// RUN: %target-swift-frontend -enable-experimental-feature InitAccessors -Xllvm -sil-print-after=definite-init -emit-sil -module-name assign_or_init_lowering %s -o /dev/null 2>&1 | %FileCheck %s

// REQUIRES: asserts

struct Test1 {
  var _a: Int
  var _b: String

  var a: Int {
    init(initialValue) initializes(_a) {
      _a = initialValue
    }

    get { _a }
    set { }
  }

  var b: String {
    init(initialValue) initializes(_a, _b) {
      _a = 0
      _b = initialValue
    }

    get { _b }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering5Test1V1aACSi_tcfC : $@convention(method) (Int, @thin Test1.Type) -> @owned Test1
  init(a: Int) {
    // CHECK: assign_or_init [init] [[VALUE:%.*]] : $Int, init {{.*}} : $@convention(thin) (Int) -> @out Int, set {{.*}} : $@callee_guaranteed (Int) -> ()
    self.a = a
    // CHECK: assign_or_init [init] [assign=0] [[VALUE:%.*]] : $String, init {{.*}} : $@convention(thin) (@owned String) -> (@out Int, @out String), set {{.*}} : $@callee_guaranteed (@owned String) -> ()
    self.a = -1
    self.b = ""
    // CHECK: assign_or_init [set] [[VALUE:%.*]] : $Int, init {{.*}} : $@convention(thin) (Int) -> @out Int, set {{.*}} : $@callee_guaranteed (Int) -> ()
    self.a = a
  }
}

struct Test2<T> {
  var _a: Int
  var _b: T
  var _c: String

  var pair: (Int, T) {
    init(initialValue) initializes(_a, _b) {
      _a = initialValue.0
      _b = initialValue.1
    }

    get { (_a, _b) }
    set { }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s23assign_or_init_lowering5Test2V1a1bACyxGSi_xtcfC : $@convention(method) <T> (Int, @in T, @thin Test2<T>.Type) -> @out Test2<T>
  init(a: Int, b: T) {
    // CHECK: assign_or_init [init] [[VALUE:%.*]] : $*(Int, T), init {{.*}} : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0), set {{.*}} : $@callee_guaranteed (Int, @in T) -> ()
    self.pair = (a, b)
    // CHECK: assign_or_init [init] [assign=0] [assign=1] [[VALUE:%.*]] : $*(Int, T), init {{.*}} : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0), set {{.*}} : $@callee_guaranteed (Int, @in T) -> ()
    self.pair = (0, b)
    self._c = ""
    // CHECK: assign_or_init [set] [[VALUE:%.*]] : $*(Int, T), init {{.*}} : $@convention(thin) <τ_0_0> (Int, @in τ_0_0) -> (@out Int, @out τ_0_0), set {{.*}} : $@callee_guaranteed (Int, @in T) -> ()
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
    // CHECK: assign_or_init [set] %0 : $Int, init {{.*}} : $@convention(thin) (Int) -> (), set {{.*}} : $@callee_guaranteed (Int) -> ()
    self.test = v
  }
}
