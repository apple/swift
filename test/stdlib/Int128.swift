// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib, back_deployment_runtime

import StdlibUnittest
defer { runAllTests() }

var Int128Tests = TestSuite("Int128Tests")

Int128Tests.test("Memory layout") {
  if #available(SwiftStdlib 6.0, *) {
    // Size and stride must both be 16B.
    expectEqual(MemoryLayout<Int128>.size, 16)
    expectEqual(MemoryLayout<Int128>.stride, 16)
    // Alignment must be at least that of UInt64, and no more than 16B.
    expectTrue(
      MemoryLayout<Int128>.alignment >= MemoryLayout<Int64>.alignment
    )
    expectTrue(MemoryLayout<Int128>.alignment <= 16)
    
    // Size and stride must both be 16B.
    expectEqual(MemoryLayout<UInt128>.size, 16)
    expectEqual(MemoryLayout<UInt128>.stride, 16)
    // Alignment must be at least that of UInt64, and no more than 16B.
    expectTrue(
      MemoryLayout<UInt128>.alignment >= MemoryLayout<UInt64>.alignment
    )
    expectTrue(MemoryLayout<UInt128>.alignment <= 16)
  }
}

@available(SwiftStdlib 6.0, *)
extension UInt128 {
  var high: UInt64 { UInt64(truncatingIfNeeded: self >> 64) }
  var low: UInt64 { UInt64(truncatingIfNeeded: self) }
  
  init(tenToThe n: Int) {
    let tens: [UInt64] = [
      1,
      10,
      100,
      1000,
      10000,
      100000,
      1000000,
      10000000,
      100000000,
      1000000000,
      10000000000,
      100000000000,
      1000000000000,
      10000000000000,
      100000000000000,
      1000000000000000,
      10000000000000000,
      100000000000000000,
      1000000000000000000,
      10000000000000000000
    ]
    if n <= 19 { self.init(tens[n]) }
    else if n <= 38 {
      let (hi, lo) = tens[19].multipliedFullWidth(by: tens[n - 19])
      self = UInt128(hi) << 64 | UInt128(lo)
    }
    else { fatalError() }
  }
}

@available(SwiftStdlib 6.0, *)
extension Int128 {
  var high: Int64 { Int64(truncatingIfNeeded: self >> 64) }
  var low: UInt64 { UInt64(truncatingIfNeeded: self) }
  
  init(tenToThe n: Int) {
    self.init(bitPattern: UInt128(tenToThe: n))
  }
}

