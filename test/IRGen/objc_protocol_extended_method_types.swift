// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %use_no_opaque_pointers %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %use_no_opaque_pointers %s -emit-ir -use-jit | %FileCheck -check-prefix=CHECK-JIT %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %s -emit-ir
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %s -emit-ir -use-jit

// REQUIRES: OS=macosx
// REQUIRES: objc_interop
// TODO: fix test for unoptimized stdlib with asserts
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib


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

// CHECK-LABEL: @_PROTOCOL_METHOD_TYPES__TtP35objc_protocol_extended_method_types1P_ = weak hidden constant [16 x i8*] [
// -- required instance methods:
//   -- requiredInstanceMethod
// CHECK:         i8* getelementptr inbounds ([31 x i8], [31 x i8]* [[NSNUMBER]], i64 0, i64 0),
//   -- requiredInstanceProperty getter
// CHECK:         i8* getelementptr inbounds ([24 x i8], [24 x i8]* [[NSMUTABLEARRAY_GET]], i64 0, i64 0),
//   -- requiredInstanceProperty setter
// CHECK:         i8* getelementptr inbounds ([27 x i8], [27 x i8]* [[NSMUTABLEARRAY_SET]], i64 0, i64 0),
//   -- requiredROInstanceProperty getter
// CHECK:         i8* getelementptr inbounds ([24 x i8], [24 x i8]* [[NSMUTABLEARRAY_GET]], i64 0, i64 0),
//   -- requiredInstanceMethod2
// CHECK:         i8* getelementptr inbounds ([31 x i8], [31 x i8]* [[NSNUMBER]], i64 0, i64 0),
//   -- requiredInstanceProperty2 getter
// CHECK:         i8* getelementptr inbounds ([24 x i8], [24 x i8]* [[NSMUTABLEARRAY_GET]], i64 0, i64 0),
//   -- requiredInstanceProperty2 setter
// CHECK:         i8* getelementptr inbounds ([27 x i8], [27 x i8]* [[NSMUTABLEARRAY_SET]], i64 0, i64 0),
//   -- requiredROInstanceProperty2 getter
// CHECK:         i8* getelementptr inbounds ([24 x i8], [24 x i8]* [[NSMUTABLEARRAY_GET]], i64 0, i64 0),
//   -- subscript getter
// CHECK:         i8* getelementptr inbounds ([11 x i8], [11 x i8]* [[SUBSCRIPT]], i64 0, i64 0),
//   -- init
// CHECK:         i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[INIT]], i64 0, i64 0),
// -- required class methods:
//   -- requiredClassMethod
// CHECK:         i8* getelementptr inbounds ([31 x i8], [31 x i8]* [[NSSTRING]], i64 0, i64 0),
//   -- requiredClassMethod2
// CHECK:         i8* getelementptr inbounds ([31 x i8], [31 x i8]* [[NSSTRING]], i64 0, i64 0),
// -- optional instance methods:
//   -- optionalInstanceMethod
// CHECK:         i8* getelementptr inbounds ([31 x i8], [31 x i8]* [[NSOBJECT]], i64 0, i64 0),
//   -- optionalInstanceMethod2
// CHECK:         i8* getelementptr inbounds ([31 x i8], [31 x i8]* [[NSOBJECT]], i64 0, i64 0),
// -- optional class methods:
//   -- optionalClassMethod
// CHECK:         i8* getelementptr inbounds ([28 x i8], [28 x i8]* [[NSMUTABLESTRING]], i64 0, i64 0),
//   -- optionalClassMethod2
// CHECK:         i8* getelementptr inbounds ([28 x i8], [28 x i8]* [[NSMUTABLESTRING]], i64 0, i64 0)
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
// CHECK-JIT: [[EXISTING_PROTOCOL:%.+]] = call %swift.protocol* @objc_getProtocol(i8* getelementptr inbounds ([45 x i8], [45 x i8]* [[PROTOCOL_NAME]], i64 0, i64 0))
// CHECK-JIT: [[EXISTS:%.+]] = icmp eq %swift.protocol* [[EXISTING_PROTOCOL]], null
// CHECK-JIT: br i1 [[EXISTS]], label %[[NEW_PROTOCOL_LABEL:[^ ]+]], label %[[EXISTING_PROTOCOL_LABEL:[^ ]+]]

