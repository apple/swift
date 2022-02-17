// RUN: %empty-directory(%t)
// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -o %t/reflect_Enum_MultiPayload_value
// RUN: %target-codesign %t/reflect_Enum_MultiPayload_value

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_MultiPayload_value | tee /dev/stderr | %FileCheck %s --check-prefix=CHECK --check-prefix=X%target-ptrsize --dump-input=fail

// REQUIRES: reflection_test_support
// REQUIRES: objc_interop
// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib

import SwiftReflectionTest

enum MinimalMPE {
case A(Int8)
case B(Int8)
}

reflect(enumValue: MinimalMPE.A(1))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MinimalMPE)
// CHECK-NEXT: Value: .A(_)

reflect(enumValue: MinimalMPE.B(2))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MinimalMPE)
// CHECK-NEXT: Value: .B(_)

reflect(enumValue: MinimalMPE?.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MinimalMPE))
// CHECK-NEXT: Value: .none

reflect(enumValue: MinimalMPE??.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MinimalMPE)))
// CHECK-NEXT: Value: .none

reflect(enumValue: MinimalMPE??.some(.none))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MinimalMPE)))
// CHECK-NEXT: Value: .some(.none)

reflect(enumValue: MinimalMPE??.some(.some(.A(0))))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MinimalMPE)))
// CHECK-NEXT: Value: .some(.some(.A(_)))

enum MPEWithInts {
case stampA
case envelopeA(Int64)
case stampB
case envelopeB(Double)
case stampC
case envelopeC((Int32, Int32))
case stampD
case stampE
}

reflect(enumValue: MPEWithInts.envelopeA(88))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithInts)
// CHECK-NEXT: Value: .envelopeA(_)

reflect(enumValue: MPEWithInts.envelopeC((88, 99)))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithInts)
// CHECK-NEXT: Value: .envelopeC(_)

reflect(enumValue: MPEWithInts.stampE)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithInts)
// CHECK-NEXT: Value: .stampE

reflect(enumValue: Optional<MPEWithInts>.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (enum reflect_Enum_MultiPayload_value.MPEWithInts))
// CHECK-NEXT: Value: .none

reflect(enumValue: Optional<MPEWithInts>.some(.stampE))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (enum reflect_Enum_MultiPayload_value.MPEWithInts))
// CHECK-NEXT: Value: .some(.stampE)

reflect(enumValue: Optional<Optional<MPEWithInts>>.some(.some(.stampE)))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.MPEWithInts)))
// CHECK-NEXT: Value: .some(.some(.stampE))

reflect(enumValue: Optional<Optional<MPEWithInts>>.some(.none))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.MPEWithInts)))
// CHECK-NEXT: Value: .some(.none)

reflect(enumValue: Optional<Optional<MPEWithInts>>.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.MPEWithInts)))
// CHECK-NEXT: Value: .none

enum SPEWithMPEPayload {
case payloadA(MPEWithInts)
case alsoA
case alsoB
case alsoC
case alsoD
}

reflect(enumValue: SPEWithMPEPayload.payloadA(.stampB))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SPEWithMPEPayload)
// CHECK-NEXT: Value: .payloadA(.stampB)

reflect(enumValue: SPEWithMPEPayload.payloadA(.envelopeC((1,2))))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SPEWithMPEPayload)
// CHECK-NEXT: Value: .payloadA(.envelopeC(_))

reflect(enumValue: SPEWithMPEPayload.alsoC)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SPEWithMPEPayload)
// CHECK-NEXT: Value: .alsoC

reflect(enumValue: Optional<Optional<SPEWithMPEPayload>>.some(.none))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:  (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (enum reflect_Enum_MultiPayload_value.SPEWithMPEPayload)))
// CHECK-NEXT: Value: .some(.none)

enum SmallMPEWithInts {
case stampA
case envelopeA(Int8)
case stampB
case envelopeB(Int16)
case stampC
case envelopeC(UInt8, Int8)
case stampD
case stampE
}

reflect(enumValue: SmallMPEWithInts.envelopeA(88))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts)
// CHECK-NEXT: Value: .envelopeA(_)

reflect(enumValue: SmallMPEWithInts.envelopeC(88, 99))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts)
// CHECK-NEXT: Value: .envelopeC(_)

reflect(enumValue: SmallMPEWithInts.stampE)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts)
// CHECK-NEXT: Value: .stampE

reflect(enumValue: Optional<SmallMPEWithInts>.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts))
// CHECK-NEXT: Value: .none

reflect(enumValue: Optional<SmallMPEWithInts>.some(.stampE))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts))
// CHECK-NEXT: Value: .some(.stampE)

reflect(enumValue: Optional<Optional<SmallMPEWithInts>>.some(.some(.stampE)))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts)))
// CHECK-NEXT: Value: .some(.some(.stampE))

