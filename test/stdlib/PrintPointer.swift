// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: reflection

import StdlibUnittest


let PrintTests = TestSuite("PrintPointer")
PrintTests.test("Printable") {
  let lowUP = UnsafeMutablePointer<Float>(bitPattern: 0x1)!
  let fourByteUP = UnsafeMutablePointer<Float>(bitPattern: 0xabcd1234 as UInt)!
  
#if _pointerBitWidth(_64)
  let eightByteAddr: UInt = 0xabcddcba12344321
  let eightByteUP = UnsafeMutablePointer<Float>(bitPattern: eightByteAddr)!
#endif
  
#if _pointerBitWidth(_32)
  let expectedLow = "0x00000001"
  expectPrinted("0xabcd1234", fourByteUP)
#else
  let expectedLow = "0x0000000000000001"
  expectPrinted("0x00000000abcd1234", fourByteUP)
  expectPrinted("0xabcddcba12344321", eightByteUP)
#endif
  
  expectPrinted(expectedLow, lowUP)
  
  expectPrinted("UnsafeBufferPointer(start: \(fourByteUP), count: 0)",
    UnsafeBufferPointer(start: fourByteUP, count: 0))
  expectPrinted("UnsafeMutableBufferPointer(start: \(fourByteUP), count: 0)",
    UnsafeMutableBufferPointer(start: fourByteUP, count: 0))
  
  let lowOpaque = OpaquePointer(lowUP)
  expectPrinted(expectedLow, lowOpaque)
#if _runtime(_ObjC)
  let lowAutoUP = AutoreleasingUnsafeMutablePointer<Int>(lowUP)
  expectPrinted(expectedLow, lowAutoUP)
#endif
}

runAllTests()
