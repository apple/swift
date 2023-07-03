// RUN: %target-swift-emit-silgen -disable-availability-checking %s | %FileCheck %s

// rdar://110391963

struct Use<each T> {}

struct FunctionProducer<each T> {
  static func get() -> () -> (repeat each T) {
    return { preconditionFailure("") }
  }
}

struct PlusFunctionProducer<U, each T> {
  static func get() -> () -> (U, repeat each T) {
    return { preconditionFailure("") }
  }
}

struct FunctionConsumer<each T> {
  static func set(_: () -> (repeat each T)) {}
}

struct PlusFunctionConsumer<U, each T> {
  static func set(_: () -> (U, repeat each T)) {}
}

func test1() {
  let fn = FunctionProducer<String>.get()
  _ = fn()
}
// CHECK-LABEL: sil {{.*}} @$s4main5test1yyF :
// CHECK:         function_ref @$sSS_QSiIegk_SSIego_TR : $@convention(thin) (@guaranteed @callee_guaranteed () -> @pack_out Pack{String}) -> @owned String
// CHECK-LABEL: sil shared {{.*}} @$sSS_QSiIegk_SSIego_TR :
// CHECK:         [[PACK:%.*]] = alloc_pack $Pack{String}
// CHECK-NEXT:    [[STRING_TEMP:%.*]] = alloc_stack $String
// CHECK-NEXT:    [[STRING_INDEX:%.*]] = scalar_pack_index 0 of $Pack{String}
// CHECK-NEXT:    pack_element_set [[STRING_TEMP]] : $*String into [[STRING_INDEX]] of [[PACK]] : $*Pack{String}
// CHECK-NEXT:    apply %0([[PACK]])
// CHECK-NEXT:    [[STRING:%.*]] = load [take] [[STRING_TEMP]] : $*String
// CHECK-NEXT:    dealloc_stack [[STRING_TEMP]] :
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    return [[STRING]]

func test2() {
  let fn = FunctionProducer<String, Int>.get()
  _ = fn()
}
// CHECK-LABEL: sil {{.*}} @$s4main5test2yyF :
// CHECK:         function_ref @$sSS_SiQSiIegk_SSSiIegod_TR : $@convention(thin) (@guaranteed @callee_guaranteed () -> @pack_out Pack{String, Int}) -> (@owned String, Int)
// CHECK:       sil shared {{.*}} @$sSS_SiQSiIegk_SSSiIegod_TR :
// CHECK:         [[PACK:%.*]] = alloc_pack $Pack{String, Int}
// CHECK-NEXT:    [[STRING_TEMP:%.*]] = alloc_stack $String
// CHECK-NEXT:    [[STRING_INDEX:%.*]] = scalar_pack_index 0 of $Pack{String, Int}
// CHECK-NEXT:    pack_element_set [[STRING_TEMP]] : $*String into [[STRING_INDEX]] of [[PACK]] : $*Pack{String, Int}
// CHECK-NEXT:    [[INT_TEMP:%.*]] = alloc_stack $Int
// CHECK-NEXT:    [[INT_INDEX:%.*]] = scalar_pack_index 1 of $Pack{String, Int}
// CHECK-NEXT:    pack_element_set [[INT_TEMP]] : $*Int into [[INT_INDEX]] of [[PACK]] : $*Pack{String, Int}
// CHECK-NEXT:    apply %0([[PACK]])
// CHECK-NEXT:    [[STRING:%.*]] = load [take] [[STRING_TEMP]] : $*String
// CHECK-NEXT:    [[INT:%.*]] = load [trivial] [[INT_TEMP]] : $*Int
// CHECK-NEXT:    [[TUPLE:%.*]] = tuple ([[STRING]] : $String, [[INT]] : $Int)
// CHECK-NEXT:    dealloc_stack [[INT_TEMP]] :
// CHECK-NEXT:    dealloc_stack [[STRING_TEMP]] :
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    return [[TUPLE]]