reflect(enumValue: Optional<Optional<SmallMPEWithInts>>.some(.none))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts)))
// CHECK-NEXT: Value: .some(.none)

reflect(enumValue: Optional<Optional<SmallMPEWithInts>>.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.SmallMPEWithInts)))
// CHECK-NEXT: Value: .none

enum SPEWithSmallMPEPayload {
case payloadA(SmallMPEWithInts)
case alsoA
case alsoB
case alsoC
case alsoD
}

reflect(enumValue: SPEWithSmallMPEPayload.payloadA(.stampB))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SPEWithSmallMPEPayload)
// CHECK-NEXT: Value: .payloadA(.stampB)

reflect(enumValue: SPEWithSmallMPEPayload.payloadA(.envelopeC(1,2)))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SPEWithSmallMPEPayload)
// CHECK-NEXT: Value: .payloadA(.envelopeC(_))

reflect(enumValue: SPEWithSmallMPEPayload.alsoC)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.SPEWithSmallMPEPayload)
// CHECK-NEXT: Value: .alsoC

reflect(enumValue: Optional<Optional<SPEWithSmallMPEPayload>>.some(.none))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:  (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (enum reflect_Enum_MultiPayload_value.SPEWithSmallMPEPayload)))
// CHECK-NEXT: Value: .some(.none)

class ClassA { let a = 7 }
class ClassB { let b = 8 }
class ClassC { let c = 9 }
enum MPEWithPointers {
case classA(ClassA)
case classB(ClassB)
case classC(ClassC)
case classD(ClassC)
case emptyA
case emptyB
case emptyC
}

reflect(enumValue: MPEWithPointers.classA(ClassA()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointers)
// CHECK-NEXT: Value: .classA(_)

reflect(enumValue: MPEWithPointers.classC(ClassC()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointers)
// CHECK-NEXT: Value: .classC(_)

reflect(enumValue: MPEWithPointers.classD(ClassC()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointers)
// CHECK-NEXT: Value: .classD(_)

reflect(enumValue: MPEWithPointers.emptyA)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointers)
// CHECK-NEXT: Value: .emptyA

reflect(enumValue: MPEWithPointers.emptyB)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointers)
// CHECK-NEXT: Value: .emptyB

reflect(enumValue: MPEWithPointers.emptyC)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointers)
// CHECK-NEXT: Value: .emptyC

reflect(enumValue: Optional<MPEWithPointers>.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (enum reflect_Enum_MultiPayload_value.MPEWithPointers))
// CHECK-NEXT: Value: .none

reflect(enumValue: Optional<Optional<MPEWithPointers>>.some(.none))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.MPEWithPointers)))
// CHECK-NEXT: Value: .some(.none)

reflect(enumValue: Optional<Optional<MPEWithPointers>>.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (enum reflect_Enum_MultiPayload_value.MPEWithPointers)))
// CHECK-NEXT: Value: .none


enum MPEWithPointerMaskedByInt {
case classA(ClassA)
case classB(ClassB)
case classC(Int)
case emptyA
case emptyB
case emptyC
}

reflect(enumValue: MPEWithPointerMaskedByInt.classC(7))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointerMaskedByInt)
// CHECK-NEXT: Value: .classC(_)

reflect(enumValue: MPEWithPointerMaskedByInt.emptyC)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointerMaskedByInt)
// CHECK-NEXT: Value: .emptyC

reflect(enumValue: MPEWithPointerMaskedByInt????????????.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointerMaskedByInt)))))))))))))
// CHECK-NEXT: Value: .none

enum MPEWithPointerPartlyMaskedByInt {
case classAB(ClassA, ClassB)
case maskerA(Int)
case emptyA
case emptyB
case emptyC
case emptyD
case emptyE
case emptyF
case emptyG
case emptyH
}

reflect(enumValue: MPEWithPointerPartlyMaskedByInt.classAB(ClassA(),ClassB()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointerPartlyMaskedByInt)
// CHECK-NEXT: Value: .classAB(_)

reflect(enumValue: MPEWithPointerPartlyMaskedByInt.emptyH)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointerPartlyMaskedByInt)
// CHECK-NEXT: Value: .emptyH

reflect(enumValue: MPEWithPointerPartlyMaskedByInt?????.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithPointerPartlyMaskedByInt))))))
// CHECK-NEXT: Value: .none

struct StructA { let a = ClassA(); let b = ClassB(); }
struct StructB { let b = 12 }
enum MPEWithStruct {
case structA(StructA)
case structB(StructB)
}

reflect(enumValue: MPEWithStruct.structA(StructA()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct)
// CHECK-NEXT: Value: .structA(_)

reflect(enumValue: MPEWithStruct.structB(StructB()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct)
// CHECK-NEXT: Value: .structB(_)

reflect(enumValue: MPEWithStruct?.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct))
// CHECK-NEXT: Value: .none

reflect(enumValue: MPEWithStruct?.some(.structA(StructA())))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct))
// CHECK-NEXT: Value: .some(.structA(_))

