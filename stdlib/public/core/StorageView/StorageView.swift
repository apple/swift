//===--- StorageView.swift ------------------------------------------------===//
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

#if hasFeature(NonescapableTypes)

// A StorageView<Element> represents a span of memory which
// contains initialized instances of `Element`.
@frozen
public struct StorageView<Element: ~Copyable /*& ~Escapable*/>: Copyable, ~Escapable {
  @usableFromInline let _start: Index
  @usableFromInline let _count: Int

  @inlinable @inline(__always)
  internal init<Owner: ~Copyable & ~Escapable>(
    _unchecked start: Index,
    count: Int,
    owner: borrowing Owner
  ) -> _borrow(owner) Self {
    self._start = start
    self._count = count
    return self
  }
}

@available(*, unavailable)
extension StorageView: Sendable {}

extension StorageView where Element: ~Copyable {

  @inlinable @inline(__always)
  internal init<Owner: ~Copyable & ~Escapable>(
    start: Index,
    count: Int,
    owner: borrowing Owner
  ) -> _borrow(owner) Self {
    precondition(count >= 0, "Count must not be negative")
    precondition(
      start.isAligned,
      "baseAddress must be properly aligned for accessing \(Element.self)"
    )
    self.init(_unchecked: start, count: count, owner: owner)
    return self
  }

  public init<Owner: ~Copyable & ~Escapable>(
    unsafeBufferPointer buffer: UnsafeBufferPointer<Element>,
    owner: borrowing Owner
  ) -> _borrow(owner) Self {
    guard let baseAddress = buffer.baseAddress else {
      fatalError("StorageView requires a non-nil base address")
    }
    self.init(unsafePointer: baseAddress, count: buffer.count, owner: owner)
    return self
  }

  public init<Owner: ~Copyable & ~Escapable>(
    unsafePointer: UnsafePointer<Element>,
    count: Int,
    owner: borrowing Owner
  ) -> _borrow(owner) Self {
    let start = Index(_rawStart: unsafePointer)
    self.init(start: start, count: count, owner: owner)
    return self
  }
}

#if hasFeature(BitwiseCopyable)
extension StorageView where Element: _BitwiseCopyable {

  @inlinable
  internal init<Owner: ~Copyable & ~Escapable>(
    start index: Index,
    count: Int,
    owner: borrowing Owner
  ) /* -> borrow(owner) Self */ {
    precondition(count >= 0, "Count must not be negative")
    self._start = index
    self._count = count
  }

  public init<Owner: ~Copyable & ~Escapable>(
    unsafeBytes buffer: UnsafeRawBufferPointer,
    as type: Element.Type,
    owner: borrowing Owner
  ) /* -> borrow(owner) Self */ {
    guard let baseAddress = buffer.baseAddress else {
      fatalError("StorageView requires a non-nil base address")
    }
    let (c, s) = (buffer.count, MemoryLayout<Element>.stride)
    let (q, r) = c.quotientAndRemainder(dividingBy: s)
    precondition(r == 0)
    self.init(
      unsafeRawPointer: baseAddress, as: Element.self, count: q, owner: owner
    )
  }

  public init<Owner: ~Copyable & ~Escapable>(
    unsafeRawPointer: UnsafeRawPointer,
    as type: Element.Type,
    count: Int,
    owner: borrowing Owner
  ) /* -> borrow(owner) Self */ {
    let start = Index(_rawStart: unsafeRawPointer)
    self.init(start: start, count: count, owner: owner)
  }
}
#endif

//MARK: Sequence

extension StorageView/*: Sequence*/ where Element: Copyable & Escapable {

  borrowing public func makeIterator() -> Iterator {
    .init(from: startIndex, to: endIndex, owner: self)
  }
}

extension StorageView where Element: Equatable {

  public func elementsEqual(_ other: Self) -> Bool {
    guard count == other.count else { return false }
    if count == 0 { return true }
    if startIndex == other.startIndex { return true }

    //FIXME: This could be short-cut
    //       with a layout constraint where stride equals size,
    //       as long as there is at most 1 unused bit pattern.
    // if Element is BitwiseRepresentable {
    // return _swift_stdlib_memcmp(lhs.baseAddress, rhs.baseAddress, count) == 0
    // }
    for o in 0..<count {
      if self[uncheckedOffset: o] != other[uncheckedOffset: o] { return false }
    }
    return true
  }

  public func elementsEqual(_ other: some Collection<Element>) -> Bool {
    guard count == other.count else { return false }
    if count == 0 { return true }

    return elementsEqual(AnySequence(other))
  }

