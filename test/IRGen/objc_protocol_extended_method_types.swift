// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %s -emit-ir -use-jit | %FileCheck -check-prefix=CHECK-JIT %s

// REQUIRES: OS=macosx
// REQUIRES: objc_interop

import Foundation

// CHECK-JIT-DAG: [[PROTOCOL_NAME:@.+]] = private unnamed_addr constant [45 x i8] c"_TtP35objc_protocol_extended_method_types1P_\00"


// CHECK-DAG: [[INIT:@.*]] = private unnamed_addr constant [8 x i8] c"@16@0:8\00"
// CHECK-DAG: [[NSNUMBER:@.*]] = private unnamed_addr constant [31 x i8] c"@\22NSNumber\2224@0:8@\22NSNumber\2216\00"
// CHECK-DAG: [[NSMUTABLEARRAY_GET:@.*]] = private unnamed_addr constant [24 x i8] c"@\22NSMutableArray\2216@0:8\00"
// CHECK-DAG: [[NSMUTABLEARRAY_SET:@.*]] = private unnamed_addr constant [27 x i8] c"v24@0:8@\22NSMutableArray\2216\00"
// CHECK-DAG: [[SUBSCRIPT:@.*]] = private unnamed_addr constant [11 x i8] c"q24@0:8q16\00"
// CHECK-DAG: [[NSSTRING:@.*]] = private unnamed_addr constant [31 x i8] c"@\22NSString\2224@0:8@\22NSString\2216\00"
// CHECK-DAG: [[NSOBJECT:@.*]] = private unnamed_addr constant [31 x i8] c"@\22NSObject\2224@0:8@\22NSObject\2216\00"
// CHECK-DAG: [[NSMUTABLESTRING:@.*]] = private unnamed_addr constant [28 x i8] c"v24@0:8@\22NSMutableString\2216\00"

// The JIT registration logic doesn't use extended types.
// CHECK-JIT-DAG: [[TY_ID_ID:@.*]] = private unnamed_addr constant [11 x i8] c"@24@0:8@16\00"
// CHECK-JIT-DAG: [[TY_VOID_ID:@.*]] = private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"
// CHECK-JIT-DAG: [[TY_ID:@.*]] = private unnamed_addr constant [8 x i8] c"@16@0:8\00"
// CHECK-JIT-DAG: [[TY_INT_INT:@.*]] = private unnamed_addr constant [11 x i8] c"q24@0:8q16\00"

// CHECK-LABEL: @_PROTOCOL_METHOD_TYPES__TtP35objc_protocol_extended_method_types1P_ = weak hidden constant [16 x ptr] [
// -- required instance methods:
//   -- requiredInstanceMethod
// CHECK:         ptr [[NSNUMBER]],
//   -- requiredInstanceProperty getter
// CHECK:         ptr [[NSMUTABLEARRAY_GET]],
//   -- requiredInstanceProperty setter
// CHECK:         ptr [[NSMUTABLEARRAY_SET]],
//   -- requiredROInstanceProperty getter
// CHECK:         ptr [[NSMUTABLEARRAY_GET]],
//   -- requiredInstanceMethod2
// CHECK:         ptr [[NSNUMBER]],
//   -- requiredInstanceProperty2 getter
// CHECK:         ptr [[NSMUTABLEARRAY_GET]],
//   -- requiredInstanceProperty2 setter
// CHECK:         ptr [[NSMUTABLEARRAY_SET]],
//   -- requiredROInstanceProperty2 getter
// CHECK:         ptr [[NSMUTABLEARRAY_GET]],
//   -- subscript getter
// CHECK:         ptr [[SUBSCRIPT]],
//   -- init
// CHECK:         ptr [[INIT]],
// -- required class methods:
//   -- requiredClassMethod
// CHECK:         ptr [[NSSTRING]],
//   -- requiredClassMethod2
// CHECK:         ptr [[NSSTRING]],
// -- optional instance methods:
//   -- optionalInstanceMethod
// CHECK:         ptr [[NSOBJECT]],
//   -- optionalInstanceMethod2
// CHECK:         ptr [[NSOBJECT]],
// -- optional class methods:
//   -- optionalClassMethod
// CHECK:         ptr [[NSMUTABLESTRING]],
//   -- optionalClassMethod2
// CHECK:         ptr [[NSMUTABLESTRING]]
// CHECK:       ]

@objc protocol P {
  func requiredInstanceMethod(_ o: NSNumber) -> NSNumber
  @objc optional func optionalInstanceMethod(_ o: NSObject) -> NSObject
  static func requiredClassMethod(_ o: NSString) -> NSString
  @objc optional static func optionalClassMethod(_ o: NSMutableString)
  var requiredInstanceProperty: NSMutableArray { get set }
  var requiredROInstanceProperty: NSMutableArray { get }