Int128Tests.test("Literals") {
  if #available(SwiftStdlib 6.0, *) {
    expectEqual(1, UInt128(tenToThe: 0))
    expectEqual(10, UInt128(tenToThe: 1))
    expectEqual(100, UInt128(tenToThe: 2))
    expectEqual(1000, UInt128(tenToThe: 3))
    expectEqual(10000, UInt128(tenToThe: 4))
    expectEqual(100000, UInt128(tenToThe: 5))
    expectEqual(1000000, UInt128(tenToThe: 6))
    expectEqual(10000000, UInt128(tenToThe: 7))
    expectEqual(100000000, UInt128(tenToThe: 8))
    expectEqual(1000000000, UInt128(tenToThe: 9))
    expectEqual(10000000000, UInt128(tenToThe: 10))
    expectEqual(100000000000, UInt128(tenToThe: 11))
    expectEqual(1000000000000, UInt128(tenToThe: 12))
    expectEqual(10000000000000, UInt128(tenToThe: 13))
    expectEqual(100000000000000, UInt128(tenToThe: 14))
    expectEqual(1000000000000000, UInt128(tenToThe: 15))
    expectEqual(10000000000000000, UInt128(tenToThe: 16))
    expectEqual(100000000000000000, UInt128(tenToThe: 17))
    expectEqual(1000000000000000000, UInt128(tenToThe: 18))
    expectEqual(10000000000000000000, UInt128(tenToThe: 19))
    expectEqual(100000000000000000000, UInt128(tenToThe: 20))
    expectEqual(1000000000000000000000, UInt128(tenToThe: 21))
    expectEqual(10000000000000000000000, UInt128(tenToThe: 22))
    expectEqual(100000000000000000000000, UInt128(tenToThe: 23))
    expectEqual(1000000000000000000000000, UInt128(tenToThe: 24))
    expectEqual(10000000000000000000000000, UInt128(tenToThe: 25))
    expectEqual(100000000000000000000000000, UInt128(tenToThe: 26))
    expectEqual(1000000000000000000000000000, UInt128(tenToThe: 27))
    expectEqual(10000000000000000000000000000, UInt128(tenToThe: 28))
    expectEqual(100000000000000000000000000000, UInt128(tenToThe: 29))
    expectEqual(1000000000000000000000000000000, UInt128(tenToThe: 30))
    expectEqual(10000000000000000000000000000000, UInt128(tenToThe: 31))
    expectEqual(100000000000000000000000000000000, UInt128(tenToThe: 32))
    expectEqual(1000000000000000000000000000000000, UInt128(tenToThe: 33))
    expectEqual(10000000000000000000000000000000000, UInt128(tenToThe: 34))
    expectEqual(100000000000000000000000000000000000, UInt128(tenToThe: 35))
    expectEqual(1000000000000000000000000000000000000, UInt128(tenToThe: 36))
    expectEqual(10000000000000000000000000000000000000, UInt128(tenToThe: 37))
    expectEqual(100000000000000000000000000000000000000, UInt128(tenToThe: 38))
    
    expectEqual(1, Int128(tenToThe: 0))
    expectEqual(10, Int128(tenToThe: 1))
    expectEqual(100, Int128(tenToThe: 2))
    expectEqual(1000, Int128(tenToThe: 3))
    expectEqual(10000, Int128(tenToThe: 4))
    expectEqual(100000, Int128(tenToThe: 5))
    expectEqual(1000000, Int128(tenToThe: 6))
    expectEqual(10000000, Int128(tenToThe: 7))
    expectEqual(100000000, Int128(tenToThe: 8))
    expectEqual(1000000000, Int128(tenToThe: 9))
    expectEqual(10000000000, Int128(tenToThe: 10))
    expectEqual(100000000000, Int128(tenToThe: 11))
    expectEqual(1000000000000, Int128(tenToThe: 12))
    expectEqual(10000000000000, Int128(tenToThe: 13))
    expectEqual(100000000000000, Int128(tenToThe: 14))
    expectEqual(1000000000000000, Int128(tenToThe: 15))
    expectEqual(10000000000000000, Int128(tenToThe: 16))
    expectEqual(100000000000000000, Int128(tenToThe: 17))
    expectEqual(1000000000000000000, Int128(tenToThe: 18))
    expectEqual(10000000000000000000, Int128(tenToThe: 19))
    expectEqual(100000000000000000000, Int128(tenToThe: 20))
    expectEqual(1000000000000000000000, Int128(tenToThe: 21))
    expectEqual(10000000000000000000000, Int128(tenToThe: 22))
    expectEqual(100000000000000000000000, Int128(tenToThe: 23))
    expectEqual(1000000000000000000000000, Int128(tenToThe: 24))
    expectEqual(10000000000000000000000000, Int128(tenToThe: 25))
    expectEqual(100000000000000000000000000, Int128(tenToThe: 26))
    expectEqual(1000000000000000000000000000, Int128(tenToThe: 27))
    expectEqual(10000000000000000000000000000, Int128(tenToThe: 28))
    expectEqual(100000000000000000000000000000, Int128(tenToThe: 29))
    expectEqual(1000000000000000000000000000000, Int128(tenToThe: 30))
    expectEqual(10000000000000000000000000000000, Int128(tenToThe: 31))
    expectEqual(100000000000000000000000000000000, Int128(tenToThe: 32))
    expectEqual(1000000000000000000000000000000000, Int128(tenToThe: 33))
    expectEqual(10000000000000000000000000000000000, Int128(tenToThe: 34))
    expectEqual(100000000000000000000000000000000000, Int128(tenToThe: 35))
    expectEqual(1000000000000000000000000000000000000, Int128(tenToThe: 36))
    expectEqual(10000000000000000000000000000000000000, Int128(tenToThe: 37))
    expectEqual(100000000000000000000000000000000000000, Int128(tenToThe: 38))
    
    expectEqual(0, UInt128.zero)
    expectEqual(0xffffffffffffffff_ffffffffffffffff, UInt128.max)
    expectEqual(340282366920938463463374607431768211455, UInt128.max)
    
    expectEqual(0, Int128.zero)
    expectEqual(0x7fffffffffffffff_ffffffffffffffff, Int128.max)
    expectEqual(170141183460469231731687303715884105727, Int128.max)
    expectEqual(-0x8000000000000000_0000000000000000, Int128.min)
    expectEqual(-170141183460469231731687303715884105728, Int128.min)
  }
}

