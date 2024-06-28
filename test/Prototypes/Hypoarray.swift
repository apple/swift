//===--- Algorithms.swift -------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -swift-version 6 -o %t/a.out %s
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out
// REQUIRES: executable_test

import StdlibUnittest

struct Hypoarray<Element: ~Copyable>: ~Copyable {
  private var _storage: UnsafeMutableBufferPointer<Element>
  private var _count: Int

  var capacity: Int { _storage.count }

  init() {
    _storage = .init(start: nil, count: 0)
    _count = 0
  }

  init(_ element: consuming Element) {
    _storage = .allocate(capacity: 1)
    _storage.initializeElement(at: 0, to: element)
    _count = 1
  }

  init(count: Int, initializedBy generator: (Int) -> Element) {
    _storage = .allocate(capacity: count)
    for i in 0 ..< count {
      _storage.initializeElement(at: i, to: generator(i))
    }
    _count = count
  }

  deinit {
    _storage.extracting(0 ..< count).deinitialize()
    _storage.deallocate()
  }
}

extension Hypoarray: @unchecked Sendable where Element: Sendable & ~Copyable {}

extension Hypoarray where Element: ~Copyable {
  typealias Index = Int

  var isEmpty: Bool { _count == 0 }
  var count: Int { _count }

  var startIndex: Int { 0 }
  var endIndex: Int { _count }
  func index(after i: Int) -> Int { i + 1 }
  func index(before i: Int) -> Int { i - 1 }
  func distance(from start: Int, to end: Int) -> Int { end - start }
  // etc.
}

extension Hypoarray where Element: ~Copyable {
  func borrowElement<E: Error, R: ~Copyable> (
    at index: Int,
    by body: (borrowing Element) throws(E) -> R
  ) throws(E) -> R {
    precondition(index >= 0 && index < _count)
    return try body(_storage[index])
  }

  mutating func updateElement<E: Error, R: ~Copyable> (
    at index: Int,
    by body: (inout Element) throws(E) -> R
  ) throws(E) -> R {
    precondition(index >= 0 && index < _count)
    return try body(&_storage[index])
  }
}

extension Hypoarray where Element: ~Copyable {
  subscript(position: Int) -> Element {
    _read {
      precondition(position >= 0 && position < _count)
      yield _storage[position]
    }
    _modify {
      precondition(position >= 0 && position < _count)
      yield &_storage[position]
    }
  }
}

extension Hypoarray where Element: ~Copyable {
  @discardableResult
  mutating func remove(at index: Int) -> Element {
    precondition(index >= 0 && index < count)
    let old = _storage.moveElement(from: index)
    let source = _storage.extracting(index + 1 ..< count)
    let target = _storage.extracting(index ..< count - 1)
    let i = target.moveInitialize(fromContentsOf: source)
    assert(i == target.endIndex)
    _count -= 1
    return old
  }
}

extension Hypoarray where Element: ~Copyable {
  mutating func reserveCapacity(_ n: Int) {
    guard capacity < n else { return }
    let newStorage: UnsafeMutableBufferPointer<Element> = .allocate(capacity: n)
    let source = _storage.extracting(0 ..< count)
    let i = newStorage.moveInitialize(fromContentsOf: source)
    assert(i == count)
    _storage.deallocate()
    _storage = newStorage
  }
}

extension Hypoarray where Element: ~Copyable {
  mutating func _ensureFreeCapacity(_ minimumCapacity: Int) {
    guard capacity < _count + minimumCapacity else { return }
    reserveCapacity(max(_count + minimumCapacity, 2 * capacity))
  }
}

extension Hypoarray where Element: ~Copyable {
  mutating func append(_ item: consuming Element) {
    _ensureFreeCapacity(1)
    _storage.initializeElement(at: _count, to: item)
    _count += 1
  }
}

extension Hypoarray where Element: ~Copyable {
  mutating func insert(_ item: consuming Element, at index: Int) {
    precondition(index >= 0 && index <= count)
    _ensureFreeCapacity(1)
    if index < count {
      let source = _storage.extracting(index ..< count)
      let target = _storage.extracting(index + 1 ..< count + 1)
      let last = target.moveInitialize(fromContentsOf: source)
      assert(last == target.endIndex)
    }
    _storage.initializeElement(at: index, to: item)
    _count += 1
  }
}