  func requiredInstanceMethod2(_ o: NSNumber) -> NSNumber
  @objc optional func optionalInstanceMethod2(_ o: NSObject) -> NSObject
  static func requiredClassMethod2(_ o: NSString) -> NSString
  @objc optional static func optionalClassMethod2(_ o: NSMutableString)
  var requiredInstanceProperty2: NSMutableArray { get set }
  var requiredROInstanceProperty2: NSMutableArray { get }

  subscript(x: Int) -> Int { get }

  init()
}

print(P.self)

// CHECK-JIT-LABEL: define private void @runtime_registration
// CHECK-JIT: [[EXISTING_PROTOCOL:%.+]] = call ptr @objc_getProtocol(ptr [[PROTOCOL_NAME]])
// CHECK-JIT: [[EXISTS:%.+]] = icmp eq ptr [[EXISTING_PROTOCOL]], null
// CHECK-JIT: br i1 [[EXISTS]], label %[[NEW_PROTOCOL_LABEL:[^ ]+]], label %[[EXISTING_PROTOCOL_LABEL:[^ ]+]]

// CHECK-JIT: [[EXISTING_PROTOCOL_LABEL]]:
// CHECK-JIT: br label %[[FINISH_LABEL:[^ ]+]]

// CHECK-JIT: [[NEW_PROTOCOL_LABEL]]:
// CHECK-JIT: [[NEW_PROTOCOL:%.+]] = call ptr @objc_allocateProtocol(ptr @{{.*}})
// -- requiredInstanceMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredInstanceMethod:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- optionalInstanceMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(optionalInstanceMethod:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID_ID]], {{(i8 0|i1 false)}}, {{(i8 1|i1 true)}})
// -- requiredClassMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredClassMethod:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID_ID]], {{(i8 1|i1 true)}}, {{(i8 0|i1 false)}})
// -- optionalClassMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(optionalClassMethod:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_VOID_ID]], {{(i8 0|i1 false)}}, {{(i8 0|i1 false)}})
// -- requiredInstanceProperty
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredInstanceProperty)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})

// Make sure we don't emit storage accessors multiple times.
// CHECK-JIT-NOT: requiredInstanceProperty

// -- setRequiredInstanceProperty:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(setRequiredInstanceProperty:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_VOID_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})

// Make sure we don't emit storage accessors multiple times.
// CHECK-JIT-NOT: requiredInstanceProperty
// CHECK-JIT-NOT: setRequiredInstanceProperty

// -- requiredROInstanceProperty
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredROInstanceProperty)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- requiredInstanceMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredInstanceMethod2:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- optionalInstanceMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(optionalInstanceMethod2:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID_ID]], {{(i8 0|i1 false)}}, {{(i8 1|i1 true)}})
// -- requiredClassMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredClassMethod2:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID_ID]], {{(i8 1|i1 true)}}, {{(i8 0|i1 false)}})
// -- optionalClassMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(optionalClassMethod2:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_VOID_ID]], {{(i8 0|i1 false)}}, {{(i8 0|i1 false)}})
// -- requiredInstanceProperty2
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredInstanceProperty2)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- setRequiredInstanceProperty2:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(setRequiredInstanceProperty2:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_VOID_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- requiredROInstanceProperty2
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(requiredROInstanceProperty2)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- objectAtIndexedSubscript:
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(objectAtIndexedSubscript:)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_INT_INT]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- init
// CHECK-JIT: [[SELECTOR:%.+]] = call ptr @sel_registerName(ptr @"\01L_selector_data(init)")
// CHECK-JIT: call void @protocol_addMethodDescription(ptr [[NEW_PROTOCOL]], ptr [[SELECTOR]], ptr [[TY_ID]], {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// CHECK-JIT: call void @objc_registerProtocol(ptr [[NEW_PROTOCOL]])
// CHECK-JIT: br label %[[FINISH_LABEL]]

// CHECK-JIT: [[FINISH_LABEL]]:
// CHECK-JIT: [[RESOLVED_PROTOCOL:%.+]] = phi ptr [ [[EXISTING_PROTOCOL]], %[[EXISTING_PROTOCOL_LABEL]] ], [ [[NEW_PROTOCOL]], %[[NEW_PROTOCOL_LABEL]] ]{{$}}
// CHECK-JIT: store ptr [[RESOLVED_PROTOCOL]], ptr @"\01l_OBJC_PROTOCOL_REFERENCE_$__TtP35objc_protocol_extended_method_types1P_", align 8 
// CHECK-JIT: ret void
// CHECK-JIT-NEXT: {{^}$}}

