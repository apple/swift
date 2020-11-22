// RUN: %empty-directory(%t)
//
// RUN: %target-clang %S/Inputs/Mirror/Mirror.mm -c -o %t/Mirror.mm.o -g
// RUN: %target-build-swift -parse-stdlib %s -module-name Reflection -I %S/Inputs/Mirror/ -Xlinker %t/Mirror.mm.o -o %t/a.out
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

// Check ObjC mirror implementation.
// CHECK-LABEL: ObjC:
print("ObjC:")
// CHECK-NEXT:  <NSObject: {{0x[0-9a-f]+}}>
dump(NSObject())

// CHECK-LABEL: ObjC subclass:
print("ObjC subclass:")
// CHECK-NEXT: woozle wuzzle
dump("woozle wuzzle" as NSString)

// Test a mixed Swift-ObjC hierarchy.
class NSGood : NSObject {
  let x: Int = 22
}
class NSBetter : NSGood {
  let y: String = "333"
}

// CHECK-LABEL: Swift ObjC subclass:
// CHECK-NEXT:    <Reflection.NSBetter: {{0x[0-9a-f]+}}> #0
// CHECK-NEXT:      super: Reflection.NSGood
// CHECK-NEXT:        super: NSObject
print("Swift ObjC subclass:")
dump(NSBetter())

// CHECK-LABEL: ObjC quick look objects:
print("ObjC quick look objects:")

// CHECK-LABEL: ObjC enums:
print("ObjC enums:")

// CHECK-NEXT: We cannot reflect NSComparisonResult yet
print("We cannot reflect \(ComparisonResult.orderedAscending) yet")

// Don't crash when introspecting framework types such as NSURL.
// <rdar://problem/16592777>
// CHECK-LABEL: NSURL:
// CHECK-NEXT:    file:///Volumes/
// CHECK-NEXT:    - super: NSObject
print("NSURL:")
dump(NSURL(fileURLWithPath: "/Volumes", isDirectory: true))

// CHECK-LABEL: Reflecting NSArray:
// CHECK-NEXT: [ 1 2 3 4 5 ]
print("Reflecting NSArray:")
let intNSArray : NSArray = [1 as NSNumber,2 as NSNumber,3 as NSNumber,4 as NSNumber,5 as NSNumber]
let arrayMirror = Mirror(reflecting: intNSArray)
var buffer = "[ "
for i in arrayMirror.children {
  buffer += "\(i.1) "
}
buffer += "]"
print(buffer)

// CHECK-LABEL: Reflecting NSSet:
// CHECK-NEXT: NSSet reflection working fine
print("Reflecting NSSet:")
let numset = NSSet(objects: 1,2,3,4)
let numsetMirror = Mirror(reflecting: numset)
var numsetNumbers = Set<Int>()
for i in numsetMirror.children {
  if let number = i.1 as? Int {
    numsetNumbers.insert(number)
  }
}
if numsetNumbers == Set([1, 2, 3, 4]) {
  print("NSSet reflection working fine")
} else {
  print("NSSet reflection broken: here are the numbers we got: \(numsetNumbers)")
}

// CHECK-NEXT: (3.0, 6.0)
// CHECK-NEXT:   x: 3.0
// CHECK-NEXT:   y: 6.0
dump(CGPoint(x: 3,y: 6))
// CHECK-NEXT: (30.0, 60.0)
// CHECK-NEXT:   width: 30.0
// CHECK-NEXT:   height: 60.0
dump(CGSize(width: 30, height: 60))
// CHECK-NEXT: (50.0, 60.0, 100.0, 150.0)
// CHECK-NEXT:  origin: (50.0, 60.0)
// CHECK-NEXT:    x: 50.0
// CHECK-NEXT:    y: 60.0
// CHECK-NEXT:  size: (100.0, 150.0)
// CHECK-NEXT:    width: 100.0
// CHECK-NEXT:    height: 150.0
dump(CGRect(x: 50, y: 60, width: 100, height: 150))

let x = float4(0)
print("float4 has \(Mirror(reflecting: x).children.count) children")
// CHECK-NEXT: float4 has 1 children

// CHECK-LABEL: and now our song is done
print("and now our song is done")