func test3() {
  let fn = FunctionProducer< >.get()
  _ = fn()
}
// CHECK-LABEL: sil {{.*}} @$s4main5test3yyF :
// CHECK:         function_ref @$syQSiIegk_Ieg_TR : $@convention(thin) (@guaranteed @callee_guaranteed () -> @pack_out Pack{}) -> ()
// CHECK:       sil shared {{.*}} @$syQSiIegk_Ieg_TR :
// CHECK:         [[PACK:%.*]] = alloc_pack $Pack{}
// CHECK-NEXT:    apply %0([[PACK]])
// CHECK-NEXT:    [[TUPLE:%.*]] = tuple ()
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    return [[TUPLE]]

func test4() {
  let fn = PlusFunctionProducer<String>.get()
  _ = fn()
}
// CHECK-LABEL: sil {{.*}} @$s4main5test4yyF :
// CHECK:         function_ref @$sSSyQSiIegrk_SSIego_TR : $@convention(thin) (@guaranteed @callee_guaranteed () -> (@out String, @pack_out Pack{})) -> @owned String
// CHECK-LABEL: sil shared {{.*}} @$sSSyQSiIegrk_SSIego_TR :
// CHECK:         [[STRING_TEMP:%.*]] = alloc_stack $String
// CHECK-NEXT:    [[PACK:%.*]] = alloc_pack $Pack{}
// CHECK-NEXT:    apply %0([[STRING_TEMP]], [[PACK]])
// CHECK-NEXT:    [[STRING:%.*]] = load [take] [[STRING_TEMP]] : $*String
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    dealloc_stack [[STRING_TEMP]] :
// CHECK-NEXT:    return [[STRING]]

func test5(fn: () -> (String, Int)) {
  FunctionConsumer.set(fn)
}
// CHECK-LABEL: sil {{.*}} @$s4main5test52fnySS_SityXE_tF :
// CHECK:         function_ref @$sSSSiIgod_SS_SiQSiIegk_TR : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> (@owned String, Int)) -> @pack_out Pack{String, Int}
// CHECK-LABEL: sil shared {{.*}} @$sSSSiIgod_SS_SiQSiIegk_TR :
// CHECK:         [[STRING_INDEX:%.*]] = scalar_pack_index 0 of $Pack{String, Int}
// CHECK-NEXT:    [[STRING_ADDR:%.*]] = pack_element_get [[STRING_INDEX]] of %0 : $*Pack{String, Int} as $*String
// CHECK-NEXT:    [[INT_INDEX:%.*]] = scalar_pack_index 1 of $Pack{String, Int}
// CHECK-NEXT:    [[INT_ADDR:%.*]] = pack_element_get [[INT_INDEX]] of %0 : $*Pack{String, Int} as $*Int
// CHECK-NEXT:    [[RESULT:%.*]] = apply %1()
// CHECK-NEXT:    ([[STRING:%.*]], [[INT:%.*]]) = destructure_tuple [[RESULT]]
// CHECK-NEXT:    store [[STRING]] to [init] [[STRING_ADDR]] :
// CHECK-NEXT:    store [[INT]] to [trivial] [[INT_ADDR]] :

func test6(fn: () -> (String, Int)) {
  PlusFunctionConsumer.set(fn)
}
// CHECK-LABEL: sil {{.*}} @$s4main5test62fnySS_SityXE_tF :
// CHECK:         function_ref @$sSSSiIgod_SSSi_QSiIegrk_TR : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> (@owned String, Int)) -> (@out String, @pack_out Pack{Int})
// CHECK-LABEL: sil shared {{.*}} @$sSSSiIgod_SSSi_QSiIegrk_TR :
// CHECK:         [[INT_INDEX:%.*]] = scalar_pack_index 0 of $Pack{Int}
// CHECK-NEXT:    [[INT_ADDR:%.*]] = pack_element_get [[INT_INDEX]] of %1 : $*Pack{Int} as $*Int
// CHECK-NEXT:    [[RESULT:%.*]] = apply %2()
// CHECK-NEXT:    ([[STRING:%.*]], [[INT:%.*]]) = destructure_tuple [[RESULT]]
// CHECK-NEXT:    store [[STRING]] to [init] %0 :
// CHECK-NEXT:    store [[INT]] to [trivial] [[INT_ADDR]] :

