// RUN: %empty-directory(%t)

// RUN: %target-build-swift-dylib(%t/%target-library-name(resilient_enums)) -enable-library-evolution %S/Inputs/reflect_Enum_values_resilient_enums.swift -emit-module -emit-module-path %t/resilient_enums.swiftmodule -module-name resilient_enums
// RUN: %target-codesign %t/%target-library-name(resilient_enums)

// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -L %t -I %t -lresilient_enums -o %t/reflect_Enum_values_resilient %target-rpath(%t)
// RUN: %target-codesign %t/reflect_Enum_values_resilient

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_values_resilient | tee /dev/stderr | %FileCheck %s --dump-input=fail

// REQUIRES: executable_test
// REQUIRES: objc_interop, OS=macosx
// UNSUPPORTED: use_os_stdlib

import SwiftReflectionTest

import resilient_enums

// Non-resilient enum wrapping a resilient enum
// This doesn't use spare bits of the inner enum
enum E2 {
case y(E1_resilient)
case z(E1_resilient)
}

// Contrast:
// E2_resilient is a resilient enum wrapping a resilient enum
// This does use spare bits of the inner enum


// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_values_resilient.E2)
// CHECK-NEXT: Value: .y(.a)

reflect(enumValue: E2.y(.a))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_values_resilient.E2)
// CHECK-NEXT: Value: .z(.b)

reflect(enumValue: E2.z(.b))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum resilient_enums.E1_resilient)
// CHECK-NEXT: Value: .a

reflect(enumValue: E1_resilient.a)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum resilient_enums.E1_resilient)
// CHECK-NEXT: Value: .b

reflect(enumValue: E1_resilient.b)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum resilient_enums.E2_resilient)
// CHECK-NEXT: Value: .c(.a)

reflect(enumValue: E2_resilient.c(.a))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum resilient_enums.E2_resilient)
// CHECK-NEXT: Value: .d(.b)

reflect(enumValue: E2_resilient.d(.b))

doneReflecting()

// CHECK: Done.
