//===----------------------------------------------------------*- swift -*-===//
//
// This source file is part of the Swift open source project
//
// Copyright (c) 2021 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

extension Slice where Base == UnsafeMutableRawBufferPointer {

  /// Copies from a collection of `UInt8` into this buffer slice's memory.
  ///
  /// If the first `source.count` bytes of memory referenced by
  /// this buffer slice are bound to a type `T`, then `T` must be a trivial
  /// type, the underlying pointer must be properly aligned for accessing `T`,
  /// and `source.count` must be a multiple of `MemoryLayout<T>.stride`.
  ///
  /// After calling `copyBytes(from:)`, the first `source.count` bytes of memory
  /// referenced by this buffer slice are initialized to raw bytes.
  /// If the memory is bound to type `T`, then it contains values of type `T`.
  ///
  /// - Parameter source: A collection of `UInt8` elements. `source.count` must
  ///   be less than or equal to this buffer slice's `count`.
  @inlinable
  @_alwaysEmitIntoClient
  public func copyBytes<C: Collection>(
    from source: C
  ) where C.Element == UInt8 {
    let buffer = Base(rebasing: self)
    buffer.copyBytes(from: source)
  }

  /// Initializes the memory referenced by this buffer slice with the given
  /// value, binds the memory to the value's type, and returns a typed
  /// buffer of the initialized memory.
  ///
  /// The memory referenced by this buffer slice must be uninitialized or
  /// initialized to a trivial type, and must be properly aligned for
  /// accessing `T`.
  ///
  /// After calling this method on a raw buffer slice referencing memory
  /// starting at `b = base.baseAddress + startIndex`,
  /// the region starting at `b` and continuing up to
  /// `b + self.count - self.count % MemoryLayout<T>.stride` is bound
  /// to type `T` and is initialized. If `T` is a nontrivial type, you must
  /// eventually deinitialize or move the values in this region to avoid leaks.
  /// If `base.baseAddress` is `nil`, this function does nothing
  /// and returns an empty buffer pointer.
  ///
  /// - Parameters:
  ///   - type: The type to bind this buffer’s memory to.
  ///   - repeatedValue: The instance to copy into memory.
  /// - Returns: A typed buffer of the memory referenced by this raw buffer.
  ///     The typed buffer contains `self.count / MemoryLayout<T>.stride`
  ///     instances of `T`.
  @discardableResult
  @inlinable
  @_alwaysEmitIntoClient
  public func initializeMemory<T>(
    as type: T.Type, repeating repeatedValue: T
  ) -> UnsafeMutableBufferPointer<T> {
    let buffer = Base(rebasing: self)
    return buffer.initializeMemory(as: T.self, repeating: repeatedValue)
  }

  /// Initializes the buffer's memory with the given elements, binding the
  /// initialized memory to the elements' type.
  ///
  /// When calling the `initializeMemory(as:from:)` method on a buffer slice,
  /// the memory referenced by the slice must be uninitialized or initialized
  /// to a trivial type, and must be properly aligned for accessing `S.Element`.
  /// The buffer must contain sufficient memory to accommodate
  /// `source.underestimatedCount`.
  ///
  /// This method initializes the buffer slice with elements from `source` until
  /// `source` is exhausted or, if `source` is a sequence but not a collection,
  /// the buffer slice has no more room for source's elements. After calling
  /// `initializeMemory(as:from:)`, the memory referenced by the returned
  /// `UnsafeMutableBufferPointer` instance is bound and initialized to type
  /// `S.Element`.
  ///
  /// - Parameters:
  ///   - type: The type of element to which this buffer's memory will be bound.
  ///   - source: A sequence of elements with which to initialize the buffer.
  /// - Returns: An iterator to any elements of `source` that didn't fit in the
  ///   buffer, and a typed buffer of the written elements. The returned
  ///   buffer references memory starting at the same base address as this
  ///   buffer.
  @inlinable
  @_alwaysEmitIntoClient
  public func initializeMemory<S: Sequence>(
    as type: S.Element.Type, from source: S
  ) -> (unwritten: S.Iterator, initialized: UnsafeMutableBufferPointer<S.Element>) {
    let buffer = Base(rebasing: self)
    return buffer.initializeMemory(as: S.Element.self, from: source)
  }

  /// Initializes the buffer slice's memory with every element of the source,
  /// binding the initialized memory to the elements' type.
  ///
  /// When calling the `initializeMemory(as:fromContentsOf:)` method,
  /// the memory referenced by the buffer slice must be uninitialized,
  /// or initialized to a trivial type. The buffer slice must reference
  /// enough memory to store `source.count` elements, and it
  /// must be properly aligned for accessing `C.Element`.
  ///
  /// This method initializes the buffer with the contents of `source`
  /// until `source` is exhausted.
  /// After calling `initializeMemory(as:fromContentsOf:)`, the memory
  /// referenced by the returned `UnsafeMutableBufferPointer` instance is bound
  /// to the type of `C.Element` and is initialized. This method does not change
  /// the binding state of the unused portion of the buffer slice, if any.
  ///
  /// - Parameters:
  ///   - type: The type of element to which this buffer's memory will be bound.
  ///   - source: A collection of elements to be used to
  ///     initialize the buffer slice's storage.
  /// - Returns: A typed buffer referencing the initialized elements.
  ///     The returned buffer references memory starting at the same
  ///     base address as this slice, and its count is equal to `source.count`
  @inlinable
  @_alwaysEmitIntoClient
  public func initializeMemory<C: Collection>(
    as type: C.Element.Type,
    fromContentsOf source: C
  ) -> UnsafeMutableBufferPointer<C.Element> {
    let buffer = Base(rebasing: self)
    return buffer.initializeMemory(as: C.Element.self, fromContentsOf: source)
  }