// CHECK-JIT: [[EXISTING_PROTOCOL_LABEL]]:
// CHECK-JIT: br label %[[FINISH_LABEL:[^ ]+]]

// CHECK-JIT: [[NEW_PROTOCOL_LABEL]]:
// CHECK-JIT: [[NEW_PROTOCOL:%.+]] = call %swift.protocol* @objc_allocateProtocol(i8* getelementptr inbounds ([45 x i8], [45 x i8]* @{{.*}}, i64 0, i64 0))
// -- requiredInstanceMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredInstanceMethod:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- optionalInstanceMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(optionalInstanceMethod:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID_ID]], i64 0, i64 0), {{(i8 0|i1 false)}}, {{(i8 1|i1 true)}})
// -- requiredClassMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredClassMethod:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 0|i1 false)}})
// -- optionalClassMethod:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(optionalClassMethod:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_VOID_ID]], i64 0, i64 0), {{(i8 0|i1 false)}}, {{(i8 0|i1 false)}})
// -- requiredInstanceProperty
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredInstanceProperty)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})

// Make sure we don't emit storage accessors multiple times.
// CHECK-JIT-NOT: requiredInstanceProperty

// -- setRequiredInstanceProperty:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(setRequiredInstanceProperty:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_VOID_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})

// Make sure we don't emit storage accessors multiple times.
// CHECK-JIT-NOT: requiredInstanceProperty
// CHECK-JIT-NOT: setRequiredInstanceProperty

// -- requiredROInstanceProperty
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredROInstanceProperty)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- requiredInstanceMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredInstanceMethod2:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- optionalInstanceMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(optionalInstanceMethod2:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID_ID]], i64 0, i64 0), {{(i8 0|i1 false)}}, {{(i8 1|i1 true)}})
// -- requiredClassMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredClassMethod2:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 0|i1 false)}})
// -- optionalClassMethod2:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(optionalClassMethod2:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_VOID_ID]], i64 0, i64 0), {{(i8 0|i1 false)}}, {{(i8 0|i1 false)}})
// -- requiredInstanceProperty2
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredInstanceProperty2)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- setRequiredInstanceProperty2:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(setRequiredInstanceProperty2:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_VOID_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- requiredROInstanceProperty2
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(requiredROInstanceProperty2)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- objectAtIndexedSubscript:
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(objectAtIndexedSubscript:)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_INT_INT]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// -- init
// CHECK-JIT: [[SELECTOR:%.+]] = call i8* @sel_registerName(i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @"\01L_selector_data(init)", i64 0, i64 0))
// CHECK-JIT: call void @protocol_addMethodDescription(%swift.protocol* [[NEW_PROTOCOL]], i8* [[SELECTOR]], i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[TY_ID]], i64 0, i64 0), {{(i8 1|i1 true)}}, {{(i8 1|i1 true)}})
// CHECK-JIT: call void @objc_registerProtocol(%swift.protocol* [[NEW_PROTOCOL]])
// CHECK-JIT: br label %[[FINISH_LABEL]]

// CHECK-JIT: [[FINISH_LABEL]]:
// CHECK-JIT: [[RESOLVED_PROTOCOL:%.+]] = phi %swift.protocol* [ [[EXISTING_PROTOCOL]], %[[EXISTING_PROTOCOL_LABEL]] ], [ [[NEW_PROTOCOL]], %[[NEW_PROTOCOL_LABEL]] ]{{$}}
// CHECK-JIT: store %swift.protocol* [[RESOLVED_PROTOCOL]], %swift.protocol** bitcast (i8** @"\01l_OBJC_PROTOCOL_REFERENCE_$__TtP35objc_protocol_extended_method_types1P_" to %swift.protocol**), align 8 
// CHECK-JIT: ret void
// CHECK-JIT-NEXT: {{^}$}}

