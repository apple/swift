// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -disable-objc-attr-requires-foundation-module -emit-module %S/Inputs/objc_extension_base.swift -o %t
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %use_no_opaque_pointers -primary-file %s -emit-ir -g | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir -g

// REQUIRES: CPU=x86_64 || CPU=arm64
// REQUIRES: objc_interop

import Foundation
import gizmo
import objc_extension_base

// Check that metadata for nested enums added in extensions to imported classes
// gets emitted concretely.

// CHECK: [[CATEGORY_NAME:@.*]] = private constant [16 x i8] c"objc_extensions\00"
// CHECK: [[METHOD_TYPE:@.*]] = private unnamed_addr constant [8 x i8] c"v16@0:8\00"

// CHECK-LABEL: @"_CATEGORY_PROTOCOLS_Gizmo_$_objc_extensions" = internal constant
// CHECK-SAME:   i64 1,
// CHECK-SAME:   @_PROTOCOL__TtP15objc_extensions11NewProtocol_

// CHECK-LABEL: @"_CATEGORY_Gizmo_$_objc_extensions" = internal constant
// CHECK-SAME:   i8* getelementptr inbounds ([16 x i8], [16 x i8]* [[CATEGORY_NAME]], i64 0, i64 0),
// CHECK-SAME:   %objc_class* @"OBJC_CLASS_$_Gizmo",
// CHECK-SAME:   @"_CATEGORY_INSTANCE_METHODS_Gizmo_$_objc_extensions",
// CHECK-SAME:   @"_CATEGORY_CLASS_METHODS_Gizmo_$_objc_extensions",
// CHECK-SAME:   @"_CATEGORY_PROTOCOLS_Gizmo_$_objc_extensions",
// CHECK-SAME:   i8* null
// CHECK-SAME: }, section "__DATA, {{.*}}", align 8

@objc protocol NewProtocol {
  func brandNewInstanceMethod()
}

extension NSObject {
  @objc func someMethod() -> String { return "Hello" }
}

extension Gizmo: NewProtocol {
  @objc func brandNewInstanceMethod() {
  }

  @objc class func brandNewClassMethod() {
  }

  // Overrides an instance method of NSObject
  override func someMethod() -> String {
    return super.someMethod()
  }

  // Overrides a class method of NSObject
  @objc override class func hasOverride() {}
}

/*
 * Make sure that two extensions of the same ObjC class in the same module can
 * coexist by having different category names.
 */

// CHECK: [[CATEGORY_NAME_1:@.*]] = private unnamed_addr constant [17 x i8] c"objc_extensions1\00"

// CHECK: @"_CATEGORY_Gizmo_$_objc_extensions1" = internal constant
// CHECK:   i8* getelementptr inbounds ([17 x i8], [17 x i8]* [[CATEGORY_NAME_1]], i64 0, i64 0),
// CHECK:   %objc_class* @"OBJC_CLASS_$_Gizmo",
// CHECK:   {{.*}} @"_CATEGORY_INSTANCE_METHODS_Gizmo_$_objc_extensions1",
// CHECK:   {{.*}} @"_CATEGORY_CLASS_METHODS_Gizmo_$_objc_extensions1",
// CHECK:   i8* null,
// CHECK:   i8* null
// CHECK: }, section "__DATA, {{.*}}", align 8

extension Gizmo {
  @objc func brandSpankingNewInstanceMethod() {
  }

  @objc class func brandSpankingNewClassMethod() {
  }
}

/*
 * Check that extensions of Swift subclasses of ObjC objects get categories.
 */

class Hoozit : NSObject {
}