func test7(fn: () -> String) {
  PlusFunctionConsumer.set(fn)
}
// CHECK-LABEL: sil {{.*}} @$s4main5test72fnySSyXE_tF :
// CHECK:         function_ref @$sSSIgo_SSyQSiIegrk_TR : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> @owned String) -> (@out String, @pack_out Pack{})
// CHECK-LABEL: sil shared {{.*}} @$sSSIgo_SSyQSiIegrk_TR :
// CHECK:         [[STRING:%.*]] = apply %2()
// CHECK-NEXT:    store [[STRING]] to [init] %0 :

func test8() {
  PlusFunctionConsumer.set(FunctionProducer<(String, Int)>.get())
}
// CHECK-LABEL: sil {{.*}} @$s4main5test8yyF :
//   We emit this with a chain of two reabstraction thunks, which is a pity,
//   because otherwise it would truly be a marvelously complex thunk.
// CHECK:         function_ref @$sSS_Sit_QSiIegk_SSSiIegod_TR : $@convention(thin) (@guaranteed @callee_guaranteed () -> @pack_out Pack{(String, Int)}) -> (@owned String, Int)
// CHECK:         function_ref @$sSSSiIgod_SSSi_QSiIegrk_TR : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> (@owned String, Int)) -> (@out String, @pack_out Pack{Int})
// CHECK-LABEL: sil shared {{.*}} @$sSS_Sit_QSiIegk_SSSiIegod_TR :
// CHECK:         [[PACK:%.*]] = alloc_pack $Pack{(String, Int)}
// CHECK-NEXT:    [[TUPLE_TEMP:%.*]] = alloc_stack $(String, Int)
// CHECK-NEXT:    [[TUPLE_INDEX:%.*]] = scalar_pack_index 0 of $Pack{(String, Int)}
// CHECK-NEXT:    pack_element_set [[TUPLE_TEMP]] : $*(String, Int) into [[TUPLE_INDEX]] of [[PACK]] : $*Pack{(String, Int)}
// CHECK-NEXT:    [[STRING_ADDR:%.*]] = tuple_element_addr [[TUPLE_TEMP]] : $*(String, Int), 0
// CHECK-NEXT:    [[INT_ADDR:%.*]] = tuple_element_addr [[TUPLE_TEMP]] : $*(String, Int), 1
// CHECK-NEXT:    apply %0([[PACK]])
// CHECK-NEXT:    [[STRING:%.*]] = load [take] [[STRING_ADDR]] : $*String
// CHECK-NEXT:    [[INT:%.*]] = load [trivial] [[INT_ADDR]] : $*Int
// CHECK-NEXT:    [[TUPLE:%.*]] = tuple ([[STRING]] : $String, [[INT]] : $Int)
// CHECK-NEXT:    dealloc_stack [[TUPLE_TEMP]] :
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    return [[TUPLE]]

