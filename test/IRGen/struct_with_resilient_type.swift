// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -I %t -emit-ir %s
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -I %t -emit-ir %s | %FileCheck %s --check-prefix=VWT-%target-os --check-prefix=VWT
// RUN: %target-swift-frontend -O -I %t -emit-ir %s

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

// VWT-macosx: define {{.*}} %swift.opaque* @"$s26struct_with_resilient_type9SomeValueVwta"(%swift.opaque* noalias returned {{.*}}, %swift.opaque* noalias {{.*}}, %swift.type* nocapture readonly [[MT:%.*]])
// VWT-macosx:   [[VAL1:%.*]] = load i64
// VWT-macosx:   store i64 [[VAL1]]
// VWT-macosx:   [[T1:%.*]] = tail call swiftcc %swift.metadata_response @"$s16resilient_struct13ResilientBoolVMa"(i64 0)
// VWT-macosx:   [[T2:%.*]] = extractvalue %swift.metadata_response [[T1]], 0
// VWT-macosx:   [[T3:%.*]] = getelementptr inbounds %swift.type, %swift.type* [[T2]], i64 -1
// VWT-macosx:   [[T4:%.*]] = bitcast %swift.type* [[T3]] to i8***
// VWT-macosx:   [[T5:%.*]] = load i8**, i8*** [[T4]]
// VWT-macosx:   [[T6:%.*]] = getelementptr inbounds i8*, i8** [[T5]], i64 5
// VWT-macosx:   [[T7:%.*]] = bitcast i8** [[T6]] to %swift.opaque* (%swift.opaque*, %swift.opaque*, %swift.type*)**
// VWT-macosx:   [[T8:%.*]] = load %swift.opaque* (%swift.opaque*, %swift.opaque*, %swift.type*)*, %swift.opaque* (%swift.opaque*, %swift.opaque*, %swift.type*)** [[T7]]
// VWT-macosx:   tail call %swift.opaque* [[T8]](
// VWT-macosx:   [[F01:%.*]] = getelementptr inbounds %swift.type, %swift.type* [[MT]], i64 3
// VWT-macosx:   [[F02:%.*]] = bitcast %swift.type* [[F01]] to i32*
// VWT-macosx:   [[F03:%.*]] = load i32, i32* [[F02]], align 8
// VWT-macosx:   [[F04:%.*]] = sext i32 [[F03]] to i64
// VWT-macosx:   [[FA1:%.*]] = getelementptr inbounds i8, i8* {{.*}}, i64 [[F04]]
// VWT-macosx:   [[FA2:%.*]] = getelementptr inbounds i8, i8* {{.*}}, i64 [[F04]]
// VWT-macosx:   [[Y_ADDR_DEST:%.*]] = bitcast i8* [[FA1]] to i64*
// VWT-macosx:   [[Y_ADDR_SRC:%.*]] = bitcast i8* [[FA2]] to i64*
// VWT-macosx:   [[VAL3:%.*]] = load i64, i64* [[Y_ADDR_SRC]]
// VWT-macosx:   store i64 [[VAL3]], i64* [[Y_ADDR_DEST]]
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
