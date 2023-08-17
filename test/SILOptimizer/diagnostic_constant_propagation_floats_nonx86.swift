// RUN: %target-swift-frontend -emit-sil -primary-file %s -o /dev/null -verify
//
// REQUIRES: !(CPU=i386 || CPU=x86_64)
//
// These are tests for diagnostics produced by constant propagation pass
// on floating-point operations that are specific to non-x86 architectures,
// which do not support Float80.

import StdlibUnittest

func testFPToIntConversion() {
  _blackHole(Int8(1E309)) // expected-error {{invalid conversion: '1E309' overflows 'Int8'}}
                          // expected-warning@-1 {{'1E309' overflows to inf because its magnitude exceeds the limits of a float literal}}

  _blackHole(UInt8(-1E309)) // expected-error {{negative literal '-1E309' cannot be converted to 'UInt8'}}
                            // expected-warning@-1 {{'-1E309' overflows to -inf because its magnitude exceeds the limits of a float literal}}

  _blackHole(Int64(1E309)) // expected-error {{invalid conversion: '1E309' overflows 'Int64'}}
                           // expected-warning@-1 {{'1E309' overflows to inf because its magnitude exceeds the limits of a float literal}}

  _blackHole(UInt64(-1E309)) // expected-error {{negative literal '-1E309' cannot be converted to 'UInt64'}}
                             // expected-warning@-1 {{'-1E309' overflows to -inf because its magnitude exceeds the limits of a float literal}}
}

func testFloatConvertOverflow() {
  let f1: Float = 1E309 // expected-warning {{'1E309' overflows to inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(f1)
  let f2: Float32 = -1.0E999 // expected-warning {{'-1.0E999' overflows to -inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(f2)

  let d4: Double = 1E309 // expected-warning {{'1E309' overflows to inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(d4)
  let d6: Float64 = -1.0E999 // expected-warning {{'-1.0E999' overflows to -inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(d6)
  let d8: Float64 = -1.7976931348623159E+308 // expected-warning {{'-1.7976931348623159E+308' overflows to -inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(d8)

  // Warnings cannot be suppressed when a literal overflows MaxBuiltinFloatType.
  _blackHole(Float(1E309)) // expected-warning {{'1E309' overflows to inf because its magnitude exceeds the limits of a float literal}}
  _blackHole(Double(1E309)) // expected-warning {{'1E309' overflows to inf because its magnitude exceeds the limits of a float literal}}
}

func testFloatConvertUnderflow() {
  // FIXME: False Negative: The literals that underflow MaxBuiltinFloatType
  // are not detected.
  let f1: Float = 1E-400
  _blackHole(f1)
  let d2: Double = 1E-309
  _blackHole(d2)
  let d4: Double = 5E-324
  _blackHole(d4)

  // All warnings are disabled during explicit conversions.
  _blackHole(Float(1E-400))
  _blackHole(Double(1E-309))
  _blackHole(Double(5E-324))
}

func testHexFloatImprecision() {
  // FIXME: False Negative: literals that underflow MaxBuiltinFloatType are
  // not detected.
  let d3: Double = 0x1.0000000000001p-1023
  _blackHole(d3)
  let d4: Double = 0x1.00000000000001p-1000
  _blackHole(d4)

  // All warnings are disabled during explicit conversions.
  _blackHole(Float(0x1.00000000000001p-127))
  _blackHole(Float(0x1.0000000000001p-1023))
  _blackHole(Double(0x1.0000000000001p-1023))
  _blackHole(Double(0x1.00000000000001p-1000))
}