func test9<each T>() -> Use<repeat each T> {
  let fn = FunctionProducer<String, repeat each T, Int>.get()
  _ = fn()
  return Use()
}
// CHECK-LABEL: sil {{.*}} @$s4main5test9AA3UseVyxxQp_QPGyRvzlF :
// CHECK:         function_ref @$sSS_xxQpSiQSiIegk_SSxxQp_QSiSiIegokd_RvzlTR : $@convention(thin) <each τ_0_0> (@guaranteed @callee_guaranteed () -> @pack_out Pack{String, repeat each τ_0_0, Int}) -> (@owned String, @pack_out Pack{repeat each τ_0_0}, Int)
// CHECK-LABEL: sil shared {{.*}} @$sSS_xxQpSiQSiIegk_SSxxQp_QSiSiIegokd_RvzlTR :
//   Create the inner pack argument.
// CHECK:         [[PACK:%.*]] = alloc_pack $Pack{String, repeat each T, Int}
//   - Set up the string result temporary.
// CHECK-NEXT:    [[STRING_TEMP:%.*]] = alloc_stack $String
// CHECK-NEXT:    [[STRING_INDEX:%.*]] = scalar_pack_index 0 of $Pack{String, repeat each T, Int}
// CHECK-NEXT:    pack_element_set [[STRING_TEMP]] : $*String into [[STRING_INDEX]] of [[PACK]] : $*Pack{String, repeat each T, Int}
//   - Forward the pack expansion addresses.
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
// CHECK-NEXT:    [[ELT_INDEX:%.*]] = pack_pack_index 1, [[EXPANSION_INDEX]] of $Pack{String, repeat each T, Int}
// CHECK-NEXT:    [[DEST_ELT_ADDR:%.*]] = pack_element_get [[EXPANSION_INDEX]] of %0 : $*Pack{repeat each T} as $*@pack_element([[UUID]]) each T
// CHECK-NEXT:    pack_element_set [[DEST_ELT_ADDR]] : $*@pack_element([[UUID]]) each T into [[ELT_INDEX]] of [[PACK]] : $*Pack{String, repeat each T, Int}
// CHECK-NEXT:    [[NEXT_IDX:%.*]] = builtin "add_Word"([[IDX]] : $Builtin.Word, [[ONE]] : $Builtin.Word) : $Builtin.Word
// CHECK-NEXT:    br bb1([[NEXT_IDX]] : $Builtin.Word)
// CHECK:       bb3:
//   - Set up the int result temporary.
// CHECK-NEXT:    [[INT_TEMP:%.*]] = alloc_stack $Int
// CHECK-NEXT:    [[INT_INDEX:%.*]] = scalar_pack_index 2 of $Pack{String, repeat each T, Int}
// CHECK-NEXT:    pack_element_set [[INT_TEMP]] : $*Int into [[INT_INDEX]] of [[PACK]] : $*Pack{String, repeat each T, Int}
//   Call the reabstracted function.
// CHECK-NEXT:    apply %1([[PACK]])
//   Load the direct components and tuple them up for the return.
// CHECK-NEXT:    [[STRING:%.*]] = load [take] [[STRING_TEMP]] : $*String
// CHECK-NEXT:    [[INT:%.*]] = load [trivial] [[INT_TEMP]] : $*Int
// CHECK-NEXT:    [[TUPLE:%.*]] = tuple ([[STRING]] : $String, [[INT]] : $Int)
// CHECK-NEXT:    dealloc_stack [[INT_TEMP]] :
// CHECK-NEXT:    dealloc_stack [[STRING_TEMP]] :
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    return [[TUPLE]]

