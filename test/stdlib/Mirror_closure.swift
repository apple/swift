//===--- Mirror_closure.swift -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -g -Onone  -module-name a %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: reflection

import StdlibUnittest

var tests = TestSuite("Mirror_closure")

struct Baz {
  let my_closure_property: () -> Int
}

tests.test("Reflect struct with closure field") {
  let baz = Baz(my_closure_property: { return 7 })
  let mirror = Mirror(reflecting: baz)
  let children = mirror.children
  let firstChild = children.first!
  let childValue: Any = firstChild.value
  expectNil(childValue as? () -> Int)
  expectNotNil(childValue as? ())
}

struct Baz2 {
  let my_closure_property: @convention(c) () -> Int
}

func c_closure() -> Int { return 7 }

tests.test("Reflect struct with @convention(c) field") {
  let baz2 = Baz2(my_closure_property: c_closure)
  let mirror = Mirror(reflecting: baz2)
  let children = mirror.children
  let firstChild = children.first!
  let childValue: Any = firstChild.value
  expectNil(childValue as? ())
  expectNotNil(childValue as? @convention(c) () -> Int)
  if let closure = childValue as? () -> Int {
    expectEqual(7, closure())
  }
}

struct Quux {
  let my_closure_property: Any
}

tests.test("Reflect struct with closure field boxed in Any") {
  let quux = Quux(my_closure_property: { return 7 } as Any)
  let mirror = Mirror(reflecting: quux)
  let children = mirror.children
  let firstChild = children.first!
  let childValue: Any = firstChild.value
  expectNil(childValue as? ())
  expectNotNil(childValue as? () -> Int)
  if let closure = childValue as? () -> Int {
    expectEqual(7, closure())
  }
}

struct Fozzle: CustomReflectable {
  let my_closure_property: () -> Int

  var customMirror: Mirror {
    Mirror(self, children: ["my_closure_property": my_closure_property])
  }
}

tests.test("Reflect struct with custom mirror") {
  let quux = Fozzle(my_closure_property: { return 7 })
  let mirror = Mirror(reflecting: quux)
  let children = mirror.children
  let firstChild = children.first!
  let childValue: Any = firstChild.value
  expectNil(childValue as? ())
  expectNotNil(childValue as? () -> Int)
  if let closure = childValue as? () -> Int {
    expectEqual(7, closure())
  }
}


runAllTests()
