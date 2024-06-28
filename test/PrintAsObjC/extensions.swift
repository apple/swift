// Please keep this file in alphabetical order!

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-module -o %t %s -disable-objc-attr-requires-foundation-module -Xllvm -sil-disable-pass=MandatoryARCOpts
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library %t/extensions.swiftmodule -typecheck -emit-objc-header-path %t/extensions.h -import-objc-header %S/../Inputs/empty.h -disable-objc-attr-requires-foundation-module -Xllvm -sil-disable-pass=MandatoryARCOpts
// RUN: %FileCheck %s --input-file %t/extensions.h
// RUN: %FileCheck --check-prefix=NEGATIVE %s --input-file %t/extensions.h
// RUN: %check-in-clang %t/extensions.h

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

// Check that two otherwise tied extensions will print in alphabetical
// order by first member with a differing Swift name.

// CHECK-LABEL: @interface A6
@objc class A6 {}

extension A6 {
  @objc(skippedBool:) func skipped(_: Bool) {}
  @objc func def() {}
}
extension A6 {
  @objc(skippedInt:) func skipped(_: Int) {}
  @objc func abc() {}
}
// CHECK: @interface A6 (SWIFT_EXTENSION(extensions))
// CHECK: - (void)skippedInt:
// CHECK: - (void)abc
// CHECK: @interface A6 (SWIFT_EXTENSION(extensions))
// CHECK: - (void)skippedBool:
// CHECK: - (void)def

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

// CHECK-LABEL: @interface NSString (CustomName)
// CHECK-NEXT: - (void)test3;
// CHECK-NEXT: @end
@objc(CustomName) extension NSString {
  func test3() {}
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
