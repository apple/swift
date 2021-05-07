// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import StaticMemberFunc

public func callStaticMemberFunc() -> CInt {
  return WithStaticMemberFunc.staticMemberFunc()
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main20callStaticMemberFuncs5Int32VyF"()
// CHECK: [[VALUE:%.*]] = call i32 @{{_ZN20WithStaticMemberFunc16staticMemberFuncEv|"\?staticMemberFunc@WithStaticMemberFunc@@SAHXZ"}}()
// CHECK: ret i32 [[VALUE]]

// CHECK: declare {{(dso_local )?}}i32 @{{_ZN20WithStaticMemberFunc16staticMemberFuncEv|"\?staticMemberFunc@WithStaticMemberFunc@@SAHXZ"}}()

public func callStaticMemberFuncAddr() -> CInt {
  return WithStaticMemberFunc.getStaticMemberFuncAddress()!()
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main24callStaticMemberFuncAddrs5Int32VyF"()
// CHECK: call i32 ()* @{{_ZN20WithStaticMemberFunc26getStaticMemberFuncAddressEv|"\?getStaticMemberFuncAddress@WithStaticMemberFunc@@SAP6AHXZXZ"}}()

// CHECK: declare {{(dso_local )?}}i32 ()* @{{_ZN20WithStaticMemberFunc26getStaticMemberFuncAddressEv|"\?getStaticMemberFuncAddress@WithStaticMemberFunc@@SAP6AHXZXZ"}}()

