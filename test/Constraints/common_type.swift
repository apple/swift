// RUN: %target-typecheck-verify-swift -debug-constraints 2>%t.err
// RUN: %FileCheck %s < %t.err

// REQUIRES: needs_adjustment_for_new_favoring

struct X {
  func g(_: Int) -> Int { return 0 }
  func g(_: Double) -> Int { return 0 }

  subscript(_: Int) -> String { return "" }
  subscript(_: Double) -> String { return "" }

  func iuo(_: Int) -> Int! { return 0 }
  func iuo(_: Double) -> Int! { return 0 }
}

struct Y {
  func g(_: Int) -> Double { return 0 }
  func g(_: Double) -> Double { return 0 }

  subscript(_: Int) -> Substring { return "" }
  subscript(_: Double) -> Substring { return "" }

  func iuo(_: Int) -> Double! { return 0 }
  func iuo(_: Double) -> Double! { return 0 }
}

func f(_: Int) -> X { return X() }
func f(_: Double) -> Y { return Y() }

func testCallCommonType() {
  // CHECK: overload set choice binding $T{{[0-9]+}} := (Int) -> X
  // CHECK: (considering: $T{{[0-9]+}}[.g: value] == [[G:\$T[0-9]+]]
  // CHECK: (common result type for [[G]] is Int)
  // CHECK: (overload set choice binding $T{{[0-9]+}} := (Double) -> Y)
  // CHECK: (considering: $T{{[0-9]+}}[.g: value] == [[F:\$T[0-9]+]]
  // CHECK: (common result type for [[F]] is Double)
  _ = f(0).g(0)
}

func testSubscriptCommonType() {
  // CHECK: subscript_expr
  // CHECK: overload set choice binding $T{{[0-9]+}} := (Int) -> X
  // CHECK: (common result type for $T{{[0-9]+}} is String)
  // CHECK: (overload set choice binding $T{{[0-9]+}} := (Double) -> Y)
  // CHECK: (common result type for $T{{[0-9]+}} is Substring)
  _ = f(0)[0]
}

func testCommonTypeIUO() {
  // CHECK: overload set choice binding $T{{[0-9]+}} := (Int) -> X
  // CHECK-NOT: common result type
    _ = f(0).iuo(0)
}

struct Z {
  init(a: Int) { }
  init(a: Double) { }

  init(b: Int) { }
  init?(b: Double) { }
}

func testCommonTypeInit() {
  // CHECK: common result type for {{.*}} is Z
  _ = Z(a: 0)

  // CHECK-NOT: common result type
  _ = Z(b: 0)
}

class DynamicSelf {
  func foo(_ a: Int) -> Self { return self }
  func foo(_ a: Double) -> Self { return self }
}

class InheritsDynamicSelf: DynamicSelf {
}

func testCommonTypeDynamicSelf(ds: DynamicSelf, ids: InheritsDynamicSelf) {
  // CHECK: common result type for {{.*}} is DynamicSelf
  _ = ds.foo(0)
  // CHECK: common result type for {{.*}} is InheritsDynamicSelf
  _ = ids.foo(0)
}

