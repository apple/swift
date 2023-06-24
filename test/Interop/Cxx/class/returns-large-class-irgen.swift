// RUN: %target-swift-emit-ir %use_no_opaque_pointers -I %S/Inputs -enable-experimental-cxx-interop %s -validate-tbd-against-ir=none | %FileCheck %s
// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s -validate-tbd-against-ir=none

// This test verifies that Swift correctly emits IR calls to C++ functions that
// had Named Return Value Optimization applied to them. The first argument of
// such functions has `sret` attribute. When calling them, the first
// parameter should be wrapped in `sret(...)`.

import ReturnsLargeClass

func foo() -> LargeClass {
  let x = funcReturnsLargeClass()
  return x
}

foo()

// CHECK: call swiftcc void @"$s4main3fooSo10LargeClassVyF"(%TSo10LargeClassV* noalias nocapture sret(%TSo10LargeClassV) %{{.*}})

// The C++ function:
// CHECK: define{{( dso_local)?}} void @{{_Z21funcReturnsLargeClassv|"\?funcReturnsLargeClass@@YA\?AULargeClass@@XZ"}}({{%struct.LargeClass\*|ptr}} noalias sret(%struct.LargeClass){{( align .*)?}} %{{.*}})
