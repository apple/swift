// RUN: %target-swift-emit-silgen -disable-availability-checking %s | %FileCheck %s

func takeEscapingFunction<each Input, Output>(function: @escaping ((repeat each Input)) -> Output) {}
func returnFunction<each Input, Output>(args: (repeat each Input).Type, result: Output.Type) -> (_: (repeat each Input)) -> Output {}

// Issue #69028: Vanishing tuple in a reabstraction into a more abstract context
func catFact(num: Int) -> String { "there are more than 10 cats" }
func test0() {
  takeEscapingFunction(function: catFact)
}
// CHECK-LABEL: sil hidden [ossa] @$s4main5test0yyF :
// CHECK:         [[THUNK:%.*]] = function_ref @$sSiSSIegyo_Si_QSiSSIegpr_TR : $@convention(thin) (@pack_guaranteed Pack{Int}, @guaranteed @callee_guaranteed (Int) -> @owned String) -> @out String
// CHECK-NEXT:    partial_apply [callee_guaranteed] [[THUNK]]

// CHECK-LABEL: sil shared {{.*}} @$sSiSSIegyo_Si_QSiSSIegpr_TR : $@convention(thin) (@pack_guaranteed Pack{Int}, @guaranteed @callee_guaranteed (Int) -> @owned String) -> @out String {
// CHECK:       bb0(%0 : $*String, %1 : $*Pack{Int}, %2 : @guaranteed $@callee_guaranteed (Int) -> @owned String):
// CHECK-NEXT:    [[INDEX:%.*]] = scalar_pack_index 0 of $Pack{Int}
// CHECK-NEXT:    [[ARG_ADDR:%.*]] = pack_element_get [[INDEX]] of %1 : $*Pack{Int} as $*Int
// CHECK-NEXT:    [[ARG:%.*]] = load [trivial] [[ARG_ADDR]] : $*Int
// CHECK-NEXT:    [[RESULT:%.*]] = apply %2([[ARG]]) : $@callee_guaranteed (Int) -> @owned String
// CHECK-NEXT:    store [[RESULT]] to [init] %0 : $*String
// CHECK-NEXT:    [[RESULT:%.*]] = tuple ()
// CHECK-NEXT:    return [[RESULT]] : $()
// CHECK-NEXT:  }

// Vanishing tuple in a reabstraction out of a more abstract context
func test1() {
  _ = returnFunction(args: String.self, result: [String].self)
}
// CHECK-LABEL: sil hidden [ossa] @$s4main5test1yyF :
// CHECK:         [[THUNK:%.*]] = function_ref @$sSS_QSiSaySSGIegpr_SSAAIeggo_TR : $@convention(thin) (@guaranteed String, @guaranteed @callee_guaranteed (@pack_guaranteed Pack{String}) -> @out Array<String>) -> @owned Array<String>
// CHECK-NEXT:    partial_apply [callee_guaranteed] [[THUNK]]

// CHECK-LABEL: sil shared {{.*}} @$sSS_QSiSaySSGIegpr_SSAAIeggo_TR : $@convention(thin) (@guaranteed String, @guaranteed @callee_guaranteed (@pack_guaranteed Pack{String}) -> @out Array<String>) -> @owned Array<String> {
// CHECK:       bb0(%0 : @guaranteed $String, %1 : @guaranteed $@callee_guaranteed (@pack_guaranteed Pack{String}) -> @out Array<String>):
// CHECK-NEXT:    [[PACK:%.*]] = alloc_pack $Pack{String}
// CHECK-NEXT:    [[ARG_TEMP:%.*]] = alloc_stack $String
//   It'd be nice to avoid this unnecessary copy.
// CHECK-NEXT:    [[ARG_TEMP_BORROW:%.*]] = store_borrow %0 to [[ARG_TEMP]]
// CHECK-NEXT:    [[INDEX:%.*]] = scalar_pack_index 0 of $Pack{String}
// CHECK-NEXT:    pack_element_set [[ARG_TEMP_BORROW]] : $*String into [[INDEX]] of [[PACK]] : $*Pack{String}
// CHECK-NEXT:    [[RESULT_TEMP:%.*]] = alloc_stack $Array<String>
// CHECK-NEXT:    apply %1([[RESULT_TEMP]], [[PACK]]) : $@callee_guaranteed (@pack_guaranteed Pack{String}) -> @out Array<String>
// CHECK-NEXT:    [[RESULT:%.*]] = load [take] [[RESULT_TEMP]] : $*Array<String>
// CHECK-NEXT:    dealloc_stack [[RESULT_TEMP]] : $*Array<String>
// CHECK-NEXT:    end_borrow [[ARG_TEMP_BORROW]]
// CHECK-NEXT:    dealloc_stack [[ARG_TEMP]] : $*String
// CHECK-NEXT:    dealloc_pack [[PACK]] : $*Pack{String}
// CHECK-NEXT:    return [[RESULT]] : $Array<String>
// CHECK-NEXT:  }