  public func elementsEqual(_ other: some Sequence<Element>) -> Bool {
    for (index, otherElement) in zip(indices, other) {
      if self[unchecked: index] != otherElement { return false }
    }
    return true
  }
}

//MARK: Bounds checking
extension StorageView where Element: ~Copyable {

  @inlinable @inline(__always)
  func boundsCheckPrecondition(_ position: Index) {
    precondition(
      position.isAligned,
      "Index is not properly aligned for accessing Element"
    )
    precondition(
      startIndex._allocation == position._allocation &&
      distance(from: startIndex, to: position) >= 0 &&
      distance(from: position, to: endIndex) > 0,
      "Index out of bounds"
    )
  }

  @inlinable @inline(__always)
  func boundsCheckPrecondition(_ bounds: Range<Index>) {
    precondition(
      bounds.lowerBound.isAligned && bounds.upperBound.isAligned,
      "Range of indices is not properly aligned for accessing Element"
    )
    precondition(
      startIndex._allocation == bounds.lowerBound._allocation &&
      startIndex._allocation == bounds.upperBound._allocation &&
      distance(from: startIndex, to: bounds.lowerBound) >= 0 &&
      distance(from: bounds.lowerBound, to: bounds.upperBound) >= 0 &&
      distance(from: bounds.upperBound, to: endIndex) >= 0,
      "Range of indices out of bounds"
    )
  }
}

#if hasFeature(BitwiseCopyable)
extension StorageView where Element: _BitwiseCopyable {
  @inlinable @inline(__always)
  func boundsCheckPrecondition(_ position: Index) {
    precondition(
      startIndex._allocation == position._allocation &&
      distance(from: startIndex, to: position) >= 0 &&
      distance(from: position, to: endIndex) > 0,
      "Index out of bounds"
    )
  }

  @inlinable @inline(__always)
  func boundsCheckPrecondition(_ bounds: Range<Index>) {
    precondition(
      startIndex._allocation == bounds.lowerBound._allocation &&
      startIndex._allocation == bounds.upperBound._allocation &&
      startIndex._rawValue.distance(to: bounds.lowerBound._rawValue) >= 0 &&
      bounds.lowerBound._rawValue.distance(to: bounds.upperBound._rawValue) >= 0 &&
      bounds.upperBound._rawValue.distance(to: endIndex._rawValue) >= 0,
      "Range of indices out of bounds"
    )
  }
}
#endif

//MARK: Collection, RandomAccessCollection
extension StorageView where Element: ~Copyable {
//  Collection,
//  BidirectionalCollection,
//  RandomAccessCollection

  public typealias Element = Element
//  public typealias Index = StorageView<Element>.Index
  public typealias SubSequence = Self

  @inlinable @inline(__always)
  public var startIndex: Index { _start }

  @inlinable @inline(__always)
  public var endIndex: Index { _start.advanced(by: _count) }

  @inlinable @inline(__always)
  public var count: Int {
    borrowing get { self._count }
  }

  @inlinable @inline(__always)
  public var indices: Range<Index> {
    .init(uncheckedBounds: (startIndex, endIndex))
  }

  @inlinable @inline(__always)
  public var isEmpty: Bool { count == 0 }

  @inlinable @inline(__always)
  public func index(after i: Index) -> Index {
    i.advanced(by: +1)
  }

  @inlinable @inline(__always)
  public func index(before i: Index) -> Index {
    i.advanced(by: -1)
  }

  @inlinable @inline(__always)
  public func formIndex(after i: inout Index) {
    i = index(after: i)
  }

  @inlinable @inline(__always)
  public func formIndex(before i: inout Index) {
    i = index(before: i)
  }