  /// Moves every element of an initialized source buffer into the
  /// uninitialized memory referenced by this buffer slice, leaving
  /// the source memory uninitialized and this slice's memory initialized.
  ///
  /// When calling the `moveInitializeMemory(as:fromContentsOf:)` method,
  /// the memory referenced by the buffer slice must be uninitialized,
  /// or initialized to a trivial type. The buffer slice must reference
  /// enough memory to store `source.count` elements, and it must be properly
  /// aligned for accessing `C.Element`. After the method returns,
  /// the memory referenced by the returned buffer is initialized and the
  /// memory region underlying `source` is uninitialized.
  ///
  /// This method initializes the buffer slice with the contents of `source`
  /// until `source` is exhausted.
  /// After calling `initializeMemory(as:fromContentsOf:)`, the memory
  /// referenced by the returned `UnsafeMutableBufferPointer` instance is bound
  /// to the type of `T` and is initialized. This method does not change
  /// the binding state of the unused portion of the buffer slice, if any.
  ///
  /// - Parameters:
  ///   - type: The type of element to which this buffer's memory will be bound.
  ///   - source: A buffer referencing the values to copy.
  ///     The memory region underlying `source` must be initialized.
  ///     The memory regions referenced by `source` and this slice may overlap.
  /// - Returns: A typed buffer referencing the initialized elements.
  ///     The returned buffer references memory starting at the same
  ///     base address as this slice, and its count is equal to `source.count`.
  @discardableResult
  @inlinable
  @_alwaysEmitIntoClient
  public func moveInitializeMemory<T>(
    as type: T.Type,
    fromContentsOf source: UnsafeMutableBufferPointer<T>
  ) -> UnsafeMutableBufferPointer<T> {
    let buffer = Base(rebasing: self)
    return buffer.moveInitializeMemory(as: T.self, fromContentsOf: source)
  }

  /// Moves every element from an initialized source buffer slice into the
  /// uninitialized memory referenced by this buffer slice, leaving
  /// the source memory uninitialized and this slice's memory initialized.
  ///
  /// When calling the `moveInitializeMemory(as:fromContentsOf:)` method,
  /// the memory referenced by the buffer slice must be uninitialized,
  /// or initialized to a trivial type. The buffer slice must reference
  /// enough memory to store `source.count` elements, and it must be properly
  /// aligned for accessing `C.Element`. After the method returns,
  /// the memory referenced by the returned buffer is initialized and the
  /// memory region underlying `source` is uninitialized.
  ///
  /// This method initializes the buffer slice with the contents of `source`
  /// until `source` is exhausted.
  /// After calling `initializeMemory(as:fromContentsOf:)`, the memory
  /// referenced by the returned `UnsafeMutableBufferPointer` instance is bound
  /// to the type of `T` and is initialized. This method does not change
  /// the binding state of the unused portion of the buffer slice, if any.
  ///
  /// - Parameters:
  ///   - type: The type of element to which this buffer's memory will be bound.
  ///   - source: A buffer referencing the values to copy.
  ///     The memory region underlying `source` must be initialized.
  ///     The memory regions referenced by `source` and this buffer may overlap.
  /// - Returns: A typed buffer referencing the initialized elements.
  ///     The returned buffer references memory starting at the same
  ///     base address as this slice, and its count is equal to `source.count`.
  @discardableResult
  @inlinable
  @_alwaysEmitIntoClient
  public func moveInitializeMemory<T>(
    as type: T.Type,
    fromContentsOf source: Slice<UnsafeMutableBufferPointer<T>>
  ) -> UnsafeMutableBufferPointer<T> {
    let buffer = Base(rebasing: self)
    return buffer.moveInitializeMemory(as: T.self, fromContentsOf: source)
  }

  /// Binds this buffer slice’s memory to the specified type and returns
  /// a typed buffer of the bound memory.
  ///
  /// Use the `bindMemory(to:)` method to bind the memory referenced
  /// by this buffer slice to the type `T`. The memory must be uninitialized or
  /// initialized to a type that is layout compatible with `T`. If the memory
  /// is uninitialized, it is still uninitialized after being bound to `T`.
  ///
  /// - Warning: A memory location may only be bound to one type at a time. The
  ///   behavior of accessing memory as a type unrelated to its bound type is
  ///   undefined.
  ///
  /// - Parameters:
  ///   - type: The type `T` to bind the memory to.
  /// - Returns: A typed buffer of the newly bound memory. The memory in this
  ///   region is bound to `T`, but has not been modified in any other way.
  ///   The typed buffer references `self.count / MemoryLayout<T>.stride`
  ///   instances of `T`.
  @discardableResult
  @inlinable
  @_alwaysEmitIntoClient
  public func bindMemory<T>(to type: T.Type) -> UnsafeMutableBufferPointer<T> {
    let buffer = Base(rebasing: self)
    return buffer.bindMemory(to: T.self)
  }

  /// Executes the given closure while temporarily binding the buffer slice to
  /// instances of type `T`.
  ///
  /// Use this method when you have a buffer slice to raw memory and you need
  /// to access that memory as instances of a given type `T`. Accessing
  /// memory as a type `T` requires that the memory be bound to that type.
  /// A memory location may only be bound to one type at a time, so accessing
  /// the same memory as an unrelated type without first rebinding the memory
  /// is undefined.
  ///
  /// Any instance of `T` within the re-bound region may be initialized or
  /// uninitialized. The memory underlying any individual instance of `T`
  /// must have the same initialization state (i.e.  initialized or
  /// uninitialized.) Accessing a `T` whose underlying memory
  /// is in a mixed initialization state shall be undefined behaviour.
  ///
  /// If the byte count of the original buffer slice is not a multiple of
  /// the stride of `T`, then the re-bound buffer is shorter
  /// than the original buffer.
  ///
  /// After executing `body`, this method rebinds memory back to its original
  /// binding state. This can be unbound memory, or bound to a different type.
  ///
  /// - Note: The buffer slice's start address must match the
  ///   alignment of `T` (as reported by `MemoryLayout<T>.alignment`). That is,
  ///   `Int(bitPattern: base.baseAddress+startIndex) % MemoryLayout<T>.alignment`
  ///   must equal zero.
  ///
  /// - Note: A raw buffer slice may represent memory that has been bound to
  ///   a type. If that is the case, then `T` must be layout compatible with the
  ///   type to which the memory has been bound. This requirement does not
  ///   apply if the raw buffer represents memory that has not been bound
  ///   to any type.
  ///
  /// - Parameters:
  ///   - type: The type to temporarily bind the memory referenced by this
  ///     buffer slice.
  ///   - body: A closure that takes a typed pointer to the
  ///     same memory as this pointer, only bound to type `T`. The closure's
  ///     pointer argument is valid only for the duration of the closure's
  ///     execution. If `body` has a return value, that value is also used as
  ///     the return value for the `withMemoryRebound(to:capacity:_:)` method.
  ///   - buffer: The buffer temporarily bound to instances of `T`.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  @_alwaysEmitIntoClient
  public func withMemoryRebound<T, Result, E: Error>(
    to type: T.Type, _ body: (UnsafeMutableBufferPointer<T>) throws(E) -> Result
  ) throws(E) -> Result {
    let buffer = Base(rebasing: self)
    return try buffer.withMemoryRebound(to: T.self, body)
  }

