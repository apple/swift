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
// RUN: cp %s %t/main.swift
//
// RUN: if [ %target-runtime == "objc" ]; \
// RUN: then \
// RUN:   %target-clang %S/Inputs/Mirror/Mirror.mm -c -o %t/Mirror.mm.o -g && \
// RUN:   %target-build-swift %t/main.swift %S/Inputs/Mirror/MirrorOther.swift -I %S/Inputs/Mirror/ -Xlinker %t/Mirror.mm.o -o %t/Mirror; \
// RUN: else \
// RUN:   %target-build-swift %t/main.swift %S/Inputs/Mirror/MirrorOther.swift -o %t/Mirror; \
// RUN: fi
// RUN: %target-codesign %t/Mirror
// RUN: %target-run %t/Mirror

// REQUIRES: executable_test
// REQUIRES: shell
// REQUIRES: reflection

// rdar://96439408
// UNSUPPORTED: use_os_stdlib

import StdlibUnittest


var mirrors = TestSuite("Mirrors")

extension Mirror {
  public var testDescription: String {
    let nil_ = "nil"
    return "[" +
      children.lazy
        .map { "\($0.0 ?? nil_): \(String(reflecting: $0.1))" }
        .joined(separator: ", ")
      + "]"
  }
}

mirrors.test("RandomAccessStructure") {
  struct Eggs : CustomReflectable {
    var customMirror: Mirror {
      return Mirror(self, unlabeledChildren: ["aay", "bee", "cee"])
    }
  }

  let x = Eggs().customMirror
  
  expectEqual("[nil: \"aay\", nil: \"bee\", nil: \"cee\"]", x.testDescription)
}

let letters = "abcdefghijklmnopqrstuvwxyz "

func find(_ substring: String, within domain: String) -> String.Index? {
  let domainCount = domain.count
  let substringCount = substring.count

  if (domainCount < substringCount) { return nil }
  var sliceStart = domain.startIndex
  var sliceEnd = domain.index(sliceStart, offsetBy: substringCount)
  var i = 0
  while true {
    if domain[sliceStart..<sliceEnd] == substring {
      return sliceStart
    }
    if i == domainCount - substringCount { break }
    sliceStart = domain.index(after: sliceStart)
    sliceEnd = domain.index(after: sliceEnd)
    i += 1
  }
  return nil
}

mirrors.test("ForwardStructure") {
  struct DoubleYou : CustomReflectable {
    var customMirror: Mirror {
      return Mirror(
        self,
        unlabeledChildren: Set(letters),
        displayStyle: .`set`)
    }
  }

  let w = DoubleYou().customMirror
  expectEqual(.`set`, w.displayStyle)
  expectEqual(letters.count, numericCast(w.children.count))
  
  // Because we don't control the order of a Set, we need to do a
  // fancy dance in order to validate the result.
  let description = w.testDescription
  for c in letters {
    let expected = "nil: \"\(c)\""
    expectNotNil(find(expected, within: description))
  }
}

mirrors.test("BidirectionalStructure") {
  struct Why : CustomReflectable {
    var customMirror: Mirror {
      return Mirror(
        self,
        unlabeledChildren: letters,
        displayStyle: .collection)
    }
  }

  // Test that the basics seem to work
  let y = Why().customMirror
  expectEqual(.`collection`, y.displayStyle)

  let description = y.testDescription
  expectEqual(
    "[nil: \"a\", nil: \"b\", nil: \"c\", nil: \"",
    description[description.startIndex..<description.firstIndex(of: "d")!])
}

mirrors.test("LabeledStructure") {
  struct Zee : CustomReflectable, CustomStringConvertible {
    var customMirror: Mirror {
      return Mirror(self, children: ["bark": 1, "bite": 0])
    }
    var description: String { return "Zee" }
  }

  let z = Zee().customMirror
  expectEqual("[bark: 1, bite: 0]", z.testDescription)
  expectNil(z.displayStyle)

  struct Zee2 : CustomReflectable {
    var customMirror: Mirror {
      return Mirror(
        self, children: ["bark": 1, "bite": 0], displayStyle: .dictionary)
    }
  }
  let z2 = Zee2().customMirror
  expectEqual(.dictionary, z2.displayStyle)
  expectEqual("[bark: 1, bite: 0]", z2.testDescription)

  struct Heterogeny : CustomReflectable {
    var customMirror: Mirror {
      return Mirror(
        self, children: ["bark": 1, "bite": Zee()])
    }
  }
  let h = Heterogeny().customMirror
  expectEqual("[bark: 1, bite: Zee]", h.testDescription)
}

mirrors.test("Legacy") {
  let m = Mirror(reflecting: [1, 2, 3])
  expectTrue(m.subjectType == [Int].self)
  
  let x0: [Mirror.Child] = [
    (label: nil, value: 1),
    (label: nil, value: 2),
    (label: nil, value: 3)
  ]
  expectFalse(
    zip(x0, m.children).contains {
      $0.0.value as! Int != $0.1.value as! Int
    })

  class B { let bx: Int = 0 }
  class D : B { let dx: Int = 1 }

  let mb = Mirror(reflecting: B())
  
  func expectBMirror(
    _ mb: Mirror, stackTrace: SourceLocStack = SourceLocStack(),
    file: String = #file, line: UInt = #line
  ) {
    expectTrue(mb.subjectType == B.self,
      stackTrace: stackTrace, file: file, line: line)
    
    expectNil(
      mb.superclassMirror,
      stackTrace: stackTrace, file: file, line: line)
    
    expectEqual(
      1, mb.children.count,
      stackTrace: stackTrace, file: file, line: line)
    
    expectEqual(
      "bx", mb.children.first?.label,
      stackTrace: stackTrace, file: file, line: line)
    
    expectEqual(
      0, mb.children.first?.value as? Int,
      stackTrace: stackTrace, file: file, line: line)
  }
  
  expectBMirror(mb)
  
  // Ensure that the base class instance is properly filtered out of
  // the child list
  do {
    let md = Mirror(reflecting: D())
    expectTrue(md.subjectType == D.self)
    
    expectEqual(1, md.children.count)
    expectEqual("dx", md.children.first?.label)
    expectEqual(1, md.children.first?.value as? Int)
    
    expectNotNil(md.superclassMirror)
    if let mb2 = md.superclassMirror { expectBMirror(mb2) }
  }

  do {
    // Ensure that we reflect on the dynamic type of the subject
    let md = Mirror(reflecting: D() as B)
    expectTrue(md.subjectType == D.self)
    
    expectEqual(1, md.children.count)
    expectEqual("dx", md.children.first?.label)
    expectEqual(1, md.children.first?.value as? Int)
    
    expectNotNil(md.superclassMirror)
    if let mb2 = md.superclassMirror { expectBMirror(mb2) }
  }
}

//===----------------------------------------------------------------------===//
//===--- Class Support ----------------------------------------------------===//

class DullClass {}

mirrors.test("ClassReflection") {
  expectEqual(.`class`, Mirror(reflecting: DullClass()).displayStyle)
}

mirrors.test("Class/Root/Uncustomized") {
  class A { var a: Int = 1 }

  let a = Mirror(reflecting: A())
  expectTrue(a.subjectType == A.self)
  expectNil(a.superclassMirror)
  expectEqual(1, a.children.count)
  expectEqual("a", a.children.first!.label)
}

//===--- Generated Superclass Mirrors -------------------------------------===//
mirrors.test("Class/Root/superclass:.generated") {
  class B : CustomReflectable {
    var b: String = "two"
    var customMirror: Mirror {
      return Mirror(
        self, children: ["bee": b], ancestorRepresentation: .generated)
    }
  }
  
  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  expectNil(b.superclassMirror)
  expectEqual(1, b.children.count)
  expectEqual("bee", b.children.first!.label)
  expectEqual("two", b.children.first!.value as? String)
}