@available(SwiftStdlib 6.0, *)
func testConversion(
  _ input: some BinaryFloatingPoint,
  _ r0: UInt128?,
  exact: Bool = false,
  line: UInt = #line
) {
  var r1: UInt128? = nil
  var r2: Int128? = nil
  var r3: Int128? = nil
  
  if let r0 {
    if r0 == 0 { r1 = 0 }
    if r0 <= Int128.max { r2 = Int128(bitPattern: r0) }
    if r0 <= UInt128(bitPattern: Int128.max) &+ 1 {
      r3 = Int128(bitPattern: 0 &- r0)
    }
    expectEqual(UInt128( input), r0, line: line)
    if let r1 { expectEqual(UInt128(-input), r1, line: line) }
    if let r2 { expectEqual( Int128( input), r2, line: line) }
    if let r3 { expectEqual( Int128(-input), r3, line: line) }
  }
  
  if exact {
    expectEqual(UInt128(exactly:  input), r0, line: line)
    expectEqual(UInt128(exactly: -input), r1, line: line)
    expectEqual( Int128(exactly:  input), r2, line: line)
    expectEqual( Int128(exactly: -input), r3, line: line)
  } else {
    expectEqual(UInt128(exactly:  input), nil, line: line)
    expectEqual(UInt128(exactly: -input), nil, line: line)
    expectEqual( Int128(exactly:  input), nil, line: line)
    expectEqual( Int128(exactly: -input), nil, line: line)
  }
}

Int128Tests.test("Conversion from Double") {
  if #available(SwiftStdlib 6.0, *) {
    
    func testCase(
      _ input: Double,
      _ r0: UInt128?,
      exact: Bool = false,
      line: UInt = #line
    ) { testConversion(input, r0, exact: exact, line: line) }
    
    testCase(.zero, 0, exact: true)
    testCase(.leastNonzeroMagnitude, 0)
    testCase(.leastNormalMagnitude, 0)
    testCase(0x1.fffffffffffffp-1,  0)
    testCase(0x1.0000000000000p0,   1, exact: true)
    testCase(0x1.0000000000001p0,   1)
    testCase(0x1.fffffffffffffp51,  0x000fffffffffffff)
    testCase(0x1.0000000000000p52,  0x0010000000000000, exact: true)
    testCase(0x1.0000000000001p52,  0x0010000000000001, exact: true)
    testCase(0x1.fffffffffffffp63,  0x0000000000000000_fffffffffffff800, exact: true)
    testCase(0x1.0000000000000p64,  0x0000000000000001_0000000000000000, exact: true)
    testCase(0x1.0000000000001p64,  0x0000000000000001_0000000000001000, exact: true)
    testCase(0x1.fffffffffffffp115, 0x000fffffffffffff_8000000000000000, exact: true)
    testCase(0x1.0000000000000p116, 0x0010000000000000_0000000000000000, exact: true)
    testCase(0x1.0000000000001p116, 0x0010000000000001_0000000000000000, exact: true)
    testCase(0x1.fffffffffffffp126, 0x7ffffffffffffc00_0000000000000000, exact: true)
    testCase(0x1.0000000000000p127, 0x8000000000000000_0000000000000000, exact: true)
    testCase(0x1.0000000000001p127, 0x8000000000000800_0000000000000000, exact: true)
    testCase(0x1.fffffffffffffp127, 0xfffffffffffff800_0000000000000000, exact: true)
    testCase(0x1.0000000000000p128, nil)
    testCase(0x1.0000000000001p128, nil)
    testCase(0x1.fffffffffffffp1023, nil)
    testCase(.infinity, nil)
    testCase(.nan, nil)
  }
}

Int128Tests.test("Conversion from Float") {
  if #available(SwiftStdlib 6.0, *) {
    
    func testCase(
      _ input: Float,
      _ r0: UInt128?,
      exact: Bool = false,
      line: UInt = #line
    ) { testConversion(input, r0, exact: exact, line: line) }
    
    testCase(.zero, 0, exact: true)
    testCase(.leastNonzeroMagnitude, 0)
    testCase(.leastNormalMagnitude, 0)
    testCase(0x1.fffffep-1,  0)
    testCase(0x1.000000p0,   1, exact: true)
    testCase(0x1.000002p0,   1)
    testCase(0x1.fffffep22,  0x00000000007fffff)
    testCase(0x1.000000p23,  0x0000000000800000, exact: true)
    testCase(0x1.000002p23,  0x0000000000800001, exact: true)
    testCase(0x1.fffffep63,  0x0000000000000000_ffffff0000000000, exact: true)
    testCase(0x1.000000p64,  0x0000000000000001_0000000000000000, exact: true)
    testCase(0x1.000002p64,  0x0000000000000001_0000020000000000, exact: true)
    testCase(0x1.fffffep86,  0x00000000007fffff_8000000000000000, exact: true)
    testCase(0x1.000000p87,  0x0000000000800000_0000000000000000, exact: true)
    testCase(0x1.000002p87,  0x0000000000800001_0000000000000000, exact: true)
    testCase(0x1.fffffep126, 0x7fffff8000000000_0000000000000000, exact: true)
    testCase(0x1.000000p127, 0x8000000000000000_0000000000000000, exact: true)
    testCase(0x1.000002p127, 0x8000010000000000_0000000000000000, exact: true)
    testCase(0x1.fffffep127, 0xffffff0000000000_0000000000000000, exact: true)
    testCase(.infinity, nil)
    testCase(.nan, nil)
  }
}

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
Int128Tests.test("Conversion from Float16") {
  if #available(SwiftStdlib 6.0, *) {
    
    func testCase(
      _ input: Float16,
      _ r0: UInt128?,
      exact: Bool = false,
      line: UInt = #line
    ) { testConversion(input, r0, exact: exact, line: line) }
    
    testCase(.zero, 0, exact: true)
    testCase(.leastNonzeroMagnitude, 0)
    testCase(.leastNormalMagnitude, 0)
    testCase(0x1.ffcp-1,  0)
    testCase(0x1.000p0,   1, exact: true)
    testCase(0x1.004p0,   1)
    testCase(0x1.ffcp9,   0x3ff)
    testCase(0x1.000p10,  0x400, exact: true)
    testCase(0x1.004p10,  0x401, exact: true)
    testCase(0x1.ffcp15, 0xffe0, exact: true)
    testCase(.infinity, nil)
    testCase(.nan, nil)
  }
}
#endif

