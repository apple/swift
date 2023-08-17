// RUN: %target-swift-frontend -emit-sil -primary-file %s -o /dev/null -verify
//
// REQUIRES: CPU=i386 || CPU=x86_64
// UNSUPPORTED: OS=windows-msvc
//
// These are tests for diagnostics produced by constant propagation pass
// on floating-point operations that are specific to x86 architectures,
// which support Float80.

import StdlibUnittest

func testFPToIntConversion() {
  let i64max: Float80 = 9.223372036854775807E18
  _blackHole(Int64(i64max))

  let i64overflow: Float80 = -9.223372036854775809E18
  _blackHole(Int64(i64overflow)) // expected-error {{invalid conversion: '-9.223372036854775809E18' overflows 'Int64'}}

  let j: Float80 = 9.223372036854775808E18
  _blackHole(Int64(j)) // expected-error {{invalid conversion: '9.223372036854775808E18' overflows 'Int64'}}

  let u64max: Float80 = 1.8446744073709551615E19
  _blackHole(UInt64(u64max))

  let uj: Float80 = 1.8446744073709551616E19
  _blackHole(UInt64(uj)) // expected-error {{invalid conversion: '1.8446744073709551616E19' overflows 'UInt64'}}

  _blackHole(Int8(1E308))   // expected-error {{invalid conversion: '1E308' overflows 'Int8'}}
  _blackHole(UInt8(-1E308)) // expected-error {{negative literal '-1E308' cannot be converted to 'UInt8'}}
  _blackHole(Int64(1E308))  // expected-error {{invalid conversion: '1E308' overflows 'Int64'}}
  _blackHole(UInt64(-1E308)) // expected-error {{negative literal '-1E308' cannot be converted to 'UInt64'}}
}

func testFloatConvertOverflow() {
  let f1: Float = 1E309 // expected-warning {{'1E309' overflows to inf during conversion to 'Float'}}
  let f2: Float32 = -1.0E999 // expected-warning {{'-1.0E999' overflows to -inf during conversion to 'Float32' (aka 'Float')}}
  _blackHole(f1)
  _blackHole(f2)

  let d4: Double = 1E309  // expected-warning {{'1E309' overflows to inf during conversion to 'Double'}}
  let d6: Float64 = -1.0E999  // expected-warning {{'-1.0E999' overflows to -inf during conversion to 'Double'}}
  let d8: Float64 = -1.7976931348623159E+308 // expected-warning {{'-1.7976931348623159E+308' overflows to -inf during conversion to 'Double'}}
  _blackHole(d4)
  _blackHole(d6)
  _blackHole(d8)

  let e1: Float80 = 1E6000 // expected-warning {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}
  let e2: Float80 = 1.18973149535723176515E4932 // expected-warning {{'1.18973149535723176515E4932' overflows to inf because its magnitude exceeds the limits of a float literal}}
  let e3: Float80 = -1.18973149535723176515E4932 // expected-warning {{'-1.18973149535723176515E4932' overflows to -inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(e1)
  _blackHole(e2)
  _blackHole(e3)

  // All warnings are disabled during explicit conversions, except when the
  // input literal overflows the largest available FP type.
  _blackHole(Float(1E309))
  _blackHole(Double(1E309))
  _blackHole(Float80(1E6000)) // expected-warning {{'1E6000' overflows to inf because its magnitude exceeds the limits of a float literal}}
}

func testFloatConvertUnderflow() {
  let f1: Float = 1E-400 // expected-warning {{'1E-400' underflows and loses precision during conversion to 'Float'}}
  _blackHole(f1)

  let d2: Double = 1E-309  // expected-warning {{'1E-309' underflows and loses precision during conversion to 'Double'}}
  _blackHole(d2)
  let d4: Double = 5E-324  // expected-warning {{'5E-324' underflows and loses precision during conversion to 'Double'}}
  _blackHole(d4)

  let e4: Float80 = 0x1p-16445
  _blackHole(e4)

  // FIXME: if a number is so tiny that it underflows even Float80,
  // nothing is reported
  let e1: Float80 = 0x1p-16446
  _blackHole(e1)

  // Test the case where conversion results in subnormality in the destination.
  let e2: Double = 0x1p-1074
  _blackHole(e2)
  let e3: Double = 0x11p-1074 // expected-warning {{'0x11p-1074' underflows and loses precision during conversion to 'Double'}}
  _blackHole(e3)

  // All warnings are disabled during explicit conversions
  _blackHole(Float(1E-400))
  _blackHole(Double(1E-309))
  _blackHole(Double(5E-324))
  _blackHole(Float80(1E-400))
}

func testHexFloatImprecision() {
  let d3: Double = 0x1.0000000000001p-1023  // expected-warning {{'0x1.0000000000001p-1023' loses precision during conversion to 'Double'}}
  _blackHole(d3)
  let d4: Double = 0x1.00000000000001p-1000  // expected-warning {{'0x1.00000000000001p-1000' loses precision during conversion to 'Double'}}
  _blackHole(d4)

  // FIXME: if a number is so tiny that it underflows even Float80,
  // nothing is reported
  let e1: Float80 = 0x1p-16446
  _blackHole(e1)
  _blackHole(Float80(0x1p-16446))

  // All warnings are disabled during explicit conversions.
  _blackHole(Float(0x1.00000000000001p-127))
  _blackHole(Float(0x1.0000000000001p-1023))
  _blackHole(Double(0x1.0000000000001p-1023))
  _blackHole(Double(0x1.00000000000001p-1000))
  _blackHole(Float80(0x1p-1075))
}

func testIntToFloatConversion() {
  let e1: Float80 =  18_446_744_073_709_551_616 // This value is 2^64
  _blackHole(e1)

  let e2: Float80 =  18_446_744_073_709_551_617 // expected-warning {{'18446744073709551617' is not exactly representable as 'Float80'; it becomes '18446744073709551616'}}
  _blackHole(e2)

  _blackHole(Float80(18_446_744_073_709_551_617)) // Ok
}
