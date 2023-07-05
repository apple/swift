// RUN: %target-swiftxx-frontend -I %S/Inputs %s -emit-ir -Xcc -fignore-exceptions | %FileCheck %s

// Verify that non-trivial/address-only C++ classes are constructed and accessed
// correctly. Make sure that we correctly IRGen functions that construct
// non-trivial C++ classes, take those classes as a parameter, and access those
// classes members.

import TypeClassification

// TODO: C++ objects with destructors should be tested here once we fully
// support them.

// CHECK-LABEL: define {{.*}}i1 @"$s4main37testStructWithCopyConstructorAndValueSbyF"
// CHECK: [[OBJ:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)Ei|"\?\?0StructWithCopyConstructorAndValue@@QEAA@H@Z"}}(ptr {{(noalias )?}}[[OBJ]], i32 42)
// CHECK: [[OBJ_VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, ptr [[OBJ]], i32 0, i32 0
// CHECK: [[I_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[OBJ_VAL]], i32 0, i32 0
// CHECK: [[I_VAL_VAL:%.*]] = load i32, ptr [[OBJ_VAL]]
// CHECK: [[OUT:%.*]] = icmp eq i32 [[I_VAL_VAL]], 42
// CHECK: ret i1 [[OUT]]
public func testStructWithCopyConstructorAndValue() -> Bool {
  let obj = StructWithCopyConstructorAndValue(42)
  return obj.value == 42
}

// CHECK-LABEL: define {{.*}}i1 @"$s4main46testStructWithSubobjectCopyConstructorAndValueSbyF"()
// CHECK: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: [[OBJ:%.*]] = alloca %TSo42StructWithSubobjectCopyConstructorAndValueV
// CHECK: alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)Ei|"\?\?0StructWithCopyConstructorAndValue@@QEAA@H@Z"}}(ptr {{(noalias )?}}[[MEMBER]], i32 42)
// CHECK: [[TEMP_MEMBER:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, ptr [[TMP]], i32 0, i32 0
// CHECK: [[TEMP_MEMBER_VALUE:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[TEMP_MEMBER]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, ptr [[TEMP_MEMBER_VALUE]]
// CHECK: [[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK: ret i1 [[OUT]]
public func testStructWithSubobjectCopyConstructorAndValue() -> Bool {
  let member = StructWithCopyConstructorAndValue(42)
  let obj = StructWithSubobjectCopyConstructorAndValue(member: member)
  return obj.member.value == 42
}

// CHECK-LABEL: define {{.*}}i1 @"$s4main041testStructWithCopyConstructorAndSubobjectefG5ValueSbyF"()
// CHECK: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: [[OBJ:%.*]] = alloca %TSo037StructWithCopyConstructorAndSubobjectcdE5ValueV
// CHECK: [[TEMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: [[TEMP2:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)Ei|"\?\?0StructWithCopyConstructorAndValue@@QEAA@H@Z"}}(ptr {{(noalias )?}}[[MEMBER]], i32 42)
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)ERKS_|"\?\?0StructWithCopyConstructorAndValue@@QEAA@AEBU0@@Z"}}(ptr [[TEMP]], ptr [[MEMBER]])
// CHECK: call {{.*}}@{{_ZN60StructWithCopyConstructorAndSubobjectCopyConstructorAndValueC(1|2)E33StructWithCopyConstructorAndValue|"\?\?0StructWithCopyConstructorAndSubobjectCopyConstructorAndValue@@QEAA@UStructWithCopyConstructorAndValue@@@Z"}}(ptr {{(noalias )?}}[[OBJ]], ptr [[TEMP]])
// CHECK: [[TEMP_MEMBER:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, ptr [[TEMP2]], i32 0, i32 0
// CHECK: [[TEMP_MEMBER_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[TEMP_MEMBER]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, ptr [[TEMP_MEMBER_VAL]]
// CHECK: [[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK: ret i1 [[OUT]]
public func testStructWithCopyConstructorAndSubobjectCopyConstructorAndValue()
-> Bool {
  let member = StructWithCopyConstructorAndValue(42)
  let obj = StructWithCopyConstructorAndSubobjectCopyConstructorAndValue(
    member
  )
  return obj.member.value == 42
}

// CHECK-LABEL: define {{.*}}i1 @"$s4main4test3objSbSo33StructWithCopyConstructorAndValueV_tF"(ptr noalias nocapture dereferenceable(4) %0)
// CHECK: [[VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, ptr %0, i32 0, i32 0
// CHECK: [[VAL_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[VAL]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, ptr [[VAL_VAL]]
// CHECK: [[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK: ret i1 [[OUT]]
public func test(obj: StructWithCopyConstructorAndValue) -> Bool {
  return obj.value == 42
}

// CHECK-LABEL: define {{.*}}i1 @"$s4main4test3objSbSo42StructWithSubobjectCopyConstructorAndValueV_tF"(ptr noalias nocapture dereferenceable(4) %0)
// CHECK: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: [[MEMBER:%.*]] = getelementptr inbounds %TSo42StructWithSubobjectCopyConstructorAndValueV, ptr %0, i32 0, i32 0
// CHECK: [[VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, ptr [[TMP]], i32 0, i32 0
// CHECK: [[VAL_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[VAL]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, ptr [[VAL_VAL]]
// CHECK: [[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK: ret i1 [[OUT]]
public func test(obj: StructWithSubobjectCopyConstructorAndValue) -> Bool {
  return obj.member.value == 42
}

// CHECK-LABEL: define {{.*}}i1 @"$s4main4test3objSbSo037StructWithCopyConstructorAndSubobjectfgH5ValueV_tF"(ptr noalias nocapture dereferenceable(4) %0)
// CHECK:[[TEMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK:[[VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, ptr [[TEMP]], i32 0, i32 0
// CHECK:[[VAL_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[VAL]], i32 0, i32 0
// CHECK:[[LHS:%.*]] = load i32, ptr [[VAL_VAL]]
// CHECK:[[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK:ret i1 [[OUT]]
public func test(
  obj: StructWithCopyConstructorAndSubobjectCopyConstructorAndValue
) -> Bool {
  return obj.member.value == 42
}