// A non-vanishing tuple in a reabstraction into a more abstract context
func lionFact(label: (String, Int)) -> String { "all lions are cats" }
func test2() {
  takeEscapingFunction(function: lionFact)
}
// CHECK-LABEL: sil hidden [ossa] @$s4main5test2yyF :
// CHECK:         [[THUNK:%.*]] = function_ref @$sSSSiSSIeggyo_SS_SiQSiSSIegpr_TR : $@convention(thin) (@pack_guaranteed Pack{String, Int}, @guaranteed @callee_guaranteed (@guaranteed String, Int) -> @owned String) -> @out String
// CHECK-NEXT:    partial_apply [callee_guaranteed] [[THUNK]]

// CHECK-LABEL: sil shared {{.*}} @$sSSSiSSIeggyo_SS_SiQSiSSIegpr_TR : $@convention(thin) (@pack_guaranteed Pack{String, Int}, @guaranteed @callee_guaranteed (@guaranteed String, Int) -> @owned String) -> @out String {
// CHECK:       bb0(%0 : $*String, %1 : $*Pack{String, Int}, %2 : @guaranteed $@callee_guaranteed (@guaranteed String, Int) -> @owned String):
// CHECK-NEXT:    [[ARG0_INDEX:%.*]] = scalar_pack_index 0 of $Pack{String, Int}
// CHECK-NEXT:    [[ARG0_ADDR:%.*]] = pack_element_get [[ARG0_INDEX]] of %1 : $*Pack{String, Int} as $*String
// CHECK-NEXT:    [[ARG0:%.*]] = load_borrow [[ARG0_ADDR]] : $*String
// CHECK-NEXT:    [[ARG1_INDEX:%.*]] = scalar_pack_index 1 of $Pack{String, Int}
// CHECK-NEXT:    [[ARG1_ADDR:%.*]] = pack_element_get [[ARG1_INDEX]] of %1 : $*Pack{String, Int} as $*Int
// CHECK-NEXT:    [[ARG1:%.*]] = load [trivial] [[ARG1_ADDR]] : $*Int
// CHECK-NEXT:    [[RESULT:%.*]] = apply %2([[ARG0]], [[ARG1]]) : $@callee_guaranteed (@guaranteed String, Int) -> @owned String
// CHECK-NEXT:    store [[RESULT]] to [init] %0 : $*String
// CHECK-NEXT:    [[RESULT:%.*]] = tuple ()
// CHECK-NEXT:    end_borrow [[ARG0]] : $String
// CHECK-NEXT:    return [[RESULT]] : $()
// CHECK-NEXT:  }

struct Args<each T> {
  static func take(function: @escaping (_: (repeat each T)) -> ()) {}
}

#if false
// For some reason, this doesn't type-check
func test3(function: @escaping (_: (Int, String)?) -> ()) {
  Args<Int, String>.take(function: function)
}
#endif

