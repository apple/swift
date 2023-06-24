// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop -Xcc -fignore-exceptions | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -Xcc -fignore-exceptions

import StaticMemberFunc

public func callStaticMemberFunc() -> CInt {
  return WithStaticMemberFunc.staticMemberFunc()
}

// CHECK: define {{.*}}i32 @"$s4main20callStaticMemberFuncs5Int32VyF"()
// CHECK: [[VALUE:%.*]] = call i32 @{{_ZN20WithStaticMemberFunc16staticMemberFuncEv|"\?staticMemberFunc@WithStaticMemberFunc@@SAHXZ"}}()
// CHECK: ret i32 [[VALUE]]

// CHECK: declare {{.*}}i32 @{{_ZN20WithStaticMemberFunc16staticMemberFuncEv|"\?staticMemberFunc@WithStaticMemberFunc@@SAHXZ"}}()

public func callStaticMemberFuncAddr() -> CInt {
  return WithStaticMemberFunc.getStaticMemberFuncAddress()!()
}

// CHECK: define {{.*}}i32 @"$s4main24callStaticMemberFuncAddrs5Int32VyF"()
// CHECK: call i32 ()* @{{_ZN20WithStaticMemberFunc26getStaticMemberFuncAddressEv|"\?getStaticMemberFuncAddress@WithStaticMemberFunc@@SAP6AHXZXZ"}}()

// CHECK: declare {{.*}}i32 ()* @{{_ZN20WithStaticMemberFunc26getStaticMemberFuncAddressEv|"\?getStaticMemberFuncAddress@WithStaticMemberFunc@@SAP6AHXZXZ"}}()

