// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -module-name distributed_actor_accessors -emit-irgen -disable-availability-checking -I %t 2>&1 %s | %IRGenFileCheck %s -check-prefix CHECK-%target-import-type

// UNSUPPORTED: back_deploy_concurrency
// REQUIRES: concurrency
// REQUIRES: distributed

// REQUIRES: CPU=x86_64 || CPU=arm64

// UNSUPPORTED: OS=windows-msvc

import Distributed
import FakeDistributedActorSystems

@available(SwiftStdlib 5.5, *)
typealias DefaultDistributedActorSystem = FakeActorSystem

enum SimpleE : Codable {
case a
}

enum E : Codable {
case a, b, c
}

enum IndirectE : Codable {
  case empty
  indirect case test(_: Int)
}

final class Obj : Codable, Sendable {
  let x: Int

  init(x: Int) {
    self.x = x
  }
}

struct LargeStruct : Codable {
  var a: Int
  var b: Int
  var c: String
  var d: Double
}

@available(SwiftStdlib 5.7, *)
public distributed actor MyActor {
  distributed func simple1(_: Int) {
  }

  // `String` would be a direct result as a struct type
  distributed func simple2(_: Int) -> String {
    return ""
  }

  // `String` is an object that gets exploded into two parameters
  distributed func simple3(_: String) -> Int {
    return 42
  }

  // Enum with a single case are special because they have an empty
  // native schema so they are dropped from parameters/result.
  distributed func single_case_enum(_ e: SimpleE) -> SimpleE {
    return e
  }

  distributed func with_indirect_enums(_: IndirectE, _: Int) -> IndirectE {
    return .empty
  }

  // Combination of multiple arguments, reference type and indirect result
  //
  // Note: Tuple types cannot be used here is either position because they
  // cannot conform to protocols.
  distributed func complex(_: [Int], _: Obj, _: String?, _: LargeStruct) -> LargeStruct {
    fatalError()
  }

  // Combination of direct and indirect arguments involving generic arguments.
  distributed func genericArgs<T: Codable, U: Codable>(_: T, _: [U]) {
  }
}

@available(SwiftStdlib 5.7, *)
public distributed actor MyOtherActor {
  distributed func empty() {
  }
}

/// ---> Thunk and distributed method accessor for `simple1`

/// Let's make sure that accessor loads the data from the buffer and calls expected accessor

// CHECK: define hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7simple1yySiYaKFTE"

// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7simple1yySiYaKFTETF"(ptr swiftasync %0, ptr nocapture %1, ptr %2, ptr %3, {{.*}}, ptr [[ACTOR:%.*]], ptr [[DECODER_TYPE:%.*]], ptr [[DECODER_PROTOCOL_WITNESS:%.*]])

/// Read the current offset and cast an element to `Int`

// CHECK: [[DECODER:%.*]] = load ptr, ptr %1
// CHECK-NEXT: [[DECODE_NEXT_ARG_REF:%.*]] = getelementptr inbounds ptr, ptr %2, i64

// CHECK: [[ARG_0_SIZE_ADJ:%.*]] = add i64 %size, 15
// CHECK-NEXT: [[ARG_0_SIZE:%.*]] = and i64 [[ARG_0_SIZE_ADJ]], -16
// CHECK-NEXT: [[ARG_0_VALUE_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_0_SIZE]])
// CHECK-NEXT: [[ENCODABLE_WITNESS:%.*]] = call ptr @swift_conformsToProtocol(ptr %arg_type, ptr @"$sSeMp")
// CHECK-NEXT: [[IS_NULL:%.*]] = icmp eq ptr [[ENCODABLE_WITNESS]], null
// CHECK-NEXT: br i1 [[IS_NULL]], label %missing-witness, label [[CONT:%.*]]
// CHECK: missing-witness:
// CHECK-NEXT: call void @llvm.trap()
// CHECK-NEXT: unreachable
// CHECK: [[DECODABLE_WITNESS:%.*]] = call ptr @swift_conformsToProtocol(ptr %arg_type, ptr @"$sSEMp")
// CHECK-NEXT: [[IS_NULL:%.*]] = icmp eq ptr [[DECODABLE_WITNESS]], null
// CHECK-NEXT: br i1 [[IS_NULL]], label %missing-witness1, label [[CONT:%.*]]
// CHECK: missing-witness1:
// CHECK-NEXT: call void @llvm.trap()
// CHECK-NEXT: unreachable
// CHECK: call swiftcc void @"$s27FakeDistributedActorSystems0A17InvocationDecoderC18decodeNextArgumentxyKSeRzSERzlF"(ptr noalias nocapture sret(%swift.opaque) [[ARG_0_VALUE_BUF]], ptr %arg_type, ptr [[ENCODABLE_WITNESS]], ptr [[DECODABLE_WITNESS]], ptr swiftself [[DECODER]], ptr noalias nocapture swifterror dereferenceable(8) %swifterror)