Int128Tests.test("Conversion from integers") {
  if #available(SwiftStdlib 6.0, *) {
    expectEqual(Int128(truncatingIfNeeded: -1), -1)
    expectEqual(Int128(truncatingIfNeeded: Int8.min), -0x80)
    expectEqual(Int128(truncatingIfNeeded: Int8.max), 0x0000000000000000_000000000000007f)
    expectEqual(Int128(truncatingIfNeeded: UInt8.max), 0x0000000000000000_00000000000000ff)
    expectEqual(Int128(truncatingIfNeeded: Int.min), -0x8000000000000000)
    expectEqual(Int128(truncatingIfNeeded: Int.max), 0x0000000000000000_7fffffffffffffff)
    expectEqual(Int128(truncatingIfNeeded: UInt.max), 0x0000000000000000_ffffffffffffffff)
    
    expectEqual(Int128(exactly: -1), -1)
    expectEqual(Int128(exactly: Int8.min), -0x80)
    expectEqual(Int128(exactly: Int8.max),  0x7f)
    expectEqual(Int128(exactly: UInt8.max), 0xff)
    expectEqual(Int128(exactly: Int.min), -0x8000000000000000)
    expectEqual(Int128(exactly: Int.max),  0x7fffffffffffffff)
    expectEqual(Int128(exactly: UInt.max), 0xffffffffffffffff)
    
    expectEqual(Int128(clamping: -1), -1)
    expectEqual(Int128(clamping: Int8.min), -0x80)
    expectEqual(Int128(clamping: Int8.max),  0x7f)
    expectEqual(Int128(clamping: UInt8.max), 0xff)
    expectEqual(Int128(clamping: Int.min), -0x8000000000000000)
    expectEqual(Int128(clamping: Int.max),  0x7fffffffffffffff)
    expectEqual(Int128(clamping: UInt.max), 0xffffffffffffffff)
    
    expectEqual(UInt128(truncatingIfNeeded: -1),       0xffffffffffffffff_ffffffffffffffff)
    expectEqual(UInt128(truncatingIfNeeded: Int8.min), 0xffffffffffffffff_ffffffffffffff80)
    expectEqual(UInt128(truncatingIfNeeded: Int8.max), 0x0000000000000000_000000000000007f)
    expectEqual(UInt128(truncatingIfNeeded: UInt8.max),0x0000000000000000_00000000000000ff)
    expectEqual(UInt128(truncatingIfNeeded: Int.min),  0xffffffffffffffff_8000000000000000)
    expectEqual(UInt128(truncatingIfNeeded: Int.max),  0x0000000000000000_7fffffffffffffff)
    expectEqual(UInt128(truncatingIfNeeded: UInt.max), 0x0000000000000000_ffffffffffffffff)
    
    expectEqual(UInt128(exactly: -1),        nil)
    expectEqual(UInt128(exactly: Int8.min),  nil)
    expectEqual(UInt128(exactly: Int8.max),  0x7f)
    expectEqual(UInt128(exactly: UInt8.max), 0xff)
    expectEqual(UInt128(exactly: Int.min),   nil)
    expectEqual(UInt128(exactly: Int.max),   0x7fffffffffffffff)
    expectEqual(UInt128(exactly: UInt.max),  0xffffffffffffffff)
    
    expectEqual(UInt128(clamping: -1),       0)
    expectEqual(UInt128(clamping: Int8.min), 0)
    expectEqual(UInt128(clamping: Int8.max), 0x7f)
    expectEqual(UInt128(clamping: UInt8.max),0xff)
    expectEqual(UInt128(clamping: Int.min),  0)
    expectEqual(UInt128(clamping: Int.max),  0x7fffffffffffffff)
    expectEqual(UInt128(clamping: UInt.max), 0xffffffffffffffff)
  }
}

