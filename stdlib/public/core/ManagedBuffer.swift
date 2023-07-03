//===--- ManagedBuffer.swift - variable-sized buffer of aligned memory ----===//
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

import SwiftShims

@usableFromInline
internal typealias _HeapObject = SwiftShims.HeapObject

@usableFromInline
@_silgen_name("swift_bufferAllocate")
internal func _swift_bufferAllocate(
  bufferType type: AnyClass,
  size: Int,
  alignmentMask: Int
) -> AnyObject

/// A class whose instances contain a property of type `Header` and raw
/// storage for an array of `Element`, whose size is determined at
/// instance creation.
///
/// Note that the `Element` array is suitably-aligned **raw memory**.
/// You are expected to construct and---if necessary---destroy objects
/// there yourself, using the APIs on `UnsafeMutablePointer<Element>`.
/// Typical usage stores a count and capacity in `Header` and destroys
/// any live elements in the `deinit` of a subclass.
/// - Note: Subclasses must not have any stored properties; any storage
///   needed should be included in `Header`.
@_fixed_layout
open class ManagedBuffer<Header, Element> {
  /// The stored `Header` instance.
  ///
  /// During instance creation, in particular during
  /// `ManagedBuffer.create`'s call to initialize, `ManagedBuffer`'s
  /// `header` property is as-yet uninitialized, and therefore
  /// reading the `header` property during `ManagedBuffer.create` is undefined.
  public final var header: Header

  // This is really unfortunate. In Swift 5.0, the method descriptor for this
  // initializer was public and subclasses would "inherit" it, referencing its
  // method descriptor from their class override table.
  @usableFromInline
  internal init(_doNotCallMe: ()) {
    _internalInvariantFailure("Only initialize these by calling create")
  }

  @inlinable
  deinit {}
}

extension ManagedBuffer {
  /// Create a new instance of the most-derived class, calling
  /// `factory` on the partially-constructed object to generate
  /// an initial `Header`.
  @inlinable
  public final class func create(
    minimumCapacity: Int,
    makingHeaderWith factory: (
      ManagedBuffer<Header, Element>) throws -> Header
  ) rethrows -> ManagedBuffer<Header, Element> {

    let p = Builtin.allocWithTailElems_1(
         self,
         minimumCapacity._builtinWordValue, Element.self)

    let initHeaderVal = try factory(p)
    p.headerAddress.initialize(to: initHeaderVal)
    // The _fixLifetime is not really needed, because p is used afterwards.
    // But let's be conservative and fix the lifetime after we use the
    // headerAddress.
    _fixLifetime(p)
    return p
  }

  /// The actual number of elements that can be stored in this object.
  ///
  /// This header may be nontrivial to compute; it is usually a good
  /// idea to store this information in the "header" area when
  /// an instance is created.
  @inlinable
  @available(OpenBSD, unavailable, message: "malloc_size is unavailable.")
  public final var capacity: Int {
    let storageAddr = UnsafeMutableRawPointer(Builtin.bridgeToRawPointer(self))
    let endAddr = storageAddr + _swift_stdlib_malloc_size(storageAddr)
    let realCapacity = endAddr.assumingMemoryBound(to: Element.self) -
      firstElementAddress
    return realCapacity
  }

  @inlinable
  internal final var firstElementAddress: UnsafeMutablePointer<Element> {
    return UnsafeMutablePointer(
      Builtin.projectTailElems(self, Element.self))
  }

  @inlinable
  internal final var headerAddress: UnsafeMutablePointer<Header> {
    return UnsafeMutablePointer<Header>(Builtin.addressof(&header))
  }

  /// Call `body` with an `UnsafeMutablePointer` to the stored
  /// `Header`.
  ///
  /// - Note: This pointer is valid only for the duration of the
  ///   call to `body`.
  @inlinable
  public final func withUnsafeMutablePointerToHeader<R>(
    _ body: (UnsafeMutablePointer<Header>) throws -> R
  ) rethrows -> R {
    return try withUnsafeMutablePointers { (v, _) in return try body(v) }
  }

  /// Call `body` with an `UnsafeMutablePointer` to the `Element`
  /// storage.
  ///
  /// - Note: This pointer is valid only for the duration of the
  ///   call to `body`.
  @inlinable
  public final func withUnsafeMutablePointerToElements<R>(
    _ body: (UnsafeMutablePointer<Element>) throws -> R
  ) rethrows -> R {
    return try withUnsafeMutablePointers { return try body($1) }
  }