// CHECK: store ptr null, ptr %swifterror
// CHECK-NEXT: %._value = getelementptr inbounds %TSi, ptr [[ARG_0_VALUE_BUF]], i32 0, i32 0
// CHECK-NEXT: [[ARG_VAL:%.*]] = load i64, ptr %._value

/// Setup task context for async call to `simple1` thunk

// CHECK-DIRECT: [[CONTEXT_SIZE:%.*]] = load i32, ptr getelementptr inbounds (%swift.async_func_pointer, ptr @"$s27distributed_actor_accessors7MyActorC7simple1yySiYaKFTETu", i32 0, i32 1)
// CHECK-INDIRECT: [[ADDR:%[0-9]+]] = load ptr, ptr inttoptr (i64 and (i64 ptrtoint (ptr @"$s27distributed_actor_accessors7MyActorC7simple1yySiYaKFTETu" to i64), i64 -2) to ptr)
// CHECK-INDIRECT-NEXT: [[SELECT:%[0-9]+]] = select i1 true, ptr @"$s27distributed_actor_accessors7MyActorC7simple1yySiYaKFTETu", ptr [[ADDR]]
// CHECK-INDIRECT-NEXT: [[LOAD:%[0-9]+]] = getelementptr inbounds %swift.async_func_pointer, ptr [[SELECT]], i32 0, i32 1
// CHECK-INDIRECT-NEXT: [[CONTEXT_SIZE:%.*]] = load i32, ptr [[LOAD]]
// CHECK-NEXT: [[CONTEXT_SIZE_64:%.*]] = zext i32 [[CONTEXT_SIZE]] to i64
// CHECK-NEXT: [[THUNK_ASYNC_CONTEXT:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[CONTEXT_SIZE_64]])

/// Call distributed thunk for `simple1` and `end` async context without results

// CHECK: [[THUNK_RESULT:%.*]] = call { ptr, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0p0s(
// CHECK-SAME: ptr @"$s27distributed_actor_accessors7MyActorC7simple1yySiYaKFTE",
// CHECK-SAME: ptr [[THUNK_ASYNC_CONTEXT]],
// CHECK-SAME: i64 [[ARG_VAL]],
// CHECK-SAME: ptr [[ACTOR]])

// CHECK-NEXT: [[TASK_REF:%.*]] = extractvalue { ptr, ptr } [[THUNK_RESULT]], 0
// CHECK-NEXT: {{.*}} = call ptr @__swift_async_resume_project_context(ptr [[TASK_REF]])
// CHECK: {{.*}} = call i1 (ptr, i1, ...) @llvm.coro.end.async({{.*}}, ptr {{.*}}, ptr {{.*}})

/// ---> Thunk and distributed method accessor for `simple2`

// CHECK: define hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7simple2ySSSiYaKFTE"

// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7simple2ySSSiYaKFTETF"

/// !!! - We are not going to double-check argument extraction here since it's the same as `simple1`.
// CHECK: [[NATIVE_ARG_VAL:%.*]] = load i64, ptr %._value

