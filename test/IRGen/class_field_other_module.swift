// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -emit-module-path=%t/other_class.swiftmodule %S/Inputs/other_class.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -emit-ir -O -enforce-exclusivity=unchecked %s | %FileCheck %s -DINT=i%target-ptrsize
// RUN: %target-swift-frontend -I %t -emit-ir -O -enforce-exclusivity=unchecked %s

import other_class

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc i32 @"$s24class_field_other_module12getSubclassXys5Int32V0c1_A00F0CF"(%T11other_class8SubclassC* nocapture readonly %0)
// CHECK-NEXT: entry:
// CHECK-NEXT: %._value = getelementptr inbounds %T11other_class8SubclassC, %T11other_class8SubclassC* %0, [[INT]] 0, i32 1, i32 0
// CHECK-NEXT: %1 = load i32, i32* %._value
// CHECK-NEXT: ret i32 %1
public func getSubclassX(_ o: Subclass) -> Int32 {
  return o.x
}

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc i32 @"$s24class_field_other_module12getSubclassYys5Int32V0c1_A00F0CF"(%T11other_class8SubclassC* nocapture readonly %0)
// CHECK-NEXT: entry:
// CHECK-NEXT: %._value = getelementptr inbounds %T11other_class8SubclassC, %T11other_class8SubclassC* %0, [[INT]] 0, i32 2, i32 0
// CHECK-NEXT: %1 = load i32, i32* %._value
// CHECK-NEXT: ret i32 %1
public func getSubclassY(_ o: Subclass) -> Int32 {
  return o.y
}