func test10<each T>() -> Use<repeat each T> {
  let fn = FunctionProducer<(String, repeat each T, Int)>.get()
  _ = fn()
  return Use()
}
// CHECK-LABEL: sil {{.*}} @$s4main6test10AA3UseVyxxQp_QPGyRvzlF :
// CHECK:         function_ref @$sSS_xxQpSit_QSiIegk_SSxxQp_QSiSiIegokd_RvzlTR : $@convention(thin) <each τ_0_0> (@guaranteed @callee_guaranteed () -> @pack_out Pack{(String, repeat each τ_0_0, Int)}) -> (@owned String, @pack_out Pack{repeat each τ_0_0}, Int)
// CHECK-LABEL: sil shared {{.*}} @$sSS_xxQpSit_QSiIegk_SSxxQp_QSiSiIegokd_RvzlTR :
//   Set up the inner pack argument with a tuple temporary.
// CHECK:         [[PACK:%.*]] = alloc_pack $Pack{(String, repeat each T, Int)}
// CHECK-NEXT:    [[TUPLE_TEMP:%.*]] = alloc_stack $(String, repeat each T, Int)
// CHECK-NEXT:    [[TUPLE_INDEX:%.*]] = scalar_pack_index 0 of $Pack{(String, repeat each T, Int)}
// CHECK-NEXT:    pack_element_set [[TUPLE_TEMP]] : $*(String, repeat each T, Int) into [[TUPLE_INDEX]] of [[PACK]] : $*Pack{(String, repeat each T, Int)}
//   Project out the tuple elements for the non-expansion elements;
//   we'll use these after the call.
// CHECK-NEXT:    [[STRING_INDEX:%.*]] = scalar_pack_index 0 of $Pack{String, repeat each T, Int}
// CHECK-NEXT:    [[STRING_ADDR:%.*]] = tuple_pack_element_addr [[STRING_INDEX]] of [[TUPLE_TEMP]] : $*(String, repeat each T, Int) as $*String
// CHECK-NEXT:    [[INT_INDEX:%.*]] = scalar_pack_index 2 of $Pack{String, repeat each T, Int}
// CHECK-NEXT:    [[INT_ADDR:%.*]] = tuple_pack_element_addr [[INT_INDEX]] of [[TUPLE_TEMP]] : $*(String, repeat each T, Int) as $*Int
//   Call the reabstracted function.
// CHECK-NEXT:    apply %1([[PACK]])
//   Load the first component of the tuple (the string).
// CHECK-NEXT:    [[STRING:%.*]] = load [take] [[STRING_ADDR]] : $*String
//   Perform a pack loop to move the pack expansion elements into
//   the outer pack.
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
// CHECK-NEXT:    [[TUPLE_ELT_INDEX:%.*]] = pack_pack_index 1, [[EXPANSION_INDEX]] of $Pack{String, repeat each T, Int}
// CHECK-NEXT:    [[SRC_ELT_ADDR:%.*]] = tuple_pack_element_addr [[TUPLE_ELT_INDEX]] of [[TUPLE_TEMP]] : $*(String, repeat each T, Int) as $*@pack_element([[UUID]]) each T
// CHECK-NEXT:    [[DEST_ELT_ADDR:%.*]] = pack_element_get [[EXPANSION_INDEX]] of %0 : $*Pack{repeat each T} as $*@pack_element([[UUID]]) each T
// CHECK-NEXT:    copy_addr [take] [[SRC_ELT_ADDR]] to [init] [[DEST_ELT_ADDR]] : $*@pack_element([[UUID]]) each T
// CHECK-NEXT:    [[NEXT_IDX:%.*]] = builtin "add_Word"([[IDX]] : $Builtin.Word, [[ONE]] : $Builtin.Word) : $Builtin.Word
// CHECK-NEXT:    br bb1([[NEXT_IDX]] : $Builtin.Word)
// CHECK:       bb3:
//   Load the last component of the tuple (the int).
// CHECK-NEXT:    [[INT:%.*]] = load [trivial] [[INT_ADDR]] : $*Int
//   Return.
// CHECK-NEXT:    [[TUPLE:%.*]] = tuple ([[STRING]] : $String, [[INT]] : $Int)
// CHECK-NEXT:    dealloc_stack [[TUPLE_TEMP]] :
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    return [[TUPLE]]

