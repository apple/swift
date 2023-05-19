// RUN: %empty-directory(%t)
// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -o %t/reflect_Enum_MultiPayload_generic7
// RUN: %target-codesign %t/reflect_Enum_MultiPayload_generic7

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_MultiPayload_generic7 | tee /dev/stderr | %FileCheck %s --check-prefix=CHECK --check-prefix=X%target-ptrsize --dump-input=fail

// REQUIRES: reflection_test_support
// REQUIRES: objc_interop
// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib

import SwiftReflectionTest
import Darwin

// This will always get treated as a single-payload enum
enum A<T> {
case a(T)
case b(Void)
case c(Void)
}

reflect(enum: A<Int>.a(7))

// CHECK: Reflecting an enum.
// CHECK-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum reflect_Enum_MultiPayload_generic7.A
// CHECK-NEXT:   (struct Swift.Int))

// According to MemoryLayout<>, A<Int> is 9 bytes, A<Int>? is 10 bytes
// the latter shows that there are no XIs in A<Int>.

// CHECK: Type info:
// X64-NEXT: (multi_payload_enum size=9 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// X64-NEXT:   (case name=a index=0 offset=0
// X64-NEXT:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// X64-NEXT:       (field name=_value offset=0
// X64-NEXT:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// X64-NEXT:   (case name=b index=1 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))
// X64-NEXT:   (case name=c index=2 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1)))

// CHECK: Mangled name: $s34reflect_Enum_MultiPayload_generic71AOySiG
// CHECK-NEXT: Demangled name: reflect_Enum_MultiPayload_generic7.A<Swift.Int>

// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=a index=0
// CHECK-NEXT:   (struct Swift.Int)
// CHECK-NEXT: )

reflect(enum: A<Int>.b(()))

// CHECK: Reflecting an enum.
// CHECK: Type info:
// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=b index=1
// CHECK-NEXT:   (tuple)
// CHECK-NEXT: )

reflect(enum: A<Void>.a(()))

// CHECK: Reflecting an enum.

// CHECK: Type info:
// X64-NEXT: (multi_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// X64-NEXT:   (case name=a index=0 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))
// X64-NEXT:   (case name=b index=1 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))
// X64-NEXT:   (case name=c index=2 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1)))

// CHECK: Mangled name: $s34reflect_Enum_MultiPayload_generic71AOyytG
// CHECK-NEXT: Demangled name: reflect_Enum_MultiPayload_generic7.A<()>

// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=a index=0
// CHECK-NEXT:   (tuple)
// CHECK-NEXT: )


reflect(enum: A<Void>.b(()))

// CHECK: Reflecting an enum.
// CHECK: Type info:
// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=b index=1
// CHECK-NEXT:   (tuple)
// CHECK-NEXT: )



// This is always laid out like an MPE
enum B<T> {
case a(Int)
case b(T)
case c(Void)
}

reflect(enum: B<Int>.a(1))

// CHECK: Reflecting an enum.
// CHECK-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum reflect_Enum_MultiPayload_generic7.B
// CHECK-NEXT:     (struct Swift.Int))

// CHECK: Type info:
// X64-NEXT: (multi_payload_enum size=9 alignment=8 stride=16 num_extra_inhabitants=253 bitwise_takable=1
// X64-NEXT:   (case name=a index=0 offset=0
// X64-NEXT:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// X64-NEXT:       (field name=_value offset=0
// X64-NEXT:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// X64-NEXT:   (case name=b index=1 offset=0
// X64-NEXT:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// X64-NEXT:       (field name=_value offset=0
// X64-NEXT:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// X64-NEXT:   (case name=c index=2 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1)))

// CHECK: Mangled name: $s34reflect_Enum_MultiPayload_generic71BOySiG
// CHECK-NEXT: Demangled name: reflect_Enum_MultiPayload_generic7.B<Swift.Int>

// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=a index=0
// CHECK-NEXT:   (struct Swift.Int)
// CHECK-NEXT: )

reflect(enum: B<Int>.b(2))

// CHECK: Reflecting an enum.
// CHECK: Type info:
// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=b index=1
// CHECK-NEXT:   (struct Swift.Int)
// CHECK-NEXT: )

reflect(enum: B<Int>.c(()))

// CHECK: Reflecting an enum.
// CHECK: Type info:
// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=c index=2
// CHECK-NEXT:   (tuple)
// CHECK-NEXT: )

reflect(enum: B<Void>.a(8))

// CHECK: Reflecting an enum.

// CHECK: Type info:
// X64-NEXT: (multi_payload_enum size=9 alignment=8 stride=16 num_extra_inhabitants=253 bitwise_takable=1
// X64-NEXT:   (case name=a index=0 offset=0
// X64-NEXT:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// X64-NEXT:       (field name=_value offset=0
// X64-NEXT:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// X64-NEXT:   (case name=b index=1 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))
// X64-NEXT:   (case name=c index=2 offset=0
// X64-NEXT:     (tuple size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1)))

// CHECK: Mangled name: $s34reflect_Enum_MultiPayload_generic71BOyytG
// CHECK-NEXT: Demangled name: reflect_Enum_MultiPayload_generic7.B<()>

// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=a index=0
// CHECK-NEXT:   (struct Swift.Int)
// CHECK-NEXT: )

reflect(enum: B<Void>.b(()))

// CHECK: Reflecting an enum.
// CHECK: Type info:
// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=b index=1
// CHECK-NEXT:   (tuple)
// CHECK-NEXT: )

reflect(enum: B<Void>.c(()))

// CHECK: Reflecting an enum.
// CHECK: Type info:
// CHECK: Enum value:
// CHECK-NEXT: (enum_value name=c index=2
// CHECK-NEXT:   (tuple)
// CHECK-NEXT: )

doneReflecting()

// CHECK: Done.