// CHECK-LABEL: @"_CATEGORY_INSTANCE_METHODS__TtC15objc_extensions6Hoozit_$_objc_extensions" = internal constant
// CHECK:   i32 24,
// CHECK:   i32 1,
// CHECK:   [1 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } {
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(blibble)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[STR:@.*]], i64 0, i64 0),
// CHECK:     i8* bitcast (void ([[OPAQUE:%.*]]*, i8*)* @"$s15objc_extensions6HoozitC7blibbleyyFTo" to i8*)
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK-LABEL: @"_CATEGORY_CLASS_METHODS__TtC15objc_extensions6Hoozit_$_objc_extensions" = internal constant
// CHECK:   i32 24,
// CHECK:   i32 1,
// CHECK:   [1 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } {
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(blobble)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[STR]], i64 0, i64 0),
// CHECK:     i8* bitcast (void (i8*, i8*)* @"$s15objc_extensions6HoozitC7blobbleyyFZTo" to i8*)
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK-LABEL: @"_CATEGORY__TtC15objc_extensions6Hoozit_$_objc_extensions" = internal constant
// CHECK:   i8* getelementptr inbounds ([16 x i8], [16 x i8]* [[CATEGORY_NAME]], i64 0, i64 0),
// CHECK:   %swift.type* {{.*}} @"$s15objc_extensions6HoozitCMf",
// CHECK:   {{.*}} @"_CATEGORY_INSTANCE_METHODS__TtC15objc_extensions6Hoozit_$_objc_extensions",
// CHECK:   {{.*}} @"_CATEGORY_CLASS_METHODS__TtC15objc_extensions6Hoozit_$_objc_extensions",
// CHECK:   i8* null,
// CHECK:   i8* null
// CHECK: }, section "__DATA, {{.*}}", align 8

extension Hoozit {
  @objc func blibble() { }
  @objc class func blobble() { }
}

class SwiftOnly { }

// CHECK-LABEL: @"_CATEGORY_INSTANCE_METHODS__TtC15objc_extensions9SwiftOnly_$_objc_extensions" = internal constant
// CHECK:   i32 24,
// CHECK:   i32 1,
// CHECK:   [1 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } {
// CHECK:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* @"\01L_selector_data(wibble)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[STR]], i64 0, i64 0),
// CHECK:     i8* bitcast (void (i8*, i8*)* @"$s15objc_extensions9SwiftOnlyC6wibbleyyFTo" to i8*)
// CHECK:   }] }, section "__DATA, {{.*}}", align 8
extension SwiftOnly {
  @objc func wibble() { }
}

class Wotsit: Hoozit {}

extension Hoozit {
  @objc func overriddenByExtensionInSubclass() {}
}

extension Wotsit {
  @objc override func overriddenByExtensionInSubclass() {}
}

extension NSObject {
  private enum SomeEnum { case X }

  @objc public func needMetadataOfSomeEnum() {
    print(NSObject.SomeEnum.X)
  }

  @objc class func hasOverride() {}
}


// CHECK-LABEL: @"_CATEGORY__TtCC15objc_extensions5Outer5Inner_$_objc_extensions" = internal constant
// CHECK-SAME:   i8* getelementptr inbounds ([16 x i8], [16 x i8]* [[CATEGORY_NAME]], i64 0, i64 0),
// CHECK-SAME:   @"_CATEGORY_INSTANCE_METHODS__TtCC15objc_extensions5Outer5Inner_$_objc_extensions",
// CHECK-SAME:   i8* null
// CHECK-SAME: }, section "__DATA, {{.*}}", align 8

class Outer : NSObject {
  class Inner : NSObject {}
}

extension Outer.Inner {
  @objc func innerExtensionMethod() {}
}


/*
 * Make sure that @NSManaged causes a category to be generated.
 */
class NSDogcow : NSObject {}

// CHECK: [[NAME:@.*]] = private unnamed_addr constant [5 x i8] c"woof\00"
// CHECK: [[ATTR:@.*]] = private unnamed_addr constant [7 x i8] c"Tq,N,D\00"
// CHECK: @"_CATEGORY_PROPERTIES__TtC15objc_extensions8NSDogcow_$_objc_extensions" = internal constant {{.*}} [[NAME]], {{.*}} [[ATTR]], {{.*}}, section "__DATA, {{.*}}", align 8
extension NSDogcow {
  @NSManaged var woof: Int
}

// CHECK: @"$sSo8NSObjectC15objc_extensionsE8SomeEnum33_1F05E59585E0BB585FCA206FBFF1A92DLLOSQACMc" =

class SwiftSubGizmo : SwiftBaseGizmo {

  // Don't crash on this call. Emit an objC method call to super.
  //
  // CHECK-LABEL: define {{.*}} @"$s15objc_extensions13SwiftSubGizmoC4frobyyF"
  // CHECK: $s15objc_extensions13SwiftSubGizmoCMa
  // CHECK: objc_msgSendSuper2
  // CHECK: ret
  public override func frob() {
    super.frob()
  }
}

