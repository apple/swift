// RUN: %empty-directory(%t/src)
// RUN: split-file %s %t/src

// RUN: %target-swift-emit-silgen -disable-availability-checking -module-name preconcurrency_conformances -enable-upcoming-feature DynamicActorIsolation %t/src/checks.swift -verify | %FileCheck %t/src/checks.swift
// RUN: %target-swift-emit-silgen -swift-version 6 -disable-availability-checking -module-name preconcurrency_conformances -disable-dynamic-actor-isolation %t/src/checks_disabled.swift -verify | %FileCheck %t/src/checks_disabled.swift

// REQUIRES: asserts
// REQUIRES: concurrency

//--- checks.swift
protocol P {
  associatedtype T

  var prop: T { get set }
  func fn() -> T?
}

protocol Q {
  static var data: [Int]? { get set }
  static func staticFn() -> String?
}

actor MyActor {
}

@globalActor
struct GlobalActor {
  static var shared: MyActor = MyActor()
}

@MainActor
struct IsolatedType<T> : @preconcurrency P {
  var prop: T
  func fn() -> T? { nil }
}

// protocol witness for P.prop.getter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP4prop1TQzvgTW : $@convention(witness_method: P) <τ_0_0> (@in_guaranteed IsolatedType<τ_0_0>) -> @out τ_0_0
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.prop.setter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP4prop1TQzvsTW : $@convention(witness_method: P) <τ_0_0> (@in τ_0_0, @inout IsolatedType<τ_0_0>) -> ()
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.prop.modify in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP4prop1TQzvMTW : $@yield_once @convention(witness_method: P) <τ_0_0> @substituted <τ_0_0, τ_0_1> (@inout τ_0_0) -> @yields @inout τ_0_1 for <IsolatedType<τ_0_0>, τ_0_0>
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.fn() in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP2fn1TQzSgyFTW : $@convention(witness_method: P) <τ_0_0> (@in_guaranteed IsolatedType<τ_0_0>) -> @out Optional<τ_0_0>
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

extension IsolatedType : @preconcurrency Q {
  static var data: [Int]? {
    get { nil }
    set {}
  }

  static func staticFn() -> String? { nil }
}

// protocol witness for static Q.data.getter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP4dataSaySiGSgvgZTW : $@convention(witness_method: Q) <τ_0_0> (@thick IsolatedType<τ_0_0>.Type) -> @owned Optional<Array<Int>>
// CHECK: [[MAIN_ACTOR_METATYPE:%.*]] = metatype $@thick MainActor.Type
// CHECK: [[SHARED_FIELD_GETTER:%.*]] = function_ref @$sScM6sharedScMvgZ : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR:%.*]] = apply [[SHARED_FIELD_GETTER]]([[MAIN_ACTOR_METATYPE]]) : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR_ACCESS:%.*]] = begin_borrow [[MAIN_ACTOR]] : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR_ACCESS]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.data.setter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP4dataSaySiGSgvsZTW : $@convention(witness_method: Q) <τ_0_0> (@owned Optional<Array<Int>>, @thick IsolatedType<τ_0_0>.Type) -> ()
// CHECK: [[MAIN_ACTOR_METATYPE:%.*]] = metatype $@thick MainActor.Type
// CHECK: [[SHARED_FIELD_GETTER:%.*]] = function_ref @$sScM6sharedScMvgZ : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR:%.*]] = apply [[SHARED_FIELD_GETTER]]([[MAIN_ACTOR_METATYPE]]) : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR_ACCESS:%.*]] = begin_borrow [[MAIN_ACTOR]] : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR_ACCESS]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.data.modify in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP4dataSaySiGSgvMZTW : $@yield_once @convention(witness_method: Q) <τ_0_0> @substituted <τ_0_0> (@thick τ_0_0.Type) -> @yields @inout Optional<Array<Int>> for <IsolatedType<τ_0_0>>
// CHECK: [[MAIN_ACTOR_METATYPE:%.*]] = metatype $@thick MainActor.Type
// CHECK: [[SHARED_FIELD_GETTER:%.*]] = function_ref @$sScM6sharedScMvgZ : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR:%.*]] = apply [[SHARED_FIELD_GETTER]]([[MAIN_ACTOR_METATYPE]]) : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR_ACCESS:%.*]] = begin_borrow [[MAIN_ACTOR]] : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR_ACCESS]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.staticFn() in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP8staticFnSSSgyFZTW : $@convention(witness_method: Q) <τ_0_0> (@thick IsolatedType<τ_0_0>.Type) -> @owned Optional<String>
// CHECK: [[MAIN_ACTOR_METATYPE:%.*]] = metatype $@thick MainActor.Type
// CHECK: [[SHARED_FIELD_GETTER:%.*]] = function_ref @$sScM6sharedScMvgZ : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR:%.*]] = apply [[SHARED_FIELD_GETTER]]([[MAIN_ACTOR_METATYPE]]) : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK-NEXT: [[MAIN_ACTOR_ACCESS:%.*]] = begin_borrow [[MAIN_ACTOR]] : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR_ACCESS]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