  /// Returns a typed buffer to the memory referenced by this buffer slice,
  /// assuming that the memory is already bound to the specified type.
  ///
  /// Use this method when you have a raw buffer to memory that has already
  /// been bound to the specified type. The memory starting at this pointer
  /// must be bound to the type `T`. Accessing memory through the returned
  /// pointer is undefined if the memory has not been bound to `T`. To bind
  /// memory to `T`, use `bindMemory(to:capacity:)` instead of this method.
  ///
  /// - Note: The buffer slice's start address must match the
  ///   alignment of `T` (as reported by `MemoryLayout<T>.alignment`). That is,
  ///   `Int(bitPattern: base.baseAddress+startIndex) % MemoryLayout<T>.alignment`
  ///   must equal zero.
  ///
  /// - Parameter to: The type `T` that the memory has already been bound to.
  /// - Returns: A typed pointer to the same memory as this raw pointer.
  @inlinable
  @_alwaysEmitIntoClient
  public func assumingMemoryBound<T>(
    to type: T.Type
  ) -> UnsafeMutableBufferPointer<T> {
    let buffer = Base(rebasing: self)
    return buffer.assumingMemoryBound(to: T.self)
  }

  /// Returns a new instance of the given type, read from the
  /// specified offset into the buffer pointer slice's raw memory.
  ///
  /// The memory at `offset` bytes into this buffer pointer slice
  /// must be properly aligned for accessing `T` and initialized to `T` or
  /// another type that is layout compatible with `T`.
  ///
  /// You can use this method to create new values from the underlying
  /// buffer pointer's bytes. The following example creates two new `Int32`
  /// instances from the memory referenced by the buffer pointer `someBytes`.
  /// The bytes for `a` are copied from the first four bytes of `someBytes`,
  /// and the bytes for `b` are copied from the next four bytes.
  ///
  ///     let a = someBytes[0..<4].load(as: Int32.self)
  ///     let b = someBytes[4..<8].load(as: Int32.self)
  ///
  /// The memory to read for the new instance must not extend beyond the
  /// memory region represented by the buffer pointer slice---that is,
  /// `offset + MemoryLayout<T>.size` must be less than or equal
  /// to the slice's `count`.
  ///
  /// - Parameters:
  ///   - offset: The offset into the slice's memory, in bytes, at
  ///     which to begin reading data for the new instance. The default is zero.
  ///   - type: The type to use for the newly constructed instance. The memory
  ///     must be initialized to a value of a type that is layout compatible
  ///     with `type`.
  /// - Returns: A new instance of type `T`, copied from the buffer pointer
  ///   slice's memory.
  @inlinable
  @_alwaysEmitIntoClient
  public func load<T>(fromByteOffset offset: Int = 0, as type: T.Type) -> T {
    let buffer = Base(rebasing: self)
    return buffer.load(fromByteOffset: offset, as: T.self)
  }

  /// Returns a new instance of the given type, read from the
  /// specified offset into the buffer pointer slice's raw memory.
  ///
  /// This function only supports loading trivial types.
  /// A trivial type does not contain any reference-counted property
  /// within its in-memory stored representation.
  /// The memory at `offset` bytes into the buffer slice must be laid out
  /// identically to the in-memory representation of `T`.
  ///
  /// You can use this method to create new values from the buffer pointer's
  /// underlying bytes. The following example creates two new `Int32`
  /// instances from the memory referenced by the buffer pointer `someBytes`.
  /// The bytes for `a` are copied from the first four bytes of `someBytes`,
  /// and the bytes for `b` are copied from the fourth through seventh bytes.
  ///
  ///     let a = someBytes[..<4].loadUnaligned(as: Int32.self)
  ///     let b = someBytes[3...].loadUnaligned(as: Int32.self)
  ///
  /// The memory to read for the new instance must not extend beyond the
  /// memory region represented by the buffer pointer slice---that is,
  /// `offset + MemoryLayout<T>.size` must be less than or equal
  /// to the slice's `count`.
  ///
  /// - Parameters:
  ///   - offset: The offset into the slice's memory, in bytes, at
  ///     which to begin reading data for the new instance. The default is zero.
  ///   - type: The type to use for the newly constructed instance. The memory
  ///     must be initialized to a value of a type that is layout compatible
  ///     with `type`.
  /// - Returns: A new instance of type `T`, copied from the buffer pointer's
  ///   memory.
  @inlinable
  @_alwaysEmitIntoClient
  public func loadUnaligned<T : BitwiseCopyable>(
    fromByteOffset offset: Int = 0,
    as type: T.Type
  ) -> T {
    let buffer = Base(rebasing: self)
    return buffer.loadUnaligned(fromByteOffset: offset, as: T.self)
  }
  @inlinable
  @_alwaysEmitIntoClient
  public func loadUnaligned<T>(
    fromByteOffset offset: Int = 0,
    as type: T.Type
  ) -> T {
    let buffer = Base(rebasing: self)
    return buffer.loadUnaligned(fromByteOffset: offset, as: T.self)
  }

  /// Stores a value's bytes into the buffer pointer slice's raw memory at the
  /// specified byte offset.
  ///
  /// The type `T` to be stored must be a trivial type. The memory must also be
  /// uninitialized, initialized to `T`, or initialized to another trivial
  /// type that is layout compatible with `T`.
  ///
  /// The memory written to must not extend beyond
  /// the memory region represented by the buffer pointer slice---that is,
  /// `offset + MemoryLayout<T>.size` must be less than or equal
  /// to the slice's `count`.
  ///
  /// After calling `storeBytes(of:toByteOffset:as:)`, the memory is
  /// initialized to the raw bytes of `value`. If the memory is bound to a
  /// type `U` that is layout compatible with `T`, then it contains a value of
  /// type `U`. Calling `storeBytes(of:toByteOffset:as:)` does not change the
  /// bound type of the memory.
  ///
  /// - Note: A trivial type can be copied with just a bit-for-bit copy without
  ///   any indirection or reference-counting operations. Generally, native
  ///   Swift types that do not contain strong or weak references or other
  ///   forms of indirection are trivial, as are imported C structs and enums.
  ///
  /// If you need to store into memory a copy of a value of a type that isn't
  /// trivial, you cannot use the `storeBytes(of:toByteOffset:as:)` method.
  /// Instead, you must know either initialize the memory or,
  /// if you know the memory was already bound to `type`, assign to the memory.
  ///
  /// - Parameters:
  ///   - value: The value to store as raw bytes.
  ///   - offset: The offset in bytes into the buffer pointer slice's memory
  ///     to begin writing bytes from the value. The default is zero.
  ///   - type: The type to use for the newly constructed instance. The memory
  ///     must be initialized to a value of a type that is layout compatible
  ///     with `type`.
  @inlinable
  @_alwaysEmitIntoClient
  public func storeBytes<T>(
    of value: T, toByteOffset offset: Int = 0, as type: T.Type
  ) {
    let buffer = Base(rebasing: self)
    buffer.storeBytes(of: value, toByteOffset: offset, as: T.self)
  }
}