extension Hypoarray {
  mutating func append(contentsOf items: some Sequence<Element>) {
    for item in items {
      append(item)
    }
  }
}

struct Counted: ~Copyable {
  var value: Int
  nonisolated(unsafe) static var instances: Int = 0

  init(_ value: Int) {
    self.value = value
    Counted.instances += 1
  }

  deinit {
    Counted.instances -= 1
    expectGE(Counted.instances, 0)
  }
}

var suite = TestSuite("Hypoarray")
defer { runAllTests() }

suite.test("basics") {
  var array = Hypoarray(Counted(42))
  expectFalse(array.isEmpty)
  expectEqual(array.count, 1)
  expectEqual(array[0].value, 42)
  expectEqual(Counted.instances, 1)

  array.append(Counted(23))
  expectFalse(array.isEmpty)
  expectEqual(array.count, 2)
  expectEqual(array[0].value, 42)
  expectEqual(array[1].value, 23)
  expectEqual(Counted.instances, 2)

  let old = array.remove(at: 0)
  expectEqual(old.value, 42)
  expectFalse(array.isEmpty)
  expectEqual(array.count, 1)
  expectEqual(array[0].value, 23)
  expectEqual(Counted.instances, 2)
  _ = consume old
  expectEqual(Counted.instances, 1)

  let old2 = array.remove(at: 0)
  expectEqual(old2.value, 23)
  expectEqual(array.count, 0)
  expectTrue(array.isEmpty)
  expectEqual(Counted.instances, 1)
  _ = consume old2
  expectEqual(Counted.instances, 0)
}

suite.test("read access") {
  let c = 100
  let array = Hypoarray<Counted>(count: c) { Counted($0) }

  for i in 0 ..< c {
    expectEqual(array.borrowElement(at: i) { $0.value }, i)
    expectEqual(array[i].value, i)
  }
}

suite.test("update access") {
  let c = 100
  var array = Hypoarray<Counted>(count: c) { Counted($0) }

  for i in 0 ..< c {
    array.updateElement(at: i) { $0.value += 100 }
    array[i].value += 100
  }

  for i in 0 ..< c {
    expectEqual(array[i].value, 200 + i)
  }

  expectEqual(Counted.instances, c)
  _ = consume array
  expectEqual(Counted.instances, 0)
}

suite.test("append") {
  var array = Hypoarray<Counted>()
  let c = 100
  for i in 0 ..< c {
    array.append(Counted(100 + i))
  }
  expectEqual(Counted.instances, c)
  expectEqual(array.count, c)

  for i in 0 ..< c {
    // FIXME: unexpected exclusivity violation (rdar://128441125)
    //expectEqual(array.borrowElement(at: i) { $0.value }, 100 + i)
    expectEqual(array[i].value, 100 + i)
  }

  _ = consume array
  expectEqual(Counted.instances, 0)
}

suite.test("insert") {
  var array = Hypoarray<Counted>()
  let c = 100
  for i in 0 ..< c {
    array.insert(Counted(100 + i), at: 0)
  }
  expectEqual(Counted.instances, c)
  expectEqual(array.count, c)

  for i in 0 ..< c {
    // FIXME: unexpected exclusivity violation (rdar://128441125)
    //expectEqual(array.borrowElement(at: i) { $0.value }, c + 99 - i)
    expectEqual(array[i].value, c + 99 - i)
  }

  _ = consume array
  expectEqual(Counted.instances, 0)
}

suite.test("remove") {
  let c = 100
  var array = Hypoarray<Counted>(count: c) { Counted(100 + $0) }
  expectEqual(Counted.instances, c)
  expectEqual(array.count, c)

  for i in 0 ..< c {
    array.remove(at: 0)
    expectEqual(array.count, c - 1 - i)
    expectEqual(Counted.instances, c - 1 - i)
  }

  expectTrue(array.isEmpty)
  expectEqual(Counted.instances, 0)
}
