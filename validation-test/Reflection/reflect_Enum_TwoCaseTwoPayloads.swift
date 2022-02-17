// RUN: %empty-directory(%t)
// RUN: %target-build-swift -g -lswiftSwiftReflectionTest %s -o %t/reflect_Enum_TwoCaseTwoPayloads
// RUN: %target-codesign %t/reflect_Enum_TwoCaseTwoPayloads

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_TwoCaseTwoPayloads | %FileCheck %s --check-prefix=CHECK-%target-ptrsize

// REQUIRES: reflection_test_support
// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib

import SwiftReflectionTest

struct Marker {
	let value = 1
	let extra = 2
}

enum TwoCaseTwoPayloadsEnum {
case valid(Marker)
case invalid(Int)
}

class ClassWithTwoCaseTwoPayloadsEnum {
  var e1: TwoCaseTwoPayloadsEnum?
  var e2: TwoCaseTwoPayloadsEnum = .valid(Marker())
  var e3: TwoCaseTwoPayloadsEnum = .invalid(7)
  var e4: TwoCaseTwoPayloadsEnum? = .valid(Marker())
  var e5: TwoCaseTwoPayloadsEnum? = .invalid(7)
	var e6: TwoCaseTwoPayloadsEnum??
}

reflect(object: ClassWithTwoCaseTwoPayloadsEnum())

// CHECK-64: Reflecting an object.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (class reflect_Enum_TwoCaseTwoPayloads.ClassWithTwoCaseTwoPayloadsEnum)