  /// Call `body` with `UnsafeMutablePointer`s to the stored `Header`
  /// and raw `Element` storage.
  ///
  /// - Note: These pointers are valid only for the duration of the
  ///   call to `body`.
  @inlinable
  public final func withUnsafeMutablePointers<R>(
    _ body: (UnsafeMutablePointer<Header>, UnsafeMutablePointer<Element>) throws -> R
  ) rethrows -> R {
    defer { _fixLifetime(self) }
    return try body(headerAddress, firstElementAddress)
  }
}

/// Contains a buffer object, and provides access to an instance of
/// `Header` and contiguous storage for an arbitrary number of
/// `Element` instances stored in that buffer.
///
/// For most purposes, the `ManagedBuffer` class can be used on its own.
/// However, in cases
/// where objects of various different classes must serve as storage,
/// you need to also use `ManagedBufferPointer`.
///
/// A valid buffer class is non-`@objc`, with no declared stored
///   properties.  Its `deinit` must destroy its
///   stored `Header` and any constructed `Element`s.
///
/// Example Buffer Class
/// --------------------
///
///      class MyBuffer<Element> { // non-@objc
///        typealias Manager = ManagedBufferPointer<(Int, String), Element>
///        deinit {
///          Manager(unsafeBufferObject: self).withUnsafeMutablePointers {
///            (pointerToHeader, pointerToElements) -> Void in
///            pointerToElements.deinitialize(count: self.count)
///            pointerToHeader.deinitialize(count: 1)
///          }
///        }
///
///        // All properties are *computed* based on members of the Header
///        var count: Int {
///          return Manager(unsafeBufferObject: self).header.0
///        }
///        var name: String {
///          return Manager(unsafeBufferObject: self).header.1
///        }
///      }
///
@frozen
public struct ManagedBufferPointer<Header, Element> {

  @usableFromInline
  internal var _nativeBuffer: Builtin.NativeObject

  /// Create with new storage containing an initial `Header` and space
  /// for at least `minimumCapacity` `element`s.
  ///
  /// - parameter bufferClass: The class of the object used for storage.
  /// - parameter minimumCapacity: The minimum number of `Element`s that
  ///   must be able to be stored in the new buffer.
  /// - parameter factory: A function that produces the initial
  ///   `Header` instance stored in the buffer, given the `buffer`
  ///   object and a function that can be called on it to get the actual
  ///   number of allocated elements.
  ///
  /// - Precondition: `minimumCapacity >= 0`, and the type indicated by
  ///   `bufferClass` is a non-`@objc` class with no declared stored
  ///   properties.  The `deinit` of `bufferClass` must destroy its
  ///   stored `Header` and any constructed `Element`s.
  @inlinable
  @available(OpenBSD, unavailable, message: "malloc_size is unavailable.")
  public init(
    bufferClass: AnyClass,
    minimumCapacity: Int,
    makingHeaderWith factory:
      (_ buffer: AnyObject, _ capacity: (AnyObject) -> Int) throws -> Header
  ) rethrows {
    self = ManagedBufferPointer(
      bufferClass: bufferClass, minimumCapacity: minimumCapacity)

    // initialize the header field
    try withUnsafeMutablePointerToHeader {
      $0.initialize(to: 
        try factory(
          self.buffer,
          {
            ManagedBufferPointer(unsafeBufferObject: $0).capacity
          }))
    }
    // FIXME: workaround for <rdar://problem/18619176>.  If we don't
    // access header somewhere, its addressor gets linked away
    _ = header
  }

  /// Manage the given `buffer`.
  ///
  /// - Precondition: `buffer` is an instance of a non-`@objc` class whose
  ///   `deinit` destroys its stored `Header` and any constructed `Element`s.
  @inlinable
  public init(unsafeBufferObject buffer: AnyObject) {
    ManagedBufferPointer._checkValidBufferClass(type(of: buffer))

    self._nativeBuffer = Builtin.unsafeCastToNativeObject(buffer)
  }

  //===--- internal/private API -------------------------------------------===//

  /// Internal version for use by _ContiguousArrayBuffer where we know that we
  /// have a valid buffer class.
  /// This version of the init function gets called from
  ///  _ContiguousArrayBuffer's deinit function. Since 'deinit' does not get
  /// specialized with current versions of the compiler, we can't get rid of the
  /// _debugPreconditions in _checkValidBufferClass for any array. Since we know
  /// for the _ContiguousArrayBuffer that this check must always succeed we omit
  /// it in this specialized constructor.
  @inlinable
  internal init(_uncheckedUnsafeBufferObject buffer: AnyObject) {
    ManagedBufferPointer._internalInvariantValidBufferClass(type(of: buffer))
    self._nativeBuffer = Builtin.unsafeCastToNativeObject(buffer)
  }