/// Setup task context for async call to `simple2` thunk

// CHECK-DIRECT: [[CONTEXT_SIZE:%.*]] = load i32, ptr getelementptr inbounds (%swift.async_func_pointer, ptr @"$s27distributed_actor_accessors7MyActorC7simple2ySSSiYaKFTETu", i32 0, i32 1)
// CHECK-INDIRECT: [[ADDR:%[0-9]+]] = load ptr, ptr inttoptr (i64 and (i64 ptrtoint (ptr @"$s27distributed_actor_accessors7MyActorC7simple2ySSSiYaKFTETu" to i64), i64 -2) to ptr), align 8
// CHECK-INDIRECT-NEXT: [[SELECT:%[0-9]+]] = select i1 true, ptr @"$s27distributed_actor_accessors7MyActorC7simple2ySSSiYaKFTETu", ptr [[ADDR]]
// CHECK-INDIRECT-NEXT: [[LOAD:%[0-9]+]] = getelementptr inbounds %swift.async_func_pointer, ptr [[SELECT]], i32 0, i32 1
// CHECK-INDIRECT-NEXT: [[CONTEXT_SIZE:%.*]] = load i32, ptr [[LOAD]]
// CHECK-NEXT: [[CONTEXT_SIZE_64:%.*]] = zext i32 [[CONTEXT_SIZE]] to i64
// CHECK-NEXT: [[THUNK_ASYNC_CONTEXT:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[CONTEXT_SIZE_64]])

/// Call the thunk with extracted argument value

// CHECK: [[THUNK_RESULT:%.*]] = call { ptr, i64, ptr, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0i64p0p0s(
// CHECK-SAME: ptr @"$s27distributed_actor_accessors7MyActorC7simple2ySSSiYaKFTE",
// CHECK-SAME: ptr [[THUNK_ASYNC_CONTEXT]],
// CHECK-SAME: i64 [[NATIVE_ARG_VAL]],
// CHECK-SAME: ptr [[ACTOR]])

// CHECK-NEXT: [[TASK_REF:%.*]] = extractvalue { ptr, i64, ptr, ptr } [[THUNK_RESULT]], 0
// CHECK-NEXT: {{.*}} = call ptr @__swift_async_resume_project_context(ptr [[TASK_REF]])

/// Extract information about `String` from result and call `end`

// CHECK: [[STR_STRUCT:%.*]] = insertvalue { i64, ptr } {{.*}}, ptr {{.*}}, 1
// CHECK: [[STR_SIZE:%.*]] = extractvalue { i64, ptr } [[STR_STRUCT]], 0
// CHECK-NEXT: [[STR_VAL:%.*]] = extractvalue { i64, ptr } [[STR_STRUCT]], 1

/// Initialize the result buffer with values produced by the thunk

// CHECK: store i64 [[STR_SIZE]], ptr %._guts._object._countAndFlagsBits._value
// CHECK: store ptr [[STR_VAL]], ptr %._guts._object._object

// CHECK: {{.*}} = call i1 (ptr, i1, ...) @llvm.coro.end.async({{.*}}, ptr {{.*}}, ptr {{.*}})

/// ---> Thunk and distributed method accessor for `simple3`

// CHECK: define hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7simple3ySiSSYaKFTE"

/// !!! in `simple3` interesting bits are: argument value extraction (because string is exploded into N arguments) and call to distributed thunk
// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7simple3ySiSSYaKFTETF"(ptr swiftasync %0, ptr nocapture [[ARG_DECODER:%.*]], ptr [[ARG_TYPES:%.*]], ptr [[RESULT_BUFF:%.*]], ptr [[SUBS:%.*]], ptr [[WITNESS_TABLES:%.*]], i64 [[NUM_WITNESS_TABLES:%.*]], ptr [[ACTOR]], ptr [[DECODER_TYPE:%.*]], ptr [[DECODER_PROTOCOL_WITNESS:%.*]])


