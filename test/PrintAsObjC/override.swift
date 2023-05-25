// RUN: %empty-directory(%t)

// FIXME: BEGIN -enable-source-import hackaround
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/ObjectiveC.swift -disable-objc-attr-requires-foundation-module
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/CoreGraphics.swift
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/Foundation.swift
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/AppKit.swift
// FIXME: END -enable-source-import hackaround

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) -emit-module -I %S/Inputs/custom-modules -o %t %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) -parse-as-library %t/override.swiftmodule -typecheck -emit-objc-header-path %t/override.h
// RUN: %FileCheck %s < %t/override.h
// RUN: %check-in-clang %t/override.h -I %S/Inputs/custom-modules -Wno-super-class-method-mismatch -Wno-overriding-method-mismatch
// RUN: not %check-in-clang %t/override.h -Wno-super-class-method-mismatch -I %S/Inputs/custom-modules 2>&1 | %FileCheck -check-prefix=CLANG %s

// REQUIRES: objc_interop

import OverrideBase
// No errors from Clang until we get to the FixMe class.
// CLANG-NOT: error

// CHECK-LABEL: @interface A_Child : Base
class A_Child : Base {
  // CHECK-NEXT: @property (nonatomic, readonly, getter=getProp) NSUInteger prop;
  override var prop: Int { return 0 }
  // CHECK-NEXT: @property (nonatomic, readonly) NSInteger originalName;
  override var renamedProp: Int { return 0 }
  // CHECK-NEXT: - (id _Nullable)objectAtIndexedSubscript:(NSUInteger)x SWIFT_WARN_UNUSED_RESULT;
  override subscript(x: Int) -> Any? { return nil }

  // CHECK-NEXT: - (NSUInteger)foo SWIFT_WARN_UNUSED_RESULT;
  override func foo() -> Int { return 0 }
  // CHECK-NEXT: - (NSUInteger)foo:(NSUInteger)_ SWIFT_WARN_UNUSED_RESULT;
  override func foo(_: Int) -> Int { return 0 }
  // CHECK-NEXT: - (NSUInteger)foo:(NSUInteger)x y:(NSUInteger)y SWIFT_WARN_UNUSED_RESULT;
  override func foo(_ x: Int, y: Int) -> Int { return x + y }
  
  
  // CHECK-NEXT: - (BOOL)doThingAndReturnError:(NSError * _Nullable * _Nullable)error;
  override func doThing() throws {}

  // CHECK-NEXT: - (BOOL)doAnotherThingWithError:(NSError * _Nullable * _Nullable)error;
  override func doAnotherThing() throws {}

  // CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
} // CHECK-NEXT: @end

// CHECK-LABEL: @interface A_Grandchild : A_Child
class A_Grandchild : A_Child {
  // CHECK-NEXT: @property (nonatomic, readonly, getter=getProp) NSUInteger prop;
  override var prop: Int { return 0 }
  // CHECK-NEXT: - (id _Nullable)objectAtIndexedSubscript:(NSUInteger)x SWIFT_WARN_UNUSED_RESULT;
  override subscript(x: Int) -> Any? { return nil }

  // CHECK-NEXT: - (NSUInteger)foo SWIFT_WARN_UNUSED_RESULT;
  override func foo() -> Int { return 0 }
  // CHECK-NEXT: - (NSUInteger)foo:(NSUInteger)_ SWIFT_WARN_UNUSED_RESULT;
  override func foo(_: Int) -> Int { return 0 }
  // CHECK-NEXT: - (NSUInteger)foo:(NSUInteger)x y:(NSUInteger)y SWIFT_WARN_UNUSED_RESULT;
  override func foo(_ x: Int, y: Int) -> Int { return x + y }

  // CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
} // CHECK-NEXT: @end


// CHECK-LABEL: @interface B_EmptyChild : Base
class B_EmptyChild : Base {
} // CHECK: @end

// CHECK-LABEL: @interface B_GrandchildViaEmpty : B_EmptyChild
class B_GrandchildViaEmpty : B_EmptyChild {
  // CHECK-NEXT: @property (nonatomic, getter=getProp) NSUInteger prop;
  override var prop: Int { 
    get { return 0 }
    set {}
  }

  // CHECK-NEXT: - (id _Nullable)objectAtIndexedSubscript:(NSUInteger)x SWIFT_WARN_UNUSED_RESULT;
  // CHECK-NEXT: - (void)setObject:(id _Nullable)newValue atIndexedSubscript:(NSUInteger)x;
  override subscript(x: Int) -> Any? {
    get { return nil }
    set {}
  }

  // CHECK-NEXT: - (NSUInteger)foo SWIFT_WARN_UNUSED_RESULT;
  override func foo() -> Int { return 0 }
  // CHECK-NEXT: - (NSUInteger)foo:(NSUInteger)_ SWIFT_WARN_UNUSED_RESULT;
  override func foo(_: Int) -> Int { return 0 }
  // CHECK-NEXT: - (NSUInteger)foo:(NSUInteger)x y:(NSUInteger)y SWIFT_WARN_UNUSED_RESULT;
  override func foo(_ x: Int, y: Int) -> Int { return x + y }

  // CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
} // CHECK-NEXT: @end

// The output in this class doesn't yet preserve NSUInteger correctly.
// CHECK-LABEL: @interface FixMe : Base
class FixMe : Base {
  // CHECK-NEXT: - (void)callback:(NSInteger (^ _Nullable)(void))fn;
  // CLANG: error: conflicting parameter types in declaration of 'callback:'
  override func callback(_ fn: (() -> Int)?) {}

  // CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
} // CHECK-NEXT: @end

// No more errors from Clang.
// CLANG-NOT: error:
