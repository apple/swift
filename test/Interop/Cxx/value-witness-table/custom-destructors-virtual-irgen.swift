// With RTTI some of the objects with virtual bases / destructors in this test
// will cause linker errors because of undefined vtables.
// FIXME: Once we can link with libc++ we can start using RTTI.
//
// RUN: %target-swift-frontend -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir -Xcc -fno-rtti | %FileCheck %s

import CustomDestructor

// CHECK-LABEL: define {{.*}}void @"$s4main022testHasVirtualBaseWithD10DestructoryySpys5Int32VGF"
// CHECK: call {{.*}}@{{_ZN28HasBaseWithVirtualDestructorD(1|2)Ev|"\?\?1HasBaseWithVirtualDestructor@@UEAA@XZ"}}(ptr
// CHECK: ret
public func testHasVirtualBaseWithVirtualDestructor(
  _ ptr: UnsafeMutablePointer<Int32>
) {
  _ = HasBaseWithVirtualDestructor(ptr)
}