  /// Create with new storage containing space for an initial `Header`
  /// and at least `minimumCapacity` `element`s.
  ///
  /// - parameter bufferClass: The class of the object used for storage.
  /// - parameter minimumCapacity: The minimum number of `Element`s that
  ///   must be able to be stored in the new buffer.
  ///
  /// - Precondition: `minimumCapacity >= 0`, and the type indicated by
  ///   `bufferClass` is a non-`@objc` class with no declared stored
  ///   properties.  The `deinit` of `bufferClass` must destroy its
  ///   stored `Header` and any constructed `Element`s.
  @inlinable
  internal init(
    bufferClass: AnyClass,
    minimumCapacity: Int
  ) {
    ManagedBufferPointer._checkValidBufferClass(bufferClass, creating: true)
    _precondition(
      minimumCapacity >= 0,
      "ManagedBufferPointer must have non-negative capacity")

    self.init(
      _uncheckedBufferClass: bufferClass, minimumCapacity: minimumCapacity)
  }

  /// Internal version for use by _ContiguousArrayBuffer.init where we know that
  /// we have a valid buffer class and that the capacity is >= 0.
  @inlinable
  internal init(
    _uncheckedBufferClass: AnyClass,
    minimumCapacity: Int
  ) {
    ManagedBufferPointer._internalInvariantValidBufferClass(_uncheckedBufferClass, creating: true)
    _internalInvariant(
      minimumCapacity >= 0,
      "ManagedBufferPointer must have non-negative capacity")

    let totalSize = ManagedBufferPointer._elementOffset
      +  minimumCapacity * MemoryLayout<Element>.stride

    let newBuffer: AnyObject = _swift_bufferAllocate(
      bufferType: _uncheckedBufferClass,
      size: totalSize,
      alignmentMask: ManagedBufferPointer._alignmentMask)

    self._nativeBuffer = Builtin.unsafeCastToNativeObject(newBuffer)
  }

  /// Manage the given `buffer`.
  ///
  /// - Note: It is an error to use the `header` property of the resulting
  ///   instance unless it has been initialized.
  @inlinable
  internal init(_ buffer: ManagedBuffer<Header, Element>) {
    _nativeBuffer = Builtin.unsafeCastToNativeObject(buffer)
  }
}

extension ManagedBufferPointer {
  /// The stored `Header` instance.
  @inlinable
  public var header: Header {
    _read {
      yield _headerPointer.pointee
    }
    _modify {
      yield &_headerPointer.pointee
    }
  }

  /// Returns the object instance being used for storage.
  @inlinable
  public var buffer: AnyObject {
    return Builtin.castFromNativeObject(_nativeBuffer)
  }

  /// The actual number of elements that can be stored in this object.
  ///
  /// This value may be nontrivial to compute; it is usually a good
  /// idea to store this information in the "header" area when
  /// an instance is created.
  @inlinable
  @available(OpenBSD, unavailable, message: "malloc_size is unavailable.")
  public var capacity: Int {
    return (
      _capacityInBytes &- ManagedBufferPointer._elementOffset
    ) / MemoryLayout<Element>.stride
  }

  /// Call `body` with an `UnsafeMutablePointer` to the stored
  /// `Header`.
  ///
  /// - Note: This pointer is valid only
  ///   for the duration of the call to `body`.
  @inlinable
  public func withUnsafeMutablePointerToHeader<R>(
    _ body: (UnsafeMutablePointer<Header>) throws -> R
  ) rethrows -> R {
    return try withUnsafeMutablePointers { (v, _) in return try body(v) }
  }

  /// Call `body` with an `UnsafeMutablePointer` to the `Element`
  /// storage.
  ///
  /// - Note: This pointer is valid only for the duration of the
  ///   call to `body`.
  @inlinable
  public func withUnsafeMutablePointerToElements<R>(
    _ body: (UnsafeMutablePointer<Element>) throws -> R
  ) rethrows -> R {
    return try withUnsafeMutablePointers { return try body($1) }
  }

