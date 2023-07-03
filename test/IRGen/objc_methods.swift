// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-%target-os-abi %s

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
// CHECK:   [10 x { ptr, ptr, ptr }] [{
// CHECK:     ptr @"\01L_selector_data(baz)",
// CHECK:     ptr [[NO_ARGS_SIGNATURE]],
// CHECK:     ptr @"$s12objc_methods3FooC3bazyyFTo"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(garply:)",
// CHECK:     ptr [[GARPLY_SIGNATURE]],
// CHECK:     ptr @"$s12objc_methods3FooC6garplyyyyXlSgFTo"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(harply::)",
// CHECK:     ptr [[HARPLY_SIGNATURE]],
// CHECK:     ptr @"$s12objc_methods3FooC6harplyyyXlSgAE_yXltFTo"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(block:)",
// CHECK:     ptr [[BLOCK_SIGNATURE_TRAD]],
// CHECK:     ptr @"$s12objc_methods3FooC5blockyyS2iXEFTo"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(block2:)",
// CHECK:     ptr [[BLOCK_SIGNATURE_TRAD]],
// CHECK:     ptr @"$s12objc_methods3FooC6block2yyS2i_SitXEFTo"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(failAndReturnError:)",
// CHECK:     ptr [[FAIL_SIGNATURE]],
// CHECK-macosx:     ptr @"$s12objc_methods3FooC4failyyKFTo"
// CHECK-ios:     ptr @"$s12objc_methods3FooC4failyyKFTo"
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8
// CHECK: @_INSTANCE_METHODS__TtC12objc_methods16ObjcDestructible = internal constant { {{.*}}] } {
// CHECK:   i32 24,
// CHECK:   i32 2,
// CHECK:   [2 x { ptr, ptr, ptr }] [{
// CHECK:     ptr @"\01L_selector_data(.cxx_destruct)",
// CHECK:     ptr [[NO_ARGS_SIGNATURE]],
// CHECK:     ptr @"$s12objc_methods16ObjcDestructibleCfETo" }]
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8
// CHECK: [[BLOCK_SIGNATURE_EXT_1:@.*]] = private unnamed_addr constant [18 x i8] c"v24@0:8@?<q@?q>16\00"
// CHECK: [[BLOCK_SIGNATURE_EXT_2:@.*]] = private unnamed_addr constant [19 x i8] c"v24@0:8@?<q@?qq>16\00"
// CHECK: [[STRING_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [31 x i8] c"@\22NSString\2224@0:8@\22NSString\2216\00"
// CHECK: [[ARRAY_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [29 x i8] c"@\22NSArray\2224@0:8@\22NSArray\2216\00"
// CHECK: [[DICT_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [39 x i8] c"@\22NSDictionary\2224@0:8@\22NSDictionary\2216\00"
// CHECK: [[SET_SIGNATURE_EXT:@.*]] = private unnamed_addr constant [25 x i8] c"@\22NSSet\2224@0:8@\22NSSet\2216\00"
// CHECK: @_PROTOCOL_METHOD_TYPES__TtP12objc_methods7Fooable_ = weak hidden constant [6 x ptr] [
// CHECK:     ptr [[BLOCK_SIGNATURE_EXT_1]]
// CHECK:     ptr [[BLOCK_SIGNATURE_EXT_2]]
// CHECK:     ptr [[STRING_SIGNATURE_EXT]]
// CHECK:     ptr [[ARRAY_SIGNATURE_EXT]]
// CHECK:     ptr [[DICT_SIGNATURE_EXT]]
// CHECK:     ptr [[SET_SIGNATURE_EXT]]
// CHECK:   ]


// rdar://16006333 - observing properties don't work in @objc classes
@objc
class ObservingAccessorTest : NSObject {
  var bounds: Int = 0 {
    willSet {}
    didSet {}
  }
}
