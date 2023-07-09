// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize

public func takesNoEscape(_: () -> ()) {}

public func has_metadata_pack<each T>(t: repeat each T) -> () -> () {
  return { _ = (repeat each T).self }
}

// CHECK-LABEL: define{{( protected)?}}{{( dllexport)?}} swiftcc { ptr, ptr } @"$s25variadic_generic_captures17has_metadata_pack1tyycxxQp_tRvzlF"(ptr noalias nocapture %0, i{{32|64}} %1, ptr %"each T") #0 {
// CHECK: [[CONTEXT0:%.*]] = call noalias ptr @swift_allocObject(

// CHECK: [[GENERIC_ARGS_ADDR:%.*]] = getelementptr inbounds {{.*}} [[CONTEXT0]], i32 0, i32 {{(1|2)}}
// CHECK: store [[INT]] %1, ptr [[GENERIC_ARGS_ADDR]]

// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[GENERIC_ARGS_ADDR]], i32 1
// CHECK: [[T_HEAP:%.*]] = call swiftcc ptr @swift_allocateMetadataPack(ptr %"each T", [[INT]] %1)
// CHECK: store ptr [[T_HEAP]], ptr [[T_ADDR]]

// CHECK: [[CONTEXT1:%.*]] = insertvalue {{.*}} @"$s25variadic_generic_captures17has_metadata_pack1tyycxxQp_tRvzlFyycfU_TA{{(\.ptrauth)?}}"
// CHECK: ret { ptr, ptr } [[CONTEXT1]]

// CHECK-LABEL: define internal swiftcc void @"$s25variadic_generic_captures17has_metadata_pack1tyycxxQp_tRvzlFyycfU_TA"(ptr swiftself %0)
// CHECK: [[GENERIC_ARGS:%.*]] = getelementptr inbounds {{.*}} %0, i32 0, i32 {{(1|2)}}

// CHECK: [[SHAPE:%.*]] = load [[INT]], ptr [[GENERIC_ARGS]]

// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[GENERIC_ARGS]], i32 1
// CHECK: [[T:%.*]] = load ptr, ptr [[T_ADDR]]

// CHECK: tail call swiftcc void @"$s25variadic_generic_captures17has_metadata_pack1tyycxxQp_tRvzlFyycfU_"([[INT]] [[SHAPE]], ptr [[T]])
// CHECK: ret void

public func has_metadata_pack_noescape<each T>(t: repeat each T) {
  takesNoEscape { _ = (repeat each T).self }
}

// CHECK-LABEL: define{{( protected)?}}{{( dllexport)?}} swiftcc void @"$s25variadic_generic_captures26has_metadata_pack_noescape1tyxxQp_tRvzlF"(ptr noalias nocapture %0, i{{32|64}} %1, ptr %"each T") #0 {
// CHECK: [[CONTEXT0:%.*]] = alloca i8, [[INT]]

// CHECK: [[GENERIC_ARGS:%.*]] = getelementptr inbounds {{.*}} [[CONTEXT0]], i32 0, i32 {{(1|2)}}
// CHECK: store [[INT]] %1, ptr [[GENERIC_ARGS]]

// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[GENERIC_ARGS]], i32 1
// CHECK: store ptr %"each T", ptr [[T_ADDR]]

// CHECK:  call swiftcc void @"$s25variadic_generic_captures13takesNoEscapeyyyyXEF"(
// CHECK: ret void

// CHECK-LABEL: define internal swiftcc void @"$s25variadic_generic_captures26has_metadata_pack_noescape1tyxxQp_tRvzlFyyXEfU_TA"(ptr swiftself %0)
// CHECK: [[GENERIC_ARGS:%.*]] = getelementptr inbounds {{.*}} %0, i32 0, i32 {{(1|2)}}

// CHECK: [[SHAPE:%.*]] = load [[INT]], ptr [[GENERIC_ARGS]]

// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[GENERIC_ARGS]], i32 1
// CHECK: [[T:%.*]] = load ptr, ptr [[T_ADDR]]

// CHECK: tail call swiftcc void @"$s25variadic_generic_captures26has_metadata_pack_noescape1tyxxQp_tRvzlFyyXEfU_"([[INT]] [[SHAPE]], ptr [[T]])
// CHECK: ret void

public func has_witness_table_pack<each T: Sequence>(t: repeat each T) -> () -> () {
  return { _ = (repeat (each T).Element).self }
}

public func has_witness_table_pack2<each T: Sequence>(t: repeat each T) -> () -> ()
    where repeat each T.Element: Sequence {
  return { _ = (repeat (each T).Element.Element).self }
}
