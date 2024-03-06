//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// The memory layout of a type, describing its size, stride, and alignment.
///
/// You can use `MemoryLayout` as a source of information about a type when
/// allocating or binding memory using raw pointers. The following example
/// declares a `Point` type with `x` and `y` coordinates and a Boolean
/// `isFilled` property.
///
///     struct Point {
///         let x: Double
///         let y: Double
///         let isFilled: Bool
///     }
///
/// The size, stride, and alignment of the `Point` type are accessible as
/// static properties of `MemoryLayout<Point>`.
///
///     // MemoryLayout<Point>.size == 17
///     // MemoryLayout<Point>.stride == 24
///     // MemoryLayout<Point>.alignment == 8
///
/// Always use a multiple of a type's `stride` instead of its `size` when
/// allocating memory or accounting for the distance between instances in
/// memory. This example allocates uninitialized raw memory with space
/// for four instances of `Point`.
///
///     let count = 4
///     let pointPointer = UnsafeMutableRawPointer.allocate(
///             byteCount: count * MemoryLayout<Point>.stride,
///             alignment: MemoryLayout<Point>.alignment)
@frozen // namespace
public enum MemoryLayout<T: ~Copyable> {}

extension MemoryLayout where T: ~Copyable {
  /// The contiguous memory footprint of `T`, in bytes.
  ///
  /// A type's size does not include any dynamically allocated or out of line
  /// storage. In particular, `MemoryLayout<T>.size`, when `T` is a class
  /// type, is the same regardless of how many stored properties `T` has.
  ///
  /// When allocating memory for multiple instances of `T` using an unsafe
  /// pointer, use a multiple of the type's stride instead of its size.
  @_transparent
  @_alwaysEmitIntoClient
  public static var size: Int {
    return Int(Builtin.sizeof(T.self))
  }

  /// The number of bytes from the start of one instance of `T` to the start of
  /// the next when stored in contiguous memory or in an `Array<T>`.
  ///
  /// This is the same as the number of bytes moved when an `UnsafePointer<T>`
  /// instance is incremented. `T` may have a lower minimal alignment that
  /// trades runtime performance for space efficiency. This value is always
  /// positive.
  @_transparent
  @_alwaysEmitIntoClient
  public static var stride: Int {
    return Int(Builtin.strideof(T.self))
  }

  /// The default memory alignment of `T`, in bytes.
  ///
  /// Use the `alignment` property for a type when allocating memory using an
  /// unsafe pointer. This value is always positive.
  @_transparent
  @_alwaysEmitIntoClient
  public static var alignment: Int {
    return Int(Builtin.alignof(T.self))
  }
}

@available(*, unavailable)
extension MemoryLayout : _BitwiseCopyable {}

extension MemoryLayout {
  // TODO: Merge this back into the noncopyable variant once we have @_preInverseGenerics
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 2)
  @usableFromInline
  internal static var size: Int {
    return Int(Builtin.sizeof(T.self))
  }

  // TODO: Merge this back into the noncopyable variant once we have @_preInverseGenerics
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 2)
  @usableFromInline
  internal static var stride: Int {
    return Int(Builtin.strideof(T.self))
  }

  // TODO: Merge this back into the noncopyable variant once we have @_preInverseGenerics
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 2)
  @usableFromInline
  internal static var alignment: Int {
    return Int(Builtin.alignof(T.self))
  }
}

extension MemoryLayout where T: ~Copyable {
  /// Returns the contiguous memory footprint of the given instance.
  ///
  /// The result does not include any dynamically allocated or out of line
  /// storage. In particular, pointers and class instances all have the same
  /// contiguous memory footprint, regardless of the size of the referenced
  /// data.
  ///
  /// When you have a type instead of an instance, use the
  /// `MemoryLayout<T>.size` static property instead.
  ///
  ///     let x: Int = 100
  ///
  ///     // Finding the size of a value's type
  ///     let s = MemoryLayout.size(ofValue: x)
  ///     // s == 8
  ///
  ///     // Finding the size of a type directly
  ///     let t = MemoryLayout<Int>.size
  ///     // t == 8
  ///
  /// - Parameter value: A value representative of the type to describe.
  /// - Returns: The size, in bytes, of the given value's type.
  @_transparent
  @_alwaysEmitIntoClient
  public static func size(ofValue value: borrowing T) -> Int {
    return MemoryLayout.size
  }

