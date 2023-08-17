// RUN: %target-swift-frontend -target %target-cpu-apple-macosx13.0 -enable-experimental-feature RawLayout -emit-ir %s | %FileCheck %s

// REQUIRES: OS=macosx

// Test that when targeting older OSes 

@_rawLayout(like: T)
struct Cell<T>: ~Copyable {}

@_rawLayout(likeArrayOf: T, count: 1)
struct PaddedCell<T>: ~Copyable {}

@_rawLayout(likeArrayOf: T, count: 8)
struct SmallVectorBuf<T>: ~Copyable {}

// Dependent layout metadata initialization:

// Cell<T>

// CHECK-LABEL: define {{.*}} swiftcc %swift.metadata_response @"$s{{[A-Za-z0-9_]*}}4CellVMr"(ptr %"Cell<T>", ptr {{.*}}, ptr {{.*}})
// CHECK: [[FIELD_LAYOUTS:%.*]] = alloca [1 x ptr]
// CHECK-NEXT: [[FIELD_OFFSETS:%.*]] = alloca i32, align 4
// CHECK-NEXT: [[OUR_LAYOUT:%.*]] = alloca %swift.type_layout
// CHECK: store ptr [[OUR_LAYOUT]], ptr [[FIELD_LAYOUTS]]
// CHECK-NEXT: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr %"Cell<T>", {{i64|i32}} 2
// CHECK-NEXT: [[T:%.*]] = load ptr, ptr [[T_ADDR]]
// CHECK-NEXT: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @swift_checkMetadataState({{i64|i32}} 319, ptr [[T]])
// CHECK-NEXT: [[T:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
// CHECK: [[T_VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[T]], {{i64|i32}} -1
// CHECK-NEXT: [[T_VWT:%.*]] = load ptr, ptr [[T_VWT_ADDR]]
// CHECK-NEXT: [[T_LAYOUT:%.*]] = getelementptr inbounds ptr, ptr [[T_VWT]], i32 8
// CHECK-NEXT: [[T_LAYOUT_LOADED:%.*]] = load %swift.type_layout, ptr [[T_LAYOUT]]
// CHECK-NEXT: [[T_SIZE:%.*]] = extractvalue %swift.type_layout [[T_LAYOUT_LOADED]], 0
// CHECK-NEXT: [[T_STRIDE:%.*]] = extractvalue %swift.type_layout [[T_LAYOUT_LOADED]], 1
// CHECK-NEXT: [[T_FLAGS:%.*]] = extractvalue %swift.type_layout [[T_LAYOUT_LOADED]], 2
// CHECK-NEXT: [[T_XI:%.*]] = extractvalue %swift.type_layout [[T_LAYOUT_LOADED]], 3
// CHECK-NEXT: [[T_ALIGNMASK:%.*]] = and i32 [[T_FLAGS]], 255
// CHECK-NEXT: [[OUR_FLAGS:%.*]] = or i32 [[T_ALIGNMASK]], 65536
// CHECK-NEXT: [[OUR_LAYOUT_0:%.*]] = insertvalue %swift.type_layout undef, {{i64|i32}} [[T_SIZE]], 0
// CHECK-NEXT: [[OUR_LAYOUT_1:%.*]] = insertvalue %swift.type_layout [[OUR_LAYOUT_0]], {{i64|i32}} [[T_STRIDE]], 1
// CHECK-NEXT: [[OUR_LAYOUT_2:%.*]] = insertvalue %swift.type_layout [[OUR_LAYOUT_1]], i32 [[OUR_FLAGS]], 2
// CHECK-NEXT: [[OUR_LAYOUT_3:%.*]] = insertvalue %swift.type_layout [[OUR_LAYOUT_2]], i32 [[T_XI]], 3
// CHECK-NEXT: store %swift.type_layout [[OUR_LAYOUT_3]], ptr [[OUR_LAYOUT]]
// CHECK-NEXT: call void @swift_initStructMetadata(ptr %"Cell<T>", {{i64|i32}} 0, {{i64|i32}} 1, ptr [[FIELD_LAYOUTS]], ptr [[FIELD_OFFSETS]])

// PaddedCell<T>

// CHECK-LABEL: define {{.*}} swiftcc %swift.metadata_response @"$s{{[A-Za-z0-9_]*}}10PaddedCellVMr"(ptr %"PaddedCell<T>", ptr {{.*}}, ptr {{.*}})
// CHECK: [[T_STRIDE:%.*]] = extractvalue %swift.type_layout {{%.*}}, 1
// CHECK: [[OUR_STRIDE:%.*]] = mul {{i64|i32}} [[T_STRIDE]], 1
// CHECK-NEXT: {{%.*}} = insertvalue %swift.type_layout undef, {{i64|i32}} [[OUR_STRIDE]], 0
// CHECK-NEXT: {{%.*}} = insertvalue %swift.type_layout {{%.*}}, {{i64|i32}} [[OUR_STRIDE]], 1
// CHECK: call void @swift_initStructMetadata(ptr %"PaddedCell<T>", {{i64|i32}} 0, {{i64|i32}} 1, ptr {{%.*}}, ptr {{%.*}})

// SmallVectorBuf<T>

// CHECK-LABEL: define {{.*}} swiftcc %swift.metadata_response @"$s{{[A-Za-z0-9_]*}}14SmallVectorBufVMr"(ptr %"SmallVectorBuf<T>", ptr {{.*}}, ptr {{.*}})
// CHECK: [[T_STRIDE:%.*]] = extractvalue %swift.type_layout {{%.*}}, 1
// CHECK: [[OUR_STRIDE:%.*]] = mul {{i64|i32}} [[T_STRIDE]], 8
// CHECK-NEXT: {{%.*}} = insertvalue %swift.type_layout undef, {{i64|i32}} [[OUR_STRIDE]], 0
// CHECK-NEXT: {{%.*}} = insertvalue %swift.type_layout {{%.*}}, {{i64|i32}} [[OUR_STRIDE]], 1
// CHECK: call void @swift_initStructMetadata(ptr %"SmallVectorBuf<T>", {{i64|i32}} 0, {{i64|i32}} 1, ptr {{%.*}}, ptr {{%.*}})
