// RUN: %swift -I %S/Inputs -enable-experimental-cxx-interop -enable-objc-interop -emit-ir %s -Xcc -fignore-exceptions | %FileCheck %s

import AnonymousUnionPartlyInvalid

let sPtr = getSPtr()
let a = sPtr![0].f()

// CHECK: i32 @main
// CHECK-NEXT: entry:
// CHECK-NEXT: call ptr
// CHECK-NEXT: ptrtoint ptr