mirrors.test("class/Root/superclass:<default>") {
  class C : CustomReflectable {
    var c: UInt = 3
    var customMirror: Mirror {
      return Mirror(self, children: ["sea": c + 1])
    }
  }
  
  let c = Mirror(reflecting: C())
  expectTrue(c.subjectType == C.self)
  expectNil(c.superclassMirror)
  expectEqual(1, c.children.count)
  expectEqual("sea", c.children.first!.label)
  expectEqual(4, c.children.first!.value as? UInt)
}

mirrors.test("class/Plain/Plain") {
  class A { var a: Int = 1 }
  class B : A { var b: UInt = 42 }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  
  if let bChild = expectNotNil(b.children.first) {
    expectEqual("b", bChild.label)
    expectEqual(42, bChild.value as? UInt)
  }
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    if let aChild = expectNotNil(a.children.first) {
      expectEqual("a", aChild.label)
      expectEqual(1, aChild.value as? Int)
      expectNil(a.superclassMirror)
    }
  }
}

mirrors.test("class/UncustomizedSuper/Synthesized/Implicit") {
  class A { var a: Int = 1 }

  class B : A, CustomReflectable {
    var b: UInt = 42
    var customMirror: Mirror {
      return Mirror(self, children: ["bee": b])
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    expectEqual("a", a.children.first?.label)
    expectNil(a.superclassMirror)
  }
}

mirrors.test("class/UncustomizedSuper/Synthesized/Explicit") {
  class A { var a: Int = 1 }

  class B : A, CustomReflectable {
    var b: UInt = 42
    var customMirror: Mirror {
      return Mirror(
        self, children: ["bee": b], ancestorRepresentation: .generated)
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    expectEqual("a", a.children.first!.label)
    expectNil(a.superclassMirror)
  }
}

mirrors.test("class/CustomizedSuper/Synthesized") {
  class A : CustomReflectable {
    var a: Int = 1
    var customMirror: Mirror {
      return Mirror(self, children: ["aye": a])
    }
  }

  class B : A {
    var b: UInt = 42
    // This is an unusual case: when writing override on a
    // customMirror implementation you would typically want to pass
    // ancestorRepresentation: .customized(super.customMirror) or, in
    // rare cases, ancestorRepresentation: .Suppressed.  However, it
    // has an expected behavior, which we test here.
    override var customMirror: Mirror {
      return Mirror(self, children: ["bee": b])
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    expectEqual("a", a.children.first!.label)
    expectNil(a.superclassMirror)
  }
}

#if _runtime(_ObjC)
import Foundation
import MirrorObjC

//===--- ObjC Base Classes ------------------------------------------------===//

mirrors.test("class/ObjCPlain/Plain") {
  class A : NSObject { var a: Int = 1 }
  class B : A { var b: UInt = 42 }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  
  if let bChild = expectNotNil(b.children.first) {
    expectEqual("b", bChild.label)
    expectEqual(42, bChild.value as? UInt)
  }
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    if let aChild = expectNotNil(a.children.first) {
      expectEqual("a", aChild.label)
      expectEqual(1, aChild.value as? Int)
      if let o = expectNotNil(a.superclassMirror) {
        expectEqual("NSObject", String(reflecting: o.subjectType))
      }
    }
  }
}

mirrors.test("class/ObjCUncustomizedSuper/Synthesized/Implicit") {
  class A : NSObject { var a: Int = 1 }

  class B : A, CustomReflectable {
    var b: UInt = 42
    var customMirror: Mirror {
      return Mirror(self, children: ["bee": b])
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    expectEqual("a", a.children.first?.label)
    if let o = expectNotNil(a.superclassMirror) {
      expectTrue(o.subjectType == NSObject.self)
    }
  }
}

mirrors.test("class/ObjCUncustomizedSuper/Synthesized/Explicit") {
  class A : NSObject { var a: Int = 1 }

  class B : A, CustomReflectable {
    var b: UInt = 42
    var customMirror: Mirror {
      return Mirror(
        self, children: ["bee": b], ancestorRepresentation: .generated)
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    expectEqual("a", a.children.first!.label)
    if let o = expectNotNil(a.superclassMirror) {
      expectTrue(o.subjectType == NSObject.self)
    }
  }
}

mirrors.test("class/ObjCCustomizedSuper/Synthesized") {
  class A : DateFormatter, CustomReflectable {
    var a: Int = 1
    var customMirror: Mirror {
      return Mirror(self, children: ["aye": a])
    }
  }

  class B : A {
    var b: UInt = 42
    // This is an unusual case: when writing override on a
    // customMirror implementation you would typically want to pass
    // ancestorRepresentation: .customized(super.customMirror) or, in
    // rare cases, ancestorRepresentation: .Suppressed.  However, it
    // has an expected behavior, which we test here.
    override var customMirror: Mirror {
      return Mirror(self, children: ["bee": b])
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    expectEqual("a", a.children.first!.label)
    if let d = expectNotNil(a.superclassMirror) {
      expectTrue(d.subjectType == DateFormatter.self)
      if let f = expectNotNil(d.superclassMirror) {
        expectTrue(f.subjectType == Formatter.self)
        if let o = expectNotNil(f.superclassMirror) {
          expectTrue(o.subjectType == NSObject.self)
          expectNil(o.superclassMirror)
        }
      }
    }
  }
}

mirrors.test("ObjC") {
  // Some Foundation classes lie about their ivars, which would crash
  // a mirror; make sure we are not automatically exposing ivars of
  // Objective-C classes from the default mirror implementation.
  expectEqual(0, Mirror(reflecting: HasIVars()).children.count)
}

// rdar://problem/39629937
@objc class ObjCClass : NSObject {
  let value: Int

  init(value: Int) { self.value = value }

  override var description: String {
    return "\(value)"
  }
}

struct WrapObjCClassArray {
  var array: [ObjCClass]
}

mirrors.test("struct/WrapNSArray") {
  let nsArray: NSArray = [
    ObjCClass(value: 1), ObjCClass(value: 2),
    ObjCClass(value: 3), ObjCClass(value: 4)
  ]
  let s = String(describing: WrapObjCClassArray(array: nsArray as! [ObjCClass]))
  expectEqual("WrapObjCClassArray(array: [1, 2, 3, 4])", s)
}

#endif // _runtime(_ObjC)

//===--- Weak and Unowned References --------------------------------------===//

// Check that Mirror correctly reflects weak/unowned refs to both
// Swift and ObjC objects from Swift structs and classes.

protocol WeakUnownedTestsP1: AnyObject {
  func f1() -> Int
}

protocol WeakUnownedTestsP2 {
  func f2() -> String
}

class WeakUnownedSwiftClass: WeakUnownedTestsP1, WeakUnownedTestsP2 {
  let tracker = LifetimeTracked(0)
  func f1() -> Int { return 2 }
  func f2() -> String { return "b" }
}

#if _runtime(_ObjC)
@objc class WeakUnownedObjCClass: NSObject, WeakUnownedTestsP1, WeakUnownedTestsP2 {
  let tracker = LifetimeTracked(0)
  func f1() -> Int { return 2 }
  func f2() -> String { return "b" }
}
#endif

// The four tests below populate objects with different types
// but identical overall structure.
// This function is used by all four to verify that the resulting
// Mirror objects have the expected entries.
func verifyWeakUnownedReflection
  <ExpectedClass: WeakUnownedTestsP1 & WeakUnownedTestsP2>
  (_ m: Mirror, expectedClass: ExpectedClass.Type )
{
  let i = m.children.makeIterator()

  func verifyClassField(child: (label: String?, value: Any), name: String) {
    expectEqual(child.label, name)
    let v = child.value as? ExpectedClass
    expectNotNil(v)
    expectEqual(v!.f1(), 2)
  }

  func verifyExistentialField(child: (label: String?, value: Any), name: String) {
    expectEqual(child.label, name)
    expectNotNil(child.value)

    let vp1 = child.value as? WeakUnownedTestsP1
    expectNotNil(vp1)
    expectEqual(vp1!.f1(), 2)
    let vp2 = child.value as? WeakUnownedTestsP2
    expectNotNil(vp2)
    expectEqual(vp2!.f2(), "b")

    let v = child.value as? ExpectedClass
    expectNotNil(v)
    expectEqual(v!.f1(), 2)
    let m = Mirror(reflecting: v!)
    expectEqual(m.displayStyle, .`class`)
    // TODO: Find a way to verify that the existential wrapper carries
    // the expected protocol witnesses.  The current Swift runtime does
    // a very good job of hiding this from users.
  }

  verifyClassField(child: i.next()!, name: "strong_class")
  verifyExistentialField(child: i.next()!, name: "strong_existential")
  verifyClassField(child: i.next()!, name: "weak_class")
  verifyExistentialField(child: i.next()!, name: "weak_existential")
  verifyClassField(child: i.next()!, name: "unowned_safe_class")
  verifyExistentialField(child: i.next()!, name: "unowned_safe_existential")

  verifyClassField(child: i.next()!, name: "unowned_unsafe_class")
  verifyExistentialField(child: i.next()!, name: "unowned_unsafe_existential")
  expectNil(i.next())

  // The original bug report from https://github.com/apple/swift/issues/47864
  // crashed when the print() code attempted to reflect the contents of an
  // unowned field.
  // The tests above _should_ suffice to check this, but let's print everything
  // anyway just to be sure.
  for c in m.children {
    print(c.label ?? "?", c.value)
  }
}

#if _runtime(_ObjC)
// Related: https://github.com/apple/swift/issues/47864 reported a crash when
// using 'Mirror' to inspect Swift class objects containing unowned pointers
// to Obj-C class objects.
mirrors.test("Weak and Unowned Obj-C refs in class") {
  class SwiftClassWithWeakAndUnowned {
    var strong_class: WeakUnownedObjCClass
    var strong_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2
    weak var weak_class: WeakUnownedObjCClass?
    weak var weak_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)?
    unowned(safe) let unowned_safe_class: WeakUnownedObjCClass
    unowned(safe) let unowned_safe_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2
    unowned(unsafe) let unowned_unsafe_class: WeakUnownedObjCClass
    unowned(unsafe) let unowned_unsafe_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2

    init(_ objc: WeakUnownedObjCClass) {
      self.strong_class = objc
      self.strong_existential = objc
      self.weak_class = objc
      self.weak_existential = objc
      self.unowned_safe_class = objc
      self.unowned_safe_existential = objc
      self.unowned_unsafe_class = objc
      self.unowned_unsafe_existential = objc
    }
  }

	if #available(SwiftStdlib 5.3, *) {
		let objc = WeakUnownedObjCClass()
		let classWithReferences = SwiftClassWithWeakAndUnowned(objc)
		let m = Mirror(reflecting: classWithReferences)
		expectEqual(m.displayStyle, .`class`)
		expectEqual(m.description, "Mirror for SwiftClassWithWeakAndUnowned")
		expectEqual(m.subjectType, SwiftClassWithWeakAndUnowned.self)
		verifyWeakUnownedReflection(m, expectedClass: WeakUnownedObjCClass.self)
	}
}

mirrors.test("Weak and Unowned Obj-C refs in struct") {
  struct SwiftStructWithWeakAndUnowned {
    var strong_class: WeakUnownedObjCClass
    var strong_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2
    weak var weak_class: WeakUnownedObjCClass?
    weak var weak_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)?
    unowned(safe) let unowned_safe_class: WeakUnownedObjCClass
    unowned(safe) let unowned_safe_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2
    unowned(unsafe) let unowned_unsafe_class: WeakUnownedObjCClass
    unowned(unsafe) let unowned_unsafe_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2

    init(_ objc: WeakUnownedObjCClass) {
      self.strong_class = objc
      self.strong_existential = objc
      self.weak_class = objc
      self.weak_existential = objc
      self.unowned_safe_class = objc
      self.unowned_safe_existential = objc
      self.unowned_unsafe_class = objc
      self.unowned_unsafe_existential = objc
    }
  }

	if #available(SwiftStdlib 5.3, *) {
		let objc = WeakUnownedObjCClass()
		let structWithReferences = SwiftStructWithWeakAndUnowned(objc)
		let m = Mirror(reflecting: structWithReferences)
		expectEqual(m.displayStyle, .`struct`)
		expectEqual(m.description, "Mirror for SwiftStructWithWeakAndUnowned")
		expectEqual(m.subjectType, SwiftStructWithWeakAndUnowned.self)
		verifyWeakUnownedReflection(m, expectedClass: WeakUnownedObjCClass.self)
	}
}

#endif

mirrors.test("Weak and Unowned Swift refs in class") {
  class SwiftClassWithWeakAndUnowned {
    var strong_class: WeakUnownedSwiftClass
    var strong_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2
    weak var weak_class: WeakUnownedSwiftClass?
    weak var weak_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)?
    unowned(safe) let unowned_safe_class: WeakUnownedSwiftClass
    unowned(safe) let unowned_safe_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)
    unowned(unsafe) let unowned_unsafe_class: WeakUnownedSwiftClass
    unowned(unsafe) let unowned_unsafe_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)

    init(_ swift: WeakUnownedSwiftClass) {
      self.strong_class = swift
      self.strong_existential = swift
      self.weak_class = swift
      self.weak_existential = swift
      self.unowned_safe_class = swift
      self.unowned_safe_existential = swift
      self.unowned_unsafe_class = swift
      self.unowned_unsafe_existential = swift
    }
  }

	if #available(SwiftStdlib 5.3, *) {
		let swift = WeakUnownedSwiftClass()
		let classWithReferences = SwiftClassWithWeakAndUnowned(swift)
		let m = Mirror(reflecting: classWithReferences)
		expectEqual(m.displayStyle, .`class`)
		expectEqual(m.description, "Mirror for SwiftClassWithWeakAndUnowned")
		expectEqual(m.subjectType, SwiftClassWithWeakAndUnowned.self)
		verifyWeakUnownedReflection(m, expectedClass: WeakUnownedSwiftClass.self)
	}
}

