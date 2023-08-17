// RUN: %target-swift-frontend -emit-sil -primary-file %s -o /dev/null -verify
//
// These are tests for diagnostics produced by constant propagation pass
// on floating-point operations.

import StdlibUnittest

func testFPToIntConversion() {
  _blackHole(Int8(-1.28E2))
  _blackHole(Int8(-128.5)) // the result is -128 and is not an overflow
  _blackHole(Int8(1.27E2))
  _blackHole(Int8(-0))
  _blackHole(Int8(3.33333))
  _blackHole(Int8(-2E2)) // expected-error {{invalid conversion: '-2E2' overflows 'Int8'}}

  _blackHole(UInt8(2E2))
  _blackHole(UInt8(3E2)) // expected-error {{invalid conversion: '3E2' overflows 'UInt8'}}
  _blackHole(UInt8(-0E0))
  _blackHole(UInt8(-2E2)) // expected-error {{negative literal '-2E2' cannot be converted to 'UInt8'}}

  _blackHole(Int8(1E6000)) // expected-error {{invalid conversion: '1E6000' overflows 'Int8'}}
                           // expected-warning@-1 {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}

  _blackHole(UInt8(1E6000)) // expected-error {{invalid conversion: '1E6000' overflows 'UInt8'}}
                            // expected-warning@-1 {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}

  _blackHole(Int16(3.2767E4))
  _blackHole(Int16(3.2768E4)) // expected-error {{invalid conversion: '3.2768E4' overflows 'Int16'}}
  _blackHole(Int16(-4E4)) // expected-error {{invalid conversion: '-4E4' overflows 'Int16'}}

  _blackHole(UInt16(6.5535E4))
  _blackHole(UInt16(6.5536E4)) // expected-error {{invalid conversion: '6.5536E4' overflows 'UInt16'}}
  _blackHole(UInt16(7E4)) // expected-error {{invalid conversion: '7E4' overflows 'UInt16'}}
  _blackHole(UInt16(-0E0))
  _blackHole(UInt16(-2E2)) // expected-error {{negative literal '-2E2' cannot be converted to 'UInt16'}}

  _blackHole(Int32(-2.147483648E9))
  _blackHole(Int32(-2.147483649E9)) // expected-error {{invalid conversion: '-2.147483649E9' overflows 'Int32'}}
  _blackHole(Int32(3E9)) // expected-error {{invalid conversion: '3E9' overflows 'Int32'}}

  _blackHole(UInt32(4.294967295E9))
  _blackHole(UInt32(4.294967296E9)) // expected-error {{invalid conversion: '4.294967296E9' overflows 'UInt32'}}
  _blackHole(UInt32(5E9)) // expected-error {{invalid conversion: '5E9' overflows 'UInt32'}}
  _blackHole(UInt32(-0E0))
  _blackHole(UInt32(-2E2)) // expected-error {{negative literal '-2E2' cannot be converted to 'UInt32'}}

  _blackHole(Int64(9.223372036854775E18))

  // A case where the imprecision due to the implicit conversion of
  // float literals to 'Double' results in an overflow.
  _blackHole(Int64(9.223372036854775807E18)) // expected-error {{invalid conversion: '9.223372036854775807E18' overflows 'Int64'}}

  // A case where implicit conversion of the float literal to 'Double'
  // elides an overflow that one would expect during conversion to 'Int64'.
  _blackHole(Int64(-9.223372036854775809E18))

  // Cases of definite overflow.
  _blackHole(Int64(9.223372036854775808E18)) // expected-error {{invalid conversion: '9.223372036854775808E18' overflows 'Int64'}}
  _blackHole(Int64(1E19)) // expected-error {{invalid conversion: '1E19' overflows 'Int64'}}

  // A case where implicit conversion of the float literal to 'Double'
  // results in an overflow during conversion to 'UInt64''.
  _blackHole(UInt64(1.844674407370955E19))
  _blackHole(UInt64(1.8446744073709551615E19)) // expected-error {{invalid conversion: '1.8446744073709551615E19' overflows 'UInt64'}}

  _blackHole(UInt64(2E19)) // expected-error {{invalid conversion: '2E19' overflows 'UInt64'}}
  _blackHole(UInt64(-0E0))
  _blackHole(UInt64(-2E2)) // expected-error {{negative literal '-2E2' cannot be converted to 'UInt64'}}

  _blackHole(Int64(1E6000)) // expected-error {{invalid conversion: '1E6000' overflows 'Int64'}}
                            // expected-warning@-1 {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}

  _blackHole(UInt64(1E6000)) // expected-error {{invalid conversion: '1E6000' overflows 'UInt64'}}
                             // expected-warning@-1 {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}
}

func testFloatConvertOverflow() {
  let f1: Float = 1E38
  _blackHole(f1)
  let f2: Float = 1E39 // expected-warning {{'1E39' overflows to inf during conversion to 'Float'}}
  _blackHole(f2)
  let f3: Float = 1234567891012345678912345671234561234512.0 // expected-warning {{'1234567891012345678912345671234561234512.0' overflows to inf during conversion to 'Float'}}
  _blackHole(f3)
  let f4: Float = 0.1234567891012345678912345671234561234512
  _blackHole(f4)
  let f5: Float32 = -3.4028236E+38 // expected-warning {{'-3.4028236E+38' overflows to -inf during conversion to 'Float32' (aka 'Float')}}
  _blackHole(f5)

  // Diagnostics for Double truncations have architecture dependent
  // messages. See _nonx86 and _x86 test files.
  let d1: Double = 1E308
  _blackHole(d1)
  let d2: Double = 1234567891012345678912345671234561234512.0
  _blackHole(d2)

  // All warnings are disabled during explicit conversions.
  // Except when the number is so large that it wouldn't even fit into largest
  // FP type available.
  _blackHole(Float(1E38))
  _blackHole(Float(1E39))
  _blackHole(Float(100000000000000000000000000000000000000000000000.0))
  _blackHole(Double(1E308))
  _blackHole(Float(1E6000)) // expected-warning {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(Float(-1E6000)) // expected-warning {{'-1E6000' overflows to -inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(Double(1E6000)) // expected-warning {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}
}