// CHECK-64: Type info:
// CHECK-64: (class_instance size=153 alignment=8 stride=160 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (field name=e1 offset=16
// CHECK-64:     (single_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-64:           (case name=valid index=0 offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=value offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:               (field name=extra offset=8
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:           (case name=invalid index=1 offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:       (case name=none index=1)))
// CHECK-64:   (field name=e2 offset=40
// CHECK-64:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-64:       (case name=valid index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:           (field name=value offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:           (field name=extra offset=8
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:       (case name=invalid index=1 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:           (field name=_value offset=0
// CHECK-64:             (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:   (field name=e3 offset=64
// CHECK-64:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-64:       (case name=valid index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:           (field name=value offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:           (field name=extra offset=8
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:       (case name=invalid index=1 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:           (field name=_value offset=0
// CHECK-64:             (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:   (field name=e4 offset=88
// CHECK-64:     (single_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-64:           (case name=valid index=0 offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=value offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:               (field name=extra offset=8
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:           (case name=invalid index=1 offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:       (case name=none index=1)))
// CHECK-64:   (field name=e5 offset=112
// CHECK-64:     (single_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-64:           (case name=valid index=0 offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=value offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:               (field name=extra offset=8
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:           (case name=invalid index=1 offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:       (case name=none index=1)))
// CHECK-64:   (field name=e6 offset=136
// CHECK-64:     (single_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (single_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-64:           (case name=some index=0 offset=0
// CHECK-64:             (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-64:               (case name=valid index=0 offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=value offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=extra offset=8
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:               (case name=invalid index=1 offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-64:           (case name=none index=1)))
// CHECK-64:       (case name=none index=1))))

// CHECK-32: Reflecting an object.
// CHECK-32: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-32: Type reference:
// CHECK-32: (class reflect_Enum_TwoCaseTwoPayloads.ClassWithTwoCaseTwoPayloadsEnum)

// CHECK-32: Type info:
// CHECK-32: (class_instance size=77 alignment=4 stride=80 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:   (field name=e1 offset=8
// CHECK-32:     (single_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (multi_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-32:           (case name=valid index=0 offset=0
// CHECK-32:             (struct size=8 alignment=4 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=value offset=0
// CHECK-32:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=_value offset=0
// CHECK-32:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-32:               (field name=extra offset=4
// CHECK-32:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=_value offset=0
// CHECK-32:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:           (case name=invalid index=1 offset=0
// CHECK-32:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=_value offset=0
// CHECK-32:                 (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:       (case name=none index=1)))
// CHECK-32:   (field name=e2 offset=20
// CHECK-32:     (multi_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-32:       (case name=valid index=0 offset=0
// CHECK-32:         (struct size=8 alignment=4 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:           (field name=value offset=0
// CHECK-32:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=_value offset=0
// CHECK-32:                 (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-32:           (field name=extra offset=4
// CHECK-32:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=_value offset=0
// CHECK-32:                 (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:       (case name=invalid index=1 offset=0
// CHECK-32:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:           (field name=_value offset=0
// CHECK-32:             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:   (field name=e3 offset=32
// CHECK-32:     (multi_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-32:       (case name=valid index=0 offset=0
// CHECK-32:         (struct size=8 alignment=4 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:           (field name=value offset=0
// CHECK-32:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=_value offset=0
// CHECK-32:                 (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-32:           (field name=extra offset=4
// CHECK-32:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=_value offset=0
// CHECK-32:                 (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:       (case name=invalid index=1 offset=0
// CHECK-32:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:           (field name=_value offset=0
// CHECK-32:             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:   (field name=e4 offset=44
// CHECK-32:     (single_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (multi_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-32:           (case name=valid index=0 offset=0
// CHECK-32:             (struct size=8 alignment=4 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=value offset=0
// CHECK-32:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=_value offset=0
// CHECK-32:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-32:               (field name=extra offset=4
// CHECK-32:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=_value offset=0
// CHECK-32:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:           (case name=invalid index=1 offset=0
// CHECK-32:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=_value offset=0
// CHECK-32:                 (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:       (case name=none index=1)))
// CHECK-32:   (field name=e5 offset=56
// CHECK-32:     (single_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (multi_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-32:           (case name=valid index=0 offset=0
// CHECK-32:             (struct size=8 alignment=4 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=value offset=0
// CHECK-32:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=_value offset=0
// CHECK-32:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-32:               (field name=extra offset=4
// CHECK-32:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=_value offset=0
// CHECK-32:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:           (case name=invalid index=1 offset=0
// CHECK-32:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:               (field name=_value offset=0
// CHECK-32:                 (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:       (case name=none index=1)))
// CHECK-32:   (field name=e6 offset=68
// CHECK-32:     (single_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (single_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK-32:           (case name=some index=0 offset=0
// CHECK-32:             (multi_payload_enum size=9 alignment=4 stride=12 num_extra_inhabitants=254 bitwise_takable=1
// CHECK-32:               (case name=valid index=0 offset=0
// CHECK-32:                 (struct size=8 alignment=4 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=value offset=0
// CHECK-32:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                       (field name=_value offset=0
// CHECK-32:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-32:                   (field name=extra offset=4
// CHECK-32:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                       (field name=_value offset=0
// CHECK-32:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:               (case name=invalid index=1 offset=0
// CHECK-32:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:                   (field name=_value offset=0
// CHECK-32:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))))
// CHECK-32:           (case name=none index=1)))
// CHECK-32:       (case name=none index=1))))

reflect(enumValue: TwoCaseTwoPayloadsEnum.valid(Marker()))

// CHECK-64: Reflecting an enum value.
// CHECK-64-NEXT: Type reference:
// CHECK-64-NEXT: (enum reflect_Enum_TwoCaseTwoPayloads.TwoCaseTwoPayloadsEnum)
// CHECK-64-NEXT: Value: .valid(_)

// CHECK-32: Reflecting an enum value.
// CHECK-32-NEXT: Type reference:
// CHECK-32-NEXT: (enum reflect_Enum_TwoCaseTwoPayloads.TwoCaseTwoPayloadsEnum)
// CHECK-32-NEXT: Value: .valid(_)

reflect(enumValue: TwoCaseTwoPayloadsEnum.invalid(7))

// CHECK-64: Reflecting an enum value.
// CHECK-64-NEXT: Type reference:
// CHECK-64-NEXT: (enum reflect_Enum_TwoCaseTwoPayloads.TwoCaseTwoPayloadsEnum)
// CHECK-64-NEXT: Value: .invalid(_)

// CHECK-32: Reflecting an enum value.
// CHECK-32-NEXT: Type reference:
// CHECK-32-NEXT: (enum reflect_Enum_TwoCaseTwoPayloads.TwoCaseTwoPayloadsEnum)
// CHECK-32-NEXT: Value: .invalid(_)

doneReflecting()

// CHECK-64: Done.

// CHECK-32: Done.
