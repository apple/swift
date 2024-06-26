// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

// REQUIRES: objc_interop

import Foundation
 
_ = JSONDecoder()

class Canary: NSObject {
  deinit {
    print("died")
  }
}

var CanaryAssocObjectHandle: UInt8 = 0


// Attach an associated object with a loud deinit so we can see that the
// object died.
func hangCanary(_ o: AnyObject) {
  objc_setAssociatedObject(o, &CanaryAssocObjectHandle, Canary(),
                           .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
}

func doit() {
// CHECK-LABEL: NSData:
print("NSData:")
do {
  var bytes: UnsafeMutablePointer<UInt8>
  let data = NSData(bytes: [2, 3, 5, 7] as [UInt8], length: 4)
  hangCanary(data)
  bytes = UnsafeMutablePointer<UInt8>(mutating: data.bytes.assumingMemoryBound(to: UInt8.self))
  print(bytes[0]) // CHECK:      2
  print(bytes[1]) // CHECK-NEXT: 3
  print(bytes[2]) // CHECK-NEXT: 5
  print(bytes[3]) // CHECK-NEXT: 7
} // CHECK-NEXT: died

// CHECK-LABEL: AnyObject:
print("AnyObject:")
do {
  var bytes: UnsafeMutablePointer<UInt8> = .init(bitPattern: 0xdeadbeef)!
  let data = NSData(bytes: [11, 13, 17, 19] as [UInt8], length: 4)
  hangCanary(data)
  autoreleasepool {
  let dataAsAny: AnyObject = data
  bytes = UnsafeMutablePointer<UInt8>(mutating: dataAsAny.bytes!.assumingMemoryBound(to: UInt8.self))
  }
  print(bytes[0]) // CHECK:      11
  print(bytes[1]) // CHECK-NEXT: 13
  print(bytes[2]) // CHECK-NEXT: 17
  print(bytes[3]) // CHECK-NEXT: 19
} // CHECK-NEXT: died

// CHECK-NEXT: done
print("done")
}

doit()