Int128Tests.test("Bytes and words") {
  if #available(SwiftStdlib 6.0, *) {
    let  ascending: Int128 = 0x0f0e0d0c0b0a09080706050403020100
    let descending: Int128 = 0x000102030405060708090a0b0c0d0e0f
    expectEqual(ascending.byteSwapped, descending)
#if _endian(little)
    expectEqual(ascending.littleEndian, ascending)
    expectEqual(ascending.bigEndian, descending)
#else
    expectEqual(ascending.littleEndian, descending)
    expectEqual(ascending.bigEndian, ascending)
#endif
#if _pointerBitWidth(_32)
    expectEqual(ascending.words[0], 0x03020100)
    expectEqual(ascending.words[3], 0x0f0e0d0c)
#else
    expectEqual(ascending.words[0], 0x0706050403020100)
    expectEqual(ascending.words[1], 0x0f0e0d0c0b0a0908)
#endif
  }
}

Int128Tests.test("Bitwise operations") {
  if #available(SwiftStdlib 6.0, *) {
    let a: UInt128 =   0xffff0000ffff0000_ffff0000ffff0000
    let b: UInt128 =   0xf0e0d0c0b0a09080_7060504030201000
    expectEqual(~a,    0x0000ffff0000ffff_0000ffff0000ffff)
    expectEqual(~b,    0x0f1f2f3f4f5f6f7f_8f9fafbfcfdfefff)
    expectEqual(a & b, 0xf0e00000b0a00000_7060000030200000)
    expectEqual(a | b, 0xffffd0c0ffff9080_ffff5040ffff1000)
    expectEqual(a ^ b, 0x0f1fd0c04f5f9080_8f9f5040cfdf1000)
    
    expectEqual(a &>> 0,   a)
    expectEqual(a &>> 1,   0x7fff80007fff8000_7fff80007fff8000)
    expectEqual(a &>> 2,   0x3fffc0003fffc000_3fffc0003fffc000)
    expectEqual(a &>> 3,   0x1fffe0001fffe000_1fffe0001fffe000)
    expectEqual(a &>> 4,   0x0ffff0000ffff000_0ffff0000ffff000)
    expectEqual(b &>>   8, 0x00f0e0d0c0b0a090_8070605040302010)
    expectEqual(b &>>  16, 0x0000f0e0d0c0b0a0_9080706050403020)
    expectEqual(b &>>  32, 0x00000000f0e0d0c0_b0a0908070605040)
    expectEqual(b &>>  64, 0x0000000000000000_f0e0d0c0b0a09080)
    expectEqual(b &>> 127, 0x0000000000000000_0000000000000001)
    expectEqual(b &>> 128, b)
    
    expectEqual(a &<< 0,   a)
    expectEqual(a &<< 1,   0xfffe0001fffe0001_fffe0001fffe0000)
    expectEqual(a &<< 2,   0xfffc0003fffc0003_fffc0003fffc0000)
    expectEqual(a &<< 3,   0xfff80007fff80007_fff80007fff80000)
    expectEqual(a &<< 4,   0xfff0000ffff0000f_fff0000ffff00000)
    expectEqual(b &<<   8, 0xe0d0c0b0a0908070_6050403020100000)
    expectEqual(b &<<  16, 0xd0c0b0a090807060_5040302010000000)
    expectEqual(b &<<  32, 0xb0a0908070605040_3020100000000000)
    expectEqual(b &<<  64, 0x7060504030201000_0000000000000000)
    expectEqual(b &<< 127, 0x0000000000000000_0000000000000000)
    expectEqual(b &<< 128, b)
    
    expectEqual(a.nonzeroBitCount, 64)
    expectEqual(b.nonzeroBitCount, 32)
    for i in 0 ..< 128 {
      expectEqual((a >> i).leadingZeroBitCount, i)
      expectEqual((~a << i).trailingZeroBitCount, i)
    }
  }
}