// CHECK: [[ARG_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK: [[ARG_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_SIZE]])
// CHECK: %._guts = getelementptr inbounds %TSS, ptr [[ARG_BUF]], i32 0, i32 0

// CHECK: [[STR_SIZE:%.*]] = load i64, ptr %._guts._object._countAndFlagsBits._value
// CHECK: [[STR_VAL:%.*]] = load ptr, ptr %._guts._object._object

/// Setup task context for async call to `simple3` thunk

// CHECK-INDIRECT: [[ADDR:%[0-9]+]] = load ptr, ptr inttoptr (i64 and (i64 ptrtoint (ptr @"$s27distributed_actor_accessors7MyActorC7simple3ySiSSYaKFTETu" to i64), i64 -2) to ptr), align 8
// CHECK-INDIRECT-NEXT: [[SELECT:%[0-9]+]] = select i1 true, ptr @"$s27distributed_actor_accessors7MyActorC7simple3ySiSSYaKFTETu", ptr [[ADDR]]
// CHECK-INDIRECT-NEXT: [[LOAD:%[0-9]+]] = getelementptr inbounds %swift.async_func_pointer, ptr [[SELECT]], i32 0, i32 1
// CHECK-INDIRECT-NEXT: [[CONTEXT_SIZE:%.*]] = load i32, ptr [[LOAD]]
// CHECK-DIRECT: [[CONTEXT_SIZE:%.*]] = load i32, ptr getelementptr inbounds (%swift.async_func_pointer, ptr @"$s27distributed_actor_accessors7MyActorC7simple3ySiSSYaKFTETu", i32 0, i32 1)
// CHECK-NEXT: [[CONTEXT_SIZE_64:%.*]] = zext i32 [[CONTEXT_SIZE]] to i64
// CHECK-NEXT: [[THUNK_ASYNC_CONTEXT:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[CONTEXT_SIZE_64]])

/// Call distributed thunk with exploded string value

// CHECK: [[THUNK_RESULT:%.*]] = call { ptr, i64, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0i64p0s(
// CHECK-SAME: ptr @"$s27distributed_actor_accessors7MyActorC7simple3ySiSSYaKFTE",
// CHECK-SAME: ptr [[THUNK_ASYNC_CONTEXT]],
// CHECK-SAME: i64 [[STR_SIZE]],
// CHECK-SAME: ptr [[STR_VAL]],
// CHECK-SAME: ptr [[ACTOR]])

// CHECK-NEXT: [[TASK_REF:%.*]] = extractvalue { ptr, i64, ptr } [[THUNK_RESULT]], 0
// CHECK-NEXT: {{.*}} = call ptr @__swift_async_resume_project_context(ptr [[TASK_REF]])
// CHECK: [[INT_RES:%.*]] = extractvalue { ptr, i64, ptr } [[THUNK_RESULT]], 1
// CHECK: %._value = getelementptr inbounds %TSi, ptr [[RESULT_BUFF]], i32 0, i32 0
// CHECK: store i64 [[INT_RES]], ptr %._value
// CHECK: {{.*}} = call i1 (ptr, i1, ...) @llvm.coro.end.async({{.*}}, ptr {{.*}}, ptr {{.*}})

/// --> Thunk and distributed method accessor for `single_case_enum`

// CHECK: define hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC16single_case_enumyAA7SimpleEOAFYaKFTE"

// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC16single_case_enumyAA7SimpleEOAFYaKFTETF"(ptr swiftasync %0, ptr nocapture [[ARG_DECODER:%.*]], ptr [[ARG_TYPES:%.*]], ptr [[RESULT_BUFF:%.*]], ptr [[SUBS:%.*]], ptr [[WITNESS_TABLES:%.*]], i64 [[NUM_WITNESS_TABLES:%.*]], ptr [[ACTOR]], ptr [[DECODER_TYPE]], ptr [[DECODER_PROTOCOL_WITNESS:%.*]])

/// Let's check that the call doesn't have any arguments and returns nothing.

