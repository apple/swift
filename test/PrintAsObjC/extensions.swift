// Please keep this file in alphabetical order!

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-sil-ownership-verifier -emit-module -o %t %s -disable-objc-attr-requires-foundation-module -Xllvm -sil-disable-pass=MandatoryARCOpts
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-sil-ownership-verifier -parse-as-library %t/extensions.swiftmodule -typecheck -emit-objc-header-path %t/extensions.h -import-objc-header %S/../Inputs/empty.h -disable-objc-attr-requires-foundation-module -Xllvm -sil-disable-pass=MandatoryARCOpts
// RUN: %FileCheck %s < %t/extensions.h
// RUN: %FileCheck --check-prefix=NEGATIVE %s < %t/extensions.h
// RUN: %check-in-clang %t/extensions.h

// This test generates invalid SIL. It will cause a compiler assert in
// both -Onone and -O builds:
// Error! Found a leaked owned value that was never consumed.
// -disable-sil-ownership-verifier is a temporary workaround.
// rdar://64375208 (Wrong lowering of PrintAsObjC code. Result is
// unowned instead of autoreleased)

// REQUIRES: objc_interop

import Foundation
import AppKit
import objc_generics

// CHECK-NOT: AppKit

// CHECK-LABEL: @interface A1{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class A1 {}

// NEGATIVE-NOT: @interface A1 (SWIFT_EXTENSION(extensions))
extension A1 {}

// CHECK-LABEL: @interface A2{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
// CHECK-LABEL: @interface A2 (SWIFT_EXTENSION(extensions))
// CHECK-DAG: @property (nonatomic, readonly) NSInteger some;
// CHECK-NEXT: @end
extension A2 {
  @objc var some: Int { return 1 }
}
@objc @objcMembers class A2 {}

// CHECK-LABEL: @interface A3{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class A3 {}

// CHECK-LABEL: @interface A3 (SWIFT_EXTENSION(extensions))
// CHECK-DAG: @interface A3 (SWIFT_EXTENSION(extensions))
// CHECK-DAG: @property (nonatomic, readonly) NSInteger more;
// CHECK-DAG: @property (nonatomic, readonly) NSInteger some;
// CHECK-DAG: @end
// CHECK: @end
extension A3 {
  @objc var some: Int { return 1 }
}
extension A3 {
  @objc var more: Int { return 10 }
}

// CHECK-LABEL: @interface A4{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class A4 {}

// CHECK-LABEL: @interface A4 (SWIFT_EXTENSION(extensions))
// CHECK-NEXT: @end
extension A4 {
  // CHECK-LABEL: @interface Inner
  // CHECK-NEXT: init
  // CHECK-NEXT: @end
  @objc @objcMembers class Inner {}
}

// CHECK-LABEL: @interface A5{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class A5 {}

// NEGATIVE-NOT: @interface A5 (SWIFT_EXTENSION(extensions))
extension A5 {
  var notObjC: NotObjC { return NotObjC() }
}

// CHECK-LABEL: @interface CustomName{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc(CustomName)
@objcMembers
class ClassWithCustomName {
}

// CHECK-LABEL: @interface CustomName (SWIFT_EXTENSION(extensions))
// CHECK-NEXT: - (void)foo;
// CHECK-NEXT: @end
extension ClassWithCustomName {
  @objc func foo() {}
}

// NEGATIVE-NOT: CGColor
extension CGColor {
  func anyOldMethod() {}
}

// CHECK-LABEL: @interface GenericClass<T> (SWIFT_EXTENSION(extensions))
// CHECK-NEXT: - (void)bar;
// CHECK-NEXT: @end
extension GenericClass {
  @objc func bar() {}
}

// NEGATIVE-NOT: NotObjC
class NotObjC {}
extension NotObjC {}

// NEGATIVE-NOT: @interface NSObject{{$}}
// NEGATIVE-NOT: @class NSObject
// CHECK-LABEL: @interface NSObject (SWIFT_EXTENSION(extensions))
// CHECK-DAG: @property (nonatomic, readonly) NSInteger some;
// CHECK-NEXT: @end
extension NSObject {
  @objc var some: Int { return 1 }
}

// NEGATIVE-NOT: @class NSString;
// CHECK: @class NSColor;
// CHECK-LABEL: @interface NSString (SWIFT_EXTENSION(extensions))
// CHECK-NEXT: - (void)test;
// CHECK-NEXT: + (void)test2;
// CHECK-NEXT: + (NSString * _Nullable)fromColor:(NSColor * _Nonnull)color SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @end
extension NSString {
  @objc func test() {}
  @objc class func test2() {}

  @objc class func fromColor(_ color: NSColor) -> NSString? { return nil; }
}

// CHECK-LABEL: @interface PettableContainer<T> (SWIFT_EXTENSION(extensions))
// CHECK-NEXT: - (PettableContainer<T> * _Nonnull)duplicate SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (PettableContainer<T> * _Nonnull)duplicate2 SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (PettableContainer<PettableOverextendedMetaphor *> * _Nonnull)duplicate3 SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (T _Nonnull)extract SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (T _Nullable)extract2 SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @end
extension PettableContainer {
  @objc func duplicate() -> PettableContainer { fatalError() }
  @objc func duplicate2() -> PettableContainer<T> { fatalError() }
  @objc func duplicate3() -> PettableContainer<PettableOverextendedMetaphor> { fatalError() }
  @objc func extract() -> T { fatalError() }
  @objc func extract2() -> T? { fatalError() }
}