  /// Returns the number of bytes from the start of one instance of `T` to the
  /// start of the next when stored in contiguous memory or in an `Array<T>`.
  ///
  /// This is the same as the number of bytes moved when an `UnsafePointer<T>`
  /// instance is incremented. `T` may have a lower minimal alignment that
  /// trades runtime performance for space efficiency. The result is always
  /// positive.
  ///
  /// When you have a type instead of an instance, use the
  /// `MemoryLayout<T>.stride` static property instead.
  ///
  ///     let x: Int = 100
  ///
  ///     // Finding the stride of a value's type
  ///     let s = MemoryLayout.stride(ofValue: x)
  ///     // s == 8
  ///
  ///     // Finding the stride of a type directly
  ///     let t = MemoryLayout<Int>.stride
  ///     // t == 8
  ///
  /// - Parameter value: A value representative of the type to describe.
  /// - Returns: The stride, in bytes, of the given value's type.
  @_transparent
  @_alwaysEmitIntoClient
  public static func stride(ofValue value: borrowing T) -> Int {
    return MemoryLayout.stride
  }

  /// Returns the default memory alignment of `T`.
  ///
  /// Use a type's alignment when allocating memory using an unsafe pointer.
  ///
  /// When you have a type instead of an instance, use the
  /// `MemoryLayout<T>.stride` static property instead.
  ///
  ///     let x: Int = 100
  ///
  ///     // Finding the alignment of a value's type
  ///     let s = MemoryLayout.alignment(ofValue: x)
  ///     // s == 8
  ///
  ///     // Finding the alignment of a type directly
  ///     let t = MemoryLayout<Int>.alignment
  ///     // t == 8
  ///
  /// - Parameter value: A value representative of the type to describe.
  /// - Returns: The default memory alignment, in bytes, of the given value's
  ///   type. This value is always positive.
  @_transparent
  @_alwaysEmitIntoClient
  public static func alignment(ofValue value: borrowing T) -> Int {
    return MemoryLayout.alignment
  }
}

extension MemoryLayout {
  // TODO: Merge this back into the noncopyable variant once we have @_preInverseGenerics
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 2)
  @usableFromInline
  internal static func size(ofValue value: borrowing T) -> Int {
    return MemoryLayout.size
  }

  // TODO: Merge this back into the noncopyable variant once we have @_preInverseGenerics
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 2)
  @usableFromInline
  internal static func stride(ofValue value: borrowing T) -> Int {
    return MemoryLayout.stride
  }

  // TODO: Merge this back into the noncopyable variant once we have @_preInverseGenerics
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 2)
  @usableFromInline
  internal static func alignment(ofValue value: borrowing T) -> Int {
    return MemoryLayout.alignment
  }
}

extension MemoryLayout {
  /// Returns the offset of an inline stored property within a type's in-memory
  /// representation.
  ///
  /// You can use this method to find the distance in bytes that can be added
  /// to a pointer of type `T` to get a pointer to the property referenced by
  /// `key`. The offset is available only if the given key refers to inline,
  /// directly addressable storage within the in-memory representation of `T`.
  ///
  /// If the return value of this method is non-`nil`, then accessing the value
  /// by key path or by an offset pointer are equivalent. For example, for a
  /// variable `root` of type `T`, a key path `key` of type
  /// `WritableKeyPath<T, U>`, and a `value` of type `U`:
  ///
  ///     // Mutation through the key path
  ///     root[keyPath: key] = value
  ///
  ///     // Mutation through the offset pointer
  ///     withUnsafeMutableBytes(of: &root) { bytes in
  ///         let offset = MemoryLayout<T>.offset(of: key)!
  ///         let rawPointerToValue = bytes.baseAddress! + offset
  ///         let pointerToValue = rawPointerToValue.assumingMemoryBound(to: U.self)
  ///         pointerToValue.pointee = value
  ///     }
  ///
  /// A property has inline, directly addressable storage when it is a stored
  /// property for which no additional work is required to extract or set the
  /// value. Properties are not directly accessible if they trigger any
  /// `didSet` or `willSet` accessors, perform any representation changes such
  /// as bridging or closure reabstraction, or mask the value out of
  /// overlapping storage as for packed bitfields. In addition, because class
  /// instance properties are always stored out-of-line, their positions are
  /// not accessible using `offset(of:)`.
  ///
  /// For example, in the `ProductCategory` type defined here, only
  /// `\.updateCounter`, `\.identifier`, and `\.identifier.name` refer to
  /// properties with inline, directly addressable storage:
  ///
  ///     struct ProductCategory {
  ///         struct Identifier {
  ///             var name: String              // addressable
  ///         }
  ///
  ///         var identifier: Identifier        // addressable
  ///         var updateCounter: Int            // addressable
  ///         var products: [Product] {         // not addressable: didSet handler
  ///             didSet { updateCounter += 1 }
  ///         }
  ///         var productCount: Int {           // not addressable: computed property
  ///             return products.count
  ///         }
  ///     }
  ///
  /// When using `offset(of:)` with a type imported from a library, don't
  /// assume that future versions of the library will have the same behavior.
  /// If a property is converted from a stored property to a computed
  /// property, the result of `offset(of:)` changes to `nil`. That kind of
  /// conversion is nonbreaking in other contexts, but would trigger a runtime
  /// error if the result of `offset(of:)` is force-unwrapped.
  ///
  /// - Parameter key: A key path referring to storage that can be accessed
  ///   through a value of type `T`.
  /// - Returns: The offset in bytes from a pointer to a value of type `T` to a
  ///   pointer to the storage referenced by `key`, or `nil` if no such offset
  ///   is available for the storage referenced by `key`. If the value is
  ///   `nil`, it can be because `key` is computed, has observers, requires
  ///   reabstraction, or overlaps storage with other properties.
  @_transparent
  @_unavailableInEmbedded
  public static func offset(of key: PartialKeyPath<T>) -> Int? {
    // FIXME(noncopyableGenerics): The new (implicit) `where T: Copyable`
    // extension constraint currently changes the mangling of this from a
    // standalone function to an extension method.
    return key._storedInlineOffset
  }
}

