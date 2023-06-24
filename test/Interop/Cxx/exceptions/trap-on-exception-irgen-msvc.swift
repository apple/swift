// RUN: %empty-directory(%t)
// RUN: split-file %S/trap-on-exception-irgen-itanium.swift %t

// RUN: %target-swift-emit-ir %use_no_opaque_pointers %t/test.swift -I %t/Inputs -enable-experimental-cxx-interop | %FileCheck %s
// RUN: %target-swift-emit-ir %t/test.swift -I %t/Inputs -enable-experimental-cxx-interop

// REQUIRES: OS=windows-msvc

// note: The test sources are in 'trap-on-exception-irgen-itanium.swift'

// CHECK: define {{.*}} @"$s4test0A23FreeFunctionNoThrowOnlys5Int32VyF"() #[[#SWIFTMETA:]] {
// CHECK-NEXT: :
// CHECK-NEXT:  call swiftcc i32 @"$s4test8makeCInts5Int32VyF"()
// CHECK-NEXT:  call {{.*}}i32 @"?freeFunctionNoThrow@@YAHH@Z"(i32 {{.*}})
// CHECK-NEXT:  ret i32
// CHECK-NEXT: }

// CHECK: define {{.*}} @"$s4test0A17FreeFunctionCallss5Int32VyF"() #[[#SWIFTMETA]] {
// CHECK: call {{.*}}i32 @"?freeFunctionThrows@@YAHH@Z"
// CHECK: call {{.*}}i32 @"?freeFunctionNoThrow@@YAHH@Z"
// CHECK: call swiftcc i32 @"$s4test8makeCInts5Int32VyF"()
// CHECK: call {{.*}}i32 @"?freeFunctionThrows@@YAHH@Z"
// CHECK: ret i32 %1
// CHECK-NEXT: }
