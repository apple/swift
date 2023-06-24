// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -disable-availability-checking %s -emit-ir | %FileCheck %s

// REQUIRES: OS=macosx
// REQUIRES: concurrency
// REQUIRES: objc_interop


import Foundation

// CHECK: [[ENCODE_ASYNC_STRING:@.*]] = private unnamed_addr constant [28 x i8] c"v24@0:8@?<v@?@\22NSString\22>16\00"
// CHECK: [[ENCODE_ASYNC_THROWS_STRING:@.*]] = private unnamed_addr constant [38 x i8] c"v24@0:8@?<v@?@\22NSString\22@\22NSError\22>16\00"

// CHECK: @_INSTANCE_METHODS__TtC19objc_async_metadata7MyClass = internal constant
// CHECK-SAME: methodWithCompletionHandler:
// CHECK-SAME: [[ENCODE_ASYNC_STRING]]
// CHECK-SAME: throwingMethodWithCompletionHandler:
// CHECK-SAME: [[ENCODE_ASYNC_THROWS_STRING]]
class MyClass: NSObject {
  @objc func method() async -> String { "" }
  @objc func throwingMethod() async throws -> String { "" }
}

// CHECK: [[ENCODE_ASYNC_STRING_PROTO:@.*]] = private unnamed_addr constant [15 x i8] c"v32@0:8@16@?24\00"
// CHECK-LABEL: @_PROTOCOL_INSTANCE_METHODS__TtP19objc_async_metadata7MyProto_ = weak hidden constant
// CHECK-SAME: _selector_data(myProtoRequirement:completionHandler:)
// CHECK-SAME: ptr [[ENCODE_ASYNC_STRING_PROTO]]

// CHECK: [[ENCODE_ASYNC_STRING_PROTO_TYPE:@.*]] = private unnamed_addr constant [41 x i8] c"v32@0:8@\22NSString\2216@?<v@?@\22NSString\22>24\00"
// CHECK-LABEL: @_PROTOCOL_METHOD_TYPES__TtP19objc_async_metadata7MyProto_ = weak hidden constant
// CHECK-SAME: ptr [[ENCODE_ASYNC_STRING_PROTO_TYPE]]

@objc protocol MyProto {
  @objc func myProtoRequirement(_ x: String) async -> String
}
