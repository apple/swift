// RUN: %empty-directory(%t)
// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -o %t/reflect_Enum_values3
// RUN: %target-codesign %t/reflect_Enum_values3

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_values3 | tee /dev/stderr | %FileCheck %s --check-prefix=CHECK%target-ptrsize --check-prefix=CHECKALL --dump-input=fail %add_num_extra_inhabitants

// REQUIRES: objc_interop
// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib

import SwiftReflectionTest

enum EnumB<T> {
    case a
    case b(Int8)
    case c(T)
    case d(Void)
    case e(Void)
    case RIGHT(Int8)
    case f(Int8)
    case g(Void)
    case LEFT(Int8, Int64)
}

class PlaceSummaryUnit<T> {
  let t: EnumB<T>
  init(t: EnumB<T>) { self.t = t }
}

reflect(object: PlaceSummaryUnit<Int8>(t: .RIGHT(5)))

// CHECKALL: Reflecting an object.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (bound_generic_class reflect_Enum_values3.PlaceSummaryUnit
// CHECKALL-NEXT:   (struct Swift.Int8))

// CHECKALL: Type info

// TODO: Check the type layout for 64- and 32-bit targets

reflect(enumValue: PlaceSummaryUnit<Int8>(t: .RIGHT(5)).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (bound_generic_enum reflect_Enum_values3.EnumB
// CHECKALL-NEXT:   (struct Swift.Int8))
// CHECKALL-NEXT: Value: .RIGHT(_)

reflect(enumValue: PlaceSummaryUnit<Int8>(t: .a).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (bound_generic_enum reflect_Enum_values3.EnumB
// CHECKALL-NEXT:   (struct Swift.Int8))
// CHECKALL-NEXT: Value: .a

reflect(enumValue: PlaceSummaryUnit<Int8>(t: .c(7)).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (bound_generic_enum reflect_Enum_values3.EnumB
// CHECKALL-NEXT:   (struct Swift.Int8))
// CHECKALL-NEXT: Value: .c(_)

reflect(enumValue: PlaceSummaryUnit<Int8>(t: .LEFT(1,2)).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (bound_generic_enum reflect_Enum_values3.EnumB
// CHECKALL-NEXT:   (struct Swift.Int8))
// CHECKALL-NEXT: Value: .LEFT(_)

reflect(enumValue: PlaceSummaryUnit<Int8>(t: .d(())).t)

// CHECKALL: Reflecting an enum value.
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (bound_generic_enum reflect_Enum_values3.EnumB
// CHECKALL-NEXT:   (struct Swift.Int8))
// CHECKALL-NEXT: Value: .d(_)

doneReflecting()

// CHECKALL: Done.