Int128Tests.test("Addition and subtraction") {
  if #available(SwiftStdlib 6.0, *) {
    func testCase(
      _ a: UInt128,
      _ b: UInt128,
      _ r: UInt128,
      _ c: Bool,
      line: UInt = #line
    ) {
      
      expectEqual(a.addingReportingOverflow(b), (r, c), line: line)
      expectEqual(r.subtractingReportingOverflow(b), (a, c), line: line)
      expectEqual(a &+ b, r, line: line)
      expectEqual(r &- b, a, line: line)
      if !c {
        expectEqual(a + b, r, line: line)
        expectEqual(r - b, a, line: line)
      }
      
      let sa = Int128(bitPattern: a)
      let sb = Int128(bitPattern: b)
      let sr = Int128(bitPattern: r)
      let o = (sr < sa) != (sb < 0)
      
      expectEqual(sa.addingReportingOverflow(sb), (sr, o), line: line)
      expectEqual(sr.subtractingReportingOverflow(sb), (sa, o), line: line)
      expectEqual(sa &+ sb, sr, line: line)
      expectEqual(sr &- sb, sa, line: line)
      if !o {
        expectEqual(sa + sb, sr, line: line)
        expectEqual(sr - sb, sa, line: line)
      }
    }
    
    testCase(0x22cece8fc3a992208da8556f20cd17b4, 0x5bf62486bf4d907e675fa524340e9d5a, 0x7ec4f31682f7229ef507fa9354dbb50e, false)
    testCase(0xe9584e473c12b6939c5082bc1a6fefb8, 0xff05d7ed3e790d90d5ef9ebcb73d9069, 0xe85e26347a8bc42472402178d1ad8021, true)
    testCase(0x7a798c5a2be3c25d535afa5034515452, 0x0d0ace9628ab60f1daf587b6b67c955c, 0x87845af0548f234f2e508206eacde9ae, false)
    testCase(0xc705e6d4914456f1f2d06da52636e43f, 0xbbe6d539ab4547db2ad744ed53397a35, 0x82ecbc0e3c899ecd1da7b29279705e74, true)
    testCase(0xf430bcb091f4ef9a4d85491e20ba04f8, 0x926a7b2ac49990b5199158bad743b159, 0x869b37db568e804f6716a1d8f7fdb651, true)
    testCase(0xb60e3a058cd6e09533e5e033531cfd51, 0xdb158676610e7781644a11bbbaad0d4b, 0x9123c07bede55816982ff1ef0dca0a9c, true)
    testCase(0xfa89a0decc7cd118ac1d6dc1c2a5551e, 0x4c7b749f2026fb70073991476ae8b14b, 0x4705157deca3cc88b356ff092d8e0669, true)
    testCase(0x28cf1805ce5619540dc3865f3333a1c7, 0x4540a2f2904597acb76aad187dd2bd8c, 0x6e0fbaf85e9bb100c52e3377b1065f53, false)
    testCase(0xfdcdfed4461bb6b5a2f9e820c942a23e, 0x56dbb5736acdf88a7ec689411190b1ee, 0x54a9b447b0e9af4021c07161dad3542c, true)
    testCase(0x7bbaa57afd5f00ab23217777a34a1aeb, 0xc4e44711b56b4225de042bfca852ce05, 0x409eec8cb2ca42d10125a3744b9ce8f0, true)
    testCase(0x1b7c7c6a905f48c0a75bf01a4dba2b15, 0x939db97340982bf994095590073c0273, 0xaf1a35ddd0f774ba3b6545aa54f62d88, false)
    testCase(0xdedff2c264dfccd2911a30751905e0f1, 0x251cb12e47c0e6549816bc8f56a7de47, 0x03fca3f0aca0b3272930ed046fadbf38, true)
    testCase(0x9f6673d7eb5c5d20be751255bfd8aab6, 0x03063926cb83f35999f2b84fec4c75a6, 0xa26cacfeb6e0507a5867caa5ac25205c, false)
    testCase(0x64946c1e78af2b998861717ca84635a7, 0x41a8c07dd680babe0dc1cc2895f8743d, 0xa63d2c9c4f2fe65796233da53e3ea9e4, false)
    testCase(0x1b6a36a075037a69d0801fc3370f7d0c, 0x243d24a92779ca04e96bebe55f4cf489, 0x3fa75b499c7d446eb9ec0ba8965c7195, false)
    testCase(0xbd30fd9f09cc20a80b6bd90d4d56a816, 0x1767f07efe015f0f848325a5efe2ccb0, 0xd498ee1e07cd7fb78feefeb33d3974c6, false)
  }
}

