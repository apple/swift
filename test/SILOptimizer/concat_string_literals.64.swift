// RUN: %target-swift-frontend -O -emit-ir  %s | %FileCheck %s
// RUN: %target-swift-frontend -Osize -emit-ir  %s | %FileCheck %s
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib

// REQUIRES: swift_in_compiler

// We have a separate test for 32-bit architectures.
// REQUIRES: PTRSIZE=64

// The string tag was recently shifted to the second byte on Android AArch64,
// so these tests will need to be adapted for that.
// XFAIL: OS=linux-android && CPU=aarch64

// NOTE: 25185.byteSwapped = 0x62 'a', 0x61 'b'
// CHECK-LABEL: test_ascii_scalar_scalar2
// CHECK:  ret { i64, ptr } { i64 25185, ptr inttoptr (i64 -{{[0-9]+}} to ptr) }
public func test_ascii_scalar_scalar2() -> String {
  return "a" + "b"
}


// NOTE: 11125601.byteSwapped = 0x61 'a', 0xC3 0xA9 'é'
// CHECK-LABEL: test_scalar_otherscalar
// CHECK:  ret { i64, ptr } { i64 11125601, ptr inttoptr (i64 -{{[0-9]+}} to ptr) }
public func test_scalar_otherscalar() -> String {
  return "a" + "é"
}

// NOTE: -8097488946593795999.byteSwapped = 0x61 'a', 0xF0 0x9F 0x95 0xb4 '🕴', ...
// CHECK-LABEL: test_scalar_char
// CHECK:  ret { i64, ptr } { i64 -8097488946593795999, ptr inttoptr (i64 -{{[0-9]+}} to ptr) }
public func test_scalar_char() -> String {
  return "a" + "🕴🏿"
}

// NOTE: 112585666577249.byteSwapped = 0x61 'a', 0xc3 0xa9 'é', 0x64 'd', 0x65 'e', 0x66 'f'
// CHECK-LABEL: test_strng_strng2
// CHECK:  ret { i64, ptr } { i64 112585666577249, ptr inttoptr (i64 -{{[0-9]+}} to ptr) }
public func test_strng_strng2() -> String {
  return "aé" + "def"
}

// NOTE: 1152921504606847019 = 43 (code-unit length) | `isTailAllocated` perf flag
// CHECK-LABEL: test_scalar_strng
// CHECK:  ret { i64, ptr } { i64 1152921504606847019, ptr inttoptr {{.*}}i64 -{{[0-9]+}}{{.*}} to ptr) }
public func test_scalar_strng() -> String {
  return "a" + "👨🏿‍💼+🧙🏿‍♂️=🕴🏿"
}

// NOTE: 7450828190687388257.byteSwapped = 0x61 'a', 0x62 'b', 0x63 'c', 0x64 'd', 0xC3 0xA8 'è', 0x66 'f', 0x67 'g', ...
// CHECK-LABEL: test_strng_concat_smol
// CHECK:  ret { i64, ptr } { i64 7450828190687388257, ptr inttoptr (i64 -{{[0-9]+}} to ptr) }
public func test_strng_concat_smol() -> String {
  return "a" + "bc" + "dèf" + "ghī"
}

// NOTE: 1152921504606846999 = 23 (code-unit length) | `isTailAllocated` perf flag
// CHECK-LABEL: test_strng_concat_large
// CHECK:  ret { i64, ptr } { i64 1152921504606846999, ptr inttoptr {{.*}}i64 -{{[0-9]+}}{{.*}} to ptr) }
public func test_strng_concat_large() -> String {
  return "a" + "bc" + "dèf" + "ghī" + "jklmn" + "o" + "𝛒qr"
}
