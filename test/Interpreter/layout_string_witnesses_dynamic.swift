// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -prespecialize-generic-metadata -enable-experimental-feature LayoutStringValueWitnesses -enable-experimental-feature LayoutStringValueWitnessesInstantiation -enable-layout-string-value-witnesses -enable-layout-string-value-witnesses-instantiation -enable-type-layout -enable-autolinking-runtime-compatibility-bytecode-layouts -parse-stdlib -emit-module -emit-module-path=%t/layout_string_witnesses_types.swiftmodule %S/Inputs/layout_string_witnesses_types.swift
// RUN: %target-build-swift-dylib(%t/%target-library-name(layout_string_witnesses_types)) -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnesses -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnessesInstantiation -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation -Xfrontend -enable-type-layout -Xfrontend -parse-stdlib -parse-as-library %S/Inputs/layout_string_witnesses_types.swift
// RUN: %target-codesign %t/%target-library-name(layout_string_witnesses_types)
// RUN: %target-swift-frontend -enable-experimental-feature LayoutStringValueWitnesses -enable-experimental-feature LayoutStringValueWitnessesInstantiation -enable-layout-string-value-witnesses -enable-layout-string-value-witnesses-instantiation -enable-library-evolution -enable-autolinking-runtime-compatibility-bytecode-layouts -emit-module -emit-module-path=%t/layout_string_witnesses_types_resilient.swiftmodule %S/Inputs/layout_string_witnesses_types_resilient.swift
// RUN: %target-build-swift -g -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnesses -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnessesInstantiation -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation -Xfrontend -enable-library-evolution -c -parse-as-library -o %t/layout_string_witnesses_types_resilient.o %S/Inputs/layout_string_witnesses_types_resilient.swift
// RUN: %target-build-swift -g -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnesses -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnessesInstantiation -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation -Xfrontend -enable-type-layout -parse-stdlib -module-name layout_string_witnesses_dynamic -llayout_string_witnesses_types -L%t %t/layout_string_witnesses_types_resilient.o -I %t -o %t/main %s %target-rpath(%t)
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main %t/%target-library-name(layout_string_witnesses_types) | %FileCheck %s --check-prefix=CHECK -check-prefix=CHECK-%target-os

// REQUIRES: executable_test

// UNSUPPORTED: back_deployment_runtime

import Swift
import layout_string_witnesses_types
import layout_string_witnesses_types_resilient

class TestClass {
    init() {}

    deinit {
        print("TestClass deinitialized!")
    }
}

func testGeneric() {
    let ptr = allocateInternalGenericPtr(of: TestClass.self)
    
    do {
        let x = TestClass()
        testGenericInit(ptr, to: x)
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: TestClass.self)

    ptr.deallocate()
}

testGeneric()