// SKIP: {{.*}} = call { ptr, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0p0s({{.*}}, ptr {{.*}}, ptr {{.*}}, ptr {{.*}})
// SKIP: {{.*}} = call i1 (ptr, i1, ...) @llvm.coro.end.async({{.*}}, ptr {{.*}}, ptr {{.*}}, ptr {{.*}})

/// --> Thunk and distributed method accessor for `with_indirect_enums`

// CHECK: define hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC19with_indirect_enumsyAA9IndirectEOAF_SitYaKFTE"
// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC19with_indirect_enumsyAA9IndirectEOAF_SitYaKFTETF"

/// First, Load both arguments from the buffer.


// CHECK: [[ARG_0_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_0_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_0_SIZE]])

// CHECK: [[NATIVE_ENUM_VAL:%.*]] = load i64, ptr [[ARG_0_BUF]]

// CHECK: [[ARG_1_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_1_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_1_SIZE]])

// CHECK: %._value = getelementptr inbounds %TSi, ptr [[ARG_1_BUF]], i32 0, i32 0
// CHECK-NEXT: [[NATIVE_INT_VAL:%.*]] = load i64, ptr %._value

/// Call distributed thunk with extracted arguments.

// CHECK: [[THUNK_RESULT:%.*]] = call { ptr, i64, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0i64p0s({{.*}}, ptr {{.*}}, i64 [[NATIVE_ENUM_VAL]], i64 [[NATIVE_INT_VAL]], ptr {{.*}})
// CHECK-NEXT: [[TASK_REF:%.*]] = extractvalue { ptr, i64, ptr } [[THUNK_RESULT]], 0
// CHECK-NEXT: {{.*}} = call ptr @__swift_async_resume_project_context(ptr [[TASK_REF]])
// CHECK: [[ENUM_RESULT:%.*]] = extractvalue { ptr, i64, ptr } [[THUNK_RESULT]], 1
// CHECK: store i64 [[ENUM_RESULT]], ptr [[RESULT_BUFF]]

// CHECK: {{.*}} = call i1 (ptr, i1, ...) @llvm.coro.end.async({{.*}}, ptr {{.*}}, ptr {{.*}})

/// ---> Thunk and distributed method for `complex`

// CHECK: define hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7complexyAA11LargeStructVSaySiG_AA3ObjCSSSgAFtYaKFTE"

// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC7complexyAA11LargeStructVSaySiG_AA3ObjCSSSgAFtYaKFTETF"(ptr swiftasync {{.*}}, ptr nocapture [[ARG_DECODER:%.*]], ptr [[ARG_TYPES:%.*]], ptr [[RESULT_BUFF:%.*]], ptr [[SUBS:%.*]], ptr [[WITNESS_TABLES:%.*]], i64 [[NUM_WITNESS_TABLES:%.*]], ptr [[ACTOR]], ptr [[DECODER_TYPE:%.*]], ptr [[DECODER_PROTOCOL_WITNESS:%.*]])

/// First, let's check that all of the different argument types here are loaded correctly.

/// Cast result buffer to the expected result type (in this case its indirect opaque pointer)

/// -> [Int]

// CHECK: [[ARG_0_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_0_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_0_SIZE]])

// CHECK: %._buffer = getelementptr inbounds %TSa, ptr [[ARG_0_BUF]], i32 0, i32 0
// CHECK: [[NATIVE_ARR_VAL:%.*]] = load ptr, ptr %._buffer._storage

/// -> Obj

// CHECK: [[ARG_1_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_1_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_1_SIZE]])

// CHECK: [[NATIVE_OBJ_VAL:%.*]] = load ptr, ptr [[ARG_1_BUF]]

/// -> String?

// CHECK: [[ARG_2_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_2_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_2_SIZE]])