extension Slice where Base == UnsafeRawBufferPointer {

  /// Binds this buffer slice’s memory to the specified type and returns
  /// a typed buffer of the bound memory.
  ///
  /// Use the `bindMemory(to:)` method to bind the memory referenced
  /// by this buffer slice to the type `T`. The memory must be uninitialized or
  /// initialized to a type that is layout compatible with `T`. If the memory
  /// is uninitialized, it is still uninitialized after being bound to `T`.
  ///
  /// - Warning: A memory location may only be bound to one type at a time. The
  ///   behavior of accessing memory as a type unrelated to its bound type is
  ///   undefined.
  ///
  /// - Parameters:
  ///   - type: The type `T` to bind the memory to.
  /// - Returns: A typed buffer of the newly bound memory. The memory in this
  ///   region is bound to `T`, but has not been modified in any other way.
  ///   The typed buffer references `self.count / MemoryLayout<T>.stride`
  ///   instances of `T`.
  @discardableResult
  @inlinable
  @_alwaysEmitIntoClient
  public func bindMemory<T>(to type: T.Type) -> UnsafeBufferPointer<T> {
    let buffer = Base(rebasing: self)
    return buffer.bindMemory(to: T.self)
  }

  /// Executes the given closure while temporarily binding the buffer slice to
  /// instances of type `T`.
  ///
  /// Use this method when you have a buffer slice to raw memory and you need
  /// to access that memory as instances of a given type `T`. Accessing
  /// memory as a type `T` requires that the memory be bound to that type.
  /// A memory location may only be bound to one type at a time, so accessing
  /// the same memory as an unrelated type without first rebinding the memory
  /// is undefined.
  ///
  /// Any instance of `T` within the re-bound region may be initialized or
  /// uninitialized. The memory underlying any individual instance of `T`
  /// must have the same initialization state (i.e.  initialized or
  /// uninitialized.) Accessing a `T` whose underlying memory
  /// is in a mixed initialization state shall be undefined behaviour.
  ///
  /// If the byte count of the original buffer slice is not a multiple of
  /// the stride of `T`, then the re-bound buffer is shorter
  /// than the original buffer.
  ///
  /// After executing `body`, this method rebinds memory back to its original
  /// binding state. This can be unbound memory, or bound to a different type.
  ///
  /// - Note: The buffer slice's start address must match the
  ///   alignment of `T` (as reported by `MemoryLayout<T>.alignment`). That is,
  ///   `Int(bitPattern: base.baseAddress+startIndex) % MemoryLayout<T>.alignment`
  ///   must equal zero.
  ///
  /// - Note: A raw buffer slice may represent memory that has been bound to
  ///   a type. If that is the case, then `T` must be layout compatible with the
  ///   type to which the memory has been bound. This requirement does not
  ///   apply if the raw buffer represents memory that has not been bound
  ///   to any type.
  ///
  /// - Parameters:
  ///   - type: The type to temporarily bind the memory referenced by this
  ///     buffer slice.
  ///   - body: A closure that takes a typed pointer to the
  ///     same memory as this pointer, only bound to type `T`. The closure's
  ///     pointer argument is valid only for the duration of the closure's
  ///     execution. If `body` has a return value, that value is also used as
  ///     the return value for the `withMemoryRebound(to:capacity:_:)` method.
  ///   - buffer: The buffer temporarily bound to instances of `T`.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  @_alwaysEmitIntoClient
  public func withMemoryRebound<T, Result, E: Error>(
    to type: T.Type, _ body: (UnsafeBufferPointer<T>) throws(E) -> Result
  ) throws(E) -> Result {
    let buffer = Base(rebasing: self)
    return try buffer.withMemoryRebound(to: T.self, body)
  }

  /// Returns a typed buffer to the memory referenced by this buffer slice,
  /// assuming that the memory is already bound to the specified type.
  ///
  /// Use this method when you have a raw buffer to memory that has already
  /// been bound to the specified type. The memory starting at this pointer
  /// must be bound to the type `T`. Accessing memory through the returned
  /// pointer is undefined if the memory has not been bound to `T`. To bind
  /// memory to `T`, use `bindMemory(to:capacity:)` instead of this method.
  ///
  /// - Note: The buffer slice's start address must match the
  ///   alignment of `T` (as reported by `MemoryLayout<T>.alignment`). That is,
  ///   `Int(bitPattern: base.baseAddress+startIndex) % MemoryLayout<T>.alignment`
  ///   must equal zero.
  ///
  /// - Parameter to: The type `T` that the memory has already been bound to.
  /// - Returns: A typed pointer to the same memory as this raw pointer.
  @inlinable
  @_alwaysEmitIntoClient
  public func assumingMemoryBound<T>(
    to type: T.Type
  ) -> UnsafeBufferPointer<T> {
    let buffer = Base(rebasing: self)
    return buffer.assumingMemoryBound(to: T.self)
  }

