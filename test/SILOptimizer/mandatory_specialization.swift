// RUN: %target-swift-frontend -sil-verify-all -primary-file %s -emit-sil | %FileCheck %s
// RUN: not %target-swift-frontend -sil-verify-all -primary-file %s -emit-sil -D ERRORS 2>&1 | %FileCheck %s --check-prefix=CHECK-ERROR

// We shouldn't have any "must_specialize" functions left over.
// CHECK-NOT: [_semantics "must_specialize"]

// CHECK-LABEL: s24mandatory_specialization11twoGenerics1x1yxx_q_tr0_lFSi_SiTg5
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization11twoGenerics1x1yxx_q_tr0_lFSi_SiTg5'

// CHECK-NOT: sil hidden [noinline] [_semantics "must_specialize"] @$s24mandatory_specialization11twoGenerics1x1yxx_q_tr0_lF
@_semantics("must_specialize")
@inline(never)
func twoGenerics<T, U>(x: T, y: U) -> T { x }

// CHECK-LABEL: sil shared [noinline] @$s24mandatory_specialization20callsWithHalfGeneric1xxx_tlFSi_Tg5
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization11twoGenerics1x1yxx_q_tr0_lFSi_SiTg5 : $@convention(thin) (Int, Int) -> Int
// CHECK: apply [[FN]]({{.*}}, {{.*}}) : $@convention(thin) (Int, Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization20callsWithHalfGeneric1xxx_tlFSi_Tg5'

// CHECK-NOT: sil hidden [noinline] [_semantics "must_specialize"] @$s24mandatory_specialization20callsWithHalfGeneric1xxx_tlF
@inline(never)
func callsWithHalfGeneric<T>(x: T) -> T { twoGenerics(x: x, y: 0) }

// CHECK-LABEL: sil shared [noinline] @$s24mandatory_specialization16callsWithGeneric1xxx_tlFSi_Tg5
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization20callsWithHalfGeneric1xxx_tlFSi_Tg5 : $@convention(thin) (Int) -> Int
// CHECK: apply [[FN]]({{.*}}) : $@convention(thin) (Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization16callsWithGeneric1xxx_tlFSi_Tg5'

// CHECK-NOT: sil hidden [noinline] [_semantics "must_specialize"] @$s24mandatory_specialization16callsWithGeneric1xxx_tlF
@inline(never)
func callsWithGeneric<T>(x: T) -> T { callsWithHalfGeneric(x: x) }

// CHECK-LABEL: sil shared [noinline] @$s24mandatory_specialization31twoUnrelatedGenericsSpecialized1x1ySix_q_tr0_lFAA6StructV_AFTg5
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization11twoGenerics1x1yxx_q_tr0_lFSi_SiTg5 : $@convention(thin) (Int, Int) -> Int
// CHECK: apply [[FN]]({{.*}}, {{.*}}) : $@convention(thin) (Int, Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization31twoUnrelatedGenericsSpecialized1x1ySix_q_tr0_lFAA6StructV_AFTg5'

// CHECK-NOT: sil hidden [noinline] [_semantics "must_specialize"] @$s24mandatory_specialization20twoUnrelatedGenerics1x1ySix_q_tr0_lF
@_semantics("must_specialize")
@inline(never)
func twoUnrelatedGenericsSpecialized<T, U>(x: T, y: U) -> Int { twoGenerics(x: 0, y: 0) }

// This one should NOT be specialized.
// CHECK-LABEL: sil hidden [noinline] @$s24mandatory_specialization20twoUnrelatedGenerics1x1ySix_q_tr0_lF
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization11twoGenerics1x1yxx_q_tr0_lFSi_SiTg5 : $@convention(thin) (Int, Int) -> Int
// CHECK: apply [[FN]]({{.*}}, {{.*}}) : $@convention(thin) (Int, Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization20twoUnrelatedGenerics1x1ySix_q_tr0_lF'
@inline(never)
func twoUnrelatedGenerics<T, U>(x: T, y: U) -> Int { twoGenerics(x: 0, y: 0) }

// CHECK-NOT: sil hidden [transparent] [_semantics "must_specialize"] @$s24mandatory_specialization18transparentGeneric1xxx_tlF
@_semantics("must_specialize")
@_transparent
func transparentGeneric<T>(x: T) -> T { x }

// CHECK-LABEL: sil shared @$s24mandatory_specialization24transparentGenericCaller1xxx_tlFSi_Tg5
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization24transparentGenericCaller1xxx_tlFSi_Tg5'

// CHECK-NOT: sil hidden @$s24mandatory_specialization24transparentGenericCaller1xxx_tlF
func transparentGenericCaller<T>(x: T) -> T { transparentGeneric(x: x) }

// CHECK-LABEL: sil shared @$s24mandatory_specialization23mustSpecializeCallsSelf1xxx_tlFSi_Tg5
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization23mustSpecializeCallsSelf1xxx_tlFSi_Tg5 : $@convention(thin) (Int) -> Int
// CHECK: apply [[FN]]({{.*}}) : $@convention(thin) (Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization23mustSpecializeCallsSelf1xxx_tlFSi_Tg5'
@_semantics("must_specialize")
func mustSpecializeCallsSelf<T>(x: T) -> T { mustSpecializeCallsSelf(x: x) }