// CHECK: [[NATIVE_OPT_VAL_0_PTR:%.*]] = getelementptr inbounds { i64, i64 }, ptr [[ARG_2_BUF]], i32 0, i32 0
// CHECK-NEXT: [[NATIVE_OPT_VAL_0:%.*]] = load i64, ptr [[NATIVE_OPT_VAL_0_PTR]]
// CHECK-NEXT: [[NATIVE_OPT_VAL_1_PTR:%.*]] = getelementptr inbounds { i64, i64 }, ptr [[ARG_2_BUF]], i32 0, i32 1
// CHECK-NEXT: [[NATIVE_OPT_VAL_1:%.*]] = load i64, ptr [[NATIVE_OPT_VAL_1_PTR]]

/// -> LargeStruct (passed indirectly)

// CHECK: [[ARG_3_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_3_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_3_SIZE]])


/// Now let's make sure that distributed thunk call uses the arguments correctly

// CHECK: [[THUNK_RESULT:%.*]] = call { ptr, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0p0s({{.*}}, ptr [[RESULT_BUFF]], ptr {{.*}}, {{.*}} [[NATIVE_ARR_VAL]], ptr [[NATIVE_OBJ_VAL]], i64 [[NATIVE_OPT_VAL_0]], i64 [[NATIVE_OPT_VAL_1]], ptr [[ARG_3_BUF]], ptr {{.*}})

/// RESULT is returned indirectly so there is nothing to pass to `end`

// CHECK: {{.*}} = call i1 (ptr, i1, ...) @llvm.coro.end.async({{.*}}, ptr {{.*}}, ptr {{.*}})

/// ---> Accessor for `genericArgs`

// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors7MyActorC11genericArgsyyx_Sayq_GtYaKSeRzSERzSeR_SER_r0_lFTETF"(ptr swiftasync %0, ptr  nocapture [[ARG_DECODER:%.*]], ptr [[ARG_TYPES:%.*]], ptr [[RESULT_BUF:%.*]], ptr [[GENERIC_SUBS:%.*]], ptr [[WITNESS_TABLES:%.*]], i64 [[NUM_WITNESS_TABLES:%.*]], ptr [[ACTOR:%.*]], ptr [[DECODER_TYPE:%.*]], ptr [[DECODER_PROTOCOL_WITNESS:%.*]])

/// ---> Load `T`

// CHECK: [[ARG_0_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_0_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_0_SIZE]])

/// ---> Load `[U]`

// CHECK: [[ARG_1_SIZE:%.*]] = and i64 {{.*}}, -16
// CHECK-NEXT: [[ARG_1_BUF:%.*]] = call swiftcc ptr @swift_task_alloc(i64 [[ARG_1_SIZE]])

// CHECK: %._buffer = getelementptr inbounds %TSa, ptr [[ARG_1_BUF]], i32 0, i32 0
// CHECK-NEXT: %._buffer._storage = getelementptr inbounds [[ARRAY_TYPE:%.*]], ptr %._buffer, i32 0, i32 0
// CHECK: [[TYPED_ARG_1:%.*]] = load ptr, ptr %._buffer._storage

/// ---> Load generic argument substitutions from the caller-provided buffer

// CHECK: [[SUB_T_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[GENERIC_SUBS]], i64 0
// CHECK-NEXT: [[SUB_T:%.*]] = load ptr, ptr [[SUB_T_ADDR]]
// CHECK-NEXT: [[SUB_U_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[GENERIC_SUBS]], i64 1
// CHECK-NEXT: [[SUB_U:%.*]] = load ptr, ptr [[SUB_U_ADDR]]

/// --> Load witness tables from caller-provided buffer

/// First, check whether the number of witness tables matches expected

// CHECK: [[IS_INCORRECT_WITNESSES:%.*]] = icmp ne i64 [[NUM_WITNESS_TABLES]], 4
// CHECK-NEXT: br i1 [[IS_INCORRECT_WITNESSES]], label %incorrect-witness-tables, label [[LOAD_WITNESS_TABLES:%.*]]
// CHECK: incorrect-witness-tables:
// CHECK-NEXT: unreachable

