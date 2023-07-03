// RUN: %target-swift-frontend -target %target-swift-abi-5.8-triple -emit-ir %s -module-name M -import-objc-header %S/Inputs/issue-49393.h | %FileCheck %s --check-prefix=CHECK-%is-darwin --check-prefix=CHECK
// REQUIRES: objc_interop

// https://github.com/apple/swift/issues/49393

import Foundation

// CHECK-isDarwin: @OBJC_METH_VAR_NAME_ = private unnamed_addr constant [26 x i8] c"JSONKeyPathsByPropertyKey\00"
// CHECK-isDarwin: @"_OBJC_$_PROTOCOL_INSTANCE_METHODS_MyJSONSerializing" = {{.*}} @OBJC_METH_VAR_NAME_
// CHECK-isDarwin: @OBJC_PROP_NAME_ATTR_ = private unnamed_addr constant [26 x i8] c"JSONKeyPathsByPropertyKey\00"
// CHECK-isDarwin: @OBJC_PROP_NAME_ATTR_.1 = private unnamed_addr constant [21 x i8] c"T@\22NSDictionary\22,R,C\00"
// CHECK-isDarwin:  @"_OBJC_$_PROP_LIST_MyJSONSerializing" {{.*}} [1 x %struct._prop_t] }
// CHECK-isDarwin-SAME: @OBJC_PROP_NAME_ATTR_
// CHECK-isDarwin-SAME: @OBJC_PROP_NAME_ATTR_.1

// CHECK-isNotDarwin-LABEL: @_PROTOCOL_INSTANCE_METHODS_MyJSONSerializing = {{.+}} @"\01L_selector_data(JSONKeyPathsByPropertyKey)"
// CHECK-isNotDarwin: [[OBJC_PROPNAME:@.+]] = private unnamed_addr constant [{{.+}} x i8] c"JSONKeyPathsByPropertyKey\00"
// CHECK-isNotDarwin: [[PROPKIND:@.+]] = private unnamed_addr constant [{{.+}} x i8] c"T@\22NSDictionary\22,N,R\00"
// CHECK-isNotDarwin: @_PROTOCOL_PROPERTIES_MyJSONSerializing =
// CHECK-isNotDarwin-SAME: [[OBJC_PROPNAME]]
// CHECK-isNotDarwin-SAME: [[PROPKIND]]

// CHECK-LABEL: @_PROTOCOL_INSTANCE_METHODS__TtP1M18MyJSONSerializing2_ = {{.+}} @"\01L_selector_data(JSONKeyPathsByPropertyKey2)"
// CHECK: [[SWIFT_PROPNAME:@.+]] = private unnamed_addr constant [{{.+}} x i8] c"JSONKeyPathsByPropertyKey2\00"

// CHECK-isDarwin: [[PROPKIND:@.+]] = private unnamed_addr constant [{{.+}} x i8] c"T@\22NSDictionary\22,N,R\00"
// CHECK: @_PROTOCOL_PROPERTIES__TtP1M18MyJSONSerializing2_ =
// CHECK-SAME: [[SWIFT_PROPNAME]]
// CHECK-SAME: [[PROPKIND]]

@objc public protocol MyJSONSerializing2 {
  var JSONKeyPathsByPropertyKey2: [String: MyJSONKeyPath]? { get }
}

extension MyJSONAdapter {
  public func model<Model: MyJSONSerializing>(of _: Model.Type = Model.self) throws -> Model {
    return __model() as! Model
  }
  public func model<Model: MyJSONSerializing2>(of _: Model.Type = Model.self) throws -> Model {
    return __model() as! Model
  }
}
