// RUN: %empty-directory(%t)
// RUN: %target-build-swift -g -lswiftSwiftReflectionTest %s -o %t/reflect_Enum_TwoCaseOnePointerPayload
// RUN: %target-codesign %t/reflect_Enum_TwoCaseOnePointerPayload

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_TwoCaseOnePointerPayload | %FileCheck %s --check-prefix=CHECK-%target-ptrsize

// REQUIRES: objc_interop
// REQUIRES: executable_test

import SwiftReflectionTest

class Marker {
	let value = 1
}

// Note: Reference/pointer types have extra inhabitants, so
// the enum can use those.  As a result, this enum should
// be the same size as a pointer.
enum TwoCaseOnePointerPayloadEnum {
case valid(Marker)
case invalid
}

class ClassWithTwoCaseOnePointerPayloadEnum {
  var e1: TwoCaseOnePointerPayloadEnum?
  var e2: TwoCaseOnePointerPayloadEnum = .valid(Marker())
  var e3: TwoCaseOnePointerPayloadEnum = .invalid
  var e4: TwoCaseOnePointerPayloadEnum? = .valid(Marker())
  var e5: TwoCaseOnePointerPayloadEnum? = .invalid
	var e6: TwoCaseOnePointerPayloadEnum??
}

reflect(object: ClassWithTwoCaseOnePointerPayloadEnum())

// CHECK-64: Reflecting an object.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (class reflect_Enum_TwoCaseOnePointerPayload.ClassWithTwoCaseOnePointerPayloadEnum)

// CHECK-64: Type info:
// CHECK-64: (class_instance size=64 alignment=8 stride=64 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (field name=e1 offset=16
// CHECK-64:     (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483645 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:           (case name=valid index=0 offset=0
// CHECK-64:             (reference kind=strong refcounting=native))
// CHECK-64:           (case name=invalid index=1)))
// CHECK-64:       (case name=none index=1)))
// CHECK-64:   (field name=e2 offset=24
// CHECK-64:     (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:       (case name=valid index=0 offset=0
// CHECK-64:         (reference kind=strong refcounting=native))
// CHECK-64:       (case name=invalid index=1)))
// CHECK-64:   (field name=e3 offset=32
// CHECK-64:     (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:       (case name=valid index=0 offset=0
// CHECK-64:         (reference kind=strong refcounting=native))
// CHECK-64:       (case name=invalid index=1)))
// CHECK-64:   (field name=e4 offset=40
// CHECK-64:     (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483645 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:           (case name=valid index=0 offset=0
// CHECK-64:             (reference kind=strong refcounting=native))
// CHECK-64:           (case name=invalid index=1)))
// CHECK-64:       (case name=none index=1)))
// CHECK-64:   (field name=e5 offset=48
// CHECK-64:     (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483645 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:           (case name=valid index=0 offset=0
// CHECK-64:             (reference kind=strong refcounting=native))
// CHECK-64:           (case name=invalid index=1)))
// CHECK-64:       (case name=none index=1)))
// CHECK-64:   (field name=e6 offset=56
// CHECK-64:     (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483644 bitwise_takable=1
// CHECK-64:       (case name=some index=0 offset=0
// CHECK-64:         (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483645 bitwise_takable=1
// CHECK-64:           (case name=some index=0 offset=0
// CHECK-64:             (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:               (case name=valid index=0 offset=0
// CHECK-64:                 (reference kind=strong refcounting=native))
// CHECK-64:               (case name=invalid index=1)))
// CHECK-64:           (case name=none index=1)))
// CHECK-64:       (case name=none index=1))))

// CHECK-32: Reflecting an object.
// CHECK-32: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-32: Type reference:
// CHECK-32: (class reflect_Enum_TwoCaseOnePointerPayload.ClassWithTwoCaseOnePointerPayloadEnum)