func testPrespecializedStructAnyObject() {
    let ptr = UnsafeMutablePointer<PrespecializedStruct<AnyObject>>.allocate(capacity: 1)

    do {
        let x = PrespecializedStruct<AnyObject>(x: SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedStruct<AnyObject>(x: SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedStructAnyObject()

func testPrespecializedStructSimpleClass() {
    let ptr = UnsafeMutablePointer<PrespecializedStruct<SimpleClass>>.allocate(capacity: 1)

    do {
        let x = PrespecializedStruct<SimpleClass>(x: SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedStruct<SimpleClass>(x: SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedStructSimpleClass()


func testPrespecializedStructInt() {
    let ptr = UnsafeMutablePointer<PrespecializedStruct<Int>>.allocate(capacity: 1)

    do {
        let x = PrespecializedStruct<Int>(x: 23)
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedStruct<Int>(x: 32)
        testAssign(ptr, from: y)
    }

    ptr.deallocate()
}

testPrespecializedStructInt()

func testPrespecializedSingletonEnumAnyObject() {
    let ptr = UnsafeMutablePointer<PrespecializedSingletonEnum<AnyObject>>.allocate(capacity: 1)

    do {
        let x = PrespecializedSingletonEnum<AnyObject>.only(23, SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedSingletonEnum<AnyObject>.only(32, SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedSingletonEnumAnyObject()

func testPrespecializedSingletonEnumSimpleClass() {
    let ptr = UnsafeMutablePointer<PrespecializedSingletonEnum<SimpleClass>>.allocate(capacity: 1)

    do {
        let x = PrespecializedSingletonEnum<SimpleClass>.only(23, SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedSingletonEnum<SimpleClass>.only(32, SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedSingletonEnumSimpleClass()


func testPrespecializedSingletonEnumInt() {
    let ptr = UnsafeMutablePointer<PrespecializedSingletonEnum<Int>>.allocate(capacity: 1)

    do {
        let x = PrespecializedSingletonEnum<Int>.only(23, 23)
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedSingletonEnum<Int>.only(32, 32)
        testAssign(ptr, from: y)
    }

    ptr.deallocate()
}

testPrespecializedSingletonEnumInt()

func testPrespecializedSinglePayloadEnumAnyObject() {
    let ptr = UnsafeMutablePointer<PrespecializedSinglePayloadEnum<AnyObject>>.allocate(capacity: 1)

    do {
        let x = PrespecializedSinglePayloadEnum<AnyObject>.nonEmpty(23, SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedSinglePayloadEnum<AnyObject>.nonEmpty(32, SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedSinglePayloadEnumAnyObject()

func testPrespecializedSinglePayloadEnumSimpleClass() {
    let ptr = UnsafeMutablePointer<PrespecializedSinglePayloadEnum<SimpleClass>>.allocate(capacity: 1)

    do {
        let x = PrespecializedSinglePayloadEnum<SimpleClass>.nonEmpty(23, SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedSinglePayloadEnum<SimpleClass>.nonEmpty(32, SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedSinglePayloadEnumSimpleClass()


func testPrespecializedSinglePayloadEnumInt() {
    let ptr = UnsafeMutablePointer<PrespecializedSinglePayloadEnum<Int>>.allocate(capacity: 1)

    do {
        let x = PrespecializedSinglePayloadEnum<Int>.nonEmpty(23, 23)
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedSinglePayloadEnum<Int>.nonEmpty(32, 32)
        testAssign(ptr, from: y)
    }

    ptr.deallocate()
}

testPrespecializedSinglePayloadEnumInt()

func testPrespecializedMultiPayloadEnumAnyObject() {
    let ptr = UnsafeMutablePointer<PrespecializedMultiPayloadEnum<AnyObject>>.allocate(capacity: 1)

    do {
        let x = PrespecializedMultiPayloadEnum<AnyObject>.nonEmpty0(23, SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedMultiPayloadEnum<AnyObject>.nonEmpty0(32, SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedMultiPayloadEnumAnyObject()

func testPrespecializedMultiPayloadEnumSimpleClass() {
    let ptr = UnsafeMutablePointer<PrespecializedMultiPayloadEnum<SimpleClass>>.allocate(capacity: 1)

    do {
        let x = PrespecializedMultiPayloadEnum<SimpleClass>.nonEmpty0(23, SimpleClass(x: 23))
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedMultiPayloadEnum<SimpleClass>.nonEmpty0(32, SimpleClass(x: 32))

        // CHECK-NEXT: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")


    // CHECK-NEXT: SimpleClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testPrespecializedMultiPayloadEnumSimpleClass()


func testPrespecializedMultiPayloadEnumInt() {
    let ptr = UnsafeMutablePointer<PrespecializedMultiPayloadEnum<Int>>.allocate(capacity: 1)

    do {
        let x = PrespecializedMultiPayloadEnum<Int>.nonEmpty0(23, 23)
        testInit(ptr, to: x)
    }

    do {
        let y = PrespecializedMultiPayloadEnum<Int>.nonEmpty0(32, 32)
        testAssign(ptr, from: y)
    }

    ptr.deallocate()
}

testPrespecializedMultiPayloadEnumInt()

func testGenericTuple() {
    let ptr = allocateInternalGenericPtr(of: GenericTupleWrapper<TestClass>.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: GenericTupleWrapper((x, 32)))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: GenericTupleWrapper((y, 32)))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: GenericTupleWrapper<TestClass>.self)

    ptr.deallocate()
}

testGenericTuple()

func testGenericNested() {
    let ptr = allocateInternalGenericPtr(of: GenericNestedOuter<TestClass>.Inner.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: GenericNestedOuter<TestClass>.Inner(x))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: GenericNestedOuter<TestClass>.Inner(y))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: GenericNestedOuter<TestClass>.Inner.self)

    ptr.deallocate()
}

testGenericNested()

func testGenericNestedRef() {
    let ptr = allocateInternalGenericPtr(of: GenericNestedRefOuter<TestClass>.Inner.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: GenericNestedRefOuter<TestClass>.Inner(x))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: GenericNestedRefOuter<TestClass>.Inner(y))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: GenericNestedRefOuter<TestClass>.Inner.self)

    ptr.deallocate()
}

testGenericNestedRef()

func testGenericEnum() {
    let ptr = allocateInternalGenericPtr(of: GenericEnumWrapper<Int>.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: GenericEnumWrapper<Int>(.a(x, 32), 32))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: GenericEnumWrapper<Int>(.a(y, 45), 45))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: GenericEnumWrapper<Int>.self)

    ptr.deallocate()
}

testGenericEnum()

func testGenericEnumSingleton() {
    let ptr = allocateInternalGenericPtr(of: SingletonEnum<TestClass>.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: SingletonEnum<TestClass>.only(x, 23))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: SingletonEnum<TestClass>.only(y, 32))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: SingletonEnum<TestClass>.self)

    ptr.deallocate()
}

testGenericEnumSingleton()

func testRecursive() {
    let ptr = allocateInternalGenericPtr(of: Recursive<TestClass>.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: Recursive<TestClass>(x, nil))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: Recursive<TestClass>(y, nil))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: Recursive<TestClass>.self)

    ptr.deallocate()
}

testRecursive()

func testComplexNesting() {
    let ptr = allocateInternalGenericPtr(of: ComplexNesting<Int, TestClass, TestClass, TestClass>.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: ComplexNesting<Int, TestClass, TestClass, TestClass>(34, x, x, x))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: ComplexNesting<Int, TestClass, TestClass, TestClass>(34, y, y, y))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: ComplexNesting<Int, TestClass, TestClass, TestClass>.self)

    ptr.deallocate()
}

testComplexNesting()

enum TestEnum {
    case empty
    case nonEmpty(TestClass)
}

func testGenericWithEnumNonEmpty() {
    let ptr = allocateInternalGenericPtr(of: TestEnum.self)

    do {
        let x = TestClass()
        testGenericInit(ptr, to: TestEnum.nonEmpty(x))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: TestEnum.nonEmpty(y))
    }

    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: TestEnum.self)

    ptr.deallocate()
}

testGenericWithEnumNonEmpty()

public struct ResilientWrapper {
    let x: SimpleResilient
    let y: Int
}

func testResilient() {
    let ptr = UnsafeMutablePointer<ResilientWrapper>.allocate(capacity: 1)

    do {
        let x = TestClass()
        testInit(ptr, to: ResilientWrapper(x: SimpleResilient(x: 23, y: x), y: 5))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testAssign(ptr, from: ResilientWrapper(x: SimpleResilient(x: 23, y: y), y: 7))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testDestroy(ptr)

    ptr.deallocate()
}

testResilient()

public struct GenericResilientWrapper<T> {
    let x: GenericResilient<T, Int>
    let y: Int
}

func testGenericResilient() {
    let ptr = UnsafeMutablePointer<GenericResilientWrapper<TestClass>>.allocate(capacity: 1)

    do {
        let x = TestClass()
        testInit(ptr, to: GenericResilientWrapper(x: GenericResilient(x: x, y: 32), y: 32))
    }

    do {
        let y = TestClass()
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: TestClass deinitialized!
        testAssign(ptr, from: GenericResilientWrapper(x: GenericResilient(x: y, y: 42), y: 32))
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: TestClass deinitialized!
    testDestroy(ptr)

    print("Dealloc")
    ptr.deallocate()
}

testGenericResilient()

func testMixedEnumWrapperWrapperGeneric() {
    let ptr = allocateInternalGenericPtr(of: MixedEnumWrapperWrapperGeneric<TestClass>.self)

    do {
        let x = MixedEnumWrapperWrapperGeneric(x: MixedEnumWrapper(x: .nonEmpty(SimpleClass(x: 23)),
                                                                   y: .e(SimpleClass(x: 32))),
                                               y: TestClass())
        testGenericInit(ptr, to: x)
    }

    do {
        let y = MixedEnumWrapperWrapperGeneric(x: MixedEnumWrapper(x: .nonEmpty(SimpleClass(x: 28)),
                                                                   y: .e(SimpleClass(x: 82))),
                                               y: TestClass())
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        // CHECK-NEXT: SimpleClass deinitialized!
        // CHECK-NEXT: TestClass deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: SimpleClass deinitialized!
    // CHECK-NEXT: SimpleClass deinitialized!
    // CHECK-NEXT: TestClass deinitialized!
    testGenericDestroy(ptr, of: MixedEnumWrapperWrapperGeneric<TestClass>.self)

    ptr.deallocate()
}

testMixedEnumWrapperWrapperGeneric()

func testGenericSinglePayloadEnum() {
    let ptr = allocateInternalGenericPtr(of: SinglePayloadEnum<SimpleClass>.self)

    do {
        let x = SinglePayloadEnum.nonEmpty(23, SimpleClass(x: 23))
        testGenericInit(ptr, to: x)
    }

    do {
        let y = SinglePayloadEnum.nonEmpty(32, SimpleClass(x: 32))
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: SimpleClass deinitialized!
    testGenericDestroy(ptr, of: SinglePayloadEnum<SimpleClass>.self)

    ptr.deallocate()
}

testGenericSinglePayloadEnum()

func testGenericSinglePayloadEnumManyXI() {
    let ptr = allocateInternalGenericPtr(of: SinglePayloadEnumManyXI.self)

    do {
        let x = SinglePayloadEnumManyXI.nonEmpty(Builtin.zeroInitializer(), SimpleClass(x: 23))
        testGenericInit(ptr, to: x)
    }

    do {
        let y = SinglePayloadEnumManyXI.nonEmpty(Builtin.zeroInitializer(), SimpleClass(x: 23))
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: SimpleClass deinitialized!
    testGenericDestroy(ptr, of: SinglePayloadEnumManyXI.self)

    ptr.deallocate()
}

testGenericSinglePayloadEnumManyXI()

struct RefPlusEnumResolve {
    let x: SimpleClass
    let y: ResilientSinglePayloadEnumComplex
}

func testRefPlusEnumResolve() {
    let ptr = allocateInternalGenericPtr(of: RefPlusEnumResolve.self)

    do {
        let x = RefPlusEnumResolve(x: SimpleClass(x: 23), y: .nonEmpty(.nonEmpty1(SimpleClass(x: 23))))
        testGenericInit(ptr, to: x)
    }

    do {
        let y = RefPlusEnumResolve(x: SimpleClass(x: 23), y: .nonEmpty(.nonEmpty1(SimpleClass(x: 23))))
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        // CHECK-NEXT: SimpleClass deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: SimpleClass deinitialized!
    // CHECK-NEXT: SimpleClass deinitialized!
    testGenericDestroy(ptr, of: RefPlusEnumResolve.self)

    ptr.deallocate()
}

testRefPlusEnumResolve()

func testResilientSingletonEnumTag() {
    let x = switch getResilientSingletonEnumNonEmpty(SimpleClass(x: 23)) {
    case .nonEmpty: 0
    }

    // CHECK: Enum case: 0
    print("Enum case: \(x)")
}

testResilientSingletonEnumTag()

func testResilientSinglePayloadEnumSimpleTag() {
    let x = switch getResilientSinglePayloadEnumSimpleEmpty0() {
    case .nonEmpty: 0
    case .empty0: 1
    case .empty1: 2
    }

    // CHECK: Enum case: 1
    print("Enum case: \(x)")
}

testResilientSinglePayloadEnumSimpleTag()

func testResilientSinglePayloadEnumComplexTag() {
    let x = switch getResilientSinglePayloadEnumComplexEmpty0() {
    case .nonEmpty: 0
    case .empty0: 1
    case .empty1: 2
    }

    // CHECK: Enum case: 1
    print("Enum case: \(x)")
}

testResilientSinglePayloadEnumComplexTag()

func testResilientMultiPayloadEnumTag() {
    let x = switch getResilientMultiPayloadEnumEmpty0() {
    case .nonEmpty0: 0
    case .nonEmpty1: 1
    case .empty0: 2
    case .empty1: 3
    }

    // CHECK: Enum case: 2
    print("Enum case: \(x)")
}

testResilientMultiPayloadEnumTag()

func testResilientSinglePayloadEnumGenericTag() {
    let x = switch getResilientSinglePayloadEnumGenericEmpty0(AnyObject.self) {
    case .nonEmpty: 0
    case .empty0: 1
    case .empty1: 2
    }

    // CHECK: Enum case: 1
    print("Enum case: \(x)")
}

testResilientSinglePayloadEnumGenericTag()

func testResilientMultiPayloadEnumGenericTag() {
    let x = switch getResilientMultiPayloadEnumGenericEmpty0(AnyObject.self) {
    case .nonEmpty0: 0
    case .nonEmpty1: 1
    case .empty0: 2
    case .empty1: 3
    }

    // CHECK: Enum case: 2
    print("Enum case: \(x)")
}

testResilientMultiPayloadEnumGenericTag()

@inline(never)
func matchResilientSinglePayloadEnumSimpleTag(_ x: ResilientSinglePayloadEnumSimple) -> Int {
    return switch x {
    case .nonEmpty: 0
    case .empty0: 1
    case .empty1: 2
    }
}

func testResilientSinglePayloadEnumSimpleInjectTag() {
    let x = ResilientSinglePayloadEnumSimple.nonEmpty(SimpleClass(x: 23))
    let y = ResilientSinglePayloadEnumSimple.empty0
    let z = ResilientSinglePayloadEnumSimple.empty1

    // CHECK: Enum case: 0
    print("Enum case: \(matchResilientSinglePayloadEnumSimpleTag(x))")
    // CHECK: Enum case: 1
    print("Enum case: \(matchResilientSinglePayloadEnumSimpleTag(y))")
    // CHECK: Enum case: 2
    print("Enum case: \(matchResilientSinglePayloadEnumSimpleTag(z))")
}

testResilientSinglePayloadEnumSimpleInjectTag()

@inline(never)
func matchResilientSinglePayloadEnumGenericTag(_ x: ResilientSinglePayloadEnumGeneric<AnyObject>) -> Int {
    return switch x {
    case .nonEmpty: 0
    case .empty0: 1
    case .empty1: 2
    }
}

func testResilientSinglePayloadEnumGenericInjectTag() {
    let x = ResilientSinglePayloadEnumGeneric<AnyObject>.nonEmpty(SimpleClass(x: 23))
    let y = ResilientSinglePayloadEnumGeneric<AnyObject>.empty0
    let z = ResilientSinglePayloadEnumGeneric<AnyObject>.empty1

    // CHECK: Enum case: 0
    print("Enum case: \(matchResilientSinglePayloadEnumGenericTag(x))")
    // CHECK: Enum case: 1
    print("Enum case: \(matchResilientSinglePayloadEnumGenericTag(y))")
    // CHECK: Enum case: 2
    print("Enum case: \(matchResilientSinglePayloadEnumGenericTag(z))")
}

testResilientSinglePayloadEnumGenericInjectTag()

@inline(never)
func matchResilientMultiPayloadEnumGenericTag(_ x: ResilientMultiPayloadEnumGeneric<AnyObject>) -> Int {
    return switch x {
    case .nonEmpty0: 0
    case .nonEmpty1: 1
    case .empty0: 2
    case .empty1: 3
    }
}

func testResilientMultiPayloadEnumGenericInjectTag() {
    let x = ResilientMultiPayloadEnumGeneric<AnyObject>.nonEmpty0(SimpleClass(x: 23))
    let y = ResilientMultiPayloadEnumGeneric<AnyObject>.nonEmpty1(SimpleClass(x: 32))
    let z = ResilientMultiPayloadEnumGeneric<AnyObject>.empty0
    let w = ResilientMultiPayloadEnumGeneric<AnyObject>.empty1

    // CHECK: Enum case: 0
    print("Enum case: \(matchResilientMultiPayloadEnumGenericTag(x))")
    // CHECK: Enum case: 1
    print("Enum case: \(matchResilientMultiPayloadEnumGenericTag(y))")
    // CHECK: Enum case: 2
    print("Enum case: \(matchResilientMultiPayloadEnumGenericTag(z))")
    // CHECK: Enum case: 3
    print("Enum case: \(matchResilientMultiPayloadEnumGenericTag(w))")
}

testResilientMultiPayloadEnumGenericInjectTag()

@inline(never)
func matchResilientSingletonEnumTag(_ x: ResilientSingletonEnum) -> Int {
    return switch x {
    case .nonEmpty: 0
    }
}

func testResilientSingletonEnumGenericInjectTag() {
    let x = ResilientSingletonEnum.nonEmpty(SimpleClass(x: 23))

    // CHECK: Enum case: 0
    print("Enum case: \(matchResilientSingletonEnumTag(x))")
}

testResilientSingletonEnumGenericInjectTag()

enum ResilientPayloadSinglePayloadEnum {
    case empty0
    case empty1
    case empty2
    case nonEmpty(ResilientSinglePayloadEnum, Int)
}

func testResilientPayloadSinglePayloadEnum() {
    let xxx = ResilientPayloadSinglePayloadEnum.nonEmpty(.empty0, 1)

    // CHECK: nonEmpty(layout_string_witnesses_types_resilient.ResilientSinglePayloadEnum.empty0, 1)
    print(xxx)
}

testResilientPayloadSinglePayloadEnum()

struct SinglePayloadSimpleResolve {
    let x: ResilientSinglePayloadEnumSimple
    let y: ResilientSinglePayloadEnumComplex
}

func testSinglePayloadSimpleResolve() {
    let ptr = allocateInternalGenericPtr(of: SinglePayloadSimpleResolve.self)

    do {
        let x = SinglePayloadSimpleResolve(x: .nonEmpty(SimpleClass(x: 23)), y: .nonEmpty(.nonEmpty1(SimpleClass(x: 23))))
        testGenericInit(ptr, to: x)
    }

    do {
        let y = SinglePayloadSimpleResolve(x: .nonEmpty(SimpleClass(x: 32)), y: .nonEmpty(.nonEmpty1(SimpleClass(x: 32))))
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: SimpleClass deinitialized!
        // CHECK-NEXT: SimpleClass deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: SimpleClass deinitialized!
    // CHECK-NEXT: SimpleClass deinitialized!
    testGenericDestroy(ptr, of: SinglePayloadSimpleResolve.self)

    ptr.deallocate()
}

testSinglePayloadSimpleResolve()

#if os(macOS)

import Foundation

@objc
final class ObjcClass: NSObject {
    deinit {
        print("ObjcClass deinitialized!")
    }
}

func testGenericObjc() {
    let ptr = allocateInternalGenericPtr(of: ObjcClass.self)

    do {
        let x = ObjcClass()
        testGenericInit(ptr, to: x)
    }

    do {
        let y = ObjcClass()
        // CHECK-macosx: Before deinit
        print("Before deinit")

        // CHECK-macosx-NEXT: ObjcClass deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-macosx-NEXT: Before deinit
    print("Before deinit")

    // CHECK-macosx-NEXT: ObjcClass deinitialized!
    testGenericDestroy(ptr, of: ObjcClass.self)

    ptr.deallocate()
}

testGenericObjc()

#endif