mirrors.test("Weak and Unowned Swift refs in struct") {
  struct SwiftStructWithWeakAndUnowned {
    var strong_class: WeakUnownedSwiftClass
    var strong_existential: WeakUnownedTestsP1 & WeakUnownedTestsP2
    weak var weak_class: WeakUnownedSwiftClass?
    weak var weak_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)?
    unowned(safe) let unowned_safe_class: WeakUnownedSwiftClass
    unowned(safe) let unowned_safe_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)
    unowned(unsafe) let unowned_unsafe_class: WeakUnownedSwiftClass
    unowned(unsafe) let unowned_unsafe_existential: (WeakUnownedTestsP1 & WeakUnownedTestsP2)

    init(_ swift: WeakUnownedSwiftClass) {
      self.strong_class = swift
      self.strong_existential = swift
      self.weak_class = swift
      self.weak_existential = swift
      self.unowned_safe_class = swift
      self.unowned_safe_existential = swift
      self.unowned_unsafe_class = swift
      self.unowned_unsafe_existential = swift
    }
  }

	if #available(SwiftStdlib 5.3, *) {
		let swift = WeakUnownedSwiftClass()
		let structWithReferences = SwiftStructWithWeakAndUnowned(swift)
		let m = Mirror(reflecting: structWithReferences)
		expectEqual(m.displayStyle, .`struct`)
		expectEqual(m.description, "Mirror for SwiftStructWithWeakAndUnowned")
		expectEqual(m.subjectType, SwiftStructWithWeakAndUnowned.self)
		verifyWeakUnownedReflection(m, expectedClass: WeakUnownedSwiftClass.self)
	}
}

//===--- Suppressed Superclass Mirrors ------------------------------------===//
mirrors.test("Class/Root/NoSuperclassMirror") {
  class B : CustomReflectable {
    var b: String = "two"
    var customMirror: Mirror {
      return Mirror(
        self, children: ["bee": b], ancestorRepresentation: .suppressed)
    }
  }
  
  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  expectNil(b.superclassMirror)
  expectEqual(1, b.children.count)
  expectEqual("bee", b.children.first!.label)
}