func testFloatConvertUnderflow() {
  let f0: Float =  0.500000006364665322827
  _blackHole(f0)
  let f1: Float = 1E-37
  _blackHole(f1)
  let f2: Float = 1E-39  // expected-warning {{'1E-39' underflows and loses precision during conversion to 'Float'}}
  _blackHole(f2)
  let f3: Float = 1E-45 // expected-warning {{'1E-45' underflows and loses precision during conversion to 'Float'}}
  _blackHole(f3)

  // A number close to 2^-150 (smaller than least non-zero float: 2^-149)
  let f6: Float = 7.0064923E-46  // expected-warning {{'7.0064923E-46' underflows and loses precision during conversion to 'Float'}}
  _blackHole(f6)

  // Some cases where tininess doesn't cause extra imprecision.

  // A number so close to 2^-130 that 2^-130 is its best approximation
  // even in Float80.
  let f4: Float = 7.3468396926392969248E-40
  _blackHole(f4)
  // A number very close to 2^-149.
  let f5: Float = 1.4012984821624085566E-45
  _blackHole(f5)

  let f7: Float = 1.1754943E-38 // expected-warning {{'1.1754943E-38' underflows and loses precision during conversion to 'Float'}}
  _blackHole(f7)
  let f8: Float = 1.17549428E-38 // expected-warning {{'1.17549428E-38' underflows and loses precision during conversion to 'Float'}}
  _blackHole(f8)

  let d1: Double = 1E-307
  _blackHole(d1)

   // All warnings are disabled during explicit conversions.
  _blackHole(Float(1E-37))
  _blackHole(Float(1E-39))
  _blackHole(Float(1E-45))
  _blackHole(Double(1E-307))
}

func testHexFloatImprecision() {
  let f1: Float = 0x0.800000p-126
  _blackHole(f1)
  // Smallest Float subnormal number.
  let f2: Float = 0x0.000002p-126
  _blackHole(f2)
  let f3: Float = 0x1.000002p-127 // expected-warning {{'0x1.000002p-127' loses precision during conversion to 'Float'}}
  _blackHole(f3)
  let f4: Float = 0x1.000001p-127 // expected-warning {{'0x1.000001p-127' loses precision during conversion to 'Float'}}
  _blackHole(f4)
  let f5: Float = 0x1.0000002p-126 // expected-warning {{'0x1.0000002p-126' loses precision during conversion to 'Float'}}
  _blackHole(f5)

  // In the following cases, the literal is truncated to a Float through a
  // (lossless) conversion to Double. There should be no warnings here.
  let t1: Double = 0x1.0000002p-126
  _blackHole(Float(t1))
  let t2: Double = 0x1.000001p-126
  _blackHole(Float(t2))
  let t3 = 0x1.000000fp25
  _blackHole(Float(t3))

  let d1: Double = 0x0.8p-1022
  _blackHole(d1)
  // Smallest non-zero number representable in Double.
  let d2: Double = 0x0.0000000000001p-1022
  _blackHole(d2)
  let d3: Double = 0x1p-1074
  _blackHole(d3)

  // Test the case where conversion results in subnormality in the destination.
  let d4: Float = 0x1p-149
  _blackHole(d4)
  let d5: Float = 0x1.8p-149 // expected-warning {{'0x1.8p-149' loses precision during conversion to 'Float}}
  _blackHole(d5)

  // All warnings are disabled during explicit conversions.
  _blackHole(Float(0x1.000002p-126))
  _blackHole(Float(0x1.0000002p-126))
  _blackHole(Float(0x1.000002p-127))
  _blackHole(Float(0x1.000001p-127))
  _blackHole(Float(Double(0x1.000000fp25)))
  _blackHole(Double(0x1p-1074))
}

func testFloatArithmetic() {
  // Ignore inf and Nan during arithmetic operations.
  // This may become a warning in the future.
  let infV: Float = 3.0 / 0.0
  _blackHole(infV)

  let a: Float = 1E38
  let b: Float = 10.0
  _blackHole(a * b)
}

func testIntToFloatConversion() {
  let f1: Float = 16777216
  _blackHole(f1)

  let f2: Float = 1_000_000_000_000 // expected-warning {{'1000000000000' is not exactly representable as 'Float'; it becomes '999999995904'}}
  _blackHole(f2)

  // First positive integer that cannot be precisely represented in Float: 2^24 + 1
  let f3: Float = 16777217 // expected-warning {{'16777217' is not exactly representable as 'Float'; it becomes '16777216'}}
  _blackHole(f3)

  let d1: Double = 9_007_199_254_740_992 // This value is 2^53
  _blackHole(d1)

  let d2: Double = 9_007_199_254_740_993 // expected-warning {{'9007199254740993' is not exactly representable as 'Double'; it becomes '9007199254740992'}}
  _blackHole(d2)

   // No warnings are emitted for conversion through explicit constructor calls.
  _blackHole(Float(16777217))
  _blackHole(Double(2_147_483_647))
}