class IsolatedMembers<T> : @preconcurrency P {
  @MainActor var prop: T {
    get { fatalError() }
    set {}
  }

  @GlobalActor func fn() -> T? { nil }
}

// protocol witness for P.prop.getter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP4prop1TQzvgTW : $@convention(witness_method: P) <τ_0_0> (@in_guaranteed IsolatedMembers<τ_0_0>) -> @out τ_0_0
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.prop.setter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP4prop1TQzvsTW : $@convention(witness_method: P) <τ_0_0> (@in τ_0_0, @inout IsolatedMembers<τ_0_0>) -> ()
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.prop.modify in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP4prop1TQzvMTW : $@yield_once @convention(witness_method: P) <τ_0_0> @substituted <τ_0_0, τ_0_1> (@inout τ_0_0) -> @yields @inout τ_0_1 for <IsolatedMembers<τ_0_0>, τ_0_0>
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.fn() in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP2fn1TQzSgyFTW : $@convention(witness_method: P) <τ_0_0> (@in_guaranteed IsolatedMembers<τ_0_0>) -> @out Optional<τ_0_0>
// CHECK: [[SHARED_REF:%.*]] = function_ref @$s27preconcurrency_conformances11GlobalActorV6sharedAA02MyD0Cvau : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[SHARED_PTR:%.*]] = apply [[SHARED_REF]]() : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[MY_ACTOR_ADDR:%.*]] = pointer_to_address [[SHARED_PTR]] : $Builtin.RawPointer to [strict] $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_access [read] [dynamic] [[MY_ACTOR_ADDR]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR:%.*]] = load [copy] [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: end_access [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_borrow [[MY_ACTOR]] : $MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MY_ACTOR_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

extension IsolatedMembers : @preconcurrency Q {
  @GlobalActor static var data: [Int]? {
    get { nil }
    set {}
  }

  @MainActor static func staticFn() -> String? { nil }
}

