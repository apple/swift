// RUN: %target-run-simple-swift(-parse-as-library -enable-experimental-feature Volatile) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: volatile

import _Volatile

@main
struct Main {
  static func main() {
    var byte: UInt8 = 42
    withUnsafePointer(to: &byte) { p in
      let volatilePointer = VolatileMappedRegister<UInt8>(unsafeBitPattern: UInt(bitPattern: p))
      print("byte = \(volatilePointer.load())")
      // CHECK: byte = 42
      volatilePointer.store(77)
    }
    print("byte = \(byte)")
    // CHECK: byte = 77

    var heapPointer = UnsafeMutablePointer<UInt8>.allocate(capacity: 16) // uninitialized content
    for i in 0 ..< 16 {
      let pointerWithOffset = heapPointer.advanced(by: i)
      let volatilePointer = VolatileMappedRegister<UInt8>(unsafeBitPattern: UInt(bitPattern: pointerWithOffset))
      volatilePointer.store(UInt8(0x61 + i))
    }
    heapPointer[15] = 0
    print(String(cString: heapPointer))
    // CHECK: abcdefghijklmno
  }
}
