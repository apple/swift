// RUN: %empty-directory(%t)
//
// RUN: %target-clang -fobjc-arc %S/Inputs/ObjCClasses/ObjCClasses.m -c -o %t/ObjCClasses.o
// RUN: %target-swift-frontend -prespecialize-generic-metadata -enable-experimental-feature LayoutStringValueWitnesses -enable-experimental-feature LayoutStringValueWitnessesInstantiation -enable-layout-string-value-witnesses -enable-layout-string-value-witnesses-instantiation -enable-type-layout -enable-autolinking-runtime-compatibility-bytecode-layouts -parse-stdlib -emit-module -emit-module-path=%t/layout_string_witnesses_types.swiftmodule %S/Inputs/layout_string_witnesses_types.swift
// RUN: %target-build-swift-dylib(%t/%target-library-name(layout_string_witnesses_types)) -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnesses -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnessesInstantiation -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation -Xfrontend -enable-type-layout -Xfrontend -parse-stdlib -parse-as-library %S/Inputs/layout_string_witnesses_types.swift
// RUN: %target-codesign %t/%target-library-name(layout_string_witnesses_types)
// RUN: %target-build-swift -g -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnesses -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnessesInstantiation -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation -Xfrontend -enable-type-layout -parse-stdlib -module-name layout_string_witnesses_dynamic -llayout_string_witnesses_types -L%t -I %S/Inputs/ObjCClasses/ %t/ObjCClasses.o -I %t -o %t/main %s %target-rpath(%t)
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main %t/%target-library-name(layout_string_witnesses_types) | %FileCheck %s --check-prefix=CHECK

// REQUIRES: executable_test
// REQUIRES: objc_interop

import Swift
import layout_string_witnesses_types
import ObjCClasses
import Foundation

func testNestedResilientObjc() {
    let ptr = allocateInternalGenericPtr(of: NestedWrapper<ObjCPrintOnDealloc>.self)

    do {
        let x = NestedWrapper<ObjCPrintOnDealloc>(x: .init(x: ObjCPrintOnDealloc()), y: .init(x: ObjCPrintOnDealloc()))
        testGenericInit(ptr, to: x)
    }

    do {
        let y = NestedWrapper<ObjCPrintOnDealloc>(x: .init(x: ObjCPrintOnDealloc()), y: .init(x: ObjCPrintOnDealloc()))
        // CHECK: Before deinit
        print("Before deinit")

        // CHECK-NEXT: ObjCPrintOnDealloc deinitialized!
        // CHECK-NEXT: ObjCPrintOnDealloc deinitialized!
        testGenericAssign(ptr, from: y)
    }

    // CHECK-NEXT: Before deinit
    print("Before deinit")

    // CHECK-NEXT: ObjCPrintOnDealloc deinitialized!
    // CHECK-NEXT: ObjCPrintOnDealloc deinitialized!
    testGenericDestroy(ptr, of: NestedWrapper<ObjCPrintOnDealloc>.self)

    ptr.deallocate()
}

testNestedResilientObjc()