// protocol witness for static Q.data.getter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP4dataSaySiGSgvgZTW : $@convention(witness_method: Q) <τ_0_0> (@thick IsolatedMembers<τ_0_0>.Type) -> @owned Optional<Array<Int>>
// CHECK: [[SHARED_REF:%.*]] = function_ref @$s27preconcurrency_conformances11GlobalActorV6sharedAA02MyD0Cvau : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[SHARED_PTR:%.*]] = apply [[SHARED_REF]]() : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[MY_ACTOR_ADDR:%.*]] = pointer_to_address [[SHARED_PTR]] : $Builtin.RawPointer to [strict] $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_access [read] [dynamic] [[MY_ACTOR_ADDR]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR:%.*]] = load [copy] [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: end_access [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_borrow [[MY_ACTOR]] : $MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MY_ACTOR_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.data.setter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP4dataSaySiGSgvsZTW : $@convention(witness_method: Q) <τ_0_0> (@owned Optional<Array<Int>>, @thick IsolatedMembers<τ_0_0>.Type) -> ()
// CHECK: [[SHARED_REF:%.*]] = function_ref @$s27preconcurrency_conformances11GlobalActorV6sharedAA02MyD0Cvau : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[SHARED_PTR:%.*]] = apply [[SHARED_REF]]() : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[MY_ACTOR_ADDR:%.*]] = pointer_to_address [[SHARED_PTR]] : $Builtin.RawPointer to [strict] $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_access [read] [dynamic] [[MY_ACTOR_ADDR]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR:%.*]] = load [copy] [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: end_access [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_borrow [[MY_ACTOR]] : $MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MY_ACTOR_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.data.modify in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP4dataSaySiGSgvMZTW : $@yield_once @convention(witness_method: Q) <τ_0_0> @substituted <τ_0_0> (@thick τ_0_0.Type) -> @yields @inout Optional<Array<Int>> for <IsolatedMembers<τ_0_0>>
// CHECK: [[SHARED_REF:%.*]] = function_ref @$s27preconcurrency_conformances11GlobalActorV6sharedAA02MyD0Cvau : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[SHARED_PTR:%.*]] = apply [[SHARED_REF]]() : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT: [[MY_ACTOR_ADDR:%.*]] = pointer_to_address [[SHARED_PTR]] : $Builtin.RawPointer to [strict] $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_access [read] [dynamic] [[MY_ACTOR_ADDR]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR:%.*]] = load [copy] [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: end_access [[MY_ACTOR_REF]] : $*MyActor
// CHECK-NEXT: [[MY_ACTOR_REF:%.*]] = begin_borrow [[MY_ACTOR]] : $MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MY_ACTOR_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.staticFn() in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP8staticFnSSSgyFZTW : $@convention(witness_method: Q) <τ_0_0> (@thick IsolatedMembers<τ_0_0>.Type) -> @owned Optional<String>
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

extension MyActor : @preconcurrency P {
  var prop: Int {
    get { 42 }
    set {}
  }

  func fn() -> Int? { nil }
}

// protocol witness for P.prop.getter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP4prop1TQzvgTW : $@convention(witness_method: P) (@in_guaranteed MyActor) -> @out Int
// CHECK: [[SELF_REF:%.*]] = load_borrow %1 : $*MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[SELF_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.prop.setter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP4prop1TQzvsTW : $@convention(witness_method: P) (@in Int, @inout MyActor) -> ()
// CHECK: [[SELF_REF:%.*]] = load_borrow %1 : $*MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[SELF_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.prop.modify in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP4prop1TQzvMTW : $@yield_once @convention(witness_method: P) @substituted <τ_0_0, τ_0_1> (@inout τ_0_0) -> @yields @inout τ_0_1 for <MyActor, Int>
// CHECK: [[SELF_REF:%.*]] = load_borrow %0 : $*MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[SELF_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for P.fn() in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP2fn1TQzSgyFTW : $@convention(witness_method: P) (@in_guaranteed MyActor) -> @out Optional<Int>
// CHECK: [[SELF_REF:%.*]] = load_borrow %1 : $*MyActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[SELF_REF]] : $MyActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

extension MyActor : @preconcurrency Q {
  @MainActor static var data: [Int]? {
    get { nil }
    set {}
  }

  @MainActor static func staticFn() -> String? { nil }
}

// protocol witness for static Q.data.getter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1QA2aDP4dataSaySiGSgvgZTW : $@convention(witness_method: Q) (@thick MyActor.Type) -> @owned Optional<Array<Int>>
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.data.setter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1QA2aDP4dataSaySiGSgvsZTW : $@convention(witness_method: Q) (@owned Optional<Array<Int>>, @thick MyActor.Type) -> ()
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// protocol witness for static Q.data.modify in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1QA2aDP4dataSaySiGSgvMZTW : $@yield_once @convention(witness_method: Q) @substituted <τ_0_0> (@thick τ_0_0.Type) -> @yields @inout Optional<Array<Int>> for <MyActor>
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

// https://github.com/apple/swift/issues/74294
protocol Parent {
  func a()
}

protocol Child : Parent {
  func b()
}

@MainActor
struct PreconcurrencyAppliesToParentToo : @preconcurrency Child {
  func a() {
  }

  func b() {
  }
}

