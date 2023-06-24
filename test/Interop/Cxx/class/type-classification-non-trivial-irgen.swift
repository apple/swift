// RUN: %target-swiftxx-frontend %use_no_opaque_pointers -I %S/Inputs %s -emit-ir -Xcc -fignore-exceptions | %FileCheck %s
// RUN: %target-swiftxx-frontend -I %S/Inputs %s -emit-ir -Xcc -fignore-exceptions

// Verify that non-trivial/address-only C++ classes are constructed and accessed
// correctly. Make sure that we correctly IRGen functions that construct
// non-trivial C++ classes, take those classes as a parameter, and access those
// classes members.

import TypeClassification

// TODO: C++ objects with destructors should be tested here once we fully
// support them.

// CHECK-LABEL: define {{.*}}i1 @"$s4main37testStructWithCopyConstructorAndValueSbyF"
// CHECK: [[OBJ:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: [[STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[OBJ]] to %struct.StructWithCopyConstructorAndValue*
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)Ei|"\?\?0StructWithCopyConstructorAndValue@@QEAA@H@Z"}}(%struct.StructWithCopyConstructorAndValue* {{(noalias )?}}[[STRUCT]], i32 42)
// CHECK: [[OBJ_VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, %TSo33StructWithCopyConstructorAndValueV* [[OBJ]], i32 0, i32 0
// CHECK: [[I_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, %Ts5Int32V* [[OBJ_VAL]], i32 0, i32 0
// CHECK: [[I_VAL_VAL:%.*]] = load i32, i32* [[OBJ_VAL]]
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
// CHECK: [[MEMBER_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)Ei|"\?\?0StructWithCopyConstructorAndValue@@QEAA@H@Z"}}(%struct.StructWithCopyConstructorAndValue* {{(noalias )?}}[[MEMBER_STRUCT]], i32 42)
// CHECK: [[TEMP_MEMBER:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, %TSo33StructWithCopyConstructorAndValueV* [[TMP]], i32 0, i32 0
// CHECK: [[TEMP_MEMBER_VALUE:%.*]] = getelementptr inbounds %Ts5Int32V, %Ts5Int32V* [[TEMP_MEMBER]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, i32* [[TEMP_MEMBER_VALUE]]
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
// CHECK: [[MEMBER_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)Ei|"\?\?0StructWithCopyConstructorAndValue@@QEAA@H@Z"}}(%struct.StructWithCopyConstructorAndValue* {{(noalias )?}}[[MEMBER_STRUCT]], i32 42)
// CHECK: [[TEMP_AS_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[TEMP]] to %struct.StructWithCopyConstructorAndValue*
// CHECK: [[MEMBER_AS_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
// CHECK: call {{.*}}@{{_ZN33StructWithCopyConstructorAndValueC(1|2)ERKS_|"\?\?0StructWithCopyConstructorAndValue@@QEAA@AEBU0@@Z"}}(%struct.StructWithCopyConstructorAndValue* [[TEMP_AS_STRUCT]], %struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT]])
// CHECK: [[TEMP_AS_STRUCT_2:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[TEMP]] to %struct.StructWithCopyConstructorAndValue*
// CHECK: [[OBJ_AS_STRUCT_2:%.*]] = bitcast %TSo037StructWithCopyConstructorAndSubobjectcdE5ValueV* [[OBJ]] to %struct.StructWithCopyConstructorAndSubobjectCopyConstructorAndValue*
// CHECK: call {{.*}}@{{_ZN60StructWithCopyConstructorAndSubobjectCopyConstructorAndValueC(1|2)E33StructWithCopyConstructorAndValue|"\?\?0StructWithCopyConstructorAndSubobjectCopyConstructorAndValue@@QEAA@UStructWithCopyConstructorAndValue@@@Z"}}(%struct.StructWithCopyConstructorAndSubobjectCopyConstructorAndValue* {{(noalias )?}}[[OBJ_AS_STRUCT_2]], %struct.StructWithCopyConstructorAndValue* [[TEMP_AS_STRUCT_2]])
// CHECK: [[TEMP_MEMBER:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, %TSo33StructWithCopyConstructorAndValueV* [[TEMP2]], i32 0, i32 0
// CHECK: [[TEMP_MEMBER_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, %Ts5Int32V* [[TEMP_MEMBER]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, i32* [[TEMP_MEMBER_VAL]]
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

// CHECK-LABEL: define {{.*}}i1 @"$s4main4test3objSbSo33StructWithCopyConstructorAndValueV_tF"(%TSo33StructWithCopyConstructorAndValueV* noalias nocapture dereferenceable(4) %0)
// CHECK: [[VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, %TSo33StructWithCopyConstructorAndValueV* %0, i32 0, i32 0
// CHECK: [[VAL_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, %Ts5Int32V* [[VAL]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, i32* [[VAL_VAL]]
// CHECK: [[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK: ret i1 [[OUT]]
public func test(obj: StructWithCopyConstructorAndValue) -> Bool {
  return obj.value == 42
}

// CHECK-LABEL: define {{.*}}i1 @"$s4main4test3objSbSo42StructWithSubobjectCopyConstructorAndValueV_tF"(%TSo42StructWithSubobjectCopyConstructorAndValueV* noalias nocapture dereferenceable(4) %0)
// CHECK: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK: [[MEMBER:%.*]] = getelementptr inbounds %TSo42StructWithSubobjectCopyConstructorAndValueV, %TSo42StructWithSubobjectCopyConstructorAndValueV* %0, i32 0, i32 0
// CHECK: [[VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, %TSo33StructWithCopyConstructorAndValueV* [[TMP]], i32 0, i32 0
// CHECK: [[VAL_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, %Ts5Int32V* [[VAL]], i32 0, i32 0
// CHECK: [[LHS:%.*]] = load i32, i32* [[VAL_VAL]]
// CHECK: [[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK: ret i1 [[OUT]]
public func test(obj: StructWithSubobjectCopyConstructorAndValue) -> Bool {
  return obj.member.value == 42
}

// CHECK-LABEL: define {{.*}}i1 @"$s4main4test3objSbSo037StructWithCopyConstructorAndSubobjectfgH5ValueV_tF"(%TSo037StructWithCopyConstructorAndSubobjectcdE5ValueV* noalias nocapture dereferenceable(4) %0)
// CHECK:[[TEMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
// CHECK:[[VAL:%.*]] = getelementptr inbounds %TSo33StructWithCopyConstructorAndValueV, %TSo33StructWithCopyConstructorAndValueV* [[TEMP]], i32 0, i32 0
// CHECK:[[VAL_VAL:%.*]] = getelementptr inbounds %Ts5Int32V, %Ts5Int32V* [[VAL]], i32 0, i32 0
// CHECK:[[LHS:%.*]] = load i32, i32* [[VAL_VAL]]
// CHECK:[[OUT:%.*]] = icmp eq i32 [[LHS]], 42
// CHECK:ret i1 [[OUT]]
public func test(
  obj: StructWithCopyConstructorAndSubobjectCopyConstructorAndValue
) -> Bool {
  return obj.member.value == 42
}
