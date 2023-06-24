// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize

public func takesMetatype<T>(_: T.Type) {}

public func makeTuple<each T>(_ t: repeat each T) {
  takesMetatype((repeat each T).self)
}

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc void @"$s24variadic_vanishing_tuple9makeTupleyyxxQpRvzlF"(ptr noalias nocapture %0, {{i32|i64}} %1, ptr %"each T")
// CHECK:   [[CMP:%.*]] = icmp eq [[INT]] %1, 1
// CHECK:   br i1 [[CMP]], label %vanishing-tuple, label %actual-tuple

// CHECK: vanishing-tuple:
// CHECK:   [[PACK_ADDR:%.*]] = ptrtoint ptr %"each T" to [[INT]]
// CHECK:   [[PACK_ADDR_MASKED:%.*]] = and [[INT]] [[PACK_ADDR]], -2
// CHECK:   [[PACK_PTR:%.*]] = inttoptr [[INT]] [[PACK_ADDR_MASKED]] to ptr
// CHECK:   [[ELT_PTR:%.*]] = getelementptr inbounds ptr, ptr [[PACK_PTR]], [[INT]] 0
// CHECK:   [[ELT:%.*]] = load ptr, ptr [[ELT_PTR]]
// CHECK:   [[RESULT:%.*]] = insertvalue %swift.metadata_response undef, ptr [[ELT]], 0
// CHECK:   [[RESULT2:%.*]] = insertvalue %swift.metadata_response [[RESULT]], [[INT]] 0, 1
// CHECK:   br label %tuple-rest

// CHECK: actual-tuple:
// CHECK:   [[PACK:%.*]] = alloca ptr, [[INT]] %1
// CHECK:   br label %pack-expansion-check

// CHECK: pack-expansion-check:
// CHECK:   br i1 {{%.*}}, label %pack-expansion-loop, label %pack-expansion-rest

// CHECK: pack-expansion-loop:
// CHECK:   br label %pack-expansion-check

// CHECK: pack-expansion-rest:
// CHECK:   [[TUPLE:%.*]] = call swiftcc %swift.metadata_response @swift_getTupleTypeMetadata([[INT]] 0, [[INT]] %1, ptr [[PACK:%.*]], ptr null, ptr null)
// CHECK:   br label %tuple-rest

// CHECK: tuple-rest:
// CHECK:   [[PHI:%.*]] = phi %swift.metadata_response [ [[RESULT2]], %vanishing-tuple ], [ [[TUPLE]], %pack-expansion-rest ]
// CHECK:   [[METADATA:%.*]] = extractvalue %swift.metadata_response [[PHI]], 0
// CHECK:   call swiftcc void @"$s24variadic_vanishing_tuple13takesMetatypeyyxmlF"(ptr [[METADATA]], ptr [[METADATA]])
// CHECK:   ret void

public func makeTuple2<each T, each U, each V: Hashable>(t: repeat each T, u: repeat each U, v: repeat each V) {
  takesMetatype((repeat each T, repeat Array<each U>, repeat Set<each V>).self)
}
