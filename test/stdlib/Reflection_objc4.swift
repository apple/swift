// RUN: %empty-directory(%t)
//
// RUN: %target-clang %S/Inputs/Mirror/Mirror.mm -c -o %t/Mirror.mm.o -g
// RUN: %target-build-swift -swift-version 4 -parse-stdlib %s -module-name Reflection -I %S/Inputs/Mirror/ -Xlinker %t/Mirror.mm.o -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %{python} %S/../Inputs/timeout.py 360 %target-run %t/a.out %S/Inputs/shuffle.jpg | %FileCheck %s
// FIXME: timeout wrapper is necessary because the ASan test runs for hours

// REQUIRES: executable_test
// REQUIRES: objc_interop

//
// DO NOT add more tests to this file.  Add them to test/1_stdlib/Runtime.swift.
//


import Swift
import Foundation
import simd

#if canImport(AppKit)
import AppKit

typealias OSImage = NSImage
typealias OSColor = NSColor
typealias OSBezierPath = NSBezierPath
#elseif canImport(UIKit)
import UIKit

typealias OSImage = UIImage
typealias OSColor = UIColor
typealias OSBezierPath = UIBezierPath
#else
#error("Platform does not support UIKit or AppKit!")
#endif

// CHECK-LABEL: PlaygroundQuickLook:
print("PlaygroundQuickLook:")

// -- Check that quick look Cocoa objects get binned correctly to their
//    associated enum tag.

// CHECK-NEXT: got the expected quick look text
switch PlaygroundQuickLook(reflecting: "woozle wuzzle" as NSString) {
case .text("woozle wuzzle"):
  print("got the expected quick look text")
case let x:
  print("NSString: got something else: \(x)")
}

// CHECK-NEXT: foobar
let somesubclassofnsstring = ("foo" + "bar") as NSString
switch PlaygroundQuickLook(reflecting: somesubclassofnsstring) {
  case .text(let text): print(text)
  case let x: print("not the expected quicklook: \(x)")
}

// CHECK-NEXT: got the expected quick look attributed string
let astr = NSAttributedString(string: "yizzle pizzle")
switch PlaygroundQuickLook(reflecting: astr) {
case .attributedString(let astr2 as NSAttributedString)
where astr == astr2:
  print("got the expected quick look attributed string")
case let x:
  print("NSAttributedString: got something else: \(x)")
}

// CHECK-NEXT: got the expected quick look int
switch PlaygroundQuickLook(reflecting: Int.max as NSNumber) {
case .int(+Int64(Int.max)):
  print("got the expected quick look int")
case let x:
  print("NSNumber(Int.max): got something else: \(x)")
}

// CHECK-NEXT: got the expected quick look uint
switch PlaygroundQuickLook(reflecting: NSNumber(value: UInt64.max)) {
case .uInt(UInt64.max):
  print("got the expected quick look uint")
case let x:
  print("NSNumber(Int64.max): got something else: \(x)")
}

// CHECK-NEXT: got the expected quick look double
switch PlaygroundQuickLook(reflecting: 22.5 as NSNumber) {
case .double(22.5):
  print("got the expected quick look double")
case let x:
  print("NSNumber(22.5): got something else: \(x)")
}

// CHECK-NEXT: got the expected quick look float
switch PlaygroundQuickLook(reflecting: Float32(1.25)) {
case .float(1.25):
  print("got the expected quick look float")
case let x:
  print("NSNumber(Float32(1.25)): got something else: \(x)")
}

// CHECK-NEXT: got the expected quick look image
// CHECK-NEXT: got the expected quick look color
// CHECK-NEXT: got the expected quick look bezier path

let image = OSImage(contentsOfFile:CommandLine.arguments[1])!
switch PlaygroundQuickLook(reflecting: image) {
case .image(let image2 as OSImage) where image === image2:
  print("got the expected quick look image")
case let x:
  print("OSImage: got something else: \(x)")
}

let color = OSColor.black
switch PlaygroundQuickLook(reflecting: color) {
case .color(let color2 as OSColor) where color === color2:
  print("got the expected quick look color")
case let x:
  print("OSColor: got something else: \(x)")
}

let path = OSBezierPath()
switch PlaygroundQuickLook(reflecting: path) {
case .bezierPath(let path2 as OSBezierPath) where path === path2:
  print("got the expected quick look bezier path")
case let x:
  print("OSBezierPath: got something else: \(x)")
}

// rdar://problem/18513769 -- Make sure that QuickLookObject lookup correctly
// manages memory.

@objc class CanaryBase {
  deinit {
    print("\(type(of: self)) overboard")
  }

  required init() { }
}

var CanaryHandle = false

class IsDebugQLO : CanaryBase, CustomStringConvertible {
  @objc var description: String {
    return "I'm a QLO"
  }
}

class HasDebugQLO : CanaryBase {
  @objc var debugQuickLookObject: AnyObject {
    return IsDebugQLO()
  }
}

class HasNumberQLO : CanaryBase {
  @objc var debugQuickLookObject: AnyObject {
    let number = NSNumber(value: 97210)
    return number
  }
}

class HasAttributedQLO : CanaryBase {
  @objc var debugQuickLookObject: AnyObject {
    let str = NSAttributedString(string: "attributed string")
    objc_setAssociatedObject(str, &CanaryHandle, CanaryBase(),
                             .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
    return str
  }
}

class HasStringQLO : CanaryBase {
  @objc var debugQuickLookObject: AnyObject {
    let str = NSString(string: "plain string")
    objc_setAssociatedObject(str, &CanaryHandle, CanaryBase(),
                             .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
    return str
  }
}

func testQLO<T : CanaryBase>(_ type: T.Type) {
  autoreleasepool {
    _ = PlaygroundQuickLook(reflecting: type.init())
  }
}

testQLO(IsDebugQLO.self)
// CHECK-NEXT: IsDebugQLO overboard

testQLO(HasDebugQLO.self)
// CHECK-NEXT: HasDebugQLO overboard
// CHECK-NEXT: IsDebugQLO overboard

testQLO(HasNumberQLO.self)
// CHECK-NEXT: HasNumberQLO overboard
// TODO: tagged numbers are immortal, so we can't reliably check for
//   cleanup here

testQLO(HasAttributedQLO.self)
// CHECK-NEXT: HasAttributedQLO overboard
// CHECK-NEXT: CanaryBase overboard

testQLO(HasStringQLO.self)
// CHECK-NEXT: HasStringQLO overboard
// CHECK-NEXT: CanaryBase overboard