@inline(never) func opaquePrint(_ value: Any) { print(value) }

/*
 * Check that we can extend ObjC generics and use both the type and metatype of
 * the generic parameter. Specifically, we're checking that we emit debug info
 * if we look up the existential bound, and that we derive the argument to
 * `opaquePrint(_:)` from the actual parameter, not just the fixed metadata.
 */
extension FungingArray {
  // CHECK-LABEL: define {{.*}} @"$sSo12FungingArrayC15objc_extensionsEyAByxGxcfC"
  // CHECK-SAME: (%objc_object* %0, %swift.type* swiftself %1)
  // CHECK: @__swift_instantiateConcreteTypeFromMangledName{{.*}}@"$sSo9NSFunging_pMD"{{.*}}!dbg

  // CHECK-LABEL: define {{.*}} @"$sSo12FungingArrayC15objc_extensionsEyAByxGxcfc"
  // CHECK-SAME: (%objc_object* %0, %TSo12FungingArrayC* swiftself %1)
  // CHECK: [[ALLOCA:%[^, =]+]] = alloca %Any, align 8
  // CHECK: @__swift_instantiateConcreteTypeFromMangledName{{.*}}@"$sSo9NSFunging_pMD"{{.*}}!dbg
  // CHECK: {{%[^, =]+}} = getelementptr inbounds %Any, %Any* [[ALLOCA]], i32 0, i32 0
  // CHECK: [[ANYBUF:%[^, =]+]] = getelementptr inbounds %Any, %Any* [[ALLOCA]], i32 0, i32 0
  // CHECK: [[BUFPTR:%[^, =]+]] = {{.*}} [[ANYBUF]]
  // CHECK: [[BUF_0:%[^, =]+]] = {{.*}} [[BUFPTR]]
  // CHECK: store {{.*}} %0, {{.*}} [[BUF_0]]
  // CHECK: call swiftcc void @"$s15objc_extensions11opaquePrintyyypF"(%Any* {{.*}} [[ALLOCA]])
  @objc public convenience init(_ elem: Element) {
    opaquePrint(elem)
    self.init()
  }

  // CHECK-LABEL: define {{.*}} @"$sSo12FungingArrayC15objc_extensionsE7pinningAByxGxm_tcfC"
  // CHECK-SAME: (%swift.type* %0, %swift.type* swiftself %1)
  // CHECK: @__swift_instantiateConcreteTypeFromMangledName{{.*}}@"$sSo9NSFunging_pMD"{{.*}}!dbg

  // CHECK-LABEL: define {{.*}} @"$sSo12FungingArrayC15objc_extensionsE7pinningAByxGxm_tcfc"
  // CHECK-SAME: (%swift.type* %0, %TSo12FungingArrayC* swiftself %1)
  // CHECK: [[ALLOCA:%[^, =]+]] = alloca %Any, align 8
  // CHECK: @__swift_instantiateConcreteTypeFromMangledName{{.*}}@"$sSo9NSFunging_pMD"{{.*}}!dbg
  // CHECK: [[OBJC_CLASS:%[^, =]+]] = call %objc_class* @swift_getObjCClassFromMetadata(%swift.type* %0)
  // CHECK: [[OBJC_CLASS_OBJ:%[^, =]+]] = bitcast %objc_class* [[OBJC_CLASS]]
  // CHECK: {{%[^, =]+}} = getelementptr inbounds %Any, %Any* [[ALLOCA]], i32 0, i32 0
  // CHECK: [[ANYBUF:%[^, =]+]] = getelementptr inbounds %Any, %Any* [[ALLOCA]], i32 0, i32 0
  // CHECK: [[BUFPTR:%[^, =]+]] = {{.*}} [[ANYBUF]]
  // CHECK: [[BUF_0:%[^, =]+]] = {{.*}} [[BUFPTR]]
  // CHECK: store {{.*}} [[OBJC_CLASS_OBJ]], {{.*}} [[BUF_0]]
  // CHECK: call swiftcc void @"$s15objc_extensions11opaquePrintyyypF"(%Any* {{.*}} [[ALLOCA]])
  @objc public convenience init(pinning: Element.Type) {
    opaquePrint(pinning as AnyObject)
    self.init()
  }
}
