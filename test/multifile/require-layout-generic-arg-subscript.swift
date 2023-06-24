// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name test -enable-objc-interop -emit-ir -verify -primary-file %s %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefixes=FILE1,FILE1-objc %s
// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name test -enable-objc-interop -emit-ir -verify %s -primary-file %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefix=FILE2 %s

// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name test -disable-objc-interop -emit-ir -verify -primary-file %s %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefixes=FILE1,FILE1-native %s
// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name test -disable-objc-interop -emit-ir -verify %s -primary-file %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefix=FILE2 %s

// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name test -enable-objc-interop -emit-ir -verify -primary-file %s %S/Inputs/require-layout-generic-class.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name test -disable-objc-interop -emit-ir -verify -primary-file %s %S/Inputs/require-layout-generic-class.swift

// REQUIRES: CPU=x86_64

// The offset of the typemetadata in the class typemetadata must match.

// FILE1: define hidden swiftcc i64 @"$s4test12AccessorTestCySiAA3SubCyxGcluig"(%T4test3SubC* %0, %T4test12AccessorTestC* swiftself %1)
// FILE1:   [[T1:%.*]] = bitcast %T4test3SubC* %0 to %swift.type**
// FILE1:   [[TYPEMETADATA:%.*]] = load %swift.type*, %swift.type** [[T1]]
// FILE1:   [[T2:%.*]] = bitcast %swift.type* [[TYPEMETADATA]] to %swift.type**
// FILE1-objc:     [[T_IN_CLASSMETADATA:%.*]] = getelementptr inbounds %swift.type*, %swift.type** [[T2]], i64 16
// FILE1-native:   [[T_IN_CLASSMETADATA:%.*]] = getelementptr inbounds %swift.type*, %swift.type** [[T2]], i64 13
// FILE1:   [[T:%.*]] = load %swift.type*, %swift.type** [[T_IN_CLASSMETADATA]]
// FILE1:   call %swift.type* @swift_getMetatypeMetadata(%swift.type* [[T]])
public class AccessorTest {
  subscript<T>(_ a: Sub<T>) -> Int {
    get {
      print(T.self)
      return 1
    }
  }
}

// FILE2-LABEL: define internal %swift.type* @"$s4test3SubCMi"(%swift.type_descriptor* %0, i8** %1, i8* %2)
// FILE2:   [[T_ADDR:%.*]] = bitcast i8** %1 to %swift.type**
// FILE2:   [[T:%.*]] = load %swift.type*, %swift.type** [[T_ADDR]]
// FILE2:   [[CLASSMETADATA:%.*]] = call %swift.type* @swift_allocateGenericClassMetadata