func test4<each T>(function: @escaping (_: Any) -> (), type: (repeat each T).Type) {
  Args<repeat each T>.take(function: function)
}
// CHECK-LABEL: sil hidden [ossa] @$s4main5test48function4typeyyypc_xxQp_tmtRvzlF :
// CHECK:         [[THUNK:%.*]] = function_ref @$sypIegn_xxQp_QSiIegp_RvzlTR : $@convention(thin) <each τ_0_0> (@pack_guaranteed Pack{repeat each τ_0_0}, @guaranteed @callee_guaranteed (@in_guaranteed Any) -> ()) -> ()
// CHECK-NEXT:    partial_apply [callee_guaranteed] [[THUNK]]

// CHECK-LABEL: sil shared {{.*}} @$sypIegn_xxQp_QSiIegp_RvzlTR : $@convention(thin) <each T> (@pack_guaranteed Pack{repeat each T}, @guaranteed @callee_guaranteed (@in_guaranteed Any) -> ()) -> () {
// CHECK:       bb0(%0 : $*Pack{repeat each T}, %1 : @guaranteed $@callee_guaranteed (@in_guaranteed Any) -> ()):
//   - Set up for emitting into a temporary Any object.
// CHECK-NEXT:    [[ANY_TEMP:%.*]] = alloc_stack $Any
// CHECK-NEXT:    [[PAYLOAD_ADDR:%.*]] = init_existential_addr [[ANY_TEMP]] : $*Any, $(repeat each T)
//   - Loop over the pack and copy the elements into the Any.
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Word, 0
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Word, 1
// CHECK-NEXT:    [[LEN:%.*]] = pack_length $Pack{repeat each T}
// CHECK-NEXT:    br bb1([[ZERO]] : $Builtin.Word)
// CHECK:       bb1([[IDX:%.*]] : $Builtin.Word)
// CHECK-NEXT:    [[IDX_EQ_LEN:%.*]] = builtin "cmp_eq_Word"([[IDX]] : $Builtin.Word, [[LEN]] : $Builtin.Word) : $Builtin.Int1
// CHECK-NEXT:     cond_br [[IDX_EQ_LEN]], bb3, bb2
// CHECK:       bb2:
// CHECK-NEXT:    [[EXPANSION_INDEX:%.*]] = dynamic_pack_index [[IDX]] of $Pack{repeat each T}
// CHECK-NEXT:    open_pack_element [[EXPANSION_INDEX]] of <each T> at <Pack{repeat each T}>, shape $each T, uuid [[UUID:".*"]]
// CHECK-NEXT:    [[SRC_ELT_ADDR:%.*]] = pack_element_get [[EXPANSION_INDEX]] of %0 : $*Pack{repeat each T} as $*@pack_element([[UUID]]) each T
// CHECK-NEXT:    [[DEST_ELT_ADDR:%.*]] = tuple_pack_element_addr [[EXPANSION_INDEX]] of [[PAYLOAD_ADDR]] : $*(repeat each T) as $*@pack_element([[UUID]]) each T
// CHECK-NEXT:    copy_addr [[SRC_ELT_ADDR]] to [init] [[DEST_ELT_ADDR]] : $*@pack_element([[UUID]]) each T
// CHECK-NEXT:    [[NEXT_IDX:%.*]] = builtin "add_Word"([[IDX]] : $Builtin.Word, [[ONE]] : $Builtin.Word) : $Builtin.Word
// CHECK-NEXT:    br bb1([[NEXT_IDX]] : $Builtin.Word)
// CHECK:       bb3:
//   - Call the thunked function.
// CHECK-NEXT:    apply %1([[ANY_TEMP]])
//   - Epilogue.
// CHECK-NEXT:    [[VOID:%.*]] = tuple ()
// CHECK-NEXT:    destroy_addr [[ANY_TEMP]] : $*Any
// CHECK-NEXT:    dealloc_stack [[ANY_TEMP]] : $*Any
// CHECK-NEXT:    return [[VOID]] : $()
