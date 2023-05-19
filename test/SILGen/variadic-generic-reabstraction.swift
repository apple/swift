// RUN: %target-swift-emit-silgen %s | %FileCheck %s

func takesVariadicFunction<each T>(function: (repeat each T) -> Int) {}
func takesVariadicOwnedFunction<each T>(function: (repeat __owned each T) -> Int) {}
func takesFunctionPack<each T, R>(functions: repeat ((each T) -> R)) {}

// CHECK-LABEL: sil{{.*}} @$s4main32forwardAndReabstractFunctionPack9functionsySbxXExQp_tRvzlF :
func forwardAndReabstractFunctionPack<each T>(functions: repeat (each T) -> Bool) {
// CHECK: bb0(%0 : $*Pack{repeat @noescape @callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Bool for <each T>}):
// CHECK:         [[ARG_PACK:%.*]] = alloc_pack $Pack{repeat @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>}
// CHECK-NEXT:    [[ARG_TUPLE:%.*]] = alloc_stack $(repeat @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>)
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Word, 0
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Word, 1
// CHECK-NEXT:    [[LEN:%.*]] = pack_length $Pack{repeat each T}
// CHECK-NEXT:    br bb1([[ZERO]] : $Builtin.Word)
// CHECK:       bb1([[IDX:%.*]] : $Builtin.Word)
// CHECK-NEXT:    [[IDX_EQ_LEN:%.*]] = builtin "cmp_eq_Word"([[IDX]] : $Builtin.Word, [[LEN]] : $Builtin.Word) : $Builtin.Int1
// CHECK-NEXT:     cond_br [[IDX_EQ_LEN]], bb3, bb2
// CHECK:       bb2:
// CHECK-NEXT:    [[INDEX:%.*]] = dynamic_pack_index [[IDX]] of $Pack{repeat (each T) -> Bool}
// CHECK-NEXT:    open_pack_element [[INDEX]] of <each T> at <Pack{repeat each T}>, shape $each T, uuid [[UUID:".*"]]
// CHECK-NEXT:    [[ARG_TUPLE_ELT_ADDR:%.*]] = tuple_pack_element_addr [[INDEX]] of [[ARG_TUPLE]] :
//   Load the parameter function from the parameter pack.
// CHECK-NEXT:    [[PARAM_ELT_ADDR:%.*]] = pack_element_get [[INDEX]] of %0 : $*Pack{repeat @noescape @callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Bool for <each T>} as $*@noescape @callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Bool for <@pack_element([[UUID]]) each T>
// CHECK-NEXT:    [[COPY:%.*]] = load [copy] [[PARAM_ELT_ADDR]] :
//   Convert that to an unsubstituted type
// CHECK-NEXT:    [[COPY_CONVERT:%.*]] = convert_function [[COPY]] : $@noescape @callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Bool for <@pack_element([[UUID]]) each T> to $@noescape @callee_guaranteed (@in_guaranteed @pack_element([[UUID]]) each T) -> Bool
//   Wrap in the conversion thunk.
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[THUNK:%.*]] = function_ref @$sxSbIgnd_xSbIegnr_lTR : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0, @guaranteed @noescape @callee_guaranteed (@in_guaranteed τ_0_0) -> Bool) -> @out Bool
// CHECK-NEXT:    [[THUNKED:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]<@pack_element([[UUID]]) each T>([[COPY_CONVERT]])
//   Convert to a substituted type.
// CHECK-NEXT:    [[THUNKED_CONVERT:%.*]] = convert_function [[THUNKED]] : $@callee_guaranteed (@in_guaranteed @pack_element([[UUID]]) each T) -> @out Bool to $@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <@pack_element([[UUID]]) each T, Bool>
//   Convert to noescape.
// CHECK-NEXT:    [[NOESCAPE:%.*]] = convert_escape_to_noescape [not_guaranteed] [[THUNKED_CONVERT]] : $@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <@pack_element([[UUID]]) each T, Bool> to $@noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <@pack_element([[UUID]]) each T, Bool>
//   Store into the tuple and put the tuple address into the argument pack.
// CHECK-NEXT:    store [[NOESCAPE]] to [init] [[ARG_TUPLE_ELT_ADDR]] :
// CHECK-NEXT:    pack_element_set [[ARG_TUPLE_ELT_ADDR]] : $*@noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <@pack_element([[UUID]]) each T, Bool> into %11 of [[ARG_PACK]] : $*Pack{repeat @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>}
//   Destroy the thunked escaping function
//   FIXME: does this leave the noescape function dangling??
// CHECK-NEXT:    destroy_value [[THUNKED_CONVERT]] :
// CHECK-NEXT:    [[NEXT_IDX:%.*]] = builtin "add_Word"([[IDX]] : $Builtin.Word, [[ONE]] : $Builtin.Word) : $Builtin.Word
// CHECK-NEXT:    br bb1([[NEXT_IDX]] : $Builtin.Word)
// CHECK:       bb3:
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[FN:%.*]] = function_ref @$s4main17takesFunctionPack9functionsyq_xXExQp_tRvzr0_lF : $@convention(thin) <each τ_0_0, τ_0_1> (@pack_guaranteed Pack{repeat @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each τ_0_0, τ_0_1>}) -> ()
// CHECK-NEXT:    apply [[FN]]<Pack{repeat each T}, Bool>([[ARG_PACK]])
// CHECK-NEXT:    destroy_addr [[ARG_TUPLE]] :
// CHECK-NEXT:    dealloc_stack [[ARG_TUPLE]] :
// CHECK-NEXT:    dealloc_pack [[ARG_PACK]] :
  takesFunctionPack(functions: repeat each functions)
}