// CHECK: [[T_ENCODABLE_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[WITNESS_TABLES]], i64 0
// CHECK-NEXT: [[T_ENCODABLE:%.*]] = load ptr, ptr [[T_ENCODABLE_ADDR]]
// CHECK-NEXT: [[T_DECODABLE_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[WITNESS_TABLES]], i64 1
// CHECK-NEXT: [[T_DECODABLE:%.*]] = load ptr, ptr [[T_DECODABLE_ADDR]]
// CHECK-NEXT: [[U_ENCODABLE_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[WITNESS_TABLES]], i64 2
// CHECK-NEXT: [[U_ENCODABLE:%.*]] = load ptr, ptr [[U_ENCODABLE_ADDR]]
// CHECK-NEXT: [[U_DECODABLE_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[WITNESS_TABLES]], i64 3
// CHECK-NEXT: [[U_DECODABLE:%.*]] = load ptr, ptr [[U_DECODABLE_ADDR]]

/// ---> Check that distributed thunk code is formed correctly

// CHECK-MACHO: [[THUNK_RESULT:%.*]] = call { ptr, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0p0s({{.*}}, ptr {{.*}}, ptr [[ARG_0_BUF]], ptr [[TYPED_ARG_1]], ptr [[SUB_T]], ptr [[SUB_U]], ptr [[T_ENCODABLE]], ptr [[T_DECODABLE]], ptr [[U_ENCODABLE]], ptr [[U_DECODABLE]], ptr [[ACTOR]])

/// ---> Thunk and distributed method for `MyOtherActor.empty`

/// Let's check that there is argument decoding since parameter list is empty

// CHECK: define linkonce_odr hidden swift{{(tail)?}}cc void @"$s27distributed_actor_accessors12MyOtherActorC5emptyyyYaKFTETF"(ptr swiftasync {{.*}}, ptr nocapture [[ARG_DECODER:%.*]], ptr [[ARG_TYPES:%.*]], ptr [[RESULT_BUFF:%.*]], ptr [[SUBS:%.*]], ptr [[WITNESS_TABLES:%.*]], i64 [[NUM_WITNESS_TABLES:%.*]], ptr {{.*}}, ptr [[DECODER_TYPE:%.*]], ptr [[DECODER_PROTOCOL_WITNESS:%.*]])
// CHECK-NEXT: entry:
// CHECK-NEXT: {{.*}} = alloca ptr
// CHECK-NEXT: %swifterror = alloca swifterror ptr
// CHECK-NEXT: {{.*}} = call token @llvm.coro.id.async(i32 16, i32 16, i32 0, ptr @"$s27distributed_actor_accessors12MyOtherActorC5emptyyyYaKFTETFTu")
// CHECK-NEXT: {{.*}} = call ptr @llvm.coro.begin(token {{%.*}}, ptr null)
// CHECK-NEXT: store ptr {{.*}}, ptr {{.*}}
// CHECK-NEXT: store ptr null, ptr %swifterror
// CHECK-DIRECT-NEXT: {{.*}} = load i32, ptr getelementptr inbounds (%swift.async_func_pointer, ptr @"$s27distributed_actor_accessors12MyOtherActorC5emptyyyYaKFTETu", i32 0, i32 1)
// CHECK-INDIRECT-NEXT: [[ADDR:%[0-9]+]] = load ptr, ptr inttoptr (i64 and (i64 ptrtoint (ptr @"$s27distributed_actor_accessors12MyOtherActorC5emptyyyYaKFTETu" to i64), i64 -2) to ptr), align 8
// CHECK-INDIRECT-NEXT: [[SELECT:%[0-9]+]] = select i1 true, ptr @"$s27distributed_actor_accessors12MyOtherActorC5emptyyyYaKFTETu", ptr [[ADDR]]
// CHECK-INDIRECT-NEXT: [[LOAD:%[0-9]+]] = getelementptr inbounds %swift.async_func_pointer, ptr [[SELECT]], i32 0, i32 1
// CHECK-INDIRECT-NEXT: {{.*}} = load i32, ptr [[LOAD]]
