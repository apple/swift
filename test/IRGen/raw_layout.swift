// RUN: %empty-directory(%t)
// RUN: %{python} %utils/chex.py < %s > %t/raw_layout.sil
// RUN: %target-swift-frontend -enable-experimental-feature RawLayout -emit-ir -disable-availability-checking -I %S/Inputs -cxx-interoperability-mode=upcoming-swift %t/raw_layout.sil | %FileCheck %t/raw_layout.sil --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize

import Builtin
import Swift
import RawLayoutCXX

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}4LockVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 4
// stride
// CHECK-SAME:  , {{i64|i32}} 4
// flags: alignment 3, noncopyable
// CHECK-SAME:  , <i32 0x800003>

@_rawLayout(size: 4, alignment: 4)
struct Lock: ~Copyable { }

struct PaddedStride {
    var x: Int32
    var y: Int8
}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}16LikePaddedStrideVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 5
// stride
// CHECK-SAME:  , {{i64|i32}} 8
// flags: alignment 3, noncopyable
// CHECK-SAME:  , <i32 0x800003>
@_rawLayout(like: PaddedStride)
struct LikePaddedStride: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}22LikePaddedStrideArray1VWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 8
// stride
// CHECK-SAME:  , {{i64|i32}} 8
// flags: alignment 3, noncopyable
// CHECK-SAME:  , <i32 0x800003>
@_rawLayout(likeArrayOf: PaddedStride, count: 1)
struct LikePaddedStrideArray1: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}22LikePaddedStrideArray2VWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 16
// stride
// CHECK-SAME:  , {{i64|i32}} 16
// flags: alignment 3, noncopyable, (on 32-bit platforms) not storable inline
// CHECK-64-SAME:  , <i32 0x800003>
// CHECK-32-SAME:  , <i32 0x820003>
@_rawLayout(likeArrayOf: PaddedStride, count: 2)
struct LikePaddedStrideArray2: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}9KeymasterVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 12
// stride
// CHECK-SAME:  , {{i64|i32}} 12
// flags: alignment 3, noncopyable
// CHECK-SAME:  , <i32 0x800003>
struct Keymaster: ~Copyable {
    let lock1: Lock
    let lock2: Lock
    let lock3: Lock
}

// Dependent Layouts:
// These types have their layouts all zeroed out and will be fixed up at
// runtime.

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}4CellVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 0
// stride
// CHECK-SAME:  , {{i64|i32}} 0
// flags: alignment 0, incomplete
// CHECK-SAME:  , <i32 0x400000>
@_rawLayout(like: T)
struct Cell<T>: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}10PaddedCellVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 0
// stride
// CHECK-SAME:  , {{i64|i32}} 0
// flags: alignment 0, incomplete
// CHECK-SAME:  , <i32 0x400000>
@_rawLayout(likeArrayOf: T, count: 1)
struct PaddedCell<T>: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}14SmallVectorBufVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 0
// stride
// CHECK-SAME:  , {{i64|i32}} 0
// flags: alignment 0, incomplete
// CHECK-SAME:  , <i32 0x400000>
@_rawLayout(likeArrayOf: T, count: 8)
struct SmallVectorBuf<T>: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}14SmallVectorOf3VWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 0
// stride
// CHECK-SAME:  , {{i64|i32}} 0
// flags: alignment 0, incomplete
// CHECK-SAME:  , <i32 0x400000>
@_rawLayout(likeArrayOf: T, count: 3)
struct SmallVectorOf3<T>: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}8UsesCellVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 8
// stride
// CHECK-SAME:  , {{i64|i32}} 8
// flags: alignment 3, noncopyable
// CHECK-SAME:  , <i32 0x800003>
struct UsesCell: ~Copyable {
    let someCondition: Bool
    let specialInt: Cell<Int32>
}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}13BufferOf3BoolVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 3
// stride
// CHECK-SAME:  , {{i64|i32}} 3
// flags: alignment 0, noncopyable
// CHECK-SAME:  , <i32 0x800000>
struct BufferOf3Bool: ~Copyable {
    let buffer: SmallVectorOf3<Bool>
}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}9BadBufferVWV" = {{.*}} %swift.vwtable
// size
// CHECK-SAME:  , {{i64|i32}} 48
// stride
// CHECK-SAME:  , {{i64|i32}} 48
// flags: alignment 7, noncopyable, is not inline
// CHECK-SAME:  , <i32 0x820007>
struct BadBuffer: ~Copyable {
    let buffer: SmallVectorOf3<Int64?>
}

