// RUN: %target-run-simple-swift(-parse-as-library -enable-experimental-feature Embedded -wmo -runtime-compatibility-version none) | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: executable_test
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu
// REQUIRES: volatile

import _Volatile

@_extern(c, "putchar")
@discardableResult
func putchar(_: CInt) -> CInt

@main
struct Main {
  static func main() {
    var byte: UInt8 = 42
    withUnsafePointer(to: &byte) { p in
      let volatilePointer = VolatileMappedRegister<UInt8>(unsafeBitPattern: UInt(bitPattern: p))
      print("byte = ", terminator: "")
      print(volatilePointer.load())
      // CHECK: byte = 42
      volatilePointer.store(77)
    }
    print("byte = ", terminator: "")
    print(byte)
    // CHECK: byte = 77

    var heapPointer = UnsafeMutablePointer<UInt8>.allocate(capacity: 16) // uninitialized content
    for i in 0 ..< 16 {
      let pointerWithOffset = heapPointer.advanced(by: i)
      let volatilePointer = VolatileMappedRegister<UInt8>(unsafeBitPattern: UInt(bitPattern: pointerWithOffset))
      volatilePointer.store(UInt8(0x61 + i))
    }
    for i in 0 ..< 16 { putchar(CInt(heapPointer[i])) }
    putchar(CInt(("\n" as Unicode.Scalar).value))
    // CHECK: abcdefghijklmnop
  }
}
