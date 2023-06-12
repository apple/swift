// RUN: %target-swift-ide-test -print-module -module-to-print=CenumsNSOptions -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s
// REQUIRES: objc_interop

import CenumsNSOptions

// CHECK: typealias NSBinarySearchingOptions = UInt
// CHECK-NEXT: struct NSBinarySearchingOptions : OptionSet, @unchecked Sendable {
// CHECK-NEXT:   init(rawValue: UInt)
// CHECK-NEXT:   let rawValue: UInt
// CHECK-NEXT:   typealias RawValue = UInt
// CHECK-NEXT:   typealias Element = NSBinarySearchingOptions
// CHECK-NEXT:   typealias ArrayLiteralElement = NSBinarySearchingOptions
// CHECK-NEXT:   static var firstEqual: NSBinarySearchingOptions { get }
// CHECK-NEXT:   @available(swift, obsoleted: 3, renamed: "firstEqual")
// CHECK-NEXT:   static var FirstEqual: NSBinarySearchingOptions { get }
// CHECK-NEXT:   static var lastEqual: NSBinarySearchingOptions { get }
// CHECK-NEXT:   @available(swift, obsoleted: 3, renamed: "lastEqual")
// CHECK-NEXT:   static var LastEqual: NSBinarySearchingOptions { get }
// CHECK-NEXT:   static var insertionIndex: NSBinarySearchingOptions { get }
// CHECK-NEXT:   @available(swift, obsoleted: 3, renamed: "insertionIndex")
// CHECK-NEXT:   static var InsertionIndex: NSBinarySearchingOptions { get }
// CHECK-NEXT: }

// CHECK: struct Bar : OptionSet, @unchecked Sendable
// CHECK: struct HasNSOptionField {
// CHECK:   var bar: Bar
// CHECK: }
