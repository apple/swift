// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop -Xcc -fignore-exceptions | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -Xcc -fignore-exceptions
//
// We should be able to support windows now. We will remove XFAIL in follow up
// XFAIL: windows

import MemberInline

public func sub(_ lhs: inout LoadableIntWrapper, _ rhs: LoadableIntWrapper) -> LoadableIntWrapper { lhs - rhs }

// CHECK: call [[RESA:i32|i64]] [[NAMEA:@(_ZN18LoadableIntWrappermiES_|"\?\?GLoadableIntWrapper@@QEAA\?AU0@U0@@Z")]](%struct.LoadableIntWrapper* {{%[0-9]+}}, {{i32|\[1 x i32\]|i64|%struct.LoadableIntWrapper\* byval\(.*\) align 4}} {{%[0-9]+}})

public func call(_ wrapper: inout LoadableIntWrapper, _ arg: Int32) -> Int32 { wrapper(arg) }

// CHECK: call [[RES:i32|i64]] [[NAME:@(_ZN18LoadableIntWrapperclEi|"\?\?GLoadableIntWrapper@@QEAAHH@Z")]](%struct.LoadableIntWrapper* {{%[0-9]+}}, {{i32|\[1 x i32\]|i64|%struct.LoadableIntWrapper\* byval\(.*\)}}{{.*}})
// CHECK: define  {{.*}}[[RES]] [[NAME]](%struct.LoadableIntWrapper* {{.*}}, {{i32|\[1 x i32\]|i64|%struct.LoadableIntWrapper\* byval\(%struct.LoadableIntWrapper\)}}{{.*}})

public func call(_ wrapper: inout AddressOnlyIntWrapper) -> Int32 { wrapper() }

// CHECK: call [[RES:i32|i64]] [[NAME:@(_ZN21AddressOnlyIntWrapperclEv|"\?\?GAddressOnlyIntWrapper@@QEAAHXZ")]](%struct.AddressOnlyIntWrapper* {{.*}})
// CHECK: define {{.*}}[[RES]] [[NAME]](%struct.AddressOnlyIntWrapper* {{.*}})

public func index(_ arr: inout ReadOnlyIntArray, _ arg: Int32) -> Int32 { arr[arg] }

// CHECK: call [[RES:i32|i64]]* [[NAME:@(_ZNK16ReadOnlyIntArrayixEi|"\?\?AReadOnlyIntArray@@QEBAAEBHH@Z")]](%struct.ReadOnlyIntArray* {{.*}}, {{i32|i64}}{{.*}})
// CHECK: define {{.*}}[[RES]]* [[NAME]](%struct.ReadOnlyIntArray* {{.*}}, {{i32|\[1 x i32\]|i64|%struct.ReadOnlyIntArray\* byval\(%struct.ReadOnlyIntArray\)}}{{.*}})
// CHECK:   [[THIS:%.*]] = load %struct.ReadOnlyIntArray*, %struct.ReadOnlyIntArray**
// CHECK:   [[VALUES:%.*]] = getelementptr inbounds %struct.ReadOnlyIntArray, %struct.ReadOnlyIntArray* [[THIS]]
// CHECK:   [[VALUE:%.*]] = getelementptr inbounds [5 x {{i32|i64}}], [5 x {{i32|i64}}]* [[VALUES]]
// CHECK:   ret {{i32|i64}}* [[VALUE]]

public func index(_ arr: inout ReadWriteIntArray, _ arg: Int32, _ val: Int32) { arr[arg] = val }

// CHECK: call [[RES:i32|i64]]* [[NAME:@(_ZN17ReadWriteIntArrayixEi|"\?\?AReadWriteIntArray@@QEAAAEAHH@Z")]](%struct.ReadWriteIntArray* {{.*}}, {{i32|i64}}{{.*}})
// CHECK: define {{.*}}[[RES]]* [[NAME]](%struct.ReadWriteIntArray* {{.*}}, {{i32|\[1 x i32\]|i64|%struct.ReadWriteIntArray\* byval\(%struct.ReadWriteIntArray\)}}{{.*}})
// CHECK:   [[THIS:%.*]] = load %struct.ReadWriteIntArray*, %struct.ReadWriteIntArray**
// CHECK:   [[VALUES:%.*]] = getelementptr inbounds %struct.ReadWriteIntArray, %struct.ReadWriteIntArray* [[THIS]]
// CHECK:   [[VALUE:%.*]] = getelementptr inbounds [5 x {{i32|i64}}], [5 x {{i32|i64}}]* [[VALUES]]
// CHECK:   ret {{i32|i64}}* [[VALUE]]

public func index(_ arr: inout NonTrivialIntArrayByVal, _ arg: Int32) -> Int32 { arr[arg] }

// CHECK: call [[RES:i32|i64]] [[NAME:@(_ZNK23NonTrivialIntArrayByValixEi|"\?\?ANonTrivialIntArrayByVal@@QEBAAEBHH@Z")]](%struct.NonTrivialIntArrayByVal* {{.*}}, {{i32|i64}}{{.*}})

// CHECK: define {{.*}}[[RES]] [[NAME]](%struct.NonTrivialIntArrayByVal* {{.*}}, {{i32|\[1 x i32\]|i64|%struct.NonTrivialIntArrayByVal\* byval\(%struct.NonTrivialIntArrayByVal\)}}{{.*}})
// CHECK:   [[THIS:%.*]] = load %struct.NonTrivialIntArrayByVal*, %struct.NonTrivialIntArrayByVal**
// CHECK:   [[VALUES:%.*]] = getelementptr inbounds %struct.NonTrivialIntArrayByVal, %struct.NonTrivialIntArrayByVal* [[THIS]]
// CHECK:   [[VALUE:%.*]] = getelementptr inbounds [5 x {{i32|i64}}], [5 x {{i32|i64}}]* [[VALUES]]
// CHECK:   [[VALUE2:%.*]] = load {{i32|i64}}, {{i32|i64}}* [[VALUE]]
// CHECK:   ret {{i32|i64}} [[VALUE2]]

// CHECK: define {{.*}}[[RESA]] [[NAMEA]](%struct.LoadableIntWrapper* {{.*}}, {{i32 .*%.*.coerce|\[1 x i32\] .*%.*.coerce|i64 .*%.*.coerce|%struct.LoadableIntWrapper\* .*byval\(%struct.LoadableIntWrapper\).*}})