  /// Returns a new instance of the given type, read from the
  /// specified offset into the buffer pointer slice's raw memory.
  ///
  /// The memory at `offset` bytes into this buffer pointer slice
  /// must be properly aligned for accessing `T` and initialized to `T` or
  /// another type that is layout compatible with `T`.
  ///
  /// You can use this method to create new values from the underlying
  /// buffer pointer's bytes. The following example creates two new `Int32`
  /// instances from the memory referenced by the buffer pointer `someBytes`.
  /// The bytes for `a` are copied from the first four bytes of `someBytes`,
  /// and the bytes for `b` are copied from the next four bytes.
  ///
  ///     let a = someBytes[0..<4].load(as: Int32.self)
  ///     let b = someBytes[4..<8].load(as: Int32.self)
  ///
  /// The memory to read for the new instance must not extend beyond the
  /// memory region represented by the buffer pointer slice---that is,
  /// `offset + MemoryLayout<T>.size` must be less than or equal
  /// to the slice's `count`.
  ///
  /// - Parameters:
  ///   - offset: The offset into the slice's memory, in bytes, at
  ///     which to begin reading data for the new instance. The default is zero.
  ///   - type: The type to use for the newly constructed instance. The memory
  ///     must be initialized to a value of a type that is layout compatible
  ///     with `type`.
  /// - Returns: A new instance of type `T`, copied from the buffer pointer
  ///   slice's memory.
  @inlinable
  @_alwaysEmitIntoClient
  public func load<T>(fromByteOffset offset: Int = 0, as type: T.Type) -> T {
    let buffer = Base(rebasing: self)
    return buffer.load(fromByteOffset: offset, as: T.self)
  }

  /// Returns a new instance of the given type, read from the
  /// specified offset into the buffer pointer slice's raw memory.
  ///
  /// This function only supports loading trivial types.
  /// A trivial type does not contain any reference-counted property
  /// within its in-memory stored representation.
  /// The memory at `offset` bytes into the buffer slice must be laid out
  /// identically to the in-memory representation of `T`.
  ///
  /// You can use this method to create new values from the buffer pointer's
  /// underlying bytes. The following example creates two new `Int32`
  /// instances from the memory referenced by the buffer pointer `someBytes`.
  /// The bytes for `a` are copied from the first four bytes of `someBytes`,
  /// and the bytes for `b` are copied from the fourth through seventh bytes.
  ///
  ///     let a = someBytes[..<4].loadUnaligned(as: Int32.self)
  ///     let b = someBytes[3...].loadUnaligned(as: Int32.self)
  ///
  /// The memory to read for the new instance must not extend beyond the
  /// memory region represented by the buffer pointer slice---that is,
  /// `offset + MemoryLayout<T>.size` must be less than or equal
  /// to the slice's `count`.
  ///
  /// - Parameters:
  ///   - offset: The offset into the slice's memory, in bytes, at
  ///     which to begin reading data for the new instance. The default is zero.
  ///   - type: The type to use for the newly constructed instance. The memory
  ///     must be initialized to a value of a type that is layout compatible
  ///     with `type`.
  /// - Returns: A new instance of type `T`, copied from the buffer pointer's
  ///   memory.
  @inlinable
  @_alwaysEmitIntoClient
  public func loadUnaligned<T : BitwiseCopyable>(
    fromByteOffset offset: Int = 0,
    as type: T.Type
  ) -> T {
    let buffer = Base(rebasing: self)
    return buffer.loadUnaligned(fromByteOffset: offset, as: T.self)
  }
  @inlinable
  @_alwaysEmitIntoClient
  public func loadUnaligned<T>(
    fromByteOffset offset: Int = 0,
    as type: T.Type
  ) -> T {
    let buffer = Base(rebasing: self)
    return buffer.loadUnaligned(fromByteOffset: offset, as: T.self)
  }
}

extension Slice {
  /// Executes the given closure while temporarily binding the memory referenced
  /// by this buffer slice to the given type.
  ///
  /// Use this method when you have a buffer slice of memory bound to one type
  /// and you need to access that memory as a buffer of another type. Accessing
  /// memory as type `T` requires that the memory be bound to that type. A
  /// memory location may only be bound to one type at a time, so accessing
  /// the same memory as an unrelated type without first rebinding the memory
  /// is undefined.
  ///
  /// The number of instances of `T` referenced by the rebound buffer may be
  /// different than the number of instances of `Element` referenced by the
  /// original buffer slice. The number of instances of `T` will be calculated
  /// at runtime.
  ///
  /// Any instance of `T` within the re-bound region may be initialized or
  /// uninitialized. Every instance of `Pointee` overlapping with a given
  /// instance of `T` should have the same initialization state (i.e.
  /// initialized or uninitialized.) Accessing a `T` whose underlying
  /// `Pointee` storage is in a mixed initialization state shall be
  /// undefined behaviour.
  ///
  /// Because this range of memory is no longer bound to its `Element` type
  /// while the `body` closure executes, do not access memory using the
  /// original buffer slice from within `body`. Instead,
  /// use the `body` closure's buffer argument to access the values
  /// in memory as instances of type `T`.
  ///
  /// After executing `body`, this method rebinds memory back to the original
  /// `Element` type.
  ///
  /// - Note: Only use this method to rebind the buffer slice's memory to a type
  ///   that is layout compatible with the currently bound `Element` type.
  ///   The stride of the temporary type (`T`) may be an integer multiple
  ///   or a whole fraction of `Element`'s stride.
  ///   To bind a region of memory to a type that does not match these
  ///   requirements, convert the buffer to a raw buffer and use the
  ///   `withMemoryRebound(to:)` method on the raw buffer.
  ///   If `T` and `Element` have different alignments, this buffer slice
  ///   must be aligned with the larger of the two alignments.
  ///
  /// - Parameters:
  ///   - type: The type to temporarily bind the memory referenced by this
  ///     buffer slice. The type `T` must be layout compatible
  ///     with the pointer's `Element` type.
  ///   - body: A closure that takes a typed buffer to the
  ///     same memory as this buffer slice, only bound to type `T`. The buffer
  ///     parameter contains a number of complete instances of `T` based
  ///     on the capacity of the original buffer and the stride of `Element`.
  ///     The closure's buffer argument is valid only for the duration of the
  ///     closure's execution. If `body` has a return value, that value
  ///     is also used as the return value for the `withMemoryRebound(to:_:)`
  ///     method.
  ///   - buffer: The buffer temporarily bound to `T`.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  @_alwaysEmitIntoClient
  public func withMemoryRebound<T, Result, Element>(
    to type: T.Type, _ body: (UnsafeBufferPointer<T>) throws -> Result
  ) rethrows -> Result where Base == UnsafeBufferPointer<Element> {
    let rebased = UnsafeBufferPointer<Element>(rebasing: self)
    return try rebased.withMemoryRebound(to: T.self, body)
  }
}

extension Slice {