// CHECK-32: Type info:
// CHECK-32: (class_instance size=32 alignment=4 stride=32 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-32:   (field name=e1 offset=8
// CHECK-32:     (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4094 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:           (case name=valid index=0 offset=0
// CHECK-32:             (reference kind=strong refcounting=native))
// CHECK-32:           (case name=invalid index=1)))
// CHECK-32:       (case name=none index=1)))
// CHECK-32:   (field name=e2 offset=12
// CHECK-32:     (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:       (case name=valid index=0 offset=0
// CHECK-32:         (reference kind=strong refcounting=native))
// CHECK-32:       (case name=invalid index=1)))
// CHECK-32:   (field name=e3 offset=16
// CHECK-32:     (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:       (case name=valid index=0 offset=0
// CHECK-32:         (reference kind=strong refcounting=native))
// CHECK-32:       (case name=invalid index=1)
// CHECK-32:   (field name=e4 offset=20
// CHECK-32:     (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4094 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:           (case name=valid index=0 offset=0
// CHECK-32:             (reference kind=strong refcounting=native))
// CHECK-32:           (case name=invalid index=1)))
// CHECK-32:       (case name=none index=1)))
// CHECK-32:   (field name=e5 offset=24
// CHECK-32:     (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4094 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:           (case name=valid index=0 offset=0
// CHECK-32:             (reference kind=strong refcounting=native))
// CHECK-32:           (case name=invalid index=1)))
// CHECK-32:       (case name=none index=1)))
// CHECK-32:   (field name=e6 offset=28
// CHECK-32:     (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4093 bitwise_takable=1
// CHECK-32:       (case name=some index=0 offset=0
// CHECK-32:         (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4094 bitwise_takable=1
// CHECK-32:           (case name=some index=0 offset=0
// CHECK-32:             (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:               (case name=valid index=0 offset=0
// CHECK-32:                 (reference kind=strong refcounting=native))
// CHECK-32:               (case name=invalid index=1)))
// CHECK-32:           (case name=none index=1)))
// CHECK-32:       (case name=none index=1))))

reflect(enum: TwoCaseOnePointerPayloadEnum.valid(Marker()))

// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_Enum_TwoCaseOnePointerPayload.TwoCaseOnePointerPayloadEnum)

// CHECK-64: Type info:
// CHECK-64: (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:   (case name=valid index=0 offset=0
// CHECK-64:     (reference kind=strong refcounting=native))
// CHECK-64:   (case name=invalid index=1))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=valid index=0
// CHECK-64:   (reference kind=strong refcounting=native)
// CHECK-64: )

// CHECK-32: Reflecting an enum.
// CHECK-32: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-32: Type reference:
// CHECK-32: (enum reflect_Enum_TwoCaseOnePointerPayload.TwoCaseOnePointerPayloadEnum)

// CHECK-32: Type info:
// CHECK-32: (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:   (case name=valid index=0 offset=0
// CHECK-32:     (reference kind=strong refcounting=native))
// CHECK-32:   (case name=invalid index=1))

// CHECK-32: Enum value:
// CHECK-32: (enum_value name=valid index=0
// CHECK-32:   (reference kind=strong refcounting=native)
// CHECK-32: )

reflect(enum: TwoCaseOnePointerPayloadEnum.invalid)

// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_Enum_TwoCaseOnePointerPayload.TwoCaseOnePointerPayloadEnum)

// CHECK-64: Type info:
// CHECK-64: (single_payload_enum size=8 alignment=8 stride=8 num_extra_inhabitants=2147483646 bitwise_takable=1
// CHECK-64:   (case name=valid index=0 offset=0
// CHECK-64:     (reference kind=strong refcounting=native))
// CHECK-64:   (case name=invalid index=1))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=invalid index=1)

// CHECK-32: Reflecting an enum.
// CHECK-32: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-32: Type reference:
// CHECK-32: (enum reflect_Enum_TwoCaseOnePointerPayload.TwoCaseOnePointerPayloadEnum)

// CHECK-32: Type info:
// CHECK-32: (single_payload_enum size=4 alignment=4 stride=4 num_extra_inhabitants=4095 bitwise_takable=1
// CHECK-32:   (case name=valid index=0 offset=0
// CHECK-32:     (reference kind=strong refcounting=native))
// CHECK-32:   (case name=invalid index=1))

// CHECK-32: Enum value:
// CHECK-32: (enum_value name=invalid index=1)

doneReflecting()

// CHECK-64: Done.

// CHECK-32: Done.
