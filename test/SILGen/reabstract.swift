
// RUN: %target-swift-emit-silgen -module-name reabstract -Xllvm -sil-full-demangle %s | %FileCheck %s
// RUN: %target-swift-emit-sil -module-name reabstract -Xllvm -sil-full-demangle %s | %FileCheck %s --check-prefix=MANDATORY

func closureTakingOptional(_ fn: (Int?) -> ()) {}
closureTakingOptional({ (_: Any) -> () in })

// CHECK-LABEL: sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] @$sypIgn_SiSgIegy_TR :
// CHECK:   [[ANYADDR:%.*]] = alloc_stack $Any
// CHECK:   [[OPTADDR:%.*]] = init_existential_addr [[ANYADDR]] : $*Any, $Optional<Int>
// CHECK:   store %0 to [trivial] [[OPTADDR]] : $*Optional<Int>
// CHECK:   apply %1([[ANYADDR]]) : $@noescape @callee_guaranteed (@in_guaranteed Any) -> ()

func takeFn<T>(_ f : (T) -> T?) {}
func liftOptional(_ x : Int) -> Int? { return x }

func test0() {
  takeFn(liftOptional)
}
// CHECK:    sil hidden [ossa] @$s10reabstract5test0yyF : $@convention(thin) () -> () {
//   Emit a generalized reference to liftOptional.
//   TODO: just emit a globalized thunk
// CHECK:      reabstract.liftOptional
// CHECK-NEXT: [[T1:%.*]] = function_ref @$s10reabstract12liftOptional{{[_0-9a-zA-Z]*}}F
// CHECK-NEXT: [[T2:%.*]] = thin_to_thick_function [[T1]]
// CHECK-NEXT: [[CVT:%.*]] = convert_escape_to_noescape [not_guaranteed] [[T2]]
// CHECK-NEXT: reabstraction thunk
// CHECK-NEXT: [[T3:%.*]] = function_ref [[THUNK:@.*]] :
// CHECK-NEXT: [[T4:%.*]] = partial_apply [callee_guaranteed] [[T3]]([[CVT]])
// CHECK-NEXT: [[T5:%.*]] = convert_function [[T4]]
// CHECK-NEXT: [[CVT:%.*]] = convert_escape_to_noescape [not_guaranteed] [[T5]]
// CHECK: destroy_value [[T5]]
// CHECK:      [[T0:%.*]] = function_ref @$s10reabstract6takeFn{{[_0-9a-zA-Z]*}}F
// CHECK-NEXT: apply [[T0]]<Int>([[CVT]])
// CHECK-NEXT: destroy_value [[CVT]]
// CHECK-NEXT: tuple ()
// CHECK-NEXT: return
// CHECK-NEXT: } // end sil function '$s10reabstract5test0yyF'

// MANDATORY:    sil hidden @$s10reabstract5test0yyF : $@convention(thin) () -> () {
//   Emit a generalized reference to liftOptional.
//   TODO: just emit a globalized thunk
// MANDATORY:      reabstract.liftOptional
// MANDATORY-NEXT: [[T1:%.*]] = function_ref @$s10reabstract12liftOptional{{[_0-9a-zA-Z]*}}F
// MANDATORY-NEXT: [[T2:%.*]] = thin_to_thick_function [[T1]]
// MANDATORY-NEXT: strong_retain [[T2]]
// MANDATORY-NEXT: [[CVT:%.*]] = convert_escape_to_noescape [[T2]]
// MANDATORY-NEXT: //{{.*}}reabstraction thunk
// MANDATORY-NEXT: [[T3:%.*]] = function_ref [[THUNK:@.*]] :
// MANDATORY-NEXT: [[T4:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[T3]]([[CVT]])
// MANDATORY-NEXT: [[T4_1:%.*]] = mark_dependence [[T4]]
// MANDATORY-NEXT: [[T5:%.*]] = convert_function [[T4_1]]
// MANDATORY-NEXT: // function_ref
// MANDATORY-NEXT: [[T0:%.*]] = function_ref @$s10reabstract6takeFn{{[_0-9a-zA-Z]*}}F
// MANDATORY-NEXT: apply [[T0]]<Int>([[T5]])
// MANDATORY-NEXT: strong_release [[T2]]
// MANDATORY-NEXT: dealloc_stack [[T4]] : $@noescape @callee_guaranteed (@in_guaranteed Int) -> @out Optional<Int>
// MANDATORY-NEXT: tuple ()
// MANDATORY-NEXT: return
// MANDATORY-NEXT: } // end sil function '$s10reabstract5test0yyF'

