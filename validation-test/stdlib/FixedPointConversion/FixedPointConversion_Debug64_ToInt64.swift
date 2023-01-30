//===----------------------------------------------------------------------===//
//
// Automatically Generated From ./Inputs/FixedPointConversion.swift.gyb
// Do Not Edit Directly!
//
//===----------------------------------------------------------------------===//
//
// REQUIRES: executable_test
// REQUIRES: PTRSIZE=64
// RUN: %target-run-simple-swift(-Onone)
//
// rdar://104232602
// UNSUPPORTED: CPU=x86_64 && (DARWIN_SIMULATOR=ios || DARWIN_SIMULATOR=watchos || DARWIN_SIMULATOR=tvos || OS=linux-gnu)
//
// END.
//
//===----------------------------------------------------------------------===//

import StdlibUnittest

let FixedPointConversion_Debug64_ToInt64 = TestSuite(
   "FixedPointConversion_Debug64_ToInt64"
)

//===----------------------------------------------------------------------===//
// MARK: UInt8: (+0)...(+255)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromUInt8_NeverTraps")
.forEach(in: [
  (getInt64(+0), getUInt8(+0)),
  (getInt64(+255), getUInt8(+255)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt8_NeverFails")
.forEach(in: [
  (getInt64(+0), getUInt8(+0)),
  (getInt64(+255), getUInt8(+255)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: Int8: (-128)...(+127)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromInt8_NeverTraps")
.forEach(in: [
  (getInt64(-128), getInt8(-128)),
  (getInt64(+0), getInt8(+0)),
  (getInt64(+127), getInt8(+127)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromInt8_NeverFails")
.forEach(in: [
  (getInt64(-128), getInt8(-128)),
  (getInt64(+0), getInt8(+0)),
  (getInt64(+127), getInt8(+127)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: UInt16: (+0)...(+65535)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromUInt16_NeverTraps")
.forEach(in: [
  (getInt64(+0), getUInt16(+0)),
  (getInt64(+65535), getUInt16(+65535)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt16_NeverFails")
.forEach(in: [
  (getInt64(+0), getUInt16(+0)),
  (getInt64(+65535), getUInt16(+65535)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: Int16: (-32768)...(+32767)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromInt16_NeverTraps")
.forEach(in: [
  (getInt64(-32768), getInt16(-32768)),
  (getInt64(+0), getInt16(+0)),
  (getInt64(+32767), getInt16(+32767)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromInt16_NeverFails")
.forEach(in: [
  (getInt64(-32768), getInt16(-32768)),
  (getInt64(+0), getInt16(+0)),
  (getInt64(+32767), getInt16(+32767)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: UInt32: (+0)...(+4294967295)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromUInt32_NeverTraps")
.forEach(in: [
  (getInt64(+0), getUInt32(+0)),
  (getInt64(+4294967295), getUInt32(+4294967295)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt32_NeverFails")
.forEach(in: [
  (getInt64(+0), getUInt32(+0)),
  (getInt64(+4294967295), getUInt32(+4294967295)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: Int32: (-2147483648)...(+2147483647)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromInt32_NeverTraps")
.forEach(in: [
  (getInt64(-2147483648), getInt32(-2147483648)),
  (getInt64(+0), getInt32(+0)),
  (getInt64(+2147483647), getInt32(+2147483647)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromInt32_NeverFails")
.forEach(in: [
  (getInt64(-2147483648), getInt32(-2147483648)),
  (getInt64(+0), getInt32(+0)),
  (getInt64(+2147483647), getInt32(+2147483647)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: UInt64: (+0)...(+18446744073709551615)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromUInt64_NeverTraps")
.forEach(in: [
  (getInt64(+0), getUInt64(+0)),
  (getInt64(+9223372036854775807), getUInt64(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt64_NeverFails")
.forEach(in: [
  (getInt64(+0), getUInt64(+0)),
  (getInt64(+9223372036854775807), getUInt64(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt64_AlwaysTraps")
.forEach(in: [
  getUInt64(+9223372036854775808),
  getUInt64(+18446744073709551615),
]) {
  expectCrashLater()
  _blackHole(Int64($0))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt64_AlwaysFails")
.forEach(in: [
  getUInt64(+9223372036854775808),
  getUInt64(+18446744073709551615),
]) {
  expectNil(Int64(exactly: $0))
}

//===----------------------------------------------------------------------===//
// MARK: Int64: (-9223372036854775808)...(+9223372036854775807)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromInt64_NeverTraps")
.forEach(in: [
  (getInt64(-9223372036854775808), getInt64(-9223372036854775808)),
  (getInt64(+0), getInt64(+0)),
  (getInt64(+9223372036854775807), getInt64(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromInt64_NeverFails")
.forEach(in: [
  (getInt64(-9223372036854775808), getInt64(-9223372036854775808)),
  (getInt64(+0), getInt64(+0)),
  (getInt64(+9223372036854775807), getInt64(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: UInt: (+0)...(+18446744073709551615)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromUInt_NeverTraps")
.forEach(in: [
  (getInt64(+0), getUInt(+0)),
  (getInt64(+9223372036854775807), getUInt(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt_NeverFails")
.forEach(in: [
  (getInt64(+0), getUInt(+0)),
  (getInt64(+9223372036854775807), getUInt(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt_AlwaysTraps")
.forEach(in: [
  getUInt(+9223372036854775808),
  getUInt(+18446744073709551615),
]) {
  expectCrashLater()
  _blackHole(Int64($0))
}

FixedPointConversion_Debug64_ToInt64
.test("FromUInt_AlwaysFails")
.forEach(in: [
  getUInt(+9223372036854775808),
  getUInt(+18446744073709551615),
]) {
  expectNil(Int64(exactly: $0))
}

//===----------------------------------------------------------------------===//
// MARK: Int: (-9223372036854775808)...(+9223372036854775807)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromInt_NeverTraps")
.forEach(in: [
  (getInt64(-9223372036854775808), getInt(-9223372036854775808)),
  (getInt64(+0), getInt(+0)),
  (getInt64(+9223372036854775807), getInt(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromInt_NeverFails")
.forEach(in: [
  (getInt64(-9223372036854775808), getInt(-9223372036854775808)),
  (getInt64(+0), getInt(+0)),
  (getInt64(+9223372036854775807), getInt(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

//===----------------------------------------------------------------------===//
// MARK: Float16: (-2047)...(+2047)
//===----------------------------------------------------------------------===//

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
if #available(SwiftStdlib 5.3, *) {

FixedPointConversion_Debug64_ToInt64
.test("FromFloat16_NeverTraps")
.forEach(in: [
  (getInt64(-2047), getFloat16(-2047)),
  (getInt64(-128), getFloat16(-128.5)),
  (getInt64(+0), getFloat16(-0.5)),
  (getInt64(+0), getFloat16(+0)),
  (getInt64(+0), getFloat16(-0)),
  (getInt64(+0), getFloat16(+0.5)),
  (getInt64(+127), getFloat16(+127.5)),
  (getInt64(+255), getFloat16(+255.5)),
  (getInt64(+2047), getFloat16(+2047)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat16_NeverFails")
.forEach(in: [
  (getInt64(-2047), getFloat16(-2047)),
  (getInt64(+0), getFloat16(+0)),
  (getInt64(+0), getFloat16(-0)),
  (getInt64(+2047), getFloat16(+2047)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat16_AlwaysTraps")
.forEach(in: [
  getFloat16(-.infinity),
  getFloat16(-.nan),
  getFloat16(-.signalingNaN),
  getFloat16(+.infinity),
  getFloat16(+.nan),
  getFloat16(+.signalingNaN),
]) {
  expectCrashLater()
  _blackHole(Int64($0))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat16_AlwaysFails")
.forEach(in: [
  getFloat16(-128.5),
  getFloat16(-0.5),
  getFloat16(+0.5),
  getFloat16(+127.5),
  getFloat16(+255.5),
  getFloat16(-.infinity),
  getFloat16(-.nan),
  getFloat16(-.signalingNaN),
  getFloat16(+.infinity),
  getFloat16(+.nan),
  getFloat16(+.signalingNaN),
]) {
  expectNil(Int64(exactly: $0))
}

}
#endif // Float16

//===----------------------------------------------------------------------===//
// MARK: Float32: (-16777215)...(+16777215)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromFloat32_NeverTraps")
.forEach(in: [
  (getInt64(-16777215), getFloat32(-16777215)),
  (getInt64(-32768), getFloat32(-32768.5)),
  (getInt64(-128), getFloat32(-128.5)),
  (getInt64(+0), getFloat32(-0.5)),
  (getInt64(+0), getFloat32(+0)),
  (getInt64(+0), getFloat32(-0)),
  (getInt64(+0), getFloat32(+0.5)),
  (getInt64(+127), getFloat32(+127.5)),
  (getInt64(+255), getFloat32(+255.5)),
  (getInt64(+32767), getFloat32(+32767.5)),
  (getInt64(+65535), getFloat32(+65535.5)),
  (getInt64(+16777215), getFloat32(+16777215)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat32_NeverFails")
.forEach(in: [
  (getInt64(-16777215), getFloat32(-16777215)),
  (getInt64(+0), getFloat32(+0)),
  (getInt64(+0), getFloat32(-0)),
  (getInt64(+16777215), getFloat32(+16777215)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat32_AlwaysTraps")
.forEach(in: [
  getFloat32(-.infinity),
  getFloat32(-.nan),
  getFloat32(-.signalingNaN),
  getFloat32(+.infinity),
  getFloat32(+.nan),
  getFloat32(+.signalingNaN),
]) {
  expectCrashLater()
  _blackHole(Int64($0))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat32_AlwaysFails")
.forEach(in: [
  getFloat32(-32768.5),
  getFloat32(-128.5),
  getFloat32(-0.5),
  getFloat32(+0.5),
  getFloat32(+127.5),
  getFloat32(+255.5),
  getFloat32(+32767.5),
  getFloat32(+65535.5),
  getFloat32(-.infinity),
  getFloat32(-.nan),
  getFloat32(-.signalingNaN),
  getFloat32(+.infinity),
  getFloat32(+.nan),
  getFloat32(+.signalingNaN),
]) {
  expectNil(Int64(exactly: $0))
}

//===----------------------------------------------------------------------===//
// MARK: Float64: (-9007199254740991)...(+9007199254740991)
//===----------------------------------------------------------------------===//

FixedPointConversion_Debug64_ToInt64
.test("FromFloat64_NeverTraps")
.forEach(in: [
  (getInt64(-9007199254740991), getFloat64(-9007199254740991)),
  (getInt64(-32768), getFloat64(-32768.5)),
  (getInt64(-128), getFloat64(-128.5)),
  (getInt64(+0), getFloat64(-0.5)),
  (getInt64(+0), getFloat64(+0)),
  (getInt64(+0), getFloat64(-0)),
  (getInt64(+0), getFloat64(+0.5)),
  (getInt64(+127), getFloat64(+127.5)),
  (getInt64(+255), getFloat64(+255.5)),
  (getInt64(+32767), getFloat64(+32767.5)),
  (getInt64(+65535), getFloat64(+65535.5)),
  (getInt64(+9007199254740991), getFloat64(+9007199254740991)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat64_NeverFails")
.forEach(in: [
  (getInt64(-9007199254740991), getFloat64(-9007199254740991)),
  (getInt64(+0), getFloat64(+0)),
  (getInt64(+0), getFloat64(-0)),
  (getInt64(+9007199254740991), getFloat64(+9007199254740991)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat64_AlwaysTraps")
.forEach(in: [
  getFloat64(-.infinity),
  getFloat64(-.nan),
  getFloat64(-.signalingNaN),
  getFloat64(+.infinity),
  getFloat64(+.nan),
  getFloat64(+.signalingNaN),
]) {
  expectCrashLater()
  _blackHole(Int64($0))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat64_AlwaysFails")
.forEach(in: [
  getFloat64(-32768.5),
  getFloat64(-128.5),
  getFloat64(-0.5),
  getFloat64(+0.5),
  getFloat64(+127.5),
  getFloat64(+255.5),
  getFloat64(+32767.5),
  getFloat64(+65535.5),
  getFloat64(-.infinity),
  getFloat64(-.nan),
  getFloat64(-.signalingNaN),
  getFloat64(+.infinity),
  getFloat64(+.nan),
  getFloat64(+.signalingNaN),
]) {
  expectNil(Int64(exactly: $0))
}

//===----------------------------------------------------------------------===//
// MARK: Float80: (-18446744073709551615)...(+18446744073709551615)
//===----------------------------------------------------------------------===//

#if !(os(Windows) || os(Android)) && (arch(i386) || arch(x86_64))

FixedPointConversion_Debug64_ToInt64
.test("FromFloat80_NeverTraps")
.forEach(in: [
  (getInt64(-9223372036854775808), getFloat80(-9223372036854775808)),
  (getInt64(-32768), getFloat80(-32768.5)),
  (getInt64(-128), getFloat80(-128.5)),
  (getInt64(+0), getFloat80(-0.5)),
  (getInt64(+0), getFloat80(-0)),
  (getInt64(+0), getFloat80(+0)),
  (getInt64(+0), getFloat80(+0.5)),
  (getInt64(+127), getFloat80(+127.5)),
  (getInt64(+255), getFloat80(+255.5)),
  (getInt64(+32767), getFloat80(+32767.5)),
  (getInt64(+65535), getFloat80(+65535.5)),
  (getInt64(+9223372036854775807), getFloat80(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64($0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat80_NeverFails")
.forEach(in: [
  (getInt64(-9223372036854775808), getFloat80(-9223372036854775808)),
  (getInt64(+0), getFloat80(-0)),
  (getInt64(+0), getFloat80(+0)),
  (getInt64(+9223372036854775807), getFloat80(+9223372036854775807)),
]) {
  expectEqual($0.0, Int64(exactly: $0.1))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat80_AlwaysTraps")
.forEach(in: [
  getFloat80(-18446744073709551615),
  getFloat80(-9223372036854775809),
  getFloat80(+9223372036854775808),
  getFloat80(+18446744073709551615),
  getFloat80(-.infinity),
  getFloat80(-.nan),
  getFloat80(-.signalingNaN),
  getFloat80(+.infinity),
  getFloat80(+.nan),
  getFloat80(+.signalingNaN),
]) {
  expectCrashLater()
  _blackHole(Int64($0))
}

FixedPointConversion_Debug64_ToInt64
.test("FromFloat80_AlwaysFails")
.forEach(in: [
  getFloat80(-18446744073709551615),
  getFloat80(-9223372036854775809),
  getFloat80(-32768.5),
  getFloat80(-128.5),
  getFloat80(-0.5),
  getFloat80(+0.5),
  getFloat80(+127.5),
  getFloat80(+255.5),
  getFloat80(+32767.5),
  getFloat80(+65535.5),
  getFloat80(+9223372036854775808),
  getFloat80(+18446744073709551615),
  getFloat80(-.infinity),
  getFloat80(-.nan),
  getFloat80(-.signalingNaN),
  getFloat80(+.infinity),
  getFloat80(+.nan),
  getFloat80(+.signalingNaN),
]) {
  expectNil(Int64(exactly: $0))
}

#endif // Float80

runAllTests()