// Test that we can reabstract the components of a pack expansion.
func test11<each T>() -> Use<repeat each T> {
  let fn = FunctionProducer<String, repeat (each T) -> Bool, Int>.get()
  _ = fn()
  return Use()
}
// CHECK-LABEL: sil {{.*}} @$s4main6test11AA3UseVyxxQp_QPGyRvzlF :
// CHECK:         function_ref @$sSS_xq_r0_lyxSbIsegnr_xQpSiQSiIegk_SSxSblyxIsegnd_xQp_QSiSiIegokd_RvzlTR : $@convention(thin) <each τ_0_0> (@guaranteed @callee_guaranteed () -> @pack_out Pack{String, repeat @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each τ_0_0, Bool>, Int}) -> (@owned String, @pack_out Pack{repeat @callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Bool for <each τ_0_0>}, Int)
// CHECK-LABEL: sil shared {{.*}} @$sSS_xq_r0_lyxSbIsegnr_xQpSiQSiIegk_SSxSblyxIsegnd_xQp_QSiSiIegokd_RvzlTR :
//   Create the inner pack argument.
// CHECK:         [[PACK:%.*]] = alloc_pack $Pack{String, repeat @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>, Int}
//   - Set up the string result temporary.
// CHECK-NEXT:    [[STRING_TEMP:%.*]] = alloc_stack $String
// CHECK-NEXT:    [[STRING_INDEX:%.*]] = scalar_pack_index 0 of $Pack{String, repeat (each T) -> Bool, Int}
// CHECK-NEXT:    pack_element_set [[STRING_TEMP]] : $*String into [[STRING_INDEX]] of [[PACK]] : $*Pack{String, repeat @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>, Int}
//   - Set up a temporary for the pack expansion values and project
//     its element addresses into the pack.
// CHECK-NEXT:    [[EXPANSION_TEMP:%.*]] = alloc_stack $(repeat @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>)
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Word, 0
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Word, 1
// CHECK-NEXT:    [[LEN:%.*]] = pack_length $Pack{repeat each T}
// CHECK-NEXT:    br bb1([[ZERO]] : $Builtin.Word)
// CHECK:       bb1([[IDX:%.*]] : $Builtin.Word)
// CHECK-NEXT:    [[IDX_EQ_LEN:%.*]] = builtin "cmp_eq_Word"([[IDX]] : $Builtin.Word, [[LEN]] : $Builtin.Word) : $Builtin.Int1
// CHECK-NEXT:     cond_br [[IDX_EQ_LEN]], bb3, bb2
// CHECK:       bb2:
// CHECK-NEXT:    [[EXPANSION_INDEX:%.*]] = dynamic_pack_index [[IDX]] of $Pack{repeat (each T) -> Bool}
// CHECK-NEXT:    open_pack_element [[EXPANSION_INDEX]] of <each T> at <Pack{repeat each T}>, shape $each T, uuid [[UUID:".*"]]
// CHECK-NEXT:    [[PACK_ELT_INDEX:%.*]] = pack_pack_index 1, [[EXPANSION_INDEX]] of $Pack{String, repeat (each T) -> Bool, Int}
// CHECK-NEXT:    [[TEMP_ELT_ADDR:%.*]] = tuple_pack_element_addr [[EXPANSION_INDEX]] of [[EXPANSION_TEMP]] : $*(repeat @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>) as $*@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <@pack_element([[UUID]]) each T, Bool>
// CHECK-NEXT:    pack_element_set [[TEMP_ELT_ADDR]] : $*@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <@pack_element([[UUID]]) each T, Bool> into [[PACK_ELT_INDEX]] of [[PACK]] :
// CHECK-NEXT:    [[NEXT_IDX:%.*]] = builtin "add_Word"([[IDX]] : $Builtin.Word, [[ONE]] : $Builtin.Word) : $Builtin.Word
// CHECK-NEXT:    br bb1([[NEXT_IDX]] : $Builtin.Word)
// CHECK:       bb3:
//   - Set up the int result temporary.
// CHECK-NEXT:    [[INT_TEMP:%.*]] = alloc_stack $Int
// CHECK-NEXT:    [[INT_INDEX:%.*]] = scalar_pack_index 2 of $Pack{String, repeat (each T) -> Bool, Int}
// CHECK-NEXT:    pack_element_set [[INT_TEMP]] : $*Int into [[INT_INDEX]] of [[PACK]] : $*Pack{String, repeat @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>, Int}
//   Call the reabstracted function.
// CHECK-NEXT:    apply %1([[PACK]])
//   Reassemble the outer results:
//   - Load the first direct component.
// CHECK-NEXT:    [[STRING:%.*]] = load [take] [[STRING_TEMP]] : $*String
//   - Reabstract the pack expansion components.
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Word, 0
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Word, 1
// CHECK-NEXT:    [[LEN:%.*]] = pack_length $Pack{repeat each T}
// CHECK-NEXT:    br bb4([[ZERO]] : $Builtin.Word)
// CHECK:       bb4([[IDX:%.*]] : $Builtin.Word)
// CHECK-NEXT:    [[IDX_EQ_LEN:%.*]] = builtin "cmp_eq_Word"([[IDX]] : $Builtin.Word, [[LEN]] : $Builtin.Word) : $Builtin.Int1
// CHECK-NEXT:     cond_br [[IDX_EQ_LEN]], bb6, bb5
// CHECK:       bb5:
//   FIXME: the pack type on this seems to be a lowered type
// CHECK-NEXT:    [[EXPANSION_INDEX:%.*]] = dynamic_pack_index [[IDX]] of
// CHECK-NEXT:    open_pack_element [[EXPANSION_INDEX]] of <each T> at <Pack{repeat each T}>, shape $each T, uuid [[UUID:".*"]]
// CHECK-NEXT:    [[SRC_ELT_ADDR:%.*]] = tuple_pack_element_addr [[EXPANSION_INDEX]] of [[EXPANSION_TEMP]] : $*(repeat @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <each T, Bool>) as $*@callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0) -> @out τ_0_1 for <@pack_element([[UUID]]) each T, Bool>
//     - Load the function value and apply the thunking conversion.
// CHECK-NEXT:    [[ELT:%.*]] = load [take] [[SRC_ELT_ADDR]] :
// CHECK-NEXT:    [[DEST_ELT_ADDR:%.*]] = pack_element_get [[EXPANSION_INDEX]] of %0 : $*Pack{repeat @callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Bool for <each T>} as $*@callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Bool for <@pack_element([[UUID]]) each T>
// CHECK-NEXT:    [[ESCAPING_ELT:%.*]] = convert_function [[ELT]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[THUNK:%.*]] = function_ref @$sxSbIegnr_xSbIegnd_lTR : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0, @guaranteed @callee_guaranteed (@in_guaranteed τ_0_0) -> @out Bool) -> Bool
// CHECK-NEXT:    [[CONVERTED_ELT:%.*]] = partial_apply
// CHECK-NEXT:    [[CONVERTED_ELT_2:%.*]] = convert_function [[CONVERTED_ELT]]
// CHECK-NEXT:    store [[CONVERTED_ELT_2]] to [init] [[DEST_ELT_ADDR]] :
// CHECK-NEXT:    [[NEXT_IDX:%.*]] = builtin "add_Word"([[IDX]] : $Builtin.Word, [[ONE]] : $Builtin.Word) : $Builtin.Word
// CHECK-NEXT:    br bb4([[NEXT_IDX]] : $Builtin.Word)
// CHECK:       bb6:
//   - Load the last direct component.
// CHECK-NEXT:    [[INT:%.*]] = load [trivial] [[INT_TEMP]] : $*Int
//   Tuple up the direct components.
// CHECK-NEXT:    [[TUPLE:%.*]] = tuple ([[STRING]] : $String, [[INT]] : $Int)
// CHECK-NEXT:    dealloc_stack [[INT_TEMP]] :
// CHECK-NEXT:    dealloc_stack [[EXPANSION_TEMP]] :
// CHECK-NEXT:    dealloc_stack [[STRING_TEMP]] :
// CHECK-NEXT:    dealloc_pack [[PACK]] :
// CHECK-NEXT:    return [[TUPLE]]