  /// Initializes every element in this buffer slice's memory to
  /// a copy of the given value.
  ///
  /// The destination memory must be uninitialized or the buffer's `Element`
  /// must be a trivial type. After a call to `initialize(repeating:)`, the
  /// entire region of memory referenced by this buffer slice is initialized.
  ///
  /// - Parameter repeatedValue: The value with which to initialize this
  ///   buffer slice's memory.
  @inlinable
  @_alwaysEmitIntoClient
  public func initialize<Element>(repeating repeatedValue: Element)
    where Base == UnsafeMutableBufferPointer<Element> {
    Base(rebasing: self).initialize(repeating: repeatedValue)
  }

  /// Initializes the buffer slice's memory with the given elements.
  ///
  /// Prior to calling the `initialize(from:)` method on a buffer slice,
  /// the memory it references must be uninitialized,
  /// or the `Element` type must be a trivial type. After the call,
  /// the memory referenced by the buffer slice up to, but not including,
  /// the returned index is initialized.
  /// The buffer slice must contain sufficient memory to accommodate
  /// `source.underestimatedCount`.
  ///
  /// The returned index is the position of the next uninitialized element
  /// in the buffer slice, which is one past the last element written.
  /// If `source` contains no elements, the returned index is equal to
  /// the buffer's `startIndex`. If `source` contains an equal or greater number
  /// of elements than the buffer slice can hold, the returned index is equal to
  /// the buffer's `endIndex`.
  ///
  /// - Parameter source: A sequence of elements with which to initialize the
  ///   buffer.
  /// - Returns: An iterator to any elements of `source` that didn't fit in the
  ///   buffer, and an index to the next uninitialized element in the buffer.
  @inlinable
  @_alwaysEmitIntoClient
  public func initialize<S>(
    from source: S
  ) -> (unwritten: S.Iterator, index: Index)
    where S: Sequence, Base == UnsafeMutableBufferPointer<S.Element> {
    let buffer = Base(rebasing: self)
    let (iterator, index) = buffer.initialize(from: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return (iterator, startIndex.advanced(by: distance))
  }

  /// Initializes the buffer slice's memory with with
  /// every element of the source.
  ///
  /// Prior to calling the `initialize(fromContentsOf:)` method
  /// on a buffer slice, the memory it references must be uninitialized,
  /// or the `Element` type must be a trivial type. After the call,
  /// the memory referenced by the buffer slice up to, but not including,
  /// the returned index is initialized.
  /// The buffer slice must reference enough memory to accommodate
  /// `source.count` elements.
  ///
  /// The returned index is the index of the next uninitialized element
  /// in the buffer slice, one past the index of the last element written.
  /// If `source` contains no elements, the returned index is equal to
  /// the buffer slice's `startIndex`. If `source` contains as many elements
  /// as the buffer slice can hold, the returned index is equal to
  /// to the slice's `endIndex`.
  ///
  /// - Precondition: `self.count` >= `source.count`
  ///
  /// - Note: The memory regions referenced by `source` and this buffer slice
  ///     must not overlap.
  ///
  /// - Parameter source: A collection of elements to be used to
  ///     initialize the buffer slice's storage.
  /// - Returns: The index one past the last element of the buffer slice
  ///    initialized by this function.
  @inlinable
  @_alwaysEmitIntoClient
  public func initialize<Element>(
    fromContentsOf source: some Collection<Element>
  ) -> Index where Base == UnsafeMutableBufferPointer<Element> {
    let buffer = Base(rebasing: self)
    let index = buffer.initialize(fromContentsOf: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return startIndex.advanced(by: distance)
  }

  /// Updates every element of this buffer slice's initialized memory.
  ///
  /// The buffer slice’s memory must be initialized or its `Element`
  /// must be a trivial type.
  ///
  /// - Note: All buffer elements must already be initialized.
  ///
  /// - Parameters:
  ///   - repeatedValue: The value used when updating this pointer's memory.
  @inlinable
  @_alwaysEmitIntoClient
  public func update<Element>(repeating repeatedValue: Element)
    where Base == UnsafeMutableBufferPointer<Element> {
    Base(rebasing: self).update(repeating: repeatedValue)
  }

  /// Updates the buffer slice's initialized memory with the given elements.
  ///
  /// The buffer slice's memory must be initialized or its `Element` type
  /// must be a trivial type.
  ///
  /// - Parameter source: A sequence of elements to be used to update
  ///   the contents of the buffer slice.
  /// - Returns: An iterator to any elements of `source` that didn't fit in the
  ///   buffer slice, and the index one past the last updated element.
  @inlinable
  @_alwaysEmitIntoClient
  public func update<S>(
    from source: S
  ) -> (unwritten: S.Iterator, index: Index)
    where S: Sequence, Base == UnsafeMutableBufferPointer<S.Element> {
    let buffer = Base(rebasing: self)
    let (iterator, index) = buffer.update(from: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return (iterator, startIndex.advanced(by: distance))
  }

  /// Updates the buffer slice's initialized memory with
  /// every element of the source.
  ///
  /// Prior to calling the `update(fromContentsOf:)` method on a buffer
  /// slice, the first `source.count` elements of the referenced memory must be
  /// initialized, or the `Element` type must be a trivial type.
  /// The buffer slice must reference enough initialized memory to accommodate
  /// `source.count` elements.
  ///
  /// The returned index is one past the index of the last element updated. If
  /// `source` contains no elements, the returned index is the buffer slice's
  /// `startIndex`. If `source` contains as many elements as the buffer slice
  /// can hold, the returned index is the buffer slice's `endIndex`.
  ///
  /// - Note: The memory regions referenced by `source` and this buffer slice
  ///     may overlap.
  ///
  /// - Precondition: `self.count` >= `source.count`
  ///
  /// - Parameter source: A collection of elements to be used to update
  ///     the buffer's contents.
  /// - Returns: An index one past the index of the last element updated.
  @inlinable
  @_alwaysEmitIntoClient
  public func update<Element>(
    fromContentsOf source: some Collection<Element>
  ) -> Index where Base == UnsafeMutableBufferPointer<Element> {
    let buffer = Base(rebasing: self)
    let index = buffer.update(fromContentsOf: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return startIndex.advanced(by: distance)
  }

  /// Moves every element of an initialized source buffer into the
  /// uninitialized memory referenced by this buffer slice, leaving the
  /// source memory uninitialized and this buffer slice's memory initialized.
  ///
  /// Prior to calling the `moveInitialize(fromContentsOf:)` method on a
  /// buffer slice, the memory it references must be uninitialized,
  /// or its `Element` type must be a trivial type. After the call,
  /// the memory referenced by the buffer slice up to, but not including,
  /// the returned index is initialized. The memory referenced by
  /// `source` is uninitialized after the function returns.
  /// The buffer slice must reference enough memory to accommodate
  /// `source.count` elements.
  ///
  /// The returned index is the position of the next uninitialized element
  /// in the buffer slice, one past the index of the last element written.
  /// If `source` contains no elements, the returned index is equal to the
  /// slice's `startIndex`. If `source` contains as many elements as the slice
  /// can hold, the returned index is equal to the slice's `endIndex`.
  ///
  /// - Note: The memory regions referenced by `source` and this buffer slice
  ///     may overlap.
  ///
  /// - Precondition: `self.count` >= `source.count`
  ///
  /// - Parameter source: A buffer containing the values to copy.
  ///     The memory region underlying `source` must be initialized.
  /// - Returns: The index one past the last element of the buffer slice
  ///    initialized by this function.
  @inlinable
  @_alwaysEmitIntoClient
  public func moveInitialize<Element>(
    fromContentsOf source: UnsafeMutableBufferPointer<Element>
  ) -> Index where Base == UnsafeMutableBufferPointer<Element> {
    let buffer = Base(rebasing: self)
    let index = buffer.moveInitialize(fromContentsOf: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return startIndex.advanced(by: distance)
  }

  /// Moves every element of an initialized source buffer slice into the
  /// uninitialized memory referenced by this buffer slice, leaving the
  /// source memory uninitialized and this buffer slice's memory initialized.
  ///
  /// Prior to calling the `moveInitialize(fromContentsOf:)` method on a
  /// buffer slice, the memory it references must be uninitialized,
  /// or its `Element` type must be a trivial type. After the call,
  /// the memory referenced by the buffer slice up to, but not including,
  /// the returned index is initialized. The memory referenced by
  /// `source` is uninitialized after the function returns.
  /// The buffer slice must reference enough memory to accommodate
  /// `source.count` elements.
  ///
  /// The returned index is the position of the next uninitialized element
  /// in the buffer slice, one past the index of the last element written.
  /// If `source` contains no elements, the returned index is equal to the
  /// slice's `startIndex`. If `source` contains as many elements as the slice
  /// can hold, the returned index is equal to the slice's `endIndex`.
  ///
  /// - Note: The memory regions referenced by `source` and this buffer slice
  ///     may overlap.
  ///
  /// - Precondition: `self.count` >= `source.count`
  ///
  /// - Parameter source: A buffer slice containing the values to copy.
  ///     The memory region underlying `source` must be initialized.
  /// - Returns: The index one past the last element of the buffer slice
  ///    initialized by this function.
  @inlinable
  @_alwaysEmitIntoClient
  public func moveInitialize<Element>(
    fromContentsOf source: Slice<UnsafeMutableBufferPointer<Element>>
  ) -> Index where Base == UnsafeMutableBufferPointer<Element> {
    let buffer = Base(rebasing: self)
    let index = buffer.moveInitialize(fromContentsOf: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return startIndex.advanced(by: distance)
  }

  /// Updates this buffer slice's initialized memory initialized memory by
  /// moving every element from the source buffer,
  /// leaving the source memory uninitialized.
  ///
  /// The region of memory starting at the beginning of this buffer slice and
  /// covering `source.count` instances of its `Element` type  must be
  /// initialized, or its `Element` type must be a trivial type.
  /// After calling `moveUpdate(fromContentsOf:)`,
  /// the region of memory underlying `source` is uninitialized.
  /// The buffer slice must reference enough initialized memory
  /// to accommodate `source.count` elements.
  ///
  /// The returned index is one past the index of the last element updated.
  /// If `source` contains no elements, the returned index is equal to the
  /// buffer's `startIndex`. If `source` contains as many elements as the buffer
  /// slice can hold, the returned index is equal to the slice's `endIndex`.
  ///
  /// - Note: The memory regions referenced by `source` and this buffer slice
  ///     must not overlap.
  ///
  /// - Precondition: `self.count` >= `source.count`
  ///
  /// - Parameter source: A buffer containing the values to move.
  ///     The memory region underlying `source` must be initialized.
  /// - Returns: An index one past the index of the last element updated.
  @inlinable
  @_alwaysEmitIntoClient
  public func moveUpdate<Element>(
    fromContentsOf source: UnsafeMutableBufferPointer<Element>
  ) -> Index where Base == UnsafeMutableBufferPointer<Element> {
    let buffer = Base(rebasing: self)
    let index = buffer.moveUpdate(fromContentsOf: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return startIndex.advanced(by: distance)
  }

  /// Updates this buffer slice's initialized memory initialized memory by
  /// moving every element from the source buffer slice,
  /// leaving the source memory uninitialized.
  ///
  /// The region of memory starting at the beginning of this buffer slice and
  /// covering `source.count` instances of its `Element` type  must be
  /// initialized, or its `Element` type must be a trivial type.
  /// After calling `moveUpdate(fromContentsOf:)`,
  /// the region of memory underlying `source` is uninitialized.
  /// The buffer slice must reference enough initialized memory
  /// to accommodate `source.count` elements.
  ///
  /// The returned index is one past the index of the last element updated.
  /// If `source` contains no elements, the returned index is equal to the
  /// buffer's `startIndex`. If `source` contains as many elements as the buffer
  /// slice can hold, the returned index is equal to the slice's `endIndex`.
  ///
  /// - Note: The memory regions referenced by `source` and this buffer slice
  ///     must not overlap.
  ///
  /// - Precondition: `self.count` >= `source.count`
  ///
  /// - Parameter source: A buffer slice containing the values to move.
  ///     The memory region underlying `source` must be initialized.
  /// - Returns: An index one past the index of the last element updated.
  @inlinable
  @_alwaysEmitIntoClient
  public func moveUpdate<Element>(
    fromContentsOf source: Slice<UnsafeMutableBufferPointer<Element>>
  ) -> Index where Base == UnsafeMutableBufferPointer<Element> {
    let buffer = Base(rebasing: self)
    let index = buffer.moveUpdate(fromContentsOf: source)
    let distance = buffer.distance(from: buffer.startIndex, to: index)
    return startIndex.advanced(by: distance)
  }

  /// Deinitializes every instance in this buffer slice.
  ///
  /// The region of memory underlying this buffer slice must be fully
  /// initialized. After calling `deinitialize(count:)`, the memory
  /// is uninitialized, but still bound to the `Element` type.
  ///
  /// - Note: All buffer elements must already be initialized.
  ///
  /// - Returns: A raw buffer to the same range of memory as this buffer.
  ///   The range of memory is still bound to `Element`.
  @discardableResult
  @inlinable
  @_alwaysEmitIntoClient
  public func deinitialize<Element>() -> UnsafeMutableRawBufferPointer
    where Base == UnsafeMutableBufferPointer<Element> {
    Base(rebasing: self).deinitialize()
  }

  /// Initializes the element at `index` to the given value.
  ///
  /// The memory underlying the destination element must be uninitialized,
  /// or `Element` must be a trivial type. After a call to `initialize(to:)`,
  /// the memory underlying this element of the buffer slice is initialized.
  ///
  /// - Parameters:
  ///   - value: The value used to initialize the buffer element's memory.
  ///   - index: The index of the element to initialize
  @inlinable
  @_alwaysEmitIntoClient
  public func initializeElement<Element>(at index: Int, to value: Element)
    where Base == UnsafeMutableBufferPointer<Element> {
    assert(startIndex <= index && index < endIndex)
    base.baseAddress.unsafelyUnwrapped.advanced(by: index).initialize(to: value)
  }

  /// Retrieves and returns the element at `index`,
  /// leaving that element's underlying memory uninitialized.
  ///
  /// The memory underlying the element at `index` must be initialized.
  /// After calling `moveElement(from:)`, the memory underlying this element
  /// of the buffer slice is uninitialized, and still bound to type `Element`.
  ///
  /// - Parameters:
  ///   - index: The index of the buffer element to retrieve and deinitialize.
  /// - Returns: The instance referenced by this index in this buffer.
  @inlinable
  @_alwaysEmitIntoClient
  public func moveElement<Element>(from index: Index) -> Element
    where Base == UnsafeMutableBufferPointer<Element> {
    assert(startIndex <= index && index < endIndex)
    return base.baseAddress.unsafelyUnwrapped.advanced(by: index).move()
  }

  /// Deinitializes the memory underlying the element at `index`.
  ///
  /// The memory underlying the element at `index` must be initialized.
  /// After calling `deinitializeElement()`, the memory underlying this element
  /// of the buffer slice is uninitialized, and still bound to type `Element`.
  ///
  /// - Parameters:
  ///   - index: The index of the buffer element to deinitialize.
  @inlinable
  @_alwaysEmitIntoClient
  public func deinitializeElement<Element>(at index: Base.Index)
    where Base == UnsafeMutableBufferPointer<Element> {
    assert(startIndex <= index && index < endIndex)
    base.baseAddress.unsafelyUnwrapped.advanced(by: index).deinitialize(count: 1)
  }

  /// Executes the given closure while temporarily binding the memory referenced
  /// by this buffer slice to the given type.
  ///
  /// Use this method when you have a buffer slice of memory bound to one type
  /// and you need to access that memory as a buffer of another type. Accessing
  /// memory as type `T` requires that the memory be bound to that type. A
  /// memory location may only be bound to one type at a time, so accessing
  /// the same memory as an unrelated type without first rebinding the memory
  /// is undefined.
  ///
  /// The number of instances of `T` referenced by the rebound buffer may be
  /// different than the number of instances of `Element` referenced by the
  /// original buffer slice. The number of instances of `T` will be calculated
  /// at runtime.
  ///
  /// Any instance of `T` within the re-bound region may be initialized or
  /// uninitialized. Every instance of `Pointee` overlapping with a given
  /// instance of `T` should have the same initialization state (i.e.
  /// initialized or uninitialized.) Accessing a `T` whose underlying
  /// `Pointee` storage is in a mixed initialization state shall be
  /// undefined behaviour.
  ///
  /// Because this range of memory is no longer bound to its `Element` type
  /// while the `body` closure executes, do not access memory using the
  /// original buffer slice from within `body`. Instead,
  /// use the `body` closure's buffer argument to access the values
  /// in memory as instances of type `T`.
  ///
  /// After executing `body`, this method rebinds memory back to the original
  /// `Element` type.
  ///
  /// - Note: Only use this method to rebind the buffer slice's memory to a type
  ///   that is layout compatible with the currently bound `Element` type.
  ///   The stride of the temporary type (`T`) may be an integer multiple
  ///   or a whole fraction of `Element`'s stride.
  ///   To bind a region of memory to a type that does not match these
  ///   requirements, convert the buffer slice to a raw buffer and use the
  ///   raw buffer's `withMemoryRebound(to:)` method.
  ///   If `T` and `Element` have different alignments, this buffer slice
  ///   must be aligned with the larger of the two alignments.
  ///
  /// - Parameters:
  ///   - type: The type to temporarily bind the memory referenced by this
  ///     buffer slice. The type `T` must be layout compatible
  ///     with the pointer's `Element` type.
  ///   - body: A closure that takes a typed buffer to the
  ///     same memory as this buffer slice, only bound to type `T`. The buffer
  ///     parameter contains a number of complete instances of `T` based
  ///     on the capacity of the original buffer and the stride of `Element`.
  ///     The closure's buffer argument is valid only for the duration of the
  ///     closure's execution. If `body` has a return value, that value
  ///     is also used as the return value for the `withMemoryRebound(to:_:)`
  ///     method.
  ///   - buffer: The buffer temporarily bound to `T`.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  @_alwaysEmitIntoClient
  public func withMemoryRebound<T, Result, Element>(
    to type: T.Type, _ body: (UnsafeMutableBufferPointer<T>) throws -> Result
  ) rethrows -> Result where Base == UnsafeMutableBufferPointer<Element> {
    try Base(rebasing: self).withMemoryRebound(to: T.self, body)
  }

  @inlinable
  @_alwaysEmitIntoClient
  public func withContiguousMutableStorageIfAvailable<R, Element>(
    _ body: (_ buffer: inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R? where Base == UnsafeMutableBufferPointer<Element> {
    try base.withContiguousStorageIfAvailable { buffer in
      let start = base.baseAddress?.advanced(by: startIndex)
      var slice = UnsafeMutableBufferPointer(start: start, count: count)
      let (b,c) = (slice.baseAddress, slice.count)
      defer {
        _precondition(
          slice.baseAddress == b && slice.count == c,
          "withContiguousMutableStorageIfAvailable: replacing the buffer is not allowed")
      }
      return try body(&slice)
    }
  }
}
