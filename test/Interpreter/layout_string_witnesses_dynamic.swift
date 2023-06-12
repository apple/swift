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

func testPrespecializedAnyObject() {
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

testPrespecializedAnyObject()

func testPrespecializedSimpleClass() {
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

testPrespecializedSimpleClass()


func testPrespecializedInt() {
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

testPrespecializedInt()

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
