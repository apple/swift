// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -disable-availability-checking -Xcc -fignore-exceptions | %FileCheck %s

import ClassTemplateInstantiationErrors

// CHECK-LABEL: define {{.*}}void @"$s4main23instantiateValidMembersyyF"()
// CHECK: call i32 @{{_ZN21CannotBeInstantiantedI10IntWrapperE8incValueEv|"\?incValue@\?\$CannotBeInstantianted@UIntWrapper@@@@QEAAHXZ"}}(ptr
// CHECK: call i32 @{{_ZN21CannotBeInstantiantedI10IntWrapperE8incValueES0_|"\?incValue@\?\$CannotBeInstantianted@UIntWrapper@@@@QEAAHUIntWrapper@@@Z"}}(ptr
// CHECK: ret void

// CHECK-LABEL: define {{.*}}i32 @{{_ZN21CannotBeInstantiantedI10IntWrapperE8incValueEv|"\?incValue@\?\$CannotBeInstantianted@UIntWrapper@@@@QEAAHXZ"}}(ptr
// CHECK: call {{.*}}i32 @{{_ZN21CannotBeInstantiantedI10IntWrapperE6getOneEv|"\?getOne@\?\$CannotBeInstantianted@UIntWrapper@@@@QEAAHXZ"}}(ptr
// CHECK: ret i32

// CHECK-LABEL: define {{.*}}i32 @{{_ZN21CannotBeInstantiantedI10IntWrapperE8incValueES0_|"\?incValue@\?\$CannotBeInstantianted@UIntWrapper@@@@QEAAHUIntWrapper@@@Z"}}(ptr {{.*}}, {{i32|i64|\[1 x i32\]|ptr .*byval\(\%struct\.IntWrapper\)}}
// CHECK: call {{.*}}i32 @{{_ZN21CannotBeInstantiantedI10IntWrapperE6getOneEv|"\?getOne@\?\$CannotBeInstantianted@UIntWrapper@@@@QEAAHXZ"}}(ptr
// CHECK: ret i32

// CHECK-LABEL: define {{.*}}i32 @{{_ZN21CannotBeInstantiantedI10IntWrapperE6getOneEv|"\?getOne@\?\$CannotBeInstantianted@UIntWrapper@@@@QEAAHXZ"}}(ptr
// CHECK: ret i32 1
public func instantiateValidMembers() {
  var x = CannotBeInstantianted<IntWrapper>(IntWrapper(value: 41))
  x.incValue()
  var y = CannotBeInstantianted<IntWrapper>(IntWrapper(value: 0))
  y.incValue(IntWrapper(value: 41))
}
