//===--- Mirror.swift -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// RUN: %empty-directory(%t)
//
// RUN: if [ %target-runtime == "objc" ]; \
// RUN: then \
// RUN:   %target-clang %S/Inputs/Mirror/Mirror.mm -c -o %t/Mirror.mm.o -g && \
// RUN:   %target-build-swift -Xfrontend -disable-access-control %s -I %S/Inputs/Mirror/ -Xlinker %t/Mirror.mm.o -o %t/Mirror; \
// RUN: else \
// RUN:   %target-build-swift %s -Xfrontend -disable-access-control -o %t/Mirror; \
// RUN: fi
// RUN: %target-codesign %t/Mirror
// RUN: %target-run %t/Mirror

// REQUIRES: executable_test
// REQUIRES: shell

import StdlibUnittest

var mirrors = TestSuite("Mirrors")

class NativeSwiftClass : NativeClassBoundExistential {
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

protocol NativeClassBoundExistential : AnyObject {
  var x: Int { get }
}
class NativeSwiftClassHasWeak {
  weak var weakProperty: AnyObject?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

class NativeSwiftClassHasNativeClassBoundExistential {
  weak var weakProperty: NativeClassBoundExistential?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

struct StructHasNativeWeakReference {
  weak var weakProperty: AnyObject?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

mirrors.test("class/NativeSwiftClassHasNativeWeakReference") {
  let parent = NativeSwiftClassHasWeak(x: 1010)
  let child = NativeSwiftClass(x: 2020)
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! NativeSwiftClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("class/NativeSwiftClassHasNativeClassBoundExistential") {
  let parent = NativeSwiftClassHasNativeClassBoundExistential(x: 1010)
  let child = NativeSwiftClass(x: 2020) as NativeClassBoundExistential
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! NativeSwiftClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("struct/StructHasNativeWeakReference") {
  var parent = StructHasNativeWeakReference(x: 1010)
  let child = NativeSwiftClass(x: 2020)
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! NativeSwiftClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

// SR-8878: Using Mirror to access a weak reference results in object
// being retained indefinitely
mirrors.test("class/NativeSwiftClassHasNativeWeakReferenceNoLeak") {
  weak var verifier: AnyObject?
  do {
    let parent = NativeSwiftClassHasWeak(x: 1010)
    let child = NativeSwiftClass(x: 2020)
    verifier = child
    parent.weakProperty = child
    let mirror = Mirror(reflecting: parent)
    let children = Array(mirror.children)
    let extractedChild = children[0].1 as! NativeSwiftClass
    expectNotNil(extractedChild)
    expectNotNil(verifier)
    // If child is destroyed, the above cast and checks will fail.
    _fixLifetime(child)
  }
  expectNil(verifier)
}

#if _runtime(_ObjC)

import Foundation

@objc protocol ObjCClassExistential : AnyObject {
  var weakProperty: AnyObject? { get set }
  var x: Int { get }
}

class ObjCClass : ObjCClassExistential {
  weak var weakProperty: AnyObject?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

class NativeSwiftClassHasObjCClassBoundExistential {
  weak var weakProperty: ObjCClassExistential?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

class ObjCClassHasWeak : NSObject {
  weak var weakProperty: AnyObject?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

class ObjCClassHasNativeClassBoundExistential : NSObject {
  weak var weakProperty: NativeClassBoundExistential?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

class ObjCClassHasObjCClassBoundExistential : NSObject {
  weak var weakProperty: ObjCClassExistential?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

struct StructHasObjCWeakReference {
  weak var weakProperty: ObjCClass?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

struct StructHasObjCClassBoundExistential {
  weak var weakProperty: ObjCClassExistential?
  let x: Int
  init(x: Int) {
    self.x = x
  }
}

mirrors.test("class/NativeSwiftClassHasObjCWeakReference") {
  let parent = NativeSwiftClassHasWeak(x: 1010)
  let child = ObjCClass(x: 2020)
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! ObjCClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("class/NativeSwiftClassHasObjCClassBoundExistential") {
  let parent = NativeSwiftClassHasObjCClassBoundExistential(x: 1010)
  let child = ObjCClass(x: 2020) as ObjCClassExistential
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! ObjCClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("class/ObjCClassHasNativeWeak") {
  let parent = ObjCClassHasWeak(x: 1010)
  let child = NativeSwiftClass(x: 2020)
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! NativeSwiftClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("class/ObjcCClassHasObjCWeakReference") {
  let parent = ObjCClassHasWeak(x: 1010)
  let child = ObjCClass(x: 2020)
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! ObjCClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("class/ObjCClassHasNativeClassBoundExistential") {
  let parent = ObjCClassHasNativeClassBoundExistential(x: 1010)
  let child = NativeSwiftClass(x: 2020) as NativeClassBoundExistential
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! NativeSwiftClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("class/ObjCClassHasObjCClassBoundExistential") {
  let parent = ObjCClassHasObjCClassBoundExistential(x: 1010)
  let child = ObjCClass(x: 2020) as ObjCClassExistential
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! ObjCClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("struct/StructHasObjCWeakReference") {
  var parent = StructHasObjCWeakReference(x: 1010)
  let child = ObjCClass(x: 2020)
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! ObjCClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

mirrors.test("struct/StructHasObjCClassBoundExistential") {
  var parent = StructHasObjCClassBoundExistential(x: 1010)
  let child = ObjCClass(x: 2020) as ObjCClassExistential
  parent.weakProperty = child
  let mirror = Mirror(reflecting: parent)
  let children = Array(mirror.children)
  let extractedChild = children[0].1 as! ObjCClass
  expectNotEqual(parent.x, extractedChild.x)
  expectEqual(ObjectIdentifier(child), ObjectIdentifier(extractedChild))
  expectEqual(child.x, extractedChild.x)
  print(extractedChild)
}

#endif

runAllTests()