  @inlinable @inline(__always)
  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    i.advanced(by: distance)
  }

  @inlinable @inline(__always)
  public func formIndex(_ i: inout Index, offsetBy distance: Int) {
    i = index(i, offsetBy: distance)
  }

  @inlinable @inline(__always)
  public func distance(from start: Index, to end: Index) -> Int {
    start.distance(to: end)
  }

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(position: Index) -> Element {
    _read {
      boundsCheckPrecondition(position)
      yield self[unchecked: position]
    }
  }

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(unchecked position: Index) -> Element {
    _read {
      let binding = Builtin.bindMemory(
        position._rawValue._rawValue, 1._builtinWordValue, Element.self
      )
      defer { Builtin.rebindMemory(position._rawValue._rawValue, binding) }
      yield UnsafePointer<Element>(position._rawValue._rawValue).pointee
    }
  }

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(bounds: Range<Index>) -> Self {
    get {
      boundsCheckPrecondition(bounds)
      return self[unchecked: bounds]
    }
  }

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(unchecked bounds: Range<Index>) -> Self {
    _read {
      yield StorageView(
        start: bounds.lowerBound,
        count: bounds.count,
        owner: self
      )
    }
  }

  //FIXME: lifetime-dependent on self
  @_alwaysEmitIntoClient
  public subscript(bounds: some RangeExpression<Index>) -> Self {
    _read {
      yield self[bounds.relative(to: indices)]
    }
  }

  //FIXME: lifetime-dependent on self
  @_alwaysEmitIntoClient
  public subscript(unchecked bounds: some RangeExpression<Index>) -> Self {
    _read {
      yield self[unchecked: bounds.relative(to: indices)]
    }
  }

  //FIXME: lifetime-dependent on self
  @_alwaysEmitIntoClient
  public subscript(x: UnboundedRange) -> Self {
    _read {
      yield self[unchecked: indices]
    }
  }
}

#if hasFeature(BitwiseCopyable)
extension StorageView where Element: _BitwiseCopyable {

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(position: Index) -> Element {
    get {
      boundsCheckPrecondition(position)
      return self[unchecked: position]
    }
  }

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(unchecked position: Index) -> Element {
    get {
      position._rawValue.loadUnaligned(as: Element.self)
    }
  }

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(bounds: Range<Index>) -> Self {
    _read {
      boundsCheckPrecondition(bounds)
      yield self[unchecked: bounds]
    }
  }

  //FIXME: lifetime-dependent on self
  @inlinable @inline(__always)
  public subscript(unchecked bounds: Range<Index>) -> Self {
    _read {
      yield StorageView(
        start: bounds.lowerBound,
        count: bounds.count,
        owner: self
      )
    }
  }

  //FIXME: lifetime-dependent on self
  @_alwaysEmitIntoClient
  public subscript(bounds: some RangeExpression<Index>) -> Self {
    _read {
      yield self[bounds.relative(to: indices)]
    }
  }

  //FIXME: lifetime-dependent on self
  @_alwaysEmitIntoClient
  public subscript(unchecked bounds: some RangeExpression<Index>) -> Self {
    _read {
      yield self[unchecked: bounds.relative(to: indices)]
    }
  }

  //FIXME: lifetime-dependent on self
  @_alwaysEmitIntoClient
  public subscript(x: UnboundedRange) -> Self {
    _read {
      yield self[unchecked: indices]
    }
  }
}
#endif

//MARK: withUnsafeRaw...
#if hasFeature(BitwiseCopyable)
extension StorageView where Element: _BitwiseCopyable {

  //FIXME: mark closure parameter as non-escaping
  public func withUnsafeBytes<R>(
    _ body: (_ buffer: UnsafeRawBufferPointer) throws -> R
  ) rethrows -> R {
    let rawBuffer = UnsafeRawBufferPointer(
      start: count==0 ? nil : _start._rawValue,
      count: count*MemoryLayout<Element>.stride
    )
    return try body(rawBuffer)
  }
}
#endif

//MARK: withUnsafePointer, etc.
extension StorageView where Element: ~Copyable {

  //FIXME: mark closure parameter as non-escaping
  public func withUnsafeBufferPointer<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    try _start._rawValue.withMemoryRebound(to: Element.self, capacity: count) {
      try body(.init(start: $0, count: count))
    }
  }

  //FIXME: mark closure parameter as non-escaping
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    try withUnsafeBufferPointer(body)
  }
}

//MARK: load
#if hasFeature(BitwiseCopyable)
extension StorageView where Element: _BitwiseCopyable {

  public func load<T>(
    fromByteOffset offset: Int = 0, as: T.Type
  ) -> T {
    boundsCheckPrecondition(
      Range(uncheckedBounds: (
        .init(
          allocation: startIndex._allocation,
          rawValue: _start._rawValue.advanced(by: offset)
        ),
        .init(
          allocation: startIndex._allocation,
          rawValue: _start._rawValue.advanced(by: offset+MemoryLayout<T>.size)
        )
      ))
    )
    return _start._rawValue.load(fromByteOffset: offset, as: T.self)
  }

  public func load<T>(from index: Index, as: T.Type) -> T {
    let o = distance(from: startIndex, to: index)*MemoryLayout<Element>.stride
    return load(fromByteOffset: o, as: T.self)
  }