reflect(enumValue: MPEWithStruct?.some(.structB(StructB())))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct))
// CHECK-NEXT: Value: .some(.structB(_))

reflect(enumValue: MPEWithStruct????.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct)))))
// CHECK-NEXT: Value: .none

reflect(enumValue: MPEWithStruct????.some(.none))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct)))))
// CHECK-NEXT: Value: .some(.none)

reflect(enumValue: MPEWithStruct????.some(.some(.none)))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct)))))
// CHECK-NEXT: Value: .some(.some(.none))

reflect(enumValue: MPEWithStruct????.some(.some(.some(.none))))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct)))))
// CHECK-NEXT: Value: .some(.some(.some(.none)))

reflect(enumValue: MPEWithStruct????.some(.some(.some(.some(.structA(StructA()))))))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT: (enum reflect_Enum_MultiPayload_value.MPEWithStruct)))))
// CHECK-NEXT: Value: .some(.some(.some(.some(.structA(_)))))

enum Either<T,U> {
case left(T)
case right(U)
}

reflect(enumValue: Either<Int,Double>.left(7))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:   (struct Swift.Int)
// CHECK-NEXT:   (struct Swift.Double))
// CHECK-NEXT: Value: .left(_)

reflect(enumValue: Either<Int,Double>.right(1.0))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:   (struct Swift.Int)
// CHECK-NEXT:   (struct Swift.Double))
// CHECK-NEXT: Value: .right(_)

reflect(enumValue: Either<Int,Double>?.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:     (struct Swift.Int)
// CHECK-NEXT:     (struct Swift.Double)))
// CHECK-NEXT: Value: .none

reflect(enumValue: Either<Int,Double>??.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:       (struct Swift.Int)
// CHECK-NEXT:       (struct Swift.Double))))
// CHECK-NEXT: Value: .none

reflect(enumValue: Either<Int,Double>???.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (bound_generic_enum Swift.Optional
// CHECK-NEXT:       (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:         (struct Swift.Int)
// CHECK-NEXT:         (struct Swift.Double)))))
// CHECK-NEXT: Value: .none

reflect(enumValue: Either<Int,Double>????.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (bound_generic_enum Swift.Optional
// CHECK-NEXT:       (bound_generic_enum Swift.Optional
// CHECK-NEXT:         (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:           (struct Swift.Int)
// CHECK-NEXT:           (struct Swift.Double))))))
// CHECK-NEXT: Value: .none

reflect(enumValue: Either<ClassA,ClassB>.left(ClassA()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:   (class reflect_Enum_MultiPayload_value.ClassA)
// CHECK-NEXT:   (class reflect_Enum_MultiPayload_value.ClassB))
// CHECK-NEXT: Value: .left(_)

reflect(enumValue: Either<ClassA,ClassB>.right(ClassB()))

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:   (class reflect_Enum_MultiPayload_value.ClassA)
// CHECK-NEXT:   (class reflect_Enum_MultiPayload_value.ClassB))
// CHECK-NEXT: Value: .right(_)

reflect(enumValue: Either<ClassA,ClassB>?.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:     (class reflect_Enum_MultiPayload_value.ClassA)
// CHECK-NEXT:     (class reflect_Enum_MultiPayload_value.ClassB))
// CHECK-NEXT: Value: .none

reflect(enumValue: Either<ClassA,ClassB>??.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:       (class reflect_Enum_MultiPayload_value.ClassA)
// CHECK-NEXT:       (class reflect_Enum_MultiPayload_value.ClassB))
// CHECK-NEXT: Value: .none

reflect(enumValue: Either<ClassA,ClassB>???.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (bound_generic_enum Swift.Optional
// CHECK-NEXT:       (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:         (class reflect_Enum_MultiPayload_value.ClassA)
// CHECK-NEXT:         (class reflect_Enum_MultiPayload_value.ClassB))
// CHECK-NEXT: Value: .none

reflect(enumValue: Either<ClassA,ClassB>????.none)

// CHECK: Reflecting an enum value.
// CHECK-NEXT: Type reference:
// CHECK-NEXT: (bound_generic_enum Swift.Optional
// CHECK-NEXT:   (bound_generic_enum Swift.Optional
// CHECK-NEXT:     (bound_generic_enum Swift.Optional
// CHECK-NEXT:       (bound_generic_enum Swift.Optional
// CHECK-NEXT:         (bound_generic_enum reflect_Enum_MultiPayload_value.Either
// CHECK-NEXT:           (class reflect_Enum_MultiPayload_value.ClassA)
// CHECK-NEXT:           (class reflect_Enum_MultiPayload_value.ClassB))
// CHECK-NEXT: Value: .none

doneReflecting()

// CHECK: Done.