// protocol witness for Child.b() in conformance PreconcurrencyAppliesToParentToo
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances32PreconcurrencyAppliesToParentTooVAA5ChildA2aDP1byyFTW : $@convention(witness_method: Child) (@in_guaranteed PreconcurrencyAppliesToParentToo) -> ()
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])


// protocol witness for Parent.a() in conformance PreconcurrencyAppliesToParentToo
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances32PreconcurrencyAppliesToParentTooVAA0F0A2aDP1ayyFTW : $@convention(witness_method: Parent) (@in_guaranteed PreconcurrencyAppliesToParentToo) -> ()
// CHECK: [[MAIN_ACTOR:%.*]] = begin_borrow {{.*}} : $MainActor
// CHECK-NEXT: [[EXEC:%.*]] = extract_executor [[MAIN_ACTOR]] : $MainActor
// CHECK: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
// CHECK-NEXT: {{.*}} = apply [[CHECK_EXEC_REF]]({{.*}}, [[EXEC]])

//--- checks_disabled.swift
protocol P {
  associatedtype T

  var prop: T { get set }
  func fn() -> T?
}

protocol Q {
  static var data: [Int]? { get set }
  static func staticFn() -> String?
}

actor MyActor {
}

@globalActor
struct GlobalActor {
  static let shared: MyActor = MyActor()
}

@MainActor
struct IsolatedType<T: Sendable> : @preconcurrency P {
  var prop: T
  func fn() -> T? { nil }
}

// protocol witness for P.prop.getter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP4prop1TQzvgTW : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> (@in_guaranteed IsolatedType<τ_0_0>) -> @out τ_0_0
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.prop.setter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP4prop1TQzvsTW : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> (@in τ_0_0, @inout IsolatedType<τ_0_0>) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.prop.modify in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP4prop1TQzvMTW : $@yield_once @convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> @substituted <τ_0_0, τ_0_1> (@inout τ_0_0) -> @yields @inout τ_0_1 for <IsolatedType<τ_0_0>, τ_0_0>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.fn() in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1PA2aEP2fn1TQzSgyFTW : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> (@in_guaranteed IsolatedType<τ_0_0>) -> @out Optional<τ_0_0>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

extension IsolatedType : @preconcurrency Q {
  static var data: [Int]? {
    get { nil }
    set {}
  }

  static func staticFn() -> String? { nil }
}

// protocol witness for static Q.data.getter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP4dataSaySiGSgvgZTW : $@convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> (@thick IsolatedType<τ_0_0>.Type) -> @owned Optional<Array<Int>>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.data.setter in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP4dataSaySiGSgvsZTW : $@convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> (@owned Optional<Array<Int>>, @thick IsolatedType<τ_0_0>.Type) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.data.modify in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP4dataSaySiGSgvMZTW : $@yield_once @convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> @substituted <τ_0_0> (@thick τ_0_0.Type) -> @yields @inout Optional<Array<Int>> for <IsolatedType<τ_0_0>>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.staticFn() in conformance IsolatedType<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances12IsolatedTypeVyxGAA1QA2aEP8staticFnSSSgyFZTW : $@convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> (@thick IsolatedType<τ_0_0>.Type) -> @owned Optional<String>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

class IsolatedMembers<T: Sendable> : @preconcurrency P {
  @MainActor var prop: T {
    get { fatalError() }
    set {}
  }

  @GlobalActor func fn() -> T? { nil }
}

// protocol witness for P.prop.getter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP4prop1TQzvgTW : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> (@in_guaranteed IsolatedMembers<τ_0_0>) -> @out τ_0_0
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.prop.setter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP4prop1TQzvsTW : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> (@in τ_0_0, @inout IsolatedMembers<τ_0_0>) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.prop.modify in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP4prop1TQzvMTW : $@yield_once @convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> @substituted <τ_0_0, τ_0_1> (@inout τ_0_0) -> @yields @inout τ_0_1 for <IsolatedMembers<τ_0_0>, τ_0_0>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.fn() in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1PA2aEP2fn1TQzSgyFTW : $@convention(witness_method: P) <τ_0_0 where τ_0_0 : Sendable> (@in_guaranteed IsolatedMembers<τ_0_0>) -> @out Optional<τ_0_0>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

