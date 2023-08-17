//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

/// This file is copied from swift-collections and should not be modified here.
/// Rather all changes should be made to swift-collections and copied back.

import Swift

extension _Deque {
  struct _Storage {
    internal typealias _Buffer = ManagedBufferPointer<_DequeBufferHeader, Element>

    internal var _buffer: _Buffer

    internal init(_buffer: _Buffer) {
      self._buffer = _buffer
    }
  }
}

extension _Deque._Storage: CustomStringConvertible {
  internal var description: String {
    "Deque<\(Element.self)>._Storage\(_buffer.header)"
  }
}

extension _Deque._Storage {
  internal init() {
    self.init(_buffer: _Buffer(unsafeBufferObject: _emptyDequeStorage))
  }

  internal init(_ object: _DequeBuffer<Element>) {
    self.init(_buffer: _Buffer(unsafeBufferObject: object))
  }

  internal init(minimumCapacity: Int) {
    let object = _DequeBuffer<Element>.create(
      minimumCapacity: minimumCapacity,
      makingHeaderWith: {
        #if os(OpenBSD)
        let capacity = minimumCapacity
        #else
        let capacity = $0.capacity
        #endif
        return _DequeBufferHeader(capacity: capacity, count: 0, startSlot: .zero)
      })
    self.init(_buffer: _Buffer(unsafeBufferObject: object))
  }
}

extension _Deque._Storage {
  #if COLLECTIONS_INTERNAL_CHECKS
  internal func _checkInvariants() {
    _buffer.withUnsafeMutablePointerToHeader { $0.pointee._checkInvariants() }
  }
  #else
  internal func _checkInvariants() {}
  #endif // COLLECTIONS_INTERNAL_CHECKS
}

extension _Deque._Storage {
  internal var identity: AnyObject { _buffer.buffer }


  internal var capacity: Int {
    _buffer.withUnsafeMutablePointerToHeader { $0.pointee.capacity }
  }

  internal var count: Int {
    _buffer.withUnsafeMutablePointerToHeader { $0.pointee.count }
  }

  internal var startSlot: _DequeSlot {
    _buffer.withUnsafeMutablePointerToHeader { $0.pointee.startSlot
    }
  }
}

extension _Deque._Storage {
  internal typealias Index = Int

  internal typealias _UnsafeHandle = _Deque._UnsafeHandle

  internal func read<R>(_ body: (_UnsafeHandle) throws -> R) rethrows -> R {
    try _buffer.withUnsafeMutablePointers { header, elements in
      let handle = _UnsafeHandle(header: header,
                                 elements: elements,
                                 isMutable: false)
      return try body(handle)
    }
  }

  internal func update<R>(_ body: (_UnsafeHandle) throws -> R) rethrows -> R {
    try _buffer.withUnsafeMutablePointers { header, elements in
      let handle = _UnsafeHandle(header: header,
                                 elements: elements,
                                 isMutable: true)
      return try body(handle)
    }
  }
}

extension _Deque._Storage {
  /// Return a boolean indicating whether this storage instance is known to have
  /// a single unique reference. If this method returns true, then it is safe to
  /// perform in-place mutations on the deque.
  internal mutating func isUnique() -> Bool {
    _buffer.isUniqueReference()
  }

  /// Ensure that this storage refers to a uniquely held buffer by copying
  /// elements if necessary.
  internal mutating func ensureUnique() {
    if isUnique() { return }
    self._makeUniqueCopy()
  }

  internal mutating func _makeUniqueCopy() {
    self = self.read { $0.copyElements() }
  }

  /// The growth factor to use to increase storage size to make place for an
  /// insertion.
  internal static var growthFactor: Double { 1.5 }

  internal func _growCapacity(
    to minimumCapacity: Int,
    linearly: Bool
  ) -> Int {
    if linearly { return Swift.max(capacity, minimumCapacity) }
    return Swift.max(Int((Self.growthFactor * Double(capacity)).rounded(.up)),
                     minimumCapacity)
  }

  /// Ensure that we have a uniquely referenced buffer with enough space to
  /// store at least `minimumCapacity` elements.
  ///
  /// - Parameter minimumCapacity: The minimum number of elements the buffer
  ///    needs to be able to hold on return.
  ///
  /// - Parameter linearGrowth: If true, then don't use an exponential growth
  ///    factor when reallocating the buffer -- just allocate space for the
  ///    requested number of elements
  internal mutating func ensureUnique(
    minimumCapacity: Int,
    linearGrowth: Bool = false
  ) {
    let unique = isUnique()
    if _slowPath(capacity < minimumCapacity || !unique) {
      _ensureUnique(minimumCapacity: minimumCapacity, linearGrowth: linearGrowth)
    }
  }

  internal mutating func _ensureUnique(
    minimumCapacity: Int,
    linearGrowth: Bool
  ) {
    if capacity >= minimumCapacity {
      assert(!self.isUnique())
      self = self.read { $0.copyElements() }
    } else if isUnique() {
      let minimumCapacity = _growCapacity(to: minimumCapacity, linearly: linearGrowth)
      self = self.update { source in
        source.moveElements(minimumCapacity: minimumCapacity)
      }
    } else {
      let minimumCapacity = _growCapacity(to: minimumCapacity, linearly: linearGrowth)
      self = self.read { source in
        source.copyElements(minimumCapacity: minimumCapacity)
      }
    }
  }
}