Int128Tests.test("Wide multiplication and division") {
  if #available(SwiftStdlib 6.0, *) {
    func testCase(
      _ a: UInt128,
      _ b: UInt128,
      _ h: UInt128,
      _ l: UInt128,
      line: UInt = #line
    ) {
      expectEqual(a.multipliedFullWidth(by: b), (h, l), line: line)
      expectEqual(a &* b, l, line: line)
      expectEqual(a.dividingFullWidth((high: h, low: l)), (b, 0), line: line)
      let (l1, c) = l.addingReportingOverflow(a &- 1)
      let h1 = h &+ (c ? 1 : 0)
      expectEqual(a.dividingFullWidth((high: h1, low: l1)), (b, a &- 1), line: line)
      let sa = Int128(bitPattern: a)
      let sb = Int128(bitPattern: b)
      let sh = Int128(bitPattern: h) &- (sa < 0 ? sb : 0) &- (sb < 0 ? sa : 0)
      expectEqual(sa.multipliedFullWidth(by: sb), (sh, l), line: line)
      expectEqual(sa &* sb, Int128(bitPattern: l), line: line)
      expectEqual(sa.dividingFullWidth((high: sh, low: l)), (sb, 0), line: line)
      let rem = sa.magnitude &- 1
      if sh < 0 {
        let (l1, c) = l.subtractingReportingOverflow(rem)
        let h1 = sh &- (c ? 1 : 0)
        expectEqual(sa.dividingFullWidth((high: h1, low: l1)), (sb, -Int128(rem)), line: line)
      } else {
        let (l1, c) = l.addingReportingOverflow(rem)
        let h1 = sh &+ (c ? 1 : 0)
        expectEqual(sa.dividingFullWidth((high: h1, low: l1)), (sb, Int128(rem)), line: line)
      }
    }
    
    testCase(0x14f6c7bb051951bc8f36082753d0ad15, 0xdba081b2a9cde4e20c6778833e3795dd, 0x11fc41b8ce63bc91660b4e6b14214280, 0xc0c6e5ba6909c6bdaf80e33b1565a421)
    testCase(0x4e0d20760a881de82733cea44f83b686, 0xff6f66e9e75392e824ff2eca4b114ad4, 0x4de10a6733150dc755f9f237e920c12d, 0x2d3e5a1b8508cace5d0b97024cbbe2f8)
    testCase(0x2e79ffe8593c337e6711ce338d24e68e, 0x0865ae2d496547bef886cbd8cd01a507, 0x018645c05e2a2bfc986d97269c67548c, 0x2d7cc7781078868df9e718f64129d3e2)
    testCase(0xb87ba0a48cef1ec31e29e9fdcfe2df98, 0x9562e44cdf7d2b477a13fab32c518a8a, 0x6ba73858c5d9434d32fe784903ab229f, 0x1f3fad1dd5d313f4760981613bec77f0)
    testCase(0x9e592a6a645a5a122f2d8d91f0fd4a41, 0xe47f8b71ee5f8d6dcd106a0a948cd06c, 0x8d564e43aefdbe7ee9c60f4be7a313fd, 0x7ff590f409e22d36b114575ba6bc236c)
    testCase(0x9b729e658174948344571ef72b076a27, 0x6974e6724777c407350cee21ff34c571, 0x4008fed5c6d6b87d6f8fe810feb01738, 0xbeb752f23799535430d2f07c1be1de37)
    testCase(0xba8a7af31e765b55d6c25fe76afa6e07, 0x0be8692c259620beb17be8b4841dfda6, 0x08ad4d6ad640f33caf27840505980f1d, 0x7618d3e7bfedd9a16b5ad8ceefeb438a)
    testCase(0xe5cd79731d8679e5e42ec8202a94d8dc, 0xa6ce214fa3403650c9e18694a06cda68, 0x95bc45ff5962bf0a20cb37f6e1f43e74, 0xc994fba22acbcb3eee8836d909f37160)
    testCase(0xeb220b552e8fe3f9b132220667a22feb, 0x0becf5688f15a28f25a35c735a84dec7, 0x0af41b46b9a01f0b0d307b1f8fdd81ce, 0xa11503513c983a2753eb6fe387cd09ad)
    testCase(0x5d169627de62266ddbc59265a154a308, 0x42ca365c49a2c5e15014254b2152a827, 0x18495a4d155bc7a7f7e315f7c6bdaa0d, 0x43ba59ef45fee3ccdd8180d35f721638)
    testCase(0x74c28baa7abcef0ea57e0c83cc235eeb, 0xc2cd3db0b1fda551418574c229518cf8, 0x58d909c3fabd4d2348cc26c441ee3ee9, 0x8108c4836103e2ca0740decbc58777a8)
    testCase(0x9fc1c89a4461a31e84eb3efe62d99eb8, 0x82c936fe8a96d4875ffb2b24b3994035, 0x519df95690eb6f4aac34ff7f7962ce45, 0x8a1753bb4bf2aade34461b4b62b3dc18)
    testCase(0xa98fd77674c6933fd8092428742d1365, 0x7dc38f365a41e190913d42048216dce3, 0x534cc3a14fbabc0c6bde9041864620cf, 0xa324e403eec48789556c0b02b550fe8f)
    testCase(0x970ec5575cbe7d2ce36963a03c8c9b67, 0x4bcad32daf8b4d550ee233b364ca5c48, 0x2cb9021470effa54bb93f9e9ecdafef2, 0x8fdbf661c079405b35bc7c7f6aaab8f8)
    testCase(0x7600cffa32b6e9cf26b67a223ef199f0, 0x81dde6c0a4c20f27f46bee142b5d1d3b, 0x3bdcb1de25f4b1c5f4a752037488e46d, 0x3b388b97a7358b645634a0661c4eaa50)
    testCase(0x66bf68e43bb06f1518298f5b3f44dfd2, 0x2aae55e5a54ca26fb9065285487add11, 0x11215fc8765040c025ded1c61f6bf4e2, 0xce48257020acb07866315c8d62df26f2)
  }
}

