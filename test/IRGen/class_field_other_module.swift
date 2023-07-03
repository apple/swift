// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -emit-module-path=%t/other_class.swiftmodule %S/Inputs/other_class.swift
// RUN: %target-swift-frontend -I %t -emit-ir -O -enforce-exclusivity=unchecked %s | %FileCheck %s -DINT=i%target-ptrsize

import other_class

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc i32 @"$s24class_field_other_module12getSubclassXys5Int32V0c1_A00F0CF"(ptr nocapture readonly %0)
// CHECK-NEXT: entry:
// CHECK-NEXT: [[GEP:%.*]] = getelementptr inbounds %T11other_class5OtherC, ptr %0, [[INT]] 0, i32 1
// CHECK-NEXT: [[RESULT:%.*]] = load i32, ptr [[GEP]]
// CHECK-NEXT: ret i32 [[RESULT]]
public func getSubclassX(_ o: Subclass) -> Int32 {
  return o.x
}

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc i32 @"$s24class_field_other_module12getSubclassYys5Int32V0c1_A00F0CF"(ptr nocapture readonly %0)
// CHECK-NEXT: entry:
// CHECK-NEXT: [[GEP:%.*]] = getelementptr inbounds %T11other_class8SubclassC, ptr %0, [[INT]] 0, i32 2
// CHECK-NEXT: [[RESULT:%.*]] = load i32, ptr [[GEP]]
// CHECK-NEXT: ret i32 [[RESULT]]
public func getSubclassY(_ o: Subclass) -> Int32 {
  return o.y
}