  /// Call `body` with `UnsafeMutablePointer`s to the stored `Header`
  /// and raw `Element` storage.
  ///
  /// - Note: These pointers are valid only for the duration of the
  ///   call to `body`.
  @inlinable
  public func withUnsafeMutablePointers<R>(
    _ body: (UnsafeMutablePointer<Header>, UnsafeMutablePointer<Element>) throws -> R
  ) rethrows -> R {
    defer { _fixLifetime(_nativeBuffer) }
    return try body(_headerPointer, _elementPointer)
  }

  /// Returns `true` if `self` holds the only strong reference to its
  /// buffer; otherwise, returns `false`.
  ///
  /// See `isKnownUniquelyReferenced` for details.
  @inlinable
  public mutating func isUniqueReference() -> Bool {
    return _isUnique(&_nativeBuffer)
  }
}

extension ManagedBufferPointer {
  @inlinable
  internal static func _checkValidBufferClass(
    _ bufferClass: AnyClass, creating: Bool = false
  ) {
    _debugPrecondition(
      _class_getInstancePositiveExtentSize(bufferClass) == MemoryLayout<_HeapObject>.size
      || (
        (!creating || bufferClass is ManagedBuffer<Header, Element>.Type)
        && _class_getInstancePositiveExtentSize(bufferClass)
          == _headerOffset + MemoryLayout<Header>.size),
      "ManagedBufferPointer buffer class has illegal stored properties"
    )
    _debugPrecondition(
      _usesNativeSwiftReferenceCounting(bufferClass),
      "ManagedBufferPointer buffer class must be non-@objc"
    )
  }

  @inlinable
  internal static func _internalInvariantValidBufferClass(
    _ bufferClass: AnyClass, creating: Bool = false
  ) {
    _internalInvariant(
      _class_getInstancePositiveExtentSize(bufferClass) == MemoryLayout<_HeapObject>.size
      || (
        (!creating || bufferClass is ManagedBuffer<Header, Element>.Type)
        && _class_getInstancePositiveExtentSize(bufferClass)
          == _headerOffset + MemoryLayout<Header>.size),
      "ManagedBufferPointer buffer class has illegal stored properties"
    )
    _internalInvariant(
      _usesNativeSwiftReferenceCounting(bufferClass),
      "ManagedBufferPointer buffer class must be non-@objc"
    )
  }
}

extension ManagedBufferPointer {
  /// The required alignment for allocations of this type, minus 1
  @inlinable
  internal static var _alignmentMask: Int {
    return max(
      MemoryLayout<_HeapObject>.alignment,
      max(MemoryLayout<Header>.alignment, MemoryLayout<Element>.alignment)) &- 1
  }

  /// The actual number of bytes allocated for this object.
  @inlinable
  @available(OpenBSD, unavailable, message: "malloc_size is unavailable.")
  internal var _capacityInBytes: Int {
    return _swift_stdlib_malloc_size(_address)
  }

  /// The address of this instance in a convenient pointer-to-bytes form
  @inlinable
  internal var _address: UnsafeMutableRawPointer {
    return UnsafeMutableRawPointer(Builtin.bridgeToRawPointer(_nativeBuffer))
  }

  /// Offset from the allocated storage for `self` to the stored `Header`
  @inlinable
  internal static var _headerOffset: Int {
    _onFastPath()
    return _roundUp(
      MemoryLayout<_HeapObject>.size,
      toAlignment: MemoryLayout<Header>.alignment)
  }

  /// An **unmanaged** pointer to the storage for the `Header`
  /// instance.  Not safe to use without _fixLifetime calls to
  /// guarantee it doesn't dangle
  @inlinable
  internal var _headerPointer: UnsafeMutablePointer<Header> {
    _onFastPath()
    return (_address + ManagedBufferPointer._headerOffset).assumingMemoryBound(
      to: Header.self)
  }

  /// An **unmanaged** pointer to the storage for `Element`s.  Not
  /// safe to use without _fixLifetime calls to guarantee it doesn't
  /// dangle.
  @inlinable
  internal var _elementPointer: UnsafeMutablePointer<Element> {
    _onFastPath()
    return (_address + ManagedBufferPointer._elementOffset).assumingMemoryBound(
      to: Element.self)
  }

  /// Offset from the allocated storage for `self` to the `Element` storage
  @inlinable
  internal static var _elementOffset: Int {
    _onFastPath()
    return _roundUp(
      _headerOffset + MemoryLayout<Header>.size,
      toAlignment: MemoryLayout<Element>.alignment)
  }
}

