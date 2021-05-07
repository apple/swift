// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import FunctionTemplates

// CHECK-LABEL: define {{.*}}i32 @"$s4main20testPassThroughConst1xs5Int32VAE_tF"(i32 %0)
// CHECK: [[RET_VAL:%.*]] = call i32 @{{_Z16passThroughConstIiEKT_S0_|"\?\?\$passThroughConst@H@@YA\?BHH@Z"}}(i32 %0)
// CHECK: ret i32 [[RET_VAL]]

// CHECK-LABEL: define linkonce_odr {{.*}}i32 @{{_Z16passThroughConstIiEKT_S0_|"\?\?\$passThroughConst@H@@YA\?BHH@Z"}}(i32 %value)
public func testPassThroughConst(x: Int32) -> Int32 {
  return passThroughConst(x)
}

// CHECK-LABEL: define {{.*}}i32 @"$s4main15testPassThrough1xs5Int32VAE_tF"(i32 %0)
// CHECK: [[RET_VAL:%.*]] = call i32 @{{_Z11passThroughIiET_S0_|"\?\?\$passThrough@H@@YAHH@Z"}}(i32 %0)
// CHECK: ret i32 [[RET_VAL]]

// CHECK-LABEL: define linkonce_odr {{.*}}i32 @{{_Z11passThroughIiET_S0_|"\?\?\$passThrough@H@@YAHH@Z"}}(i32 %value)
public func testPassThrough(x: Int32) -> Int32 {
  return passThrough(x)
}

// CHECK-LABEL: define {{.*}}i32 @"$s4main22testAddMixedTypeParams1xs5Int32VAE_tF"(i32 %0)
// CHECK: [[OUT_VAL:%.*]] = call i32 @{{_Z18addMixedTypeParamsIiiET_S0_T0_|"\?\?\$addMixedTypeParams@HH@@YAHHH@Z"}}(i32 %0, i32 %0)
// CHECK: ret i32 [[OUT_VAL]]

// CHECK-LABEL: define linkonce_odr {{.*}}i32 @{{_Z18addMixedTypeParamsIiiET_S0_T0_|"\?\?\$addMixedTypeParams@HH@@YAHHH@Z"}}(i32 %a, i32 %b)
public func testAddMixedTypeParams(x: Int32) -> Int32 {
  return addMixedTypeParams(x, x)
}

// CHECK-LABEL: define {{.*}}i32 @"$s4main21testAddSameTypeParams1xs5Int32VAE_tF"(i32 %0)
// CHECK: [[OUT_VAL:%.*]] = call i32 @{{_Z17addSameTypeParamsIiET_S0_S0_|"\?\?\$addSameTypeParams@H@@YAHHH@Z"}}(i32 %0, i32 %0)
// CHECK: ret i32 [[OUT_VAL]]

// CHECK-LABEL: define linkonce_odr {{.*}}i32 @{{_Z17addSameTypeParamsIiET_S0_S0_|"\?\?\$addSameTypeParams@H@@YAHHH@Z"}}(i32 %a, i32 %b)
public func testAddSameTypeParams(x: Int32) -> Int32 {
  return addSameTypeParams(x, x)
}