// CHECK:    sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] [[THUNK]] :
// CHECK:      [[T0:%.*]] = load [trivial] %1 : $*Int
// CHECK-NEXT: [[T1:%.*]] = apply %2([[T0]])
// CHECK-NEXT: store [[T1]] to [trivial] %0
// CHECK-NEXT: tuple ()
// CHECK-NEXT: return

// CHECK-LABEL: sil hidden [ossa] @$s10reabstract10testThrowsyyypF
// CHECK:         function_ref @$sytIegr_Ieg_TR
// CHECK:         function_ref @$syts5Error_pIegrzo_sAA_pIegzo_TR
func testThrows(_ x: Any) {
  _ = x as? () -> ()
  _ = x as? () throws -> ()
}

// Make sure that we preserve inout-ness when lowering types with maximum
// abstraction level -- <rdar://problem/21329377>
class C {}

struct Box<T> {
  let t: T
}

func notFun(_ c: inout C, i: Int) {}

func testInoutOpaque(_ c: C, i: Int) {
  var c = c
  let box = Box(t: notFun)
  box.t(&c, i)
}

// CHECK-LABEL: sil hidden [ossa] @$s10reabstract15testInoutOpaque_1iyAA1CC_SitF
// CHECK:         function_ref @$s10reabstract6notFun_1iyAA1CCz_SitF
// CHECK:         thin_to_thick_function {{%[0-9]+}}
// CHECK:         function_ref @$s10reabstract1CCSiIegly_ACSiytIeglnr_TR
// CHECK:         partial_apply
// CHECK:         store
// CHECK:         [[CLOSURE:%.*]] = struct_extract {{.*}}, #Box.t
// CHECK:         [[CLOSURE1:%.*]] = copy_value [[CLOSURE]]
// CHECK:         [[CLOSURE2:%.*]] = begin_borrow [[CLOSURE1]]
// CHECK:         apply [[CLOSURE2]]
// CHECK: } // end sil function '$s10reabstract15testInoutOpaque_1iyAA1CC_SitF'

// CHECK-LABEL: sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] @$s10reabstract1CCSiIegly_ACSiytIeglnr_TR : $@convention(thin) (@inout C, @in_guaranteed Int, @guaranteed @callee_guaranteed (@inout C, Int) -> ()) -> @out () {

// Same behavior as above with other ownership qualifiers.
func evenLessFun(_ s: __shared C, _ o: __owned C) {}

// CHECK-LABEL: sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] @$s10reabstract1CCACIeggx_A2CytIegnir_TR : $@convention(thin) (@in_guaranteed C, @in C, @guaranteed @callee_guaranteed (@guaranteed C, @owned C) -> ()) -> @out ()
func testSharedOwnedOpaque(_ s: C, o: C) {
  let box = Box(t: evenLessFun)
  box.t(s, o)
}

// Make sure that when we generate the reabstraction thunk from Klass -> P, we
// pass off the value at +1.
// CHECK-LABEL: sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] @$s10reabstract1P_pIegg_xIegg_AaBRzlTR : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@guaranteed τ_0_0, @guaranteed @callee_guaranteed (@guaranteed any P) -> ()) -> () {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $τ_0_0,
// CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
// CHECK:   [[EXISTENTIAL:%.*]] = init_existential_ref [[ARG_COPY]]
// CHECK:   [[BORROWED_EXISTENTIAL:%.*]] = begin_borrow [[EXISTENTIAL]]
// CHECK:   apply {{%.*}}([[BORROWED_EXISTENTIAL]])
// CHECK:   end_borrow [[BORROWED_EXISTENTIAL]]
// CHECK:   destroy_value [[EXISTENTIAL]]
// CHECK: } // end sil function '$s10reabstract1P_pIegg_xIegg_AaBRzlTR'
protocol P : class {}
class Klass : P { }
extension P { static func crash(setup: ((Self) -> ())?) {} }
func checkInitExistentialThunk() -> P? {
  let cls : P.Type = Klass.self
  cls.crash(setup: { (arg:  P) -> () in  })
  return nil
}