extension ManagedBufferPointer: Equatable {
  @inlinable
  public static func == (
    lhs: ManagedBufferPointer,
    rhs: ManagedBufferPointer
  ) -> Bool {
    return lhs._address == rhs._address
  }
}

// FIXME: when our calling convention changes to pass self at +0,
// inout should be dropped from the arguments to these functions.
// FIXME(docs): isKnownUniquelyReferenced should check weak/unowned counts too, 
// but currently does not. rdar://problem/29341361

/// Returns a Boolean value indicating whether the given object is known to
/// have a single strong reference.
///
/// The `isKnownUniquelyReferenced(_:)` function is useful for implementing the
/// copy-on-write optimization for the deep storage of value types:
///
///     mutating func update(withValue value: T) {
///         if !isKnownUniquelyReferenced(&myStorage) {
///             myStorage = self.copiedStorage()
///         }
///         myStorage.update(withValue: value)
///     }
///
/// Use care when calling `isKnownUniquelyReferenced(_:)` from within a Boolean
/// expression. In debug builds, an instance in the left-hand side of a `&&`
/// or `||` expression may still be referenced when evaluating the right-hand
/// side, inflating the instance's reference count. For example, this version
/// of the `update(withValue)` method will re-copy `myStorage` on every call:
///
///     // Copies too frequently:
///     mutating func badUpdate(withValue value: T) {
///         if myStorage.shouldCopy || !isKnownUniquelyReferenced(&myStorage) {
///             myStorage = self.copiedStorage()
///         }
///         myStorage.update(withValue: value)
///     }
///
/// To avoid this behavior, swap the call `isKnownUniquelyReferenced(_:)` to
/// the left-hand side or store the result of the first expression in a local
/// constant:
///
///     mutating func goodUpdate(withValue value: T) {
///         let shouldCopy = myStorage.shouldCopy
///         if shouldCopy || !isKnownUniquelyReferenced(&myStorage) {
///             myStorage = self.copiedStorage()
///         }
///         myStorage.update(withValue: value)
///     }
///
/// `isKnownUniquelyReferenced(_:)` checks only for strong references to the
/// given object---if `object` has additional weak or unowned references, the
/// result may still be `true`. Because weak and unowned references cannot be
/// the only reference to an object, passing a weak or unowned reference as
/// `object` always results in `false`.
///
/// If the instance passed as `object` is being accessed by multiple threads
/// simultaneously, this function may still return `true`. Therefore, you must
/// only call this function from mutating methods with appropriate thread
/// synchronization. That will ensure that `isKnownUniquelyReferenced(_:)`
/// only returns `true` when there is really one accessor, or when there is a
/// race condition, which is already undefined behavior.
///
/// - Parameter object: An instance of a class. This function does *not* modify
///   `object`; the use of `inout` is an implementation artifact.
/// - Returns: `true` if `object` is known to have a single strong reference;
///   otherwise, `false`.
@inlinable
public func isKnownUniquelyReferenced<T: AnyObject>(_ object: inout T) -> Bool
{
  return _isUnique(&object)
}

/// Returns a Boolean value indicating whether the given object is known to
/// have a single strong reference.
///
/// The `isKnownUniquelyReferenced(_:)` function is useful for implementing the
/// copy-on-write optimization for the deep storage of value types:
///
///     mutating func update(withValue value: T) {
///         if !isKnownUniquelyReferenced(&myStorage) {
///             myStorage = self.copiedStorage()
///         }
///         myStorage.update(withValue: value)
///     }
///
/// `isKnownUniquelyReferenced(_:)` checks only for strong references to the
/// given object---if `object` has additional weak or unowned references, the
/// result may still be `true`. Because weak and unowned references cannot be
/// the only reference to an object, passing a weak or unowned reference as
/// `object` always results in `false`.
///
/// If the instance passed as `object` is being accessed by multiple threads
/// simultaneously, this function may still return `true`. Therefore, you must
/// only call this function from mutating methods with appropriate thread
/// synchronization. That will ensure that `isKnownUniquelyReferenced(_:)`
/// only returns `true` when there is really one accessor, or when there is a
/// race condition, which is already undefined behavior.
///
/// - Parameter object: An instance of a class. This function does *not* modify
///   `object`; the use of `inout` is an implementation artifact.
/// - Returns: `true` if `object` is known to have a single strong reference;
///   otherwise, `false`. If `object` is `nil`, the return value is `false`.
@inlinable
public func isKnownUniquelyReferenced<T: AnyObject>(
  _ object: inout T?
) -> Bool {
  return _isUnique(&object)
}