// Raw Layout types that move like their like type

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}19CellThatMovesAsLikeVWV" = {{.*}} %swift.vwtable
// initializeWithTake
// CHECK-SAME:  , ptr @"$s10raw_layout19CellThatMovesAsLikeVwtk
// assignWithTake
// CHECK-SAME:  , ptr @"$s10raw_layout19CellThatMovesAsLikeVwta
// size
// CHECK-SAME:  , {{i64|i32}} 0
// stride
// CHECK-SAME:  , {{i64|i32}} 0
// flags: alignment 0, incomplete
// CHECK-SAME:  , <i32 0x400000>
@_rawLayout(like: T, movesAsLike)
struct CellThatMovesAsLike<T>: ~Copyable {}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}18ConcreteMoveAsLikeVWV" = {{.*}} %swift.vwtable
// initializeWithTake
// CHECK-SAME:  , ptr @"$s10raw_layout18ConcreteMoveAsLikeVwtk
// assignWithTake
// CHECK-SAME:  , ptr @"$s10raw_layout18ConcreteMoveAsLikeVwta
// size
// CHECK-SAME:  , {{i64|i32}} 1
// stride
// CHECK-SAME:  , {{i64|i32}} 1
// flags: not copyable, not bitwise takable, not pod, not inline
// CHECK-SAME:  , <i32 0x930000>
struct ConcreteMoveAsLike: ~Copyable {
  let cell: CellThatMovesAsLike<NonBitwiseTakableCXXType>
}

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}21ConcreteIntMoveAsLikeVWV" = {{.*}} %swift.vwtable
// initializeWithTake
// CHECK-SAME:  , ptr @__swift_memcpy4_4
// assignWithTake
// CHECK-SAME:  , ptr @__swift_memcpy4_4
// size
// CHECK-SAME:  , {{i64|i32}} 4
// stride
// CHECK-SAME:  , {{i64|i32}} 4
// flags: alignment 3, not copyable
// CHECK-SAME:  , <i32 0x800003>
struct ConcreteIntMoveAsLike: ~Copyable {
  let cell: CellThatMovesAsLike<Int32>
}

sil @use_lock : $@convention(thin) (@in_guaranteed Lock) -> () {
entry(%L: $*Lock):
    return undef : $()
}

sil @use_keymaster_locks : $@convention(thin) (@in_guaranteed Keymaster) -> () {
entry(%K: $*Keymaster):
    %f = function_ref @use_lock : $@convention(thin) (@in_guaranteed Lock) -> ()
    %a = struct_element_addr %K : $*Keymaster, #Keymaster.lock1
    apply %f(%a) : $@convention(thin) (@in_guaranteed Lock) -> ()
    %b = struct_element_addr %K : $*Keymaster, #Keymaster.lock2
    apply %f(%b) : $@convention(thin) (@in_guaranteed Lock) -> ()
    %c = struct_element_addr %K : $*Keymaster, #Keymaster.lock2
    apply %f(%c) : $@convention(thin) (@in_guaranteed Lock) -> ()
    return undef : $()
}

// CHECK: define {{.*}}swiftcc ptr @get_cell_addr(ptr %"Cell<T>", ptr {{.*}} swiftself [[SELF:%.*]])
// CHECK-NEXT:   entry:
// CHECK-NEXT:     ret ptr [[SELF]]
sil @get_cell_addr : $@convention(method) <T> (@in_guaranteed Cell<T>) -> UnsafeMutablePointer<T> {
entry(%0 : $*Cell<T>):
    %1 = builtin "addressOfRawLayout"(%0 : $*Cell<T>) : $Builtin.RawPointer
    %2 = struct $UnsafeMutablePointer<T> (%1 : $Builtin.RawPointer)
    return %2 : $UnsafeMutablePointer<T>
}

// Dependent layout metadata initialization:

// Cell<T>