  public func loadUnaligned<T: _BitwiseCopyable>(
    fromByteOffset offset: Int = 0, as: T.Type
  ) -> T {
    boundsCheckPrecondition(
      Range(uncheckedBounds: (
        .init(
          allocation: startIndex._allocation,
          rawValue: _start._rawValue.advanced(by: offset)
        ),
        .init(
          allocation: startIndex._allocation,
          rawValue: _start._rawValue.advanced(by: offset+MemoryLayout<T>.size)
        )
      ))
    )
    return _start._rawValue.loadUnaligned(fromByteOffset: offset, as: T.self)
  }

  public func loadUnaligned<T: _BitwiseCopyable>(
    from index: Index, as: T.Type
  ) -> T {
    let o = distance(from: startIndex, to: index)*MemoryLayout<Element>.stride
    return loadUnaligned(fromByteOffset: o, as: T.self)
  }

  //FIXME: lifetime-dependent on self
  /// View the memory span represented by this view as a different type
  ///
  /// The memory must be laid out identically to the in-memory representation
  /// of `T`. The memory span must be over a whole number of instances of `T`.
  ///
  /// - Parameters:
  ///   - type: The type you wish to view the memory as
  /// - Returns: A new `StorageView` over elements of type `T`
  borrowing public func view<T: _BitwiseCopyable>(
    as: T.Type
  ) -> _borrow(self) StorageView<T> {
    let bc = count*MemoryLayout<Element>.stride
    let (nc, rem) = bc.quotientAndRemainder(dividingBy: MemoryLayout<T>.stride)
    precondition(rem == 0)
    let start = StorageView<T>.Index(
      allocation: startIndex._allocation,
      rawValue: startIndex._rawValue
    )
    return StorageView<T>(_unchecked: start, count: nc, owner: self)
  }
}
#endif

//MARK: integer offset subscripts

extension StorageView where Element: ~Copyable {

  @inlinable @inline(__always)
  public subscript(offset offset: Int) -> Element {
    _read {
      precondition(0 <= offset && offset < count)
      yield self[uncheckedOffset: offset]
    }
  }

  @inlinable @inline(__always)
  public subscript(uncheckedOffset offset: Int) -> Element {
    _read {
      yield self[unchecked: index(startIndex, offsetBy: offset)]
    }
  }
}

extension StorageView where Element: Copyable {
  @inlinable
  public var first: Element? {
    isEmpty ? nil : self[unchecked: startIndex]
  }

  @inlinable
  public var last: Element? {
    isEmpty ? nil : self[unchecked: index(startIndex, offsetBy: count &- 1)]
  }
}

//MARK: one-sided slicing operations
extension StorageView where Element: ~Copyable {

  borrowing public func prefix(upTo index: Index) -> _borrow(self) Self {
    index == startIndex
    ? Self(_unchecked: _start, count: 0, owner: self)
    : prefix(through: index.advanced(by: -1))
  }

  borrowing public func prefix(through index: Index) -> _borrow(self) Self {
    boundsCheckPrecondition(index)
    let nc = distance(from: startIndex, to: index) &+ 1
    return Self(_unchecked: _start, count: nc, owner: self)
  }

  borrowing public func prefix(_ maxLength: Int) -> _borrow(self) Self {
    precondition(maxLength >= 0, "Can't have a prefix of negative length.")
    let nc = maxLength < count ? maxLength : count
    return Self(_unchecked: _start, count: nc, owner: self)
  }

  borrowing public func dropLast(_ k: Int = 1) -> _borrow(self) Self {
    precondition(k >= 0, "Can't drop a negative number of elements.")
    let nc = k < count ? count&-k : 0
    return Self(_unchecked: _start, count: nc, owner: self)
  }

  borrowing public func suffix(from index: Index) -> _borrow(self) Self {
    if index == endIndex {
      return Self(_unchecked: index, count: 0, owner: self )
    }
    boundsCheckPrecondition(index)
    let nc = distance(from: index, to: endIndex)
    return Self(_unchecked: index, count: nc, owner: self)
  }

  borrowing public func suffix(_ maxLength: Int) -> _borrow(self) Self {
    precondition(maxLength >= 0, "Can't have a suffix of negative length.")
    let nc = maxLength < count ? maxLength : count
    let newStart = _start.advanced(by: count&-nc)
    return Self(_unchecked: newStart, count: nc, owner: self)
  }

  borrowing public func dropFirst(_ k: Int = 1) -> _borrow(self) Self {
    precondition(k >= 0, "Can't drop a negative number of elements.")
    let dc = k < count ? k : count
    let newStart = _start.advanced(by: dc)
    return Self(_unchecked: newStart, count: count&-dc, owner: self)
  }
}

#endif