// CHECK-LABEL: sil{{.*}} @$s4main22passConcreteToVariadic2fnyS2i_SStXE_tF :
// CHECK:         [[COPY:%.*]] = copy_value %0 : $@noescape @callee_guaranteed (Int, @guaranteed String) -> Int
// CHECK:         [[THUNK:%.*]] = function_ref @$sSiSSSiIgygd_Si_SSQSiSiIegpd_TR
// CHECK:         partial_apply [callee_guaranteed] [[THUNK]]([[COPY]])
func passConcreteToVariadic(fn: (Int, String) -> Int) {
  takesVariadicFunction(function: fn)
}

// CHECK-LABEL: sil{{.*}} @$sSiSSSiIgygd_Si_SSQSiSiIegpd_TR :
// CHECK:       bb0(%0 : $*Pack{Int, String}, %1 : @guaranteed $@noescape @callee_guaranteed (Int, @guaranteed String) -> Int):
// CHECK-NEXT:    [[INT_INDEX:%.*]] = scalar_pack_index 0 of $Pack{Int, String}
// CHECK-NEXT:    [[INT_ADDR:%.*]] = pack_element_get [[INT_INDEX]] of %0 : $*Pack{Int, String}
// CHECK-NEXT:    [[INT:%.*]] = load [trivial] [[INT_ADDR]] : $*Int
// CHECK-NEXT:    [[STRING_INDEX:%.*]] = scalar_pack_index 1 of $Pack{Int, String}
// CHECK-NEXT:    [[STRING_ADDR:%.*]] = pack_element_get [[STRING_INDEX]] of %0 : $*Pack{Int, String}
// CHECK-NEXT:    [[STRING:%.*]] = load_borrow [[STRING_ADDR]] : $*String
// CHECK-NEXT:    [[RESULT:%.*]] = apply %1([[INT]], [[STRING]])
// CHECK-NEXT:    end_borrow [[STRING]] : $String
// CHECK-NEXT:    return [[RESULT]] : $Int

//   FIXME: we aren't preserving that the argument is owned
// CHECK-LABEL: sil{{.*}} @$s4main27passConcreteToOwnedVariadic2fnyS2i_SStXE_tF :
// CHECK:         [[COPY:%.*]] = copy_value %0 : $@noescape @callee_guaranteed (Int, @guaranteed String) -> Int
// CHECK:         [[THUNK:%.*]] = function_ref @$sSiSSSiIgygd_Si_SSQSiSiIegpd_TR
// CHECK:         partial_apply [callee_guaranteed] [[THUNK]]([[COPY]])
func passConcreteToOwnedVariadic(fn: (Int, String) -> Int) {
  takesVariadicOwnedFunction(function: fn)
}

// CHECK-LABEL: sil{{.*}} @$s4main29passConcreteClosureToVariadic2fnyS2i_SStXE_tF :
// CHECK:       bb0(%0 : @guaranteed $@noescape @callee_guaranteed (Int, @guaranteed String) -> Int)
// CHECK:         // function_ref
// CHECK-NEXT:   [[CLOSURE:%.*]] = function_ref @$s4main29passConcreteClosureToVariadic2fnyS2i_SStXE_tFS2i_SStXEfU_ : $@convention(thin) @substituted <each τ_0_0> (@pack_guaranteed Pack{repeat each τ_0_0}, @guaranteed @noescape @callee_guaranteed (Int, @guaranteed String) -> Int) -> Int for <Pack{Int, String}>
// CHECK-NEXT:    [[T0:%.*]] = copy_value %0 :
// CHECK-NEXT:    partial_apply [callee_guaranteed] [[CLOSURE]]([[T0]])
func passConcreteClosureToVariadic(fn: (Int, String) -> Int) {
  takesVariadicFunction {x,y in fn(x, y)}
}

// CHECK-LABEL: sil{{.*}} @$s4main29passConcreteClosureToVariadic2fnyS2i_SStXE_tFS2i_SStXEfU_ : 
// CHECK:       bb0(%0 : $*Pack{Int, String}, %1 : @closureCapture @guaranteed $@noescape @callee_guaranteed (Int, @guaranteed String) -> Int):
// CHECK-NEXT:    [[INT_INDEX:%.*]] = scalar_pack_index 0 of $Pack{Int, String}
// CHECK-NEXT:    [[INT_ADDR:%.*]] = pack_element_get [[INT_INDEX]] of %0 : $*Pack{Int, String} as $*Int
// CHECK-NEXT:    [[X:%.*]] = load [trivial] [[INT_ADDR]] : $*Int
// CHECK-NEXT:    debug_value [[X]] : $Int, let, name "x", argno 1
// CHECK-NEXT:    [[STRING_INDEX:%.*]] = scalar_pack_index 1 of $Pack{Int, String}
// CHECK-NEXT:    [[STRING_ADDR:%.*]] = pack_element_get [[STRING_INDEX]] of %0 : $*Pack{Int, String} as $*String
// CHECK-NEXT:    [[Y:%.*]] = load_borrow [[STRING_ADDR]] : $*String
// CHECK-NEXT:    debug_value [[Y]] : $String, let, name "y", argno 2
// CHECK:         apply {{.*}}([[X]], [[Y]])
// CHECK:         end_borrow [[Y]] : $String

/* Doesn't currently type-check
func passConcreteClosureToVariadic2(fn: (Int, String) -> Int, x: Int) {
  takesVariadicFunction {y in fn(x, y)}
}
 */
