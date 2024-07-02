// RUN: %target-run-stdlib-swift -parse-stdlib %s | %FileCheck %s
// REQUIRES: executable_test

import Swift

// FIXME: https://github.com/apple/swift/issues/57444
// Work around the inability for static-library based Swift runtime builds to
// directly link against Darwin.swiftmodule by using a benign dependency on
// StdlibUnittest.
import StdlibUnittest
runAllTests()

#if canImport(Darwin)
  import Darwin
  #if _runtime(_ObjC)
    import CoreGraphics
  #else
    #if arch(x86_64) || arch(arm64)
      typealias CGFloat = Double
    #else
      typealias CGFloat = Float
    #endif
  #endif
#elseif canImport(Glibc)
  import Glibc
  typealias CGFloat = Double
#elseif os(WASI)
  import WASILibc
  typealias CGFloat = Double
#elseif canImport(Android)
  import Android
  typealias CGFloat = Double
#elseif os(Windows)
  import CRT
  #if arch(x86_64) || arch(arm64)
    typealias CGFloat = Double
  #else
    typealias CGFloat = Float
  #endif
#else
#error("Unsupported platform")
#endif

func my_printf(_ format: String, _ arguments: CVarArg...) {
  _ = withVaList(arguments) {
    vprintf(format, $0)
  }
}

func test_varArgs0() {
  // CHECK: The answer to life and everything is 42, 42, -42, 3.14
  my_printf(
    "The answer to life and everything is %ld, %u, %d, %f\n",
    42, UInt32(42), Int16(-42), 3.14159279)
}
test_varArgs0()

func test_varArgs1() {
  var args = [CVarArg]()

  var format = "dig it: "
  for i in 0..<12 {
    args.append(Int16(-i))
    args.append(Float(i))
    format += "%d %2g "
  }
  
  // CHECK: dig it: 0  0 -1  1 -2  2 -3  3 -4  4 -5  5 -6  6 -7  7 -8  8 -9  9 -10 10 -11 11
  _ = withVaList(args) {
    vprintf(format + "\n", $0)
  }
}
test_varArgs1()

func test_varArgs3() {
  var args = [CVarArg]()

  let format = "pointers: '%p' '%p' '%p' '%p' '%p'\n"
  args.append(OpaquePointer(bitPattern: 0x1234_5670)!)
  args.append(OpaquePointer(bitPattern: 0x1234_5671)!)
  args.append(UnsafePointer<Int>(bitPattern: 0x1234_5672)!)
  args.append(UnsafeMutablePointer<Float>(bitPattern: 0x1234_5673)!)

#if _runtime(_ObjC)
  args.append(AutoreleasingUnsafeMutablePointer<AnyObject>(
        UnsafeMutablePointer<AnyObject>(bitPattern: 0x1234_5674)!))
#else
  //Linux does not support AutoreleasingUnsafeMutablePointer; put placeholder.
  args.append(UnsafeMutablePointer<Float>(bitPattern: 0x1234_5674)!)
#endif

  // CHECK: {{pointers: '(0x)?0*12345670' '(0x)?0*12345671' '(0x)?0*12345672' '(0x)?0*12345673' '(0x)?0*12345674'}}
  _ = withVaList(args) {
    vprintf(format, $0)
  }
}
test_varArgs3()

func test_varArgs4() {
  // Verify alignment of va_list contents.
  // On some architectures some types are better-
  // aligned than Int and must be packaged with care.

  let  i8 = Int8(1)
  let i16 = Int16(2)
  let i32 = Int32(3)
  let i64 = 4444444444444444 as Int64

  let  u8 = UInt8(10)
  let u16 = UInt16(20)
  let u32 = UInt32(30)
  let u64 = 4040404040404040 as UInt64

  let f32 = Float(1.1)
  let f64 = Double(2.2)
  let fCG = CGFloat(3.3)

  my_printf("a %g %d %g %d %g %d a\n",            f32, i8, f64, i8, fCG, i8)
  my_printf("b %d %g %d %g %d %g %d b\n",     i8, f32, i8, f64, i8, fCG, i8)
  my_printf("c %d %d %d %d %d %lld %d c\n",       i8, i16, i8, i32, i8, i64, i8)
  my_printf("d %d %d %d %d %d %d %lld %d d\n",i8, i8, i16, i8, i32, i8, i64, i8)
  my_printf("e %u %u %u %u %u %llu %u e\n",       u8, u16, u8, u32, u8, u64, u8)
  my_printf("f %u %u %u %u %u %u %llu %u f\n",u8, u8, u16, u8, u32, u8, u64, u8)
  // CHECK: a 1.1 1 2.2 1 3.3 1 a
  // CHECK: b 1 1.1 1 2.2 1 3.3 1 b
  // CHECK: c 1 2 1 3 1 4444444444444444 1 c
  // CHECK: d 1 1 2 1 3 1 4444444444444444 1 d
  // CHECK: e 10 20 10 30 10 4040404040404040 10 e
  // CHECK: f 10 10 20 10 30 10 4040404040404040 10 f
}
test_varArgs4()

func test_varArgs5() {
  var args = [CVarArg]()

  // Confirm the absence of a bug (on x86-64) wherein floats were stored in
  // the GP register-save area after the SSE register-save area was
  // exhausted, rather than spilling into the overflow argument area.
  //
  // This is not caught by test_varArgs1 above, because it exhausts the
  // GP register-save area before the SSE area.

  var format = "rdar-32547102: "
  for i in 0..<12 {
    args.append(Float(i))
    format += "%.1f "
  }

  // CHECK: rdar-32547102: 0.0 1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 9.0 10.0 11.0
  _ = withVaList(args) {
    vprintf(format + "\n", $0)
  }
}
test_varArgs5()

func test_varArgs6() {
  // Verify alignment of va_list contents when `Float80` is present.
  let  i8 = Int8(1)
  let f32 = Float(1.1)
  let f64 = Double(2.2)
#if !os(Windows) && (arch(i386) || arch(x86_64))
  let f80 = Float80(4.5)
  my_printf("a %g %d %g %d %Lg %d %g a\n",            f32, i8, f64, i8, f80, i8, f32)
  my_printf("b %d %g %d %g %d %Lg %d %g b\n",     i8, f32, i8, f64, i8, f80, i8, f32)
#else // just a dummy to make FileCheck happy, since it ignores `#if`s
  let dummy = Double(4.5)
  my_printf("a %g %d %g %d %g %d %g a\n",            f32, i8, f64, i8, dummy, i8, f32)
  my_printf("b %d %g %d %g %d %g %d %g b\n",     i8, f32, i8, f64, i8, dummy, i8, f32)
#endif
  // CHECK: a 1.1 1 2.2 1 4.5 1 1.1 a
  // CHECK: b 1 1.1 1 2.2 1 4.5 1 1.1 b
}
test_varArgs6()


// CHECK: done.
my_printf("done.")
