// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -I %t -emit-ir %s
// RUN: %target-swift-frontend -O -I %t -emit-ir %s | %FileCheck %s --check-prefix=VWT-%target-os --check-prefix=VWT

// REQUIRES: CPU=x86_64

import resilient_struct

struct StructWithFunc {
  func foo(ptr: @escaping () -> Void) {
  }
}

struct ProtAndResilStruct {
  let foundationType: ResilientBool
  
  let fooImp: StructWithFunc
  
  init(fType: ResilientBool, fooImp: StructWithFunc) {
    self.foundationType = fType
    self.fooImp = fooImp
  }
  
  func bar() {
  }
  
  func crash() {
    fooImp.foo(ptr: bar)
  }
}

func crashCaller() {
  let fType = ResilientBool(b: false)
  let fooImp = StructWithFunc()
  let badStruct = ProtAndResilStruct(fType: fType, fooImp: fooImp)
  badStruct.crash()
}

crashCaller()


// VWT: define {{.*}} @"$s26struct_with_resilient_type9SomeValueVwtk"

// Don't use the type layout based value witness based generation (i.e we load field offsets below).

// VWT-macosx: define {{.*}} ptr @"$s26struct_with_resilient_type9SomeValueVwta"(ptr noalias returned {{.*}}, ptr noalias {{.*}}, ptr nocapture readonly [[MT:%.*]])
// VWT-macosx:   [[VAL1:%.*]] = load i64
// VWT-macosx:   store i64 [[VAL1]]
// VWT-macosx:   [[T1:%.*]] = tail call swiftcc %swift.metadata_response @"$s16resilient_struct13ResilientBoolVMa"(i64 0)
// VWT-macosx:   [[T2:%.*]] = extractvalue %swift.metadata_response [[T1]], 0
// VWT-macosx:   [[T3:%.*]] = getelementptr inbounds ptr, ptr [[T2]], i64 -1
// VWT-macosx:   [[T5:%.*]] = load ptr, ptr [[T3]]
// VWT-macosx:   [[T6:%.*]] = getelementptr inbounds ptr, ptr [[T5]], i64 5
// VWT-macosx:   [[T8:%.*]] = load ptr, ptr [[T6]]
// VWT-macosx:   tail call ptr [[T8]](
// VWT-macosx:   [[F01:%.*]] = getelementptr inbounds i32, ptr [[MT]], i64 6
// VWT-macosx:   [[F03:%.*]] = load i32, ptr [[F01]], align 8
// VWT-macosx:   [[F04:%.*]] = sext i32 [[F03]] to i64
// VWT-macosx:   [[FA1:%.*]] = getelementptr inbounds i8, ptr {{.*}}, i64 [[F04]]
// VWT-macosx:   [[FA2:%.*]] = getelementptr inbounds i8, ptr {{.*}}, i64 [[F04]]
// VWT-macosx:   [[VAL3:%.*]] = load i64, ptr [[FA2]]
// VWT-macosx:   store i64 [[VAL3]], ptr [[FA1]]
// VWT-macosx:   ret
// VWT-macosx: }
public struct SomeValue {
  var x = 1
  var b : ResilientBool
  var y = 2

  init(_ b: ResilientBool) {
    self.b = b
  }
}