// CHECK-LABEL: define {{.*}} swiftcc %swift.metadata_response @"$s{{[A-Za-z0-9_]*}}4CellVMr"(ptr %"Cell<T>", ptr {{.*}}, ptr {{.*}})
// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr %"Cell<T>", {{i64|i32}} 2
// CHECK-NEXT: [[T:%.*]] = load ptr, ptr [[T_ADDR]]
// CHECK-NEXT: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @swift_checkMetadataState({{i64|i32}} 319, ptr [[T]])
// CHECK-NEXT: [[T:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
// CHECK: [[T_VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[T]], {{i64|i32}} -1
// CHECK-NEXT: {{%.*}} = load ptr, ptr [[T_VWT_ADDR]]
// CHECK: [[T_LAYOUT:%.*]] = getelementptr inbounds ptr, ptr {{%.*}}, i32 8
// CHECK-NEXT: call void @swift_initRawStructMetadata(ptr %"Cell<T>", {{i64|i32}} 0, ptr [[T_LAYOUT]], {{i64|i32}} -1)

// PaddedCell<T>

// CHECK-LABEL: define {{.*}} swiftcc %swift.metadata_response @"$s{{[A-Za-z0-9_]*}}10PaddedCellVMr"(ptr %"PaddedCell<T>", ptr {{.*}}, ptr {{.*}})
// CHECK: call void @swift_initRawStructMetadata(ptr %"PaddedCell<T>", {{i64|i32}} 0, ptr {{%.*}}, {{i64|i32}} 1)

// SmallVectorBuf<T>

// CHECK-LABEL: define {{.*}} swiftcc %swift.metadata_response @"$s{{[A-Za-z0-9_]*}}14SmallVectorBufVMr"(ptr %"SmallVectorBuf<T>", ptr {{.*}}, ptr {{.*}})
// CHECK: call void @swift_initRawStructMetadata(ptr %"SmallVectorBuf<T>", {{i64|i32}} 0, ptr {{%.*}}, {{i64|i32}} 8)

// Ensure that 'movesAsLike' is correctly calling the underlying type's move constructor

// CellThatMovesAsLike<T> initializeWithTake

// CHECK-LABEL: define {{.*}} ptr @"$s10raw_layout19CellThatMovesAsLikeVwtk"(ptr {{.*}} %dest, ptr {{.*}} %src, ptr %"CellThatMovesAsLike<T>")
// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr %"CellThatMovesAsLike<T>", {{i64|i32}} 2
// CHECK-NEXT: [[T:%.*]] = load ptr, ptr [[T_ADDR]]
// CHECK: {{%.*}} = call ptr %InitializeWithTake(ptr {{.*}} %dest, ptr {{.*}} %src, ptr [[T]])

// CellThatMovesAsLike<T> assignWithTake

// CHECK-LABEL: define {{.*}} ptr @"$s10raw_layout19CellThatMovesAsLikeVwta"(ptr {{.*}} %dest, ptr {{.*}} %src, ptr %"CellThatMovesAsLike<T>")
// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr %"CellThatMovesAsLike<T>", {{i64|i32}} 2
// CHECK-NEXT: [[T:%.*]] = load ptr, ptr [[T_ADDR]]
// CHECK: {{%.*}} = call ptr %AssignWithTake(ptr {{.*}} %dest, ptr {{.*}} %src, ptr [[T]])

// ConcreteMoveAsLike initializeWithTake

// CHECK-LABEL: define {{.*}} ptr @"$s10raw_layout18ConcreteMoveAsLikeVwtk"(ptr {{.*}} %dest, ptr {{.*}} %src, ptr %ConcreteMoveAsLike)
// CHECK: [[DEST_CELL:%.*]] = getelementptr inbounds %T10raw_layout18ConcreteMoveAsLikeV, ptr %dest, i32 0, i32 0
// CHECK: [[SRC_CELL:%.*]] = getelementptr inbounds %T10raw_layout18ConcreteMoveAsLikeV, ptr %src, i32 0, i32 0
// CHECK: {{invoke void|invoke ptr|call ptr}} @{{.*}}(ptr [[DEST_CELL]], ptr [[SRC_CELL]])

// ConcreteMoveAsLike assignWithTake

// CHECK-LABEL: define {{.*}} ptr @"$s10raw_layout18ConcreteMoveAsLikeVwta"(ptr {{.*}} %dest, ptr {{.*}} %src, ptr %ConcreteMoveAsLike)
// CHECK: [[DEST_CELL:%.*]] = getelementptr inbounds %T10raw_layout18ConcreteMoveAsLikeV, ptr %dest, i32 0, i32 0
// CHECK: [[SRC_CELL:%.*]] = getelementptr inbounds %T10raw_layout18ConcreteMoveAsLikeV, ptr %src, i32 0, i32 0
// CHECK: {{invoke void|invoke ptr|call ptr}} @{{.*}}(ptr [[DEST_CELL]], ptr [[SRC_CELL]])