// Not-yet-public alignment conveniences
extension MemoryLayout where T: ~Copyable {
  internal static var _alignmentMask: Int { return alignment - 1 }

  internal static func _roundingUpToAlignment(_ value: Int) -> Int {
    return (value + _alignmentMask) & ~_alignmentMask
  }
  internal static func _roundingDownToAlignment(_ value: Int) -> Int {
    return value & ~_alignmentMask
  }

  internal static func _roundingUpToAlignment(_ value: UInt) -> UInt {
    return (value + UInt(bitPattern: _alignmentMask)) & ~UInt(bitPattern: _alignmentMask)
  }
  internal static func _roundingDownToAlignment(_ value: UInt) -> UInt {
    return value & ~UInt(bitPattern: _alignmentMask)
  }

  internal static func _roundingUpToAlignment(_ value: UnsafeRawPointer) -> UnsafeRawPointer {
    return UnsafeRawPointer(bitPattern:
     _roundingUpToAlignment(UInt(bitPattern: value))).unsafelyUnwrapped
  }
  internal static func _roundingDownToAlignment(_ value: UnsafeRawPointer) -> UnsafeRawPointer {
    return UnsafeRawPointer(bitPattern:
     _roundingDownToAlignment(UInt(bitPattern: value))).unsafelyUnwrapped
  }

  internal static func _roundingUpToAlignment(_ value: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer {
    return UnsafeMutableRawPointer(bitPattern:
     _roundingUpToAlignment(UInt(bitPattern: value))).unsafelyUnwrapped
  }
  internal static func _roundingDownToAlignment(_ value: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer {
    return UnsafeMutableRawPointer(bitPattern:
     _roundingDownToAlignment(UInt(bitPattern: value))).unsafelyUnwrapped
  }

  internal static func _roundingUpBaseToAlignment(_ value: UnsafeRawBufferPointer) -> UnsafeRawBufferPointer {
    let baseAddressBits = Int(bitPattern: value.baseAddress)
    var misalignment = baseAddressBits & _alignmentMask
    if misalignment != 0 {
      misalignment = _alignmentMask & -misalignment
      return UnsafeRawBufferPointer(
        start: UnsafeRawPointer(bitPattern: baseAddressBits + misalignment),
        count: value.count - misalignment)
    }
    return value
  }

  internal static func _roundingUpBaseToAlignment(_ value: UnsafeMutableRawBufferPointer) -> UnsafeMutableRawBufferPointer {
    let baseAddressBits = Int(bitPattern: value.baseAddress)
    var misalignment = baseAddressBits & _alignmentMask
    if misalignment != 0 {
      misalignment = _alignmentMask & -misalignment
      return UnsafeMutableRawBufferPointer(
        start: UnsafeMutableRawPointer(bitPattern: baseAddressBits + misalignment),
        count: value.count - misalignment)
    }
    return value
  }
}
