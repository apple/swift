// RUN: %target-swift-frontend %use_no_opaque_pointers -parse-as-library -O -target-cpu core2 -emit-ir  %s | %FileCheck %s
// RUN: %target-swift-frontend -parse-as-library -O -target-cpu core2 -emit-ir  %s
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib,CPU=x86_64
// REQUIRES: swift_in_compiler

// This is an end-to-end test to ensure that the optimizer generates
// a simple literal for character literals.

// Please note: this test targets "core2" to ensure consistent output
// on all x86 host processors.

// We generate this as an LLVM constant global directly, no runtime heap
// allocation. Match that.
// CHECK-LABEL: @"{{.*}}charArrayy{{.*}}" ={{.*}} global {{.*}}ContiguousArrayStorage{{.*}} {{.*}}{ i64 97 }{{.*}}{ i64 98 }{{.*}}{ i64 99 }{{.*}}{ i64 100 }{{.*}}
//
// CHECK-LABEL: define {{.*}}charArray
// CHECK:  {{.*}} = tail call %swift.refcounted* @swift_initStaticObject({{.*}} @"{{.*}}charArrayy{{.*}}"
// CHECK: ret
public func charArray(_ i: Int) -> [Character] {
  return [ "a", "b", "c", "d" ]
}

// NOTE: 97 = 'a'
// NOTE: -2233785415175766016 = 0xE1 = 0xE0 (ASCII discrim) | 0x01 (count)
//
// CHECK-LABEL: define {{.*}}singleChar
// CHECK-NEXT: entry:
// CHECK-NEXT: ret { i64, %swift.bridge* } { i64 97, %swift.bridge* inttoptr (i64 -2233785415175766016 to %swift.bridge*) }
public func singleChar() -> Character {
  return "a"
}

// NOTE: 10852326 = 0xE6 0x97 0xA5 (little endian), the encoding of U+65E5
// NOTE: -6701356245527298048 = 0xA3 = 0xA0 (non-ASCII discrim) | 0x03 (count)
//
// CHECK-LABEL: define {{.*}}singleNonAsciiChar
// CHECK-NEXT: entry:
// CHECK-NEXT: ret { i64, %swift.bridge* } { i64 10852326, %swift.bridge* inttoptr (i64 -6701356245527298048 to %swift.bridge*) }
public func singleNonAsciiChar() -> Character {
  return "日"
}

// NOTE: -9223372036854775808 = 0x80 = immortal large discrim
// NOTE: 1152921504606847001 = 25 (code unit length) | `isTailAllocated` perf flag
//
// CHECK-LABEL: define {{.*}}singleNonSmolChar
// CHECK-NEXT: entry:
// CHECK:   ret { i64, %swift.bridge* } { i64 1152921504606847001, %swift.bridge* {{.*}}@".str.25.\F0\9F\91\A9\E2\80\8D\F0\9F\91\A9\E2\80\8D\F0\9F\91\A6\E2\80\8D\F0\9F\91\A6" {{.*}}i64 -9223372036854775808
public func singleNonSmolChar() -> Character {
  return "👩‍👩‍👦‍👦"
}

