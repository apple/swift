// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-%target-os-abi %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

// rdar://76863553
// UNSUPPORTED: OS=watchos && CPU=x86_64

import Foundation

// Protocol methods require extended method type encodings to capture block
// signatures and parameter object types.
@objc protocol Fooable {
  func block(_: (Int) -> Int)
  func block2(_: (Int,Int) -> Int)

  func takesString(_: String) -> String
  func takesArray(_: [AnyObject]) -> [AnyObject]
  func takesDict(_: [NSObject: AnyObject]) -> [NSObject: AnyObject]
  func takesSet(_: Set<NSObject>) -> Set<NSObject>
}

class Foo: Fooable {
  func bar() {}
  @objc func baz() {}
  @IBAction func garply(_: AnyObject?) {}
  @IBSegueAction func harply(_: AnyObject?, _: AnyObject) -> AnyObject? {fatalError()}
  @objc func block(_: (Int) -> Int) {}
  @objc func block2(_: (Int,Int) -> Int) {}

  @objc func takesString(_ x: String) -> String { return x }
  @objc func takesArray(_ x: [AnyObject]) -> [AnyObject] { return x }
  @objc func takesDict(_ x: [NSObject: AnyObject]) -> [NSObject: AnyObject] { return x }
  @objc func takesSet(_ x: Set<NSObject>) -> Set<NSObject> { return x }

  @objc func fail() throws {}
}

class ObjcDestructible: NSObject {
  var object: NSObject
  
  init(object: NSObject) {
    self.object = object
  }
}

// CHECK: [[NO_ARGS_SIGNATURE:@.*]] = private unnamed_addr constant [8 x i8] c"v16@0:8\00"
// CHECK: [[GARPLY_SIGNATURE:@.*]] = private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"
// CHECK: [[HARPLY_SIGNATURE:@.*]] = private unnamed_addr constant [14 x i8] c"@32@0:8@16@24\00"
// CHECK: [[BLOCK_SIGNATURE_TRAD:@.*]] = private unnamed_addr constant [12 x i8] c"v24@0:8@?16\00"
// CHECK-macosx: [[FAIL_SIGNATURE:@.*]] = private unnamed_addr constant [12 x i8] c"c24@0:8^@16\00"
// CHECK-ios: [[FAIL_SIGNATURE:@.*]] = private unnamed_addr constant [12 x i8] c"B24@0:8^@16\00"
// CHECK-tvos: [[FAIL_SIGNATURE:@.*]] = private unnamed_addr constant [12 x i8] c"B24@0:8^@16\00"
// CHECK-watchos: [[FAIL_SIGNATURE:@.*]] = private unnamed_addr constant [12 x i8] c"B24@0:8^@16\00"
// CHECK: @_INSTANCE_METHODS__TtC12objc_methods3Foo = internal constant { {{.*}}] } {
// CHECK:   i32 24,
// CHECK:   i32 10,
// CHECK:   [10 x { i8*, i8*, i8* }] [{
// CHECK:     i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_selector_data(baz)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[NO_ARGS_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast (void (i8*, i8*)* @"$s12objc_methods3FooC3bazyyFTo" to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(garply:)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([11 x i8], [11 x i8]* [[GARPLY_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast (void (i8*, i8*, i8*)* @"$s12objc_methods3FooC6garplyyyyXlSgFTo" to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([9 x i8], [9 x i8]* @"\01L_selector_data(harply::)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([14 x i8], [14 x i8]* [[HARPLY_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast (i8* (i8*, i8*, i8*, i8*)* @"$s12objc_methods3FooC6harplyyyXlSgAE_yXltFTo" to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* @"\01L_selector_data(block:)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([12 x i8], [12 x i8]* [[BLOCK_SIGNATURE_TRAD]], i64 0, i64 0),
// CHECK:     i8* bitcast (void (i8*, i8*, i64 (i64)*)* @"$s12objc_methods3FooC5blockyyS2iXEFTo" to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(block2:)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([12 x i8], [12 x i8]* [[BLOCK_SIGNATURE_TRAD]], i64 0, i64 0),
// CHECK:     i8* bitcast (void (i8*, i8*, i64 (i64, i64)*)* @"$s12objc_methods3FooC6block2yyS2i_SitXEFTo" to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([20 x i8], [20 x i8]* @"\01L_selector_data(failAndReturnError:)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([12 x i8], [12 x i8]* [[FAIL_SIGNATURE]], i64 0, i64 0),
// CHECK-macosx:     i8* bitcast (i8 (i8*, i8*, %4**)* @"$s12objc_methods3FooC4failyyKFTo" to i8*)
// CHECK-ios:     i8* bitcast (i1 (i8*, i8*, %4**)* @"$s12objc_methods3FooC4failyyKFTo" to i8*)
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8
// CHECK: @_INSTANCE_METHODS__TtC12objc_methods16ObjcDestructible = internal constant { {{.*}}] } {
// CHECK:   i32 24,
// CHECK:   i32 2,
// CHECK:   [2 x { i8*, i8*, i8* }] [{
// CHECK:     i8* getelementptr inbounds ([14 x i8], [14 x i8]* @"\01L_selector_data(.cxx_destruct)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[NO_ARGS_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast (void (%6*, i8*)* @"$s12objc_methods16ObjcDestructibleCfETo" to i8*) }]
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8
// CHECK: [[BLOCK_SIGNATURE_EXT_1:@.*]] = private unnamed_addr constant [18 x i8] c"v24@0:8@?<q@?q>16\00"
// CHECK: [[BLOCK_SIGNATURE_EXT_2:@.*]] = private unnamed_addr constant [19 x i8] c"v24@0:8@?<q@?qq>16\00"
// CHECK: [[STRING_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [31 x i8] c"@\22NSString\2224@0:8@\22NSString\2216\00"
// CHECK: [[ARRAY_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [29 x i8] c"@\22NSArray\2224@0:8@\22NSArray\2216\00"
// CHECK: [[DICT_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [39 x i8] c"@\22NSDictionary\2224@0:8@\22NSDictionary\2216\00"
// CHECK: [[SET_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [25 x i8] c"@\22NSSet\2224@0:8@\22NSSet\2216\00"
// CHECK: @_PROTOCOL_METHOD_TYPES__TtP12objc_methods7Fooable_ = weak hidden constant [6 x i8*] [
// CHECK:     i8* getelementptr inbounds ([18 x i8], [18 x i8]* [[BLOCK_SIGNATURE_EXT_1]], i64 0, i64 0)
// CHECK:     i8* getelementptr inbounds ([19 x i8], [19 x i8]* [[BLOCK_SIGNATURE_EXT_2]], i64 0, i64 0)
// CHECK:     i8* getelementptr inbounds ([31 x i8], [31 x i8]* [[STRING_SIGNATURE_EXT]], i64 0, i64 0)
// CHECK:     i8* getelementptr inbounds ([29 x i8], [29 x i8]* [[ARRAY_SIGNATURE_EXT]], i64 0, i64 0)
// CHECK:     i8* getelementptr inbounds ([39 x i8], [39 x i8]* [[DICT_SIGNATURE_EXT]], i64 0, i64 0)
// CHECK:     i8* getelementptr inbounds ([25 x i8], [25 x i8]* [[SET_SIGNATURE_EXT]], i64 0, i64 0)
// CHECK:   ]


// rdar://16006333 - observing properties don't work in @objc classes
@objc
class ObservingAccessorTest : NSObject {
  var bounds: Int = 0 {
    willSet {}
    didSet {}
  }
}
