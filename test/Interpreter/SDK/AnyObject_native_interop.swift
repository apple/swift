// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

// REQUIRES: objc_interop

// Test interop of pure Swift objects as ObjC objects accessed through AnyObject.

import Foundation

class FullyNative {
  deinit { print("dead") }
}

autoreleasepool {
  let c: AnyObject = FullyNative() as AnyObject

  // CHECK: {{^}}main.FullyNative{{$}}
  print(c.description!)
  _fixLifetime(c)
}
// CHECK-NEXT: dead