Int128Tests.test("Narrow multiplication and division") {
  if #available(SwiftStdlib 6.0, *) {
    func testCase(
      _ a: UInt128,
      _ b: UInt128,
      _ q: UInt128,
      _ r: UInt128,
      line: UInt = #line
    ) {
      expectEqual(a, b*q + r, line: line)
      expectEqual(a, b&*q &+ r, line: line)
      expectEqual(q, a/b, line: line)
      expectEqual(r, a%b, line: line)
      expectEqual((q,r), a.quotientAndRemainder(dividingBy: b), line: line)
      for sgn in 0 ..< 4 {
        let sa = (sgn & 1 == 0 ? 1 : -1) * Int128(bitPattern: a)
        let sb = (sgn & 2 == 0 ? 1 : -1) * Int128(bitPattern: b)
        let sq = (sgn == 0 || sgn == 3 ? 1 : -1) * Int128(bitPattern: q)
        let sr = (sgn & 1 == 0 ? 1 : -1) * Int128(bitPattern: r)
        expectEqual(sa, sb*sq + sr, line: line)
        expectEqual(sa, sb&*sq &+ sr, line: line)
        expectEqual(sq, sa/sb, line: line)
        expectEqual(sr, sa%sb, line: line)
        expectEqual((sq,sr), sa.quotientAndRemainder(dividingBy: sb), line: line)
      }
    }
    testCase(0x2d,                               0x1,                        0x2d,                              0x0)
    testCase(0x809b,                             0x3f,                       0x20a,                             0x25)
    testCase(0x1ffe1f,                           0xb73df,                    0x2,                               0x91661)
    testCase(0x6b324345,                         0x2ff256,                   0x23c,                             0x10cb1d)
    testCase(0x8b1339b357,                       0x31e,                      0x2c9d960a,                        0x2b)
    testCase(0x2517b04df6ce,                     0x23abc4a,                  0x10a33c,                          0x6b776)
    testCase(0x21e7e8fa820294,                   0x880fb,                    0x3fcb4bafa,                       0x1af76)
    testCase(0xde4770ddf9e0f3d3,                 0x65af7,                    0x22f9a7842600,                    0x449d3)
    testCase(0x33f29c91b6bc266b83,               0xc7,                       0x42d3c2fbc5c50466,                0x39)
    testCase(0xc5b6d1764958768637e9,             0x1a,                       0x79ab94978f98e679fb0,             0x9)
    testCase(0x45b7f2f79a6d80e6c51aca,           0x16322b389a60ca4,          0x3241b89a,                        0x8e20ef8227a022)
    testCase(0x917f58ede4323d3ba84bd18d,         0x604e0,                    0x182c40337c3375ba4a05,            0x4f92d)
    testCase(0x82f81933bff7f5eac96a153a7d,       0x5dfd04eff918d9c8e73bc217, 0x164,                             0x443e560991670f67dafb5281)
    testCase(0x1c2f66fc69f41900b99513223c17,     0x958363a31680f9c30,        0x3042609f065,                     0x844cb780cc4dc9d27)
    testCase(0x35fd60b643a620e916d79ed68f6456,   0x7ecdc2aa76f2b195896e3a0d, 0x6cff88,                          0x442240b30e2e91c88ab19a6e)
    testCase(0x4e1d078976ed907b2b06ec3ab6c09750, 0x17,                       0x3656fa1cd84c37fca37f4028d82ceed, 0x5)
  }
}

Int128Tests.test("String roundtrip") {
  if #available(SwiftStdlib 6.0, *) {
    let values: [UInt128] = [
      0x8000ffff0000ffff_0000ffff0000ffff,
      0x80f0e0d0c0b0a090_8070605040302010,
      0x0000000000000000_ffffffffffffffff,
      0xffffffffffffffff_ffffffffffffffff
    ]
    for a in values {
      expectEqual(a, UInt128(String(a)))
    }
    for a in values.map(Int128.init(bitPattern:)) {
      expectEqual(a, Int128(String(a)))
    }
  }
}
