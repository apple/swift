//===--- ManagedBuffer.swift ----------------------------------------------===//
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
// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// XFAIL: OS=openbsd

import StdlibUnittest


#if _runtime(_ObjC)
import Foundation
#endif

// Check that the generic parameters are called 'Header' and 'Element'.
protocol TestProtocol1 {}

extension ManagedBuffer
  where Header : TestProtocol1, Element : TestProtocol1 {

  var _valueAndElementAreTestProtocol1_: Bool {
    fatalError("not implemented")
  }
}

extension ManagedBufferPointer
  where Header : TestProtocol1, Element : TestProtocol1 {

  var _valueAndElementAreTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

struct CountAndCapacity {
  var count: LifetimeTracked
  let capacity: Int
}

// An example of ManagedBuffer, very similar to what Array will use.
// However, only half of the element storage is actually used to store
// elements, interleaved with garbage, as a simple way of catching
// potential bugs.
final class TestManagedBuffer<T> : ManagedBuffer<CountAndCapacity, T> {
  class func create(_ capacity: Int) -> TestManagedBuffer {
    let r = super.create(minimumCapacity: capacity) {
      CountAndCapacity(
        count: LifetimeTracked(0), capacity: $0.capacity)
    }
    return r as! TestManagedBuffer
  }

  var count: Int {
    get {
      return header.count.value
    }
    set {
      header.count = LifetimeTracked(newValue)
    }
  }
  
  var myCapacity: Int {
    return header.capacity
  }
  
  deinit {
    teardown()
  }

  // This doesn't seem to compile properly when embedded directly in
  // deinit.
  func teardown() {
    let count = self.count
    
    withUnsafeMutablePointerToElements {
      (x: UnsafeMutablePointer<T>) -> () in
      for i in stride(from: 0, to: count, by: 2) {
        (x + i).deinitialize(count: 1)
      }
    }
  }
  
  func append(_ x: T) {
    let count = self.count
    precondition(count + 2 <= myCapacity)
    
    withUnsafeMutablePointerToElements {
      (p: UnsafeMutablePointer<T>) -> () in
      (p + count).initialize(to: x)
    }
    self.count = count + 2
  }
}

class MyBuffer<T> {
  typealias Manager = ManagedBufferPointer<CountAndCapacity, T>
  deinit {
    Manager(unsafeBufferObject: self).withUnsafeMutablePointers {
      (pointerToHeader, pointerToElements) -> Void in
      pointerToElements.deinitialize(count: self.count)
      pointerToHeader.deinitialize(count: 1)
    }
  }

  var count: Int {
    return Manager(unsafeBufferObject: self).header.count.value
  }
  var capacity: Int {
    return Manager(unsafeBufferObject: self).header.capacity
  }
}

var tests = TestSuite("ManagedBuffer")

tests.test("basic") {
  do {
    let s = TestManagedBuffer<LifetimeTracked>.create(0)
    withExtendedLifetime(s) {
      expectEqual(1, LifetimeTracked.instances)
    }
  }
  
  expectEqual(0, LifetimeTracked.instances)
  do {
    let s = TestManagedBuffer<LifetimeTracked>.create(10)
    expectEqual(0, s.count)
    expectLE(10, s.myCapacity)
    expectGE(13, s.myCapacity)  // allow some over-allocation but not too much
    
    expectEqual(1, LifetimeTracked.instances)
    for i in 1..<6 {
      s.append(LifetimeTracked(i))
      expectEqual(i + 1, LifetimeTracked.instances)
      expectEqual(i * 2, s.count)
      expectEqual(
        s.count,
        s.withUnsafeMutablePointerToHeader { $0.pointee.count.value }
      )
      expectEqual(
        s.capacity,
        s.withUnsafeMutablePointerToHeader { $0.pointee.capacity }
      )
      expectEqual(
        LifetimeTracked(i),
        s.withUnsafeMutablePointerToElements { $0[(i - 1) * 2] }
      )
    }
  }
}

tests.test("ManagedBufferPointer/SizeValidation/TestmanagedBuffer") {
  let x = ManagedBufferPointer<CountAndCapacity, LifetimeTracked>(
    bufferClass: TestManagedBuffer<LifetimeTracked>.self,
    minimumCapacity: 10
  ) {
    buffer, getRealCapacity in 
    CountAndCapacity(
      count: LifetimeTracked(0), capacity: getRealCapacity(buffer))
  }
}

tests.test("ManagedBufferPointer/SizeValidation/MyBuffer") {
  let x = ManagedBufferPointer<CountAndCapacity, LifetimeTracked>(
    bufferClass:  MyBuffer<LifetimeTracked>.self,
    minimumCapacity: 0
  ) { _, _ in CountAndCapacity(count: LifetimeTracked(0), capacity: 99) }
}

tests.test("ManagedBufferPointer") {
  typealias Manager = ManagedBufferPointer<CountAndCapacity, LifetimeTracked>

  do {
    var mgr = Manager(
      bufferClass: TestManagedBuffer<LifetimeTracked>.self,
      minimumCapacity: 10
    ) {
      buffer, getRealCapacity in 
      CountAndCapacity(
        count: LifetimeTracked(0), capacity: getRealCapacity(buffer))
    }
    expectTrue(mgr.isUniqueReference())

    let buf = mgr.buffer as? TestManagedBuffer<LifetimeTracked>
    expectTrue(buf != nil)
    expectFalse(mgr.isUniqueReference())
    
    let s = buf!
    expectEqual(0, s.count)
    expectLE(10, s.capacity)
    
    expectEqual(s.count, mgr.header.count.value)
    expectEqual(s.capacity, mgr.header.capacity)

    expectEqual(
      mgr.withUnsafeMutablePointerToHeader { $0 },
      s.withUnsafeMutablePointerToHeader { $0 })
    
    expectEqual(
      mgr.withUnsafeMutablePointerToElements { $0 },
      s.withUnsafeMutablePointerToElements { $0 })
    
    for i in 1..<6 {
      s.append(LifetimeTracked(i))
      expectEqual(i * 2, s.count)
      expectEqual(s.count, mgr.header.count.value)
    }
    
    mgr = Manager(
      bufferClass:  MyBuffer<LifetimeTracked>.self,
      minimumCapacity: 0
    ) { _, _ in CountAndCapacity(count: LifetimeTracked(0), capacity: 99) }

    expectTrue(mgr.isUniqueReference())
    expectEqual(mgr.header.count.value, 0)
    expectEqual(mgr.header.capacity, 99)

    let s2 = mgr.buffer as! MyBuffer<LifetimeTracked>
    withExtendedLifetime(s2) {
      expectFalse(mgr.isUniqueReference())
    }
    let val = mgr.withUnsafeMutablePointerToHeader { $0 }.pointee
    expectEqual(val.count.value, 0)
    expectEqual(val.capacity, 99)
  }
}

tests.test("isKnownUniquelyReferenced") {
  var s = TestManagedBuffer<LifetimeTracked>.create(0)
  expectTrue(isKnownUniquelyReferenced(&s))
  var s2 = s
  expectFalse(isKnownUniquelyReferenced(&s))
  expectFalse(isKnownUniquelyReferenced(&s2))
#if _runtime(_ObjC)
  var s3 = NSArray()
  expectFalse(isKnownUniquelyReferenced(&s3))
#endif
  _fixLifetime(s)
  _fixLifetime(s2)
}

runAllTests()
