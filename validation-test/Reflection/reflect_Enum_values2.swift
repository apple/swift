// RUN: %empty-directory(%t)
// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -o %t/reflect_Enum_values2
// RUN: %target-codesign %t/reflect_Enum_values2

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_values2 | tee /dev/stderr | %FileCheck %s --check-prefix=CHECK%target-ptrsize --check-prefix=CHECKALL --dump-input=fail %add_num_extra_inhabitants

// REQUIRES: objc_interop
// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib

import SwiftReflectionTest

enum EnumB {
    case a
    case b(Int8)
    case c(Int8)
    case d(Void)
    case e(Void)
    case RIGHT(Int8)
    case f(Int8)
    case g(Void)
    case LEFT(Int8, Int64)
}

class PlaceSummaryUnit {
  let t: EnumB
  init(t: EnumB) { self.t = t }
}

reflect(object: PlaceSummaryUnit(t: .RIGHT(5)))

// CHECKALL: Reflecting an object.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (class reflect_Enum_values2.PlaceSummaryUnit)

// CHECKALL: Type info

// TODO: Check the type layout for 64- and 32-bit targets

reflect(enumValue: PlaceSummaryUnit(t: .RIGHT(5)).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values2.EnumB)
// CHECKALL-NEXT: Value: .RIGHT(_)

reflect(enumValue: PlaceSummaryUnit(t: .a).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values2.EnumB)
// CHECKALL-NEXT: Value: .a

reflect(enumValue: PlaceSummaryUnit(t: .LEFT(1,2)).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values2.EnumB)
// CHECKALL-NEXT: Value: .LEFT(_)

reflect(enumValue: PlaceSummaryUnit(t: .d(())).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values2.EnumB)
// CHECKALL-NEXT: Value: .d(_)

doneReflecting()

// CHECKALL: Done.

