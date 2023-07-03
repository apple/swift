// RUN: %target-swift-frontend -module-name test -enable-objc-interop -emit-ir -verify -primary-file %s %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefixes=FILE1,FILE1-objc %s
// RUN: %target-swift-frontend -module-name test -enable-objc-interop -emit-ir -verify %s -primary-file %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefix=FILE2 %s

// RUN: %target-swift-frontend -module-name test -disable-objc-interop -emit-ir -verify -primary-file %s %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefixes=FILE1,FILE1-native %s
// RUN: %target-swift-frontend -module-name test -disable-objc-interop -emit-ir -verify %s -primary-file %S/Inputs/require-layout-generic-class.swift | %FileCheck --check-prefix=FILE2 %s

// REQUIRES: CPU=x86_64

// The offset of the typemetadata in the class typemetadata must match.

// FILE1-LABEL: define internal swiftcc void @"$s4test12requestType21xyx_tlFyAA3SubCyxG_Sit_tXEfU_
// FILE1: entry:
// FILE1:   [[TYPEMETADATA:%.*]] = load ptr, ptr %0
// FILE1-objc:     [[T_PTR:%.*]] = getelementptr inbounds ptr, ptr [[TYPEMETADATA]], i64 16
// FILE1-native:   [[T_PTR:%.*]] = getelementptr inbounds ptr, ptr [[TYPEMETADATA]], i64 13
// FILE1:   [[T:%.*]] = load ptr, ptr [[T_PTR]]
// FILE1:   call swiftcc %swift.metadata_response @"$s4test3SubCMa"(i64 255, ptr [[T]])

public func requestType2<T>(x: T) {
  requestTypeThrough(closure: { x in print(x) }, arg: x)
}
// FILE2-LABEL: define internal ptr @"$s4test3SubCMi"(ptr %0, ptr %1, ptr %2)
// FILE2:   [[T:%.*]] = load ptr, ptr %1
// FILE2:   [[CLASSMETADATA:%.*]] = call ptr @swift_allocateGenericClassMetadata