// CHECK-LABEL: sil shared @$s24mandatory_specialization25mustSpecializeCallsCaller1xyx_tlFSi_Tg5
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization15recursiveCalleryyF : $@convention(thin) () -> ()
// CHECK: apply [[FN]]() : $@convention(thin) () -> ()
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization25mustSpecializeCallsCaller1xyx_tlFSi_Tg5'
@_semantics("must_specialize")
func mustSpecializeCallsCaller<T>(x: T) { recursiveCaller() }

// CHECK-LABEL: sil @$s24mandatory_specialization15recursiveCalleryyF
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization25mustSpecializeCallsCaller1xyx_tlFSi_Tg5 : $@convention(thin) (Int) -> ()
// CHECK: apply [[FN]]({{.*}}) : $@convention(thin) (Int) -> ()
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization15recursiveCalleryyF'
public func recursiveCaller() { mustSpecializeCallsCaller(x: 0) }

struct Struct { }

// CHECK-LABEL: @$s24mandatory_specialization5test1yyF
// CHECK-NOT: apply
// CHECK: [[FN1:%.*]] = function_ref @$s24mandatory_specialization16callsWithGeneric1xxx_tlFSi_Tg5 : $@convention(thin) (Int) -> Int
// CHECK: apply [[FN1]]({{.*}}) : $@convention(thin) (Int) -> Int
// CHECK-NOT: apply
// CHECK: [[FN2:%.*]] = function_ref @$s24mandatory_specialization11twoGenerics1x1yxx_q_tr0_lFSi_SiTg5 : $@convention(thin) (Int, Int) -> Int
// CHECK: apply [[FN2]]({{.*}}, {{.*}}) : $@convention(thin) (Int, Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization5test1yyF'
@inline(never)
public func test1() {
  _ = callsWithGeneric(x: 1)
  _ = twoGenerics(x: 41, y: 0)
}

// CHECK-LABEL: sil [noinline] @$s24mandatory_specialization5test2yyF
// There are two calls to constructors for "Struct". Other than that we shouldn't have any other applies.
// Note: when mand combine is updated to remove dead instructions, you can remove the following two applies.
// CHECK: apply
// CHECK: apply
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization31twoUnrelatedGenericsSpecialized1x1ySix_q_tr0_lFAA6StructV_AFTg5 : $@convention(thin) (Struct, Struct) -> Int
// CHECK: apply [[FN]]({{.*}}, {{.*}}) : $@convention(thin) (Struct, Struct) -> Int
// Two more constructors...
// CHECK: apply
// CHECK: apply
// CHECK-NOT: apply
// CHECK: [[FN2:%.*]] = function_ref @$s24mandatory_specialization20twoUnrelatedGenerics1x1ySix_q_tr0_lF : $@convention(thin) <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Int
// CHECK: apply [[FN2:%.*]]<Struct, Struct>({{.*}}, {{.*}}) : $@convention(thin) <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization5test2yyF'
@inline(never)
public func test2() {
  _ = twoUnrelatedGenericsSpecialized(x: Struct(), y: Struct())
  _ = twoUnrelatedGenerics(x: Struct(), y: Struct())
}

// CHECK-LABEL: sil [noinline] @$s24mandatory_specialization5test3yyF
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization5test2yyF : $@convention(thin) () -> ()
// CHECK: apply [[FN]]() : $@convention(thin) () -> ()
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization5test3yyF'
@inline(never)
public func test3() {
  test2()
}

// CHECK-LABEL: sil @$s24mandatory_specialization5test4yyF
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization24transparentGenericCaller1xxx_tlFSi_Tg5 : $@convention(thin) (Int) -> Int
// CHECK: apply [[FN]]({{.*}}) : $@convention(thin) (Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL:end sil function '$s24mandatory_specialization5test4yyF'
public func test4() { _ = transparentGenericCaller(x: 0) }

// CHECK-LABEL: sil @$s24mandatory_specialization5test5yyF
// CHECK-NOT: apply
// CHECK: [[FN:%.*]] = function_ref @$s24mandatory_specialization23mustSpecializeCallsSelf1xxx_tlFSi_Tg5 : $@convention(thin) (Int) -> Int
// CHECK: apply [[FN]]({{.*}}) : $@convention(thin) (Int) -> Int
// CHECK-NOT: apply
// CHECK-LABEL: end sil function '$s24mandatory_specialization5test5yyF'
public func test5() { _ = mustSpecializeCallsSelf(x: 0) }

#if ERRORS

// CHECK-ERROR: error: function topLevelGeneric<A>(x:) was never called with concrete generic substitutions.
// CHECK-ERROR: note: function marked 'must_specialize' called here callsWithGeneric<A>(x:)
@inline(never)
func topLevelGeneric<T>(x: T) -> T { callsWithGeneric(x: x) }

// CHECK-ERROR: error: function publicMustSpecialize<A, B>(x:y:) marked as 'must_specialize' must not be publicly available.
@_semantics("must_specialize")
public func publicMustSpecialize<T, U>(x: T, y: U) -> T { x }

// CHECK-ERROR: error: function publicGeneric<A>(x:) marked as 'must_specialize' must not be publicly available.
@inline(never)
public func publicGeneric<T>(x: T) -> T { callsWithHalfGeneric(x: x) }

#endif
