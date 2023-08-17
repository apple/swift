// RUN: %target-swift-emit-silgen %s -verify -swift-version 5 -disable-availability-checking | %FileCheck %s

// CHECK-LABEL: sil hidden [ossa] @$s33variadic_generic_overload_ranking05test_d15_concrete_over_A0yyF
func test_ranking_concrete_over_variadic() {
  func test() {}
  func test<T>(_: T) {}
  func test<each T>(_: repeat each T) {}

  // CHECK: // function_ref test #1 () in test_ranking_concrete_over_variadic()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_d15_concrete_over_A0yyF0E0L_yyF : $@convention(thin) () -> ()
  test()
  // CHECK: // function_ref test #2 <A>(_:) in test_ranking_concrete_over_variadic()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_d15_concrete_over_A0yyF0E0L0_yyxlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> ()
  test(1)
  // CHECK: // function_ref test #3 <each A>(_:) in test_ranking_concrete_over_variadic()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_d15_concrete_over_A0yyF0E0L1_yyxxQpRvzlF : $@convention(thin) <each τ_0_0> (@pack_guaranteed Pack{repeat each τ_0_0}) -> ()
  test(1, "")
}

// CHECK-LABEL: sil hidden [ossa] @$s33variadic_generic_overload_ranking05test_d1_A31_over_concrete_with_conversionsyyF
func test_ranking_variadic_over_concrete_with_conversions() {
  func test<T>(_: T, _: Any) {}
  func test<each T>(_: repeat each T) {}

  // CHECK: // function_ref test #2 <each A>(_:) in test_ranking_variadic_over_concrete_with_conversions()
  // CHECK-LABEL: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_d1_A31_over_concrete_with_conversionsyyF0E0L0_yyxxQpRvzlF : $@convention(thin) <each τ_0_0> (@pack_guaranteed Pack{repeat each τ_0_0}) -> ()
  test(1, "")

  func test_disfavored<T>(_: T, _: Any) {}
  @_disfavoredOverload
  func test_disfavored<each T>(_: repeat each T) {}

  // CHECK: // function_ref test_disfavored #1 <A>(_:_:) in test_ranking_variadic_over_concrete_with_conversions()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_d1_A31_over_concrete_with_conversionsyyF0E11_disfavoredL_yyx_yptlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0, @in_guaranteed Any) -> ()
  test_disfavored(2, "a")
}

// CHECK-LABEL: sil hidden [ossa] @$s33variadic_generic_overload_ranking05test_d1_a1_B13_over_regularyyF : $@convention(thin) () -> ()
func test_ranking_variadic_generic_over_regular() {
  func test1<T>(_: T...) {}
  func test1<each T>(_: repeat each T) {}

  // CHECK: // function_ref test1 #2 <each A>(_:) in test_ranking_variadic_generic_over_regular()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_d1_a1_B13_over_regularyyF5test1L0_yyxxQpRvzlF : $@convention(thin) <each τ_0_0> (@pack_guaranteed Pack{repeat each τ_0_0}) -> ()
  test1(1, "a")
}

protocol P {
}

// CHECK-LABEL: sil hidden [ossa] @$s33variadic_generic_overload_ranking05test_D25_with_multiple_expansionsyyF
func test_ranking_with_multiple_expansions() {
  struct Empty : P {}
  struct Tuple<T> : P {
    init(_: T) {}
  }

  struct Builder {
    static func build() -> Empty { Empty() }
    static func build<T: P>(_ a: T) -> T { a }
    static func build<T: P>(_ a: T, _ b: T) -> Tuple<(T, T)> { Tuple((a, b)) }
    static func build<each T: P>(_ v: repeat each T) -> Tuple<(repeat each T)> { Tuple((repeat each v)) }

    static func otherBuild<T: P, U: P>(a: T, b: U) {}
    static func otherBuild<each T: P, each U: P>(a: repeat each T, b: repeat each U) {}
  }

  // CHECK: // function_ref static otherBuild<A, B>(a:b:) in Builder #1 in test_ranking_with_multiple_expansions()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_D25_with_multiple_expansionsyyF7BuilderL_V10otherBuild1a1byx_q_tAA1PRzAaHR_r0_lFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : P, τ_0_1 : P> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1, @thin Builder.Type) -> ()
  Builder.otherBuild(a: Empty(), b: Empty())
  // CHECK: // function_ref static otherBuild<A, B>(a:b:) in Builder #1 in test_ranking_with_multiple_expansions()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_D25_with_multiple_expansionsyyF7BuilderL_V10otherBuild1a1byx_q_tAA1PRzAaHR_r0_lFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : P, τ_0_1 : P> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1, @thin Builder.Type) -> ()
  Builder.otherBuild(a: Empty(), b: Tuple<Int>(42))

  // CHECK: // function_ref static build() in Builder #1 in test_ranking_with_multiple_expansions()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_D25_with_multiple_expansionsyyF7BuilderL_V5buildAaByyF5EmptyL_VyFZ : $@convention(method) (@thin Builder.Type) -> Empty
  _ = Builder.build()
  // CHECK: // function_ref static build<A>(_:) in Builder #1 in test_ranking_with_multiple_expansions()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_D25_with_multiple_expansionsyyF7BuilderL_V5buildyxxAA1PRzlFZ : $@convention(method) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0, @thin Builder.Type) -> @out τ_0_0
  _ = Builder.build(Empty())
  // CHECK: // function_ref static build<A>(_:_:) in Builder #1 in test_ranking_with_multiple_expansions()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_D25_with_multiple_expansionsyyF7BuilderL_V5buildyAaByyF5TupleL_Vyx_xtGx_xtAA1PRzlFZ : $@convention(method) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_0, @thin Builder.Type) -> Tuple<(τ_0_0, τ_0_0)>
  _ = Builder.build(Empty(), Empty())
  // CHECK: // function_ref static build<each A>(_:) in Builder #1 in test_ranking_with_multiple_expansions()
  // CHECK-NEXT: {{.*}} = function_ref @$s33variadic_generic_overload_ranking05test_D25_with_multiple_expansionsyyF7BuilderL_V5buildyAaByyF5TupleL_VyxxQp_tGxxQpRvzAA1PRzlFZ : $@convention(method) <each τ_0_0 where repeat each τ_0_0 : P> (@pack_guaranteed Pack{repeat each τ_0_0}, @thin Builder.Type) -> Tuple<(repeat each τ_0_0)>
  _ = Builder.build(Empty(), Tuple<(Int, String)>((42, "")))
}
