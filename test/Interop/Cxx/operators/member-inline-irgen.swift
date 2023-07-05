// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -Xcc -fignore-exceptions | %FileCheck %s
//
// We should be able to support windows now. We will remove XFAIL in follow up
// XFAIL: windows

import MemberInline

public func sub(_ lhs: inout LoadableIntWrapper, _ rhs: LoadableIntWrapper) -> LoadableIntWrapper { lhs - rhs }

// CHECK: call [[RESA:i32|i64]] [[NAMEA:@(_ZN18LoadableIntWrappermiES_|"\?\?GLoadableIntWrapper@@QEAA\?AU0@U0@@Z")]](ptr {{%[0-9]+}}, {{i32|\[1 x i32\]|i64|%struct.LoadableIntWrapper\* byval\(.*\) align 4}} {{%[0-9]+}})

public func call(_ wrapper: inout LoadableIntWrapper, _ arg: Int32) -> Int32 { wrapper(arg) }

// CHECK: call [[RES:i32|i64]] [[NAME:@(_ZN18LoadableIntWrapperclEi|"\?\?GLoadableIntWrapper@@QEAAHH@Z")]](ptr {{%[0-9]+}}, {{i32|\[1 x i32\]|i64|%struct.LoadableIntWrapper\* byval\(.*\)}}{{.*}})
// CHECK: define  {{.*}}[[RES]] [[NAME]](ptr {{.*}}, {{i32|\[1 x i32\]|i64|%struct.LoadableIntWrapper\* byval\(%struct.LoadableIntWrapper\)}}{{.*}})

public func call(_ wrapper: inout AddressOnlyIntWrapper) -> Int32 { wrapper() }

// CHECK: call [[RES:i32|i64]] [[NAME:@(_ZN21AddressOnlyIntWrapperclEv|"\?\?GAddressOnlyIntWrapper@@QEAAHXZ")]](ptr {{.*}})
// CHECK: define {{.*}}[[RES]] [[NAME]](ptr {{.*}})

public func index(_ arr: inout ReadOnlyIntArray, _ arg: Int32) -> Int32 { arr[arg] }

// CHECK: call ptr [[NAME:@(_ZNK16ReadOnlyIntArrayixEi|"\?\?AReadOnlyIntArray@@QEBAAEBHH@Z")]](ptr {{.*}}, {{i32|i64}}{{.*}})
// CHECK: define {{.*}}ptr [[NAME]](ptr {{.*}}, {{i32|\[1 x i32\]|i64|%struct.ReadOnlyIntArray\* byval\(%struct.ReadOnlyIntArray\)}}{{.*}})
// CHECK:   [[THIS:%.*]] = load ptr, ptr
// CHECK:   [[VALUES:%.*]] = getelementptr inbounds %struct.ReadOnlyIntArray, ptr [[THIS]]
// CHECK:   [[VALUE:%.*]] = getelementptr inbounds [5 x {{i32|i64}}], ptr [[VALUES]]
// CHECK:   ret ptr [[VALUE]]

public func index(_ arr: inout ReadWriteIntArray, _ arg: Int32, _ val: Int32) { arr[arg] = val }

// CHECK: call ptr [[NAME:@(_ZN17ReadWriteIntArrayixEi|"\?\?AReadWriteIntArray@@QEAAAEAHH@Z")]](ptr {{.*}}, {{i32|i64}}{{.*}})
// CHECK: define {{.*}}ptr [[NAME]](ptr {{.*}}, {{i32|\[1 x i32\]|i64|%struct.ReadWriteIntArray\* byval\(%struct.ReadWriteIntArray\)}}{{.*}})
// CHECK:   [[THIS:%.*]] = load ptr, ptr
// CHECK:   [[VALUES:%.*]] = getelementptr inbounds %struct.ReadWriteIntArray, ptr [[THIS]]
// CHECK:   [[VALUE:%.*]] = getelementptr inbounds [5 x {{i32|i64}}], ptr [[VALUES]]
// CHECK:   ret ptr [[VALUE]]

public func index(_ arr: inout NonTrivialIntArrayByVal, _ arg: Int32) -> Int32 { arr[arg] }

// CHECK: call [[RES:i32|i64]] [[NAME:@(_ZNK23NonTrivialIntArrayByValixEi|"\?\?ANonTrivialIntArrayByVal@@QEBAAEBHH@Z")]](ptr {{.*}}, {{i32|i64}}{{.*}})

// CHECK: define {{.*}}[[RES:i32|i64]] [[NAME]](ptr {{.*}}, {{i32|\[1 x i32\]|i64|%struct.NonTrivialIntArrayByVal\* byval\(%struct.NonTrivialIntArrayByVal\)}}{{.*}})
// CHECK:   [[THIS:%.*]] = load ptr, ptr
// CHECK:   [[VALUES:%.*]] = getelementptr inbounds %struct.NonTrivialIntArrayByVal, ptr [[THIS]]
// CHECK:   [[VALUE:%.*]] = getelementptr inbounds [5 x {{i32|i64}}], ptr [[VALUES]]
// CHECK:   [[VALUE2:%.*]] = load {{i32|i64}}, ptr [[VALUE]]
// CHECK:   ret {{i32|i64}} [[VALUE2]]

// CHECK: define {{.*}}[[RESA]] [[NAMEA]](ptr {{.*}}, {{i32 .*%.*.coerce|\[1 x i32\] .*%.*.coerce|i64 .*%.*.coerce|%struct.LoadableIntWrapper\* .*byval\(%struct.LoadableIntWrapper\).*}})