mirrors.test("class/UncustomizedSuper/NoSuperclassMirror") {
  class A { var a: Int = 1 }

  class B : A, CustomReflectable {
    var b: UInt = 42
    var customMirror: Mirror {
      return Mirror(
        self, children: ["bee": b], ancestorRepresentation: .suppressed)
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  expectNil(b.superclassMirror)
}

mirrors.test("class/CustomizedSuper/NoSuperclassMirror") {
  class A : CustomReflectable {
    var a: Int = 1
    var customMirror: Mirror {
      return Mirror(self, children: ["aye": a])
    }
  }

  class B : A {
    var b: UInt = 42
    override var customMirror: Mirror {
      return Mirror(
        self, children: ["bee": b], ancestorRepresentation: .suppressed)
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  expectNil(b.superclassMirror)
}

//===--- Override Superclass Mirrors --------------------------------------===//
mirrors.test("class/CustomizedSuper/SuperclassCustomMirror/Direct") {
  class A : CustomReflectable {
    var a: Int = 1
    var customMirror: Mirror {
      return Mirror(self, children: ["aye": a])
    }
  }

  // B inherits A directly
  class B : A {
    var b: UInt = 42
    override var customMirror: Mirror {
      return Mirror(
        self,
        children: ["bee": b],
        ancestorRepresentation: .customized({ super.customMirror }))
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    expectEqual("aye", a.children.first!.label)
    expectNil(a.superclassMirror)
  }
}

mirrors.test("class/CustomizedSuper/SuperclassCustomMirror/Indirect") {
  class A : CustomReflectable {
    var a: Int = 1
    var customMirror: Mirror {
      return Mirror(self, children: ["aye": a])
    }
  }

  class X : A {}

  class Y : X {}

  // B inherits A indirectly through X and Y
  class B : Y {
    var b: UInt = 42
    override var customMirror: Mirror {
      return Mirror(
        self,
        children: ["bee": b],
        ancestorRepresentation: .customized({ super.customMirror }))
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let y = expectNotNil(b.superclassMirror) {
    expectTrue(y.subjectType == Y.self)
    if let x = expectNotNil(y.superclassMirror) {
      expectTrue(x.subjectType == X.self)
      expectEqual(0, x.children.count)
      if let a = expectNotNil(x.superclassMirror) {
        expectTrue(a.subjectType == A.self)
        if let aye = expectNotNil(a.children.first) {
          expectEqual("aye", aye.label)
        }
      }
    }
  }
}

mirrors.test("class/CustomizedSuper/SuperclassCustomMirror/Indirect2") {
  class A : CustomLeafReflectable {
    var a: Int = 1
    var customMirror: Mirror {
      return Mirror(
        self, children: ["aye": a])
    }
  }

  class X : A {}

  class Y : X {}

  // B inherits A indirectly through X and Y
  class B : Y {
    var b: UInt = 42
    override var customMirror: Mirror {
      return Mirror(
        self,
        children: ["bee": b],
        ancestorRepresentation: .customized({ super.customMirror }))
    }
  }

  let b = Mirror(reflecting: B())
  expectTrue(b.subjectType == B.self)
  if let a = expectNotNil(b.superclassMirror) {
    expectTrue(a.subjectType == A.self)
    if let aye = expectNotNil(a.children.first) {
      expectEqual("aye", aye.label)
    }
  }
}

mirrors.test("class/Cluster") {
  class A : CustomLeafReflectable {
    var a: Int = 1
    var customMirror: Mirror {
      return Mirror(
        self, children: ["aye": a])
    }
  }

  class X : A {}

  class Y : X {}

  let a = Mirror(reflecting: Y())
  expectTrue(a.subjectType == A.self)
  if let aye = expectNotNil(a.children.first) {
    expectEqual("aye", aye.label)
  }
}

//===--- Miscellaneous ----------------------------------------------------===//
//===----------------------------------------------------------------------===//

mirrors.test("Visibility") {
  struct Secrets {
    public var exposed: Int = 88
    var visible: Int = 7
    private var invisible: Int = 12
    var computed: Int { return 99 }
  }
  let m = Mirror(reflecting: Secrets())
  let d = m.testDescription
  expectEqual(d, "[exposed: 88, visible: 7, invisible: 12]")
}

mirrors.test("Addressing") {
  let m0 = Mirror(reflecting: [1, 2, 3])
  expectEqual(1, m0.descendant(0) as? Int)
  expectEqual(2, m0.descendant(1) as? Int)
  expectEqual(3, m0.descendant(2) as? Int)
  
  let m1 = Mirror(reflecting: (a: ["one", "two", "three"], 4))
  let ott0 = m1.descendant(0) as? [String]
  expectNotNil(ott0)
  let ott1 = m1.descendant("a") as? [String]
  expectNotNil(ott1)
  if ott0 != nil && ott1 != nil {
    expectEqualSequence(ott0!, ott1!)
  }
  expectEqual(4, m1.descendant(1) as? Int)
  expectEqual(4, m1.descendant(".1") as? Int)
  expectEqual("one", m1.descendant(0, 0) as? String)
  expectEqual("two", m1.descendant(0, 1) as? String)
  expectEqual("three", m1.descendant(0, 2) as? String)
  expectEqual("one", m1.descendant("a", 0) as? String)

  struct Zee : CustomReflectable {
    var customMirror: Mirror {
      return Mirror(self, children: ["bark": 1, "bite": 0])
    }
  }
  
  let x = [
    (a: ["one", "two", "three"], b: Zee()),
    (a: ["five"], b: Zee()),
    (a: [], b: Zee())]

  let m = Mirror(reflecting: x)
  let two = m.descendant(0, "a", 1)
  expectEqual("two", two as? String)
  expectEqual(1, m.descendant(1, 1, "bark") as? Int)
  expectEqual(0, m.descendant(1, 1, "bite") as? Int)
  expectNil(m.descendant(1, 1, "bork"))
}

#if !os(WASI)
// Trap tests aren't available on WASI.
mirrors.test("Invalid Path Type")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .code {
  struct X : MirrorPath {}
  let m = Mirror(reflecting: [1, 2, 3])
  expectEqual(1, m.descendant(0) as? Int)
  expectCrashLater()
  _ = m.descendant(X())
}
#endif

mirrors.test("PlaygroundQuickLook") {
  // Customization works.
  struct CustomQuickie : CustomPlaygroundQuickLookable {
    var customPlaygroundQuickLook: PlaygroundQuickLook {
      return .point(1.25, 42)
    }
  }
  switch PlaygroundQuickLook(reflecting: CustomQuickie()) {
  case .point(1.25, 42): break
  default: expectTrue(false)
  }
  
  // PlaygroundQuickLook support from Legacy Mirrors works.
  switch PlaygroundQuickLook(reflecting: true) {
  case .bool(true): break
  default: expectTrue(false)
  }

  // With no Legacy Mirror QuickLook support, we fall back to
  // String(reflecting: ).
  struct X {}
  switch PlaygroundQuickLook(reflecting: X()) {
  case .text(let text):
#if _runtime(_ObjC)
// FIXME: Enable if non-objc hasSuffix is implemented.
    expectTrue(text.contains(").X"), text)
#endif
  default:
    expectTrue(false)
  }
  struct Y : CustomDebugStringConvertible {
    var debugDescription: String { return "Why?" }
  }
  switch PlaygroundQuickLook(reflecting: Y()) {
  case .text("Why?"): break
  default: expectTrue(false)
  }
}

class Parent {}

extension Parent : _DefaultCustomPlaygroundQuickLookable {
  var _defaultCustomPlaygroundQuickLook: PlaygroundQuickLook {
    return .text("base")
  }
}

class Child : Parent { }

class FancyChild : Parent, CustomPlaygroundQuickLookable {
  var customPlaygroundQuickLook: PlaygroundQuickLook {
    return .text("child")
  }
}

mirrors.test("_DefaultCustomPlaygroundQuickLookable") {
  // testing the workaround for custom quicklookables in subclasses
  switch PlaygroundQuickLook(reflecting: Child()) {
  case .text("base"): break
  default: expectUnreachable("Base custom quicklookable was expected")
  }

  switch PlaygroundQuickLook(reflecting: FancyChild()) {
  case .text("child"): break
  default: expectUnreachable("FancyChild custom quicklookable was expected")
  }
}

mirrors.test("String.init") {
  expectEqual("42", String(42))
  expectEqual("42", String("42"))
  expectEqual("42", String(reflecting: 42))
  expectEqual("\"42\"", String(reflecting: "42"))
}

//===--- Structs ----------------------------------------------------------===//
//===----------------------------------------------------------------------===//

struct StructWithDefaultMirror {
  let s: String

  init (_ s: String) {
    self.s = s
  }
}

mirrors.test("Struct/NonGeneric/DefaultMirror") {
  do {
    var output = ""
    dump(StructWithDefaultMirror("123"), to: &output)
    expectEqual("▿ Mirror.StructWithDefaultMirror\n  - s: \"123\"\n", output)
  }

  do {
    // Build a String around an interpolation as a way of smoke-testing that
    // the internal _Mirror implementation gets memory management right.
    var output = ""
    dump(StructWithDefaultMirror("\(456)"), to: &output)
    expectEqual("▿ Mirror.StructWithDefaultMirror\n  - s: \"456\"\n", output)
  }

  expectEqual(
    .`struct`,
    Mirror(reflecting: StructWithDefaultMirror("")).displayStyle)
}

struct GenericStructWithDefaultMirror<T, U> {
  let first: T
  let second: U
}

mirrors.test("Struct/Generic/DefaultMirror") {
  do {
    let value = GenericStructWithDefaultMirror<Int, [Any?]>(
      first: 123,
      second: ["abc", 456, 789.25])
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ Mirror.GenericStructWithDefaultMirror<Swift.Int, Swift.Array<Swift.Optional<Any>>>\n" +
      "  - first: 123\n" +
      "  ▿ second: 3 elements\n" +
      "    ▿ Optional(\"abc\")\n" +
      "      - some: \"abc\"\n" +
      "    ▿ Optional(456)\n" +
      "      - some: 456\n" +
      "    ▿ Optional(789.25)\n" +
      "      - some: 789.25\n"

    expectEqual(expected, output)

  }
}

//===--- Enums ------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

enum NoPayloadEnumWithDefaultMirror {
  case A, ß
}

mirrors.test("Enum/NoPayload/DefaultMirror") {
  do {
    let value: [NoPayloadEnumWithDefaultMirror] =
        [.A, .ß]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 2 elements\n" +
      "  - Mirror.NoPayloadEnumWithDefaultMirror.A\n" +
      "  - Mirror.NoPayloadEnumWithDefaultMirror.ß\n"

    expectEqual(expected, output)
  }
}

enum SingletonNonGenericEnumWithDefaultMirror {
  case OnlyOne(Int)
}

mirrors.test("Enum/SingletonNonGeneric/DefaultMirror") {
  do {
    let value = SingletonNonGenericEnumWithDefaultMirror.OnlyOne(5)
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ Mirror.SingletonNonGenericEnumWithDefaultMirror.OnlyOne\n" +
      "  - OnlyOne: 5\n"

    expectEqual(expected, output)
  }
}

enum ZeroSizedEnumWithDefaultMirror {
  case π
}

enum SingletonZeroSizedEnumWithDefaultMirror {
  case wrap(ZeroSizedEnumWithDefaultMirror)
}

mirrors.test("Enum/SingletonZeroSizedEnumWithDefaultMirror/DefaultMirror") {
  do {
    let value = SingletonZeroSizedEnumWithDefaultMirror.wrap(.π)
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ Mirror.SingletonZeroSizedEnumWithDefaultMirror.wrap\n" +
      "  - wrap: Mirror.ZeroSizedEnumWithDefaultMirror.π\n"

    expectEqual(expected, output)
  }
}

enum SingletonGenericEnumWithDefaultMirror<T> {
  case OnlyOne(T)
}

mirrors.test("Enum/SingletonGeneric/DefaultMirror") {
  do {
    let value = SingletonGenericEnumWithDefaultMirror.OnlyOne("IIfx")
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ Mirror.SingletonGenericEnumWithDefaultMirror<Swift.String>.OnlyOne\n" +
      "  - OnlyOne: \"IIfx\"\n"

    expectEqual(expected, output)
  }
  expectEqual(0, LifetimeTracked.instances)
  do {
    let value = SingletonGenericEnumWithDefaultMirror.OnlyOne(
        LifetimeTracked(0))
    expectEqual(1, LifetimeTracked.instances)
    var output = ""
    dump(value, to: &output)
  }
  expectEqual(0, LifetimeTracked.instances)
}

enum SinglePayloadNonGenericEnumWithDefaultMirror {
  case Cat
  case Dog
  case Volleyball(String, Int)
}

mirrors.test("Enum/SinglePayloadNonGeneric/DefaultMirror") {
  do {
    let value: [SinglePayloadNonGenericEnumWithDefaultMirror] =
        [.Cat,
         .Dog,
         .Volleyball("Wilson", 2000)]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 3 elements\n" +
      "  - Mirror.SinglePayloadNonGenericEnumWithDefaultMirror.Cat\n" +
      "  - Mirror.SinglePayloadNonGenericEnumWithDefaultMirror.Dog\n" +
      "  ▿ Mirror.SinglePayloadNonGenericEnumWithDefaultMirror.Volleyball\n" +
      "    ▿ Volleyball: (2 elements)\n" +
      "      - .0: \"Wilson\"\n" +
      "      - .1: 2000\n"

    expectEqual(expected, output)
  }
}

enum SinglePayloadGenericEnumWithDefaultMirror<T, U> {
  case Well
  case Faucet
  case Pipe(T, U)
}

mirrors.test("Enum/SinglePayloadGeneric/DefaultMirror") {
  do {
    let value: [SinglePayloadGenericEnumWithDefaultMirror<Int, [Int]>] =
        [.Well,
         .Faucet,
         .Pipe(408, [415])]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 3 elements\n" +
      "  - Mirror.SinglePayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.Array<Swift.Int>>.Well\n" +
      "  - Mirror.SinglePayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.Array<Swift.Int>>.Faucet\n" +
      "  ▿ Mirror.SinglePayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.Array<Swift.Int>>.Pipe\n" +
      "    ▿ Pipe: (2 elements)\n" +
      "      - .0: 408\n" +
      "      ▿ .1: 1 element\n" +
      "        - 415\n"

    expectEqual(expected, output)
  }
}

enum MultiPayloadTagBitsNonGenericEnumWithDefaultMirror {
  case Plus
  case SE30
  case Classic(mhz: Int)
  case Performa(model: Int)
}

mirrors.test("Enum/MultiPayloadTagBitsNonGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadTagBitsNonGenericEnumWithDefaultMirror] =
        [.Plus,
         .SE30,
         .Classic(mhz: 16),
         .Performa(model: 220)]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 4 elements\n" +
      "  - Mirror.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.Plus\n" +
      "  - Mirror.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.SE30\n" +
      "  ▿ Mirror.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.Classic\n" +
      "    ▿ Classic: (1 element)\n" +
      "      - mhz: 16\n" +
      "  ▿ Mirror.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.Performa\n" +
      "    ▿ Performa: (1 element)\n" +
      "      - model: 220\n"

    expectEqual(expected, output)
  }
}

class Floppy {
  let capacity: Int

  init(capacity: Int) { self.capacity = capacity }
}

class CDROM {
  let capacity: Int

  init(capacity: Int) { self.capacity = capacity }
}

enum MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror {
  case MacWrite
  case MacPaint
  case FileMaker
  case ClarisWorks(floppy: Floppy)
  case HyperCard(cdrom: CDROM)
}

mirrors.test("Enum/MultiPayloadSpareBitsNonGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror] =
        [.MacWrite,
         .MacPaint,
         .FileMaker,
         .ClarisWorks(floppy: Floppy(capacity: 800)),
         .HyperCard(cdrom: CDROM(capacity: 600))]

    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 5 elements\n" +
      "  - Mirror.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.MacWrite\n" +
      "  - Mirror.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.MacPaint\n" +
      "  - Mirror.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.FileMaker\n" +
      "  ▿ Mirror.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.ClarisWorks\n" +
      "    ▿ ClarisWorks: (1 element)\n" +
      "      ▿ floppy: Mirror.Floppy #0\n" +
      "        - capacity: 800\n" +
      "  ▿ Mirror.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.HyperCard\n" +
      "    ▿ HyperCard: (1 element)\n" +
      "      ▿ cdrom: Mirror.CDROM #1\n" +
      "        - capacity: 600\n"

    expectEqual(expected, output)
  }
}

enum MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror {
  case MacWrite
  case MacPaint
  case FileMaker
  case ClarisWorks(floppy: Bool)
  case HyperCard(cdrom: Bool)
}

mirrors.test("Enum/MultiPayloadTagBitsSmallNonGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror] =
        [.MacWrite,
         .MacPaint,
         .FileMaker,
         .ClarisWorks(floppy: true),
         .HyperCard(cdrom: false)]

    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 5 elements\n" +
      "  - Mirror.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.MacWrite\n" +
      "  - Mirror.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.MacPaint\n" +
      "  - Mirror.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.FileMaker\n" +
      "  ▿ Mirror.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.ClarisWorks\n" +
      "    ▿ ClarisWorks: (1 element)\n" +
      "      - floppy: true\n" +
      "  ▿ Mirror.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.HyperCard\n" +
      "    ▿ HyperCard: (1 element)\n" +
      "      - cdrom: false\n"

    expectEqual(expected, output)
  }
}

enum MultiPayloadGenericEnumWithDefaultMirror<T, U> {
  case IIe
  case IIgs
  case Centris(ram: T)
  case Quadra(hdd: U)
  case PowerBook170
  case PowerBookDuo220
}

mirrors.test("Enum/MultiPayloadGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadGenericEnumWithDefaultMirror<Int, String>] =
        [.IIe,
         .IIgs,
         .Centris(ram: 4096),
         .Quadra(hdd: "160MB"),
         .PowerBook170,
         .PowerBookDuo220]

    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 6 elements\n" +
      "  - Mirror.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.IIe\n" +
      "  - Mirror.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.IIgs\n" +
      "  ▿ Mirror.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.Centris\n" +
      "    ▿ Centris: (1 element)\n" +
      "      - ram: 4096\n" +
      "  ▿ Mirror.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.Quadra\n" +
      "    ▿ Quadra: (1 element)\n" +
      "      - hdd: \"160MB\"\n" +
      "  - Mirror.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.PowerBook170\n" +
      "  - Mirror.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.PowerBookDuo220\n"

    expectEqual(expected, output)
  }
  expectEqual(0, LifetimeTracked.instances)
  do {
    let value = MultiPayloadGenericEnumWithDefaultMirror<LifetimeTracked,
                                                         LifetimeTracked>
        .Quadra(hdd: LifetimeTracked(0))
    expectEqual(1, LifetimeTracked.instances)
    var output = ""
    dump(value, to: &output)
  }
  expectEqual(0, LifetimeTracked.instances)
}

enum Foo<T> {
  indirect case Foo(Int)
  case Bar(T)
}

enum List<T> {
  case Nil
  indirect case Cons(first: T, rest: List<T>)
}

mirrors.test("Enum/IndirectGeneric/DefaultMirror") {
  let x = Foo<String>.Foo(22)
  let y = Foo<String>.Bar("twenty-two")

  expectEqual("\(x)", "Foo(22)")
  expectEqual("\(y)", "Bar(\"twenty-two\")")

  let list = List.Cons(first: 0, rest: .Cons(first: 1, rest: .Nil))
  expectEqual("Cons(first: 0, rest: Mirror.List<Swift.Int>.Cons(first: 1, rest: Mirror.List<Swift.Int>.Nil))",
    "\(list)")
}

enum MyError: Error {
  case myFirstError(LifetimeTracked)
}

mirrors.test("Enum/CaseName/Error") {
  // Just make sure this doesn't leak.
  let e: Error = MyError.myFirstError(LifetimeTracked(0))
  _ = String(describing: e)
}

class Brilliant : CustomReflectable {
  let first: Int
  let second: String

  init(_ fst: Int, _ snd: String) {
    self.first = fst
    self.second = snd
  }

  var customMirror: Mirror {
    return Mirror(self, children: ["first": first, "second": second, "self": self])
  }
}

//===--- Custom mirrors ---------------------------------------------------===//
//===----------------------------------------------------------------------===//

/// Subclasses inherit their parents' custom mirrors.
class Irradiant : Brilliant {
  init() {
    super.init(400, "")
  }
}

mirrors.test("CustomMirror") {
  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output)

    let expected =
      "▿ Mirror.Brilliant #0\n" +
      "  - first: 123\n" +
      "  - second: \"four five six\"\n" +
      "  ▿ self: Mirror.Brilliant #0\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxDepth: 0)
    expectEqual("▹ Mirror.Brilliant #0\n", output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxItems: 3)

    let expected =
      "▿ Mirror.Brilliant #0\n" +
      "  - first: 123\n" +
      "  - second: \"four five six\"\n" +
      "    (1 more child)\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxItems: 2)

    let expected =
      "▿ Mirror.Brilliant #0\n" +
      "  - first: 123\n" +
      "    (2 more children)\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxItems: 1)

    let expected =
      "▿ Mirror.Brilliant #0\n" +
      "    (3 children)\n"

    expectEqual(expected, output)
  }
}

mirrors.test("CustomMirrorIsInherited") {
  do {
    var output = ""
    dump(Irradiant(), to: &output)

    let expected =
      "▿ Mirror.Brilliant #0\n" +
      "  - first: 400\n" +
      "  - second: \"\"\n" +
      "  ▿ self: Mirror.Brilliant #0\n"

    expectEqual(expected, output)
  }
}

//===--- Metatypes --------------------------------------------------------===//
//===----------------------------------------------------------------------===//

protocol SomeNativeProto {}
protocol SomeOtherNativeProto {}
extension Int: SomeNativeProto {}

class SomeClass {}

mirrors.test("MetatypeMirror") {
  do {
    var output = ""
    let concreteMetatype = Int.self
    dump(concreteMetatype, to: &output)

    let expectedInt = "- Swift.Int #0\n"
    expectEqual(expectedInt, output)

    let anyMetatype: Any.Type = Int.self
    output = ""
    dump(anyMetatype, to: &output)
    expectEqual(expectedInt, output)

    let nativeProtocolMetatype: SomeNativeProto.Type = Int.self
    output = ""
    dump(nativeProtocolMetatype, to: &output)
    expectEqual(expectedInt, output)

    let concreteClassMetatype = SomeClass.self
    let expectedSomeClass = "- Mirror.SomeClass #0\n"
    output = ""
    dump(concreteClassMetatype, to: &output)
    expectEqual(expectedSomeClass, output)

    let nativeProtocolConcreteMetatype = SomeNativeProto.self
    let expectedNativeProtocolConcrete = "- Mirror.SomeNativeProto #0\n"
    output = ""
    dump(nativeProtocolConcreteMetatype, to: &output)
    expectEqual(expectedNativeProtocolConcrete, output)

    let nativeProtocolCompositionMetatype =
        (SomeNativeProto & SomeOtherNativeProto).self
    output = ""
    dump(nativeProtocolCompositionMetatype, to: &output)
    expectEqual(
      "- Mirror.SomeNativeProto & Mirror.SomeOtherNativeProto #0\n",
      output)
  }
}

//===--- Tuples -----------------------------------------------------------===//
//===----------------------------------------------------------------------===//

mirrors.test("TupleMirror") {
  do {
    var output = ""
    let tuple =
      (Brilliant(384, "seven six eight"), StructWithDefaultMirror("nine"))
    dump(tuple, to: &output)

    let expected =
      "▿ (2 elements)\n" +
      "  ▿ .0: Mirror.Brilliant #0\n" +
      "    - first: 384\n" +
      "    - second: \"seven six eight\"\n" +
      "    ▿ self: Mirror.Brilliant #0\n" +
      "  ▿ .1: Mirror.StructWithDefaultMirror\n" +
      "    - s: \"nine\"\n"

    expectEqual(expected, output)

    expectEqual(.tuple, Mirror(reflecting: tuple).displayStyle)
  }

  do {
    // A tuple of stdlib types with mirrors.
    var output = ""
    let tuple = (1, 2.5, false, "three")
    dump(tuple, to: &output)

    let expected =
      "▿ (4 elements)\n" +
      "  - .0: 1\n" +
      "  - .1: 2.5\n" +
      "  - .2: false\n" +
      "  - .3: \"three\"\n"

    expectEqual(expected, output)
  }

  do {
    // A nested tuple.
    var output = ""
    let tuple = (1, ("Hello", "World"))
    dump(tuple, to: &output)

    let expected =
      "▿ (2 elements)\n" +
      "  - .0: 1\n" +
      "  ▿ .1: (2 elements)\n" +
      "    - .0: \"Hello\"\n" +
      "    - .1: \"World\"\n"

    expectEqual(expected, output)
  }
}

//===--- Standard library types -------------------------------------------===//
//===----------------------------------------------------------------------===//

mirrors.test("String/Mirror") {
  do {
    var output = ""
    dump("", to: &output)

    let expected =
      "- \"\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+0061 LATIN SMALL LETTER A
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    // U+1F425 FRONT-FACING BABY CHICK
    var output = ""
    dump("\u{61}\u{304b}\u{3099}\u{1f425}", to: &output)

    let expected =
      "- \"\u{61}\u{304b}\u{3099}\u{1f425}\"\n"

    expectEqual(expected, output)
  }
}

mirrors.test("String.UTF8View/Mirror") {
  // U+0061 LATIN SMALL LETTER A
  // U+304B HIRAGANA LETTER KA
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  var output = ""
  dump("\u{61}\u{304b}\u{3099}".utf8, to: &output)

  let expected =
    "▿ UTF8View(\"\u{61}\u{304b}\u{3099}\")\n" +
    "  - 97\n" +
    "  - 227\n" +
    "  - 129\n" +
    "  - 139\n" +
    "  - 227\n" +
    "  - 130\n" +
    "  - 153\n"

  expectEqual(expected, output)
}

mirrors.test("String.UTF16View/Mirror") {
  // U+0061 LATIN SMALL LETTER A
  // U+304B HIRAGANA LETTER KA
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  // U+1F425 FRONT-FACING BABY CHICK
  var output = ""
  dump("\u{61}\u{304b}\u{3099}\u{1f425}".utf16, to: &output)

  let expected =
    "▿ StringUTF16(\"\u{61}\u{304b}\u{3099}\u{1f425}\")\n" +
    "  - 97\n" +
    "  - 12363\n" +
    "  - 12441\n" +
    "  - 55357\n" +
    "  - 56357\n"

  expectEqual(expected, output)
}

mirrors.test("String.UnicodeScalarView/Mirror") {
  // U+0061 LATIN SMALL LETTER A
  // U+304B HIRAGANA LETTER KA
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  // U+1F425 FRONT-FACING BABY CHICK
  var output = ""
  dump("\u{61}\u{304b}\u{3099}\u{1f425}".unicodeScalars, to: &output)

  let expected =
    "▿ StringUnicodeScalarView(\"\u{61}\u{304b}\u{3099}\u{1f425}\")\n" +
    "  - \"\u{61}\"\n" +
    "  - \"\\u{304B}\"\n" +
    "  - \"\\u{3099}\"\n" +
    "  - \"\\u{0001F425}\"\n"

  expectEqual(expected, output)
}

mirrors.test("Character/Mirror") {
  do {
    // U+0061 LATIN SMALL LETTER A
    let input: Character = "\u{61}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{61}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    let input: Character = "\u{304b}\u{3099}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{304b}\u{3099}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+1F425 FRONT-FACING BABY CHICK
    let input: Character = "\u{1f425}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{1f425}\"\n"

    expectEqual(expected, output)
  }
}

mirrors.test("UnicodeScalar") {
  do {
    // U+0061 LATIN SMALL LETTER A
    let input: UnicodeScalar = "\u{61}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{61}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+304B HIRAGANA LETTER KA
    let input: UnicodeScalar = "\u{304b}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\\u{304B}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    let input: UnicodeScalar = "\u{3099}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\\u{3099}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+1F425 FRONT-FACING BABY CHICK
    let input: UnicodeScalar = "\u{1f425}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\\u{0001F425}\"\n"

    expectEqual(expected, output)
  }
}

mirrors.test("Bool") {
  do {
    var output = ""
    dump(false, to: &output)

    let expected =
      "- false\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(true, to: &output)

    let expected =
      "- true\n"

    expectEqual(expected, output)
  }
}

// FIXME: these tests should cover Float80.
// FIXME: these tests should be automatically generated from the list of
// available floating point types.
mirrors.test("Float") {
  do {
    var output = ""
    dump(Float.nan, to: &output)

    let expected =
      "- nan\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Float.infinity, to: &output)

    let expected =
      "- inf\n"

    expectEqual(expected, output)
  }

  do {
    let input: Float = 42.125
    var output = ""
    dump(input, to: &output)

    let expected =
      "- 42.125\n"

    expectEqual(expected, output)
  }
}

mirrors.test("Double") {
  do {
    var output = ""
    dump(Double.nan, to: &output)

    let expected =
      "- nan\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Double.infinity, to: &output)

    let expected =
      "- inf\n"

    expectEqual(expected, output)
  }

  do {
    let input: Double = 42.125
    var output = ""
    dump(input, to: &output)

    let expected =
      "- 42.125\n"

    expectEqual(expected, output)
  }
}

mirrors.test("StaticString/Mirror") {
  do {
    var output = ""
    dump("" as StaticString, to: &output)

    let expected =
      "- \"\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+0061 LATIN SMALL LETTER A
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    // U+1F425 FRONT-FACING BABY CHICK
    var output = ""
    dump("\u{61}\u{304b}\u{3099}\u{1f425}" as StaticString, to: &output)

    let expected =
      "- \"\u{61}\u{304b}\u{3099}\u{1f425}\"\n"

    expectEqual(expected, output)
  }
}

mirrors.test("DictionaryIterator/Mirror") {
  let d: [MinimalHashableValue : OpaqueValue<Int>] =
    [ MinimalHashableValue(0) : OpaqueValue(0) ]

  var output = ""
  dump(d.makeIterator(), to: &output)

  let expected =
    "- Swift.Dictionary<StdlibUnittest.MinimalHashableValue, StdlibUnittest.OpaqueValue<Swift.Int>>.Iterator\n"

  expectEqual(expected, output)
}

mirrors.test("SetIterator/Mirror") {
  let s: Set<MinimalHashableValue> = [ MinimalHashableValue(0)]

  var output = ""
  dump(s.makeIterator(), to: &output)

  let expected =
    "- Swift.Set<StdlibUnittest.MinimalHashableValue>.Iterator\n"

  expectEqual(expected, output)
}

//===--- Regressions ------------------------------------------------------===//
//===----------------------------------------------------------------------===//

// A struct type and class type whose NominalTypeDescriptor.FieldNames
// data is exactly eight bytes long. FieldNames data of exactly
// 4 or 8 or 16 bytes was once miscompiled on arm64.
struct EightByteFieldNamesStruct {
  let abcdef = 42
}
class EightByteFieldNamesClass {
  let abcdef = 42
}

mirrors.test("FieldNamesBug") {
  do {
    let expected =
      "▿ Mirror.EightByteFieldNamesStruct\n" +
      "  - abcdef: 42\n"
    var output = ""
    dump(EightByteFieldNamesStruct(), to: &output)
    expectEqual(expected, output)
  }

  do {
    let expected =
      "▿ Mirror.EightByteFieldNamesClass #0\n" +
      "  - abcdef: 42\n"
    var output = ""
    dump(EightByteFieldNamesClass(), to: &output)
    expectEqual(expected, output)
  }
}

mirrors.test("MirrorMirror") {
  let object = 1
  let mirror = Mirror(reflecting: object)
  let mirrorMirror = Mirror(reflecting: mirror)

  expectEqual(0, mirrorMirror.children.count)
}

mirrors.test("OpaquePointer/null") {
  // Don't crash on null pointers. rdar://problem/19708338
  let pointer: OpaquePointer? = nil
  let mirror = Mirror(reflecting: pointer as Any)
  expectEqual(0, mirror.children.count)
}

struct a<b>  {
    enum c{}
}
class d  {}
struct e<f> {
    var constraints: [Int: a<f>.c] = [:]
}

mirrors.test("GenericNestedTypeField") {
  let x = e<d>()
  
  expectTrue(type(of: Mirror(reflecting: x).children.first!.value)
              == [Int: a<d>.c].self)
}

extension OtherOuter {
  struct Inner {}
}

extension OtherOuterGeneric {
  struct Inner<U> {}
}

mirrors.test("SymbolicReferenceInsideType") {
  let s = OtherStruct(a: OtherOuter.Inner(),
                      b: OtherOuterGeneric<Int>.Inner<String>())

  var output = ""
  dump(s, to: &output)

  let expected =
    "▿ Mirror.OtherStruct\n" +
    "  - a: Mirror.OtherOuter.Inner\n" +
    "  - b: Mirror.OtherOuterGeneric<Swift.Int>.Inner<Swift.String>\n"

  expectEqual(expected, output)
}

protocol P1 { }
protocol P2 { }
protocol P3 { }

struct ConformsToP1: P1 { }
struct ConformsToP2: P2 { }
struct ConformsToP3: P3 { }

struct OuterTwoParams<T: P1, U: P2> {}

struct ConformsToP1AndP2 : P1, P2 { }

extension OuterTwoParams where U == T {
  struct InnerEqualParams<V: P3> {
    var x: T
    var y: U
    var z: V
  }
}

mirrors.test("GenericNestedWithSameTypeConstraints") {
  let value = OuterTwoParams.InnerEqualParams(x: ConformsToP1AndP2(),
                                              y: ConformsToP1AndP2(),
                                              z: ConformsToP3())
  var output = ""
  dump(value, to: &output)

  let expected =
    "▿ (extension in Mirror):Mirror.OuterTwoParams<Mirror.ConformsToP1AndP2, Mirror.ConformsToP1AndP2>.InnerEqualParams<Mirror.ConformsToP3>\n" +
    "  - x: Mirror.ConformsToP1AndP2\n" +
    "  - y: Mirror.ConformsToP1AndP2\n" +
    "  - z: Mirror.ConformsToP3\n"

  expectEqual(expected, output)
}

@_alignment(16) struct CustomAlignment {
  var x: Int
  var y: Int
}

mirrors.test("CustomAlignment") {
  let value = CustomAlignment(x: 123, y: 321)
  var output = ""
  dump(value, to: &output)

  let expected =
    "▿ Mirror.CustomAlignment\n" +
    "  - x: 123\n" +
    "  - y: 321\n"

  expectEqual(expected, output)
}

protocol STSTagProtocol {}
struct STSOuter : STSTagProtocol {}

enum STSContainer<T : STSTagProtocol> {
  class Superclass {}
  class Subclass<U>: Superclass where T == STSOuter {
    class ExtraNested: Superclass {}
  }

  class GenericSuperclass<U> {}
  class Subclass2<U>: GenericSuperclass<U> where T == STSOuter {}

  class Subclass3<U: Collection>: Superclass where T == U.Element {}

  class MoreNesting<X> {
    class Subclass<U>: Superclass where T == STSOuter {}
  }

  struct Fields<U> where T == STSOuter {
    var x: T?
    var y: U?
  }

  enum Cases<U> where T == STSOuter {
    case a(T)
    case b(U)
  }
}

// A new type with an easily-recognizable, easily-strippable suffix character.
enum STSContainer℠<T : STSTagProtocol> {
  class Superclass {}
  class GenericSuperclass<U> {}
}
extension STSContainer℠ where T == STSOuter {
  class Subclass<U>: Superclass {
    class ExtraNested: Superclass {}
  }

  class Subclass2<U>: GenericSuperclass<U> {}

  class MoreNesting<X> {
    class Subclass<U>: Superclass {}
  }

  struct Fields<U> {
    var x: T?
    var y: U?
  }

  enum Cases<U> {
    case a(T)
    case b(U)
  }
}

var sameTypeSuite = TestSuite("Mirrors.SameTypeConstraints")

func testSTSDump<RegularValue, ExtensionValue>(
  _ regular: RegularValue,
  _ ext: ExtensionValue,
  _ expected: String
) {
  var output = ""
  dump(regular, to: &output)
  expectEqual(expected, output)

  func clean(_ dumpOutput: String) -> String {
    // This isn't efficient but it doesn't have to be.
    var result = dumpOutput
    result.removeAll { $0 == "℠" }
    if let openParenIndex = result.firstIndex(of: "(") {
      if result[openParenIndex...].hasPrefix("(extension in Mirror):") {
        let colonIndex = result[openParenIndex...].firstIndex(of: ":")!
        result.removeSubrange(openParenIndex...colonIndex)
      }
    }
    return result
  }

  var extensionOutput = ""
  dump(ext, to: &extensionOutput)
  expectEqual(expected, clean(extensionOutput))
}

func testSTSDump<RegularValue>(_ regular: RegularValue, _ expected: String) {
  var output = ""
  dump(regular, to: &output)
  expectEqual(expected, output)
}


if #available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *) {
  sameTypeSuite.test("Subclass") {
    testSTSDump(STSContainer.Subclass<Int>(),
                STSContainer℠.Subclass<Int>(),
                """
      - Mirror.STSContainer<Mirror.STSOuter>.Subclass<Swift.Int> #0
        - super: Mirror.STSContainer<Mirror.STSOuter>.Superclass\n
      """)

    testSTSDump(STSContainer.Subclass2<Int>(),
                STSContainer℠.Subclass2<Int>(),
                """
      - Mirror.STSContainer<Mirror.STSOuter>.Subclass2<Swift.Int> #0
        - super: Mirror.STSContainer<Mirror.STSOuter>.GenericSuperclass<Swift.Int>\n
      """)

    testSTSDump(STSContainer.Subclass3<[STSOuter]>(),
                """
      - Mirror.STSContainer<Mirror.STSOuter>.Subclass3<Swift.Array<Mirror.STSOuter>> #0
        - super: Mirror.STSContainer<Mirror.STSOuter>.Superclass\n
      """)

    testSTSDump(STSContainer.Subclass<Int>.ExtraNested(),
                STSContainer℠.Subclass<Int>.ExtraNested(),
                """
      - Mirror.STSContainer<Mirror.STSOuter>.Subclass<Swift.Int>.ExtraNested #0
        - super: Mirror.STSContainer<Mirror.STSOuter>.Superclass\n
      """)

    testSTSDump(STSContainer.MoreNesting<Bool>.Subclass<Int>(),
                STSContainer℠.MoreNesting<Bool>.Subclass<Int>(),
                """
      - Mirror.STSContainer<Mirror.STSOuter>.MoreNesting<Swift.Bool>.Subclass<Swift.Int> #0
        - super: Mirror.STSContainer<Mirror.STSOuter>.Superclass\n
      """)
  }

  sameTypeSuite.test("Fields") {
    testSTSDump(STSContainer.Fields<Int>(),
                STSContainer℠.Fields<Int>(),
                """
      ▿ Mirror.STSContainer<Mirror.STSOuter>.Fields<Swift.Int>
        - x: nil
        - y: nil\n
      """)
  }

  sameTypeSuite.test("Cases") {
    testSTSDump(STSContainer.Cases<Int>.a(.init()),
                STSContainer℠.Cases<Int>.a(.init()),
                """
      ▿ Mirror.STSContainer<Mirror.STSOuter>.Cases<Swift.Int>.a
        - a: Mirror.STSOuter\n
      """)

    testSTSDump(STSContainer.Cases<Int>.b(.init()),
                STSContainer℠.Cases<Int>.b(.init()),
                """
      ▿ Mirror.STSContainer<Mirror.STSOuter>.Cases<Swift.Int>.b
        - b: 0\n
      """)
  }
}

runAllTests()