extension IsolatedMembers : @preconcurrency Q {
  @GlobalActor static var data: [Int]? {
    get { nil }
    set {}
  }

  @MainActor static func staticFn() -> String? { nil }
}

// protocol witness for static Q.data.getter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP4dataSaySiGSgvgZTW : $@convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> (@thick IsolatedMembers<τ_0_0>.Type) -> @owned Optional<Array<Int>>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.data.setter in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP4dataSaySiGSgvsZTW : $@convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> (@owned Optional<Array<Int>>, @thick IsolatedMembers<τ_0_0>.Type) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.data.modify in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP4dataSaySiGSgvMZTW : $@yield_once @convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> @substituted <τ_0_0> (@thick τ_0_0.Type) -> @yields @inout Optional<Array<Int>> for <IsolatedMembers<τ_0_0>>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.staticFn() in conformance IsolatedMembers<A>
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances15IsolatedMembersCyxGAA1QA2aEP8staticFnSSSgyFZTW : $@convention(witness_method: Q) <τ_0_0 where τ_0_0 : Sendable> (@thick IsolatedMembers<τ_0_0>.Type) -> @owned Optional<String>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

extension MyActor : @preconcurrency P {
  var prop: Int {
    get { 42 }
    set {}
  }

  func fn() -> Int? { nil }
}

// protocol witness for P.prop.getter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP4prop1TQzvgTW : $@convention(witness_method: P) (@in_guaranteed MyActor) -> @out Int
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.prop.setter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP4prop1TQzvsTW : $@convention(witness_method: P) (@in Int, @inout MyActor) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.prop.modify in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP4prop1TQzvMTW : $@yield_once @convention(witness_method: P) @substituted <τ_0_0, τ_0_1> (@inout τ_0_0) -> @yields @inout τ_0_1 for <MyActor, Int>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for P.fn() in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1PA2aDP2fn1TQzSgyFTW : $@convention(witness_method: P) (@in_guaranteed MyActor) -> @out Optional<Int>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

extension MyActor : @preconcurrency Q {
  @MainActor static var data: [Int]? {
    get { nil }
    set {}
  }

  @MainActor static func staticFn() -> String? { nil }
}

// protocol witness for static Q.data.getter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1QA2aDP4dataSaySiGSgvgZTW : $@convention(witness_method: Q) (@thick MyActor.Type) -> @owned Optional<Array<Int>>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.data.setter in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1QA2aDP4dataSaySiGSgvsZTW : $@convention(witness_method: Q) (@owned Optional<Array<Int>>, @thick MyActor.Type) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for static Q.data.modify in conformance MyActor
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances7MyActorCAA1QA2aDP4dataSaySiGSgvMZTW : $@yield_once @convention(witness_method: Q) @substituted <τ_0_0> (@thick τ_0_0.Type) -> @yields @inout Optional<Array<Int>> for <MyActor>
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// https://github.com/apple/swift/issues/74294
protocol Parent {
  func a()
}

protocol Child : Parent {
  func b()
}

@MainActor
struct PreconcurrencyAppliesToParentToo : @preconcurrency Child {
  func a() {
  }

  func b() {
  }
}

// protocol witness for Child.b() in conformance PreconcurrencyAppliesToParentToo
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances32PreconcurrencyAppliesToParentTooVAA5ChildA2aDP1byyFTW : $@convention(witness_method: Child) (@in_guaranteed PreconcurrencyAppliesToParentToo) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF

// protocol witness for Parent.a() in conformance PreconcurrencyAppliesToParentToo
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s27preconcurrency_conformances32PreconcurrencyAppliesToParentTooVAA0F0A2aDP1ayyFTW : $@convention(witness_method: Parent) (@in_guaranteed PreconcurrencyAppliesToParentToo) -> ()
// CHECK-NOT: [[CHECK_EXEC_REF:%.*]] = function_ref @$ss22_checkExpectedExecutor14_filenameStart01_D6Length01_D7IsASCII5_line9_executoryBp_BwBi1_BwBetF
