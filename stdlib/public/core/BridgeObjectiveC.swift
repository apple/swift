//===----------------------------------------------------------------------===//
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

/// A Swift Array or Dictionary of types conforming to
/// `_ObjectiveCBridgeable` can be passed to Objective-C as an NSArray or
/// NSDictionary, respectively.  The elements of the resulting NSArray
/// or NSDictionary will be the result of calling `_bridgeToObjectiveC`
/// on each element of the source container.
public protocol _ObjectiveCBridgeable {
  associatedtype _ObjectiveCType: AnyObject

  /// Convert `self` to Objective-C.
  func _bridgeToObjectiveC() -> _ObjectiveCType

  /// Bridge from an Objective-C object of the bridged class type to a
  /// value of the Self type.
  ///
  /// This bridging operation is used for forced downcasting (e.g.,
  /// via as), and may defer complete checking until later. For
  /// example, when bridging from `NSArray` to `Array<Element>`, we can defer
  /// the checking for the individual elements of the array.
  ///
  /// - parameter result: The location where the result is written. The optional
  ///   will always contain a value.
  static func _forceBridgeFromObjectiveC(
    _ source: _ObjectiveCType,
    result: inout Self?
  )

  /// Try to bridge from an Objective-C object of the bridged class
  /// type to a value of the Self type.
  ///
  /// This conditional bridging operation is used for conditional
  /// downcasting (e.g., via as?) and therefore must perform a
  /// complete conversion to the value type; it cannot defer checking
  /// to a later time.
  ///
  /// - parameter result: The location where the result is written.
  ///
  /// - Returns: `true` if bridging succeeded, `false` otherwise. This redundant
  ///   information is provided for the convenience of the runtime's `dynamic_cast`
  ///   implementation, so that it need not look into the optional representation
  ///   to determine success.
  @discardableResult
  static func _conditionallyBridgeFromObjectiveC(
    _ source: _ObjectiveCType,
    result: inout Self?
  ) -> Bool

  /// Bridge from an Objective-C object of the bridged class type to a
  /// value of the Self type.
  ///
  /// This bridging operation is used for unconditional bridging when
  /// interoperating with Objective-C code, either in the body of an
  /// Objective-C thunk or when calling Objective-C code, and may
  /// defer complete checking until later. For example, when bridging
  /// from `NSArray` to `Array<Element>`, we can defer the checking
  /// for the individual elements of the array.
  ///
  /// \param source The Objective-C object from which we are
  /// bridging. This optional value will only be `nil` in cases where
  /// an Objective-C method has returned a `nil` despite being marked
  /// as `_Nonnull`/`nonnull`. In most such cases, bridging will
  /// generally force the value immediately. However, this gives
  /// bridging the flexibility to substitute a default value to cope
  /// with historical decisions, e.g., an existing Objective-C method
  /// that returns `nil` to for "empty result" rather than (say) an
  /// empty array. In such cases, when `nil` does occur, the
  /// implementation of `Swift.Array`'s conformance to
  /// `_ObjectiveCBridgeable` will produce an empty array rather than
  /// dynamically failing.
  @_effects(readonly)
  static func _unconditionallyBridgeFromObjectiveC(_ source: _ObjectiveCType?)
      -> Self
}

#if _runtime(_ObjC)

// Note: This function is not intended to be called from Swift.  The
// availability information here is perfunctory; this function isn't considered
// part of the Stdlib's Swift ABI.
@available(SwiftStdlib 5.2, *)
@_cdecl("_SwiftCreateBridgedArray")
@usableFromInline
internal func _SwiftCreateBridgedArray_DoNotCall(
  values: UnsafePointer<AnyObject>,
  numValues: Int
) -> Unmanaged<AnyObject> {
  let bufPtr = UnsafeBufferPointer(start: values, count: numValues)
  let bridged = Array(bufPtr)._bridgeToObjectiveCImpl()
  return Unmanaged<AnyObject>.passRetained(bridged)
}

// Note: This function is not intended to be called from Swift.  The
// availability information here is perfunctory; this function isn't considered
// part of the Stdlib's Swift ABI.
@available(SwiftStdlib 5.2, *)
@_cdecl("_SwiftCreateBridgedMutableArray")
@usableFromInline
internal func _SwiftCreateBridgedMutableArray_DoNotCall(
  values: UnsafePointer<AnyObject>,
  numValues: Int
) -> Unmanaged<AnyObject> {
  let bufPtr = UnsafeBufferPointer(start: values, count: numValues)
  let bridged = _SwiftNSMutableArray(Array(bufPtr))
  return Unmanaged<AnyObject>.passRetained(bridged)
}

@_silgen_name("swift_stdlib_connectNSBaseClasses")
internal func _connectNSBaseClasses() -> Bool


private let _bridgeInitializedSuccessfully = _connectNSBaseClasses()

// Note: This is marked nonisolated unsafe because we will only ever set this
// value to true. Even if multiple threads race to call
// _connectOrphanedFoundationSubclassesIfNeeded, all invocations will write the
// same value. Readers of this variable should never see an in-between value
// during a write since this is just a boolean.
internal nonisolated(unsafe)
var _orphanedFoundationSubclassesReparented: Bool = false

/// Reparents the SwiftNativeNS*Base classes to be subclasses of their respective
/// Foundation types, or is false if they couldn't be reparented. Must be run
/// in order to bridge Swift Strings, Arrays, Dictionaries, Sets, or Enumerators to ObjC.
internal func _connectOrphanedFoundationSubclassesIfNeeded() -> Void {
  let bridgeWorks = _bridgeInitializedSuccessfully
  _debugPrecondition(bridgeWorks)
  _orphanedFoundationSubclassesReparented = true
}

//===--- Bridging for metatypes -------------------------------------------===//

/// A stand-in for a value of metatype type.
///
/// The language and runtime do not yet support protocol conformances for
/// structural types like metatypes. However, we can use a struct that contains
/// a metatype, make it conform to _ObjectiveCBridgeable, and its witness table
/// will be ABI-compatible with one that directly provided conformance to the
/// metatype type itself.
public struct _BridgeableMetatype: _ObjectiveCBridgeable {
  internal var value: AnyObject.Type

  internal init(value: AnyObject.Type) {
    self.value = value
  }

  public typealias _ObjectiveCType = AnyObject

  public func _bridgeToObjectiveC() -> AnyObject {
    return value
  }

  public static func _forceBridgeFromObjectiveC(
    _ source: AnyObject,
    result: inout _BridgeableMetatype?
  ) {
    result = _BridgeableMetatype(value: source as! AnyObject.Type)
  }

  public static func _conditionallyBridgeFromObjectiveC(
    _ source: AnyObject,
    result: inout _BridgeableMetatype?
  ) -> Bool {
    if let type = source as? AnyObject.Type {
      result = _BridgeableMetatype(value: type)
      return true
    }

    result = nil
    return false
  }

  @_effects(readonly)
  public static func _unconditionallyBridgeFromObjectiveC(_ source: AnyObject?)
      -> _BridgeableMetatype {
    var result: _BridgeableMetatype?
    _forceBridgeFromObjectiveC(source!, result: &result)
    return result!
  }
}

extension _BridgeableMetatype: Sendable {}

//===--- Bridging facilities written in Objective-C -----------------------===//
// Functions that must discover and possibly use an arbitrary type's
// conformance to a given protocol.  See ../runtime/Casting.cpp for
// implementations.
//===----------------------------------------------------------------------===//

/// Bridge an arbitrary value to an Objective-C object.
///
/// - If `T` is a class type, it is always bridged verbatim, the function
///   returns `x`;
///
/// - otherwise, if `T` conforms to `_ObjectiveCBridgeable`,
///   returns the result of `x._bridgeToObjectiveC()`;
///
/// - otherwise, we use **boxing** to bring the value into Objective-C.
///   The value is wrapped in an instance of a private Objective-C class
///   that is `id`-compatible and dynamically castable back to the type of
///   the boxed value, but is otherwise opaque.
///
// COMPILER_INTRINSIC
@inlinable
public func _bridgeAnythingToObjectiveC<T>(_ x: T) -> AnyObject {
  if _fastPath(_isClassOrObjCExistential(T.self)) {
    return unsafeBitCast(x, to: AnyObject.self)
  }
  return _bridgeAnythingNonVerbatimToObjectiveC(x)
}

@_silgen_name("")
public // @testable
func _bridgeAnythingNonVerbatimToObjectiveC<T>(_ x: __owned T) -> AnyObject

/// Convert a purportedly-nonnull `id` value from Objective-C into an Any.
///
/// Since Objective-C APIs sometimes get their nullability annotations wrong,
/// this includes a failsafe against nil `AnyObject`s, wrapping them up as
/// a nil `AnyObject?`-inside-an-`Any`.
///
// COMPILER_INTRINSIC
public func _bridgeAnyObjectToAny(_ possiblyNullObject: AnyObject?) -> Any {
  if let nonnullObject = possiblyNullObject {
    return nonnullObject // AnyObject-in-Any
  }
  return possiblyNullObject as Any
}

/// Convert `x` from its Objective-C representation to its Swift
/// representation.
///
/// - If `T` is a class type:
///   - if the dynamic type of `x` is `T` or a subclass of it, it is bridged
///     verbatim, the function returns `x`;
/// - otherwise, if `T` conforms to `_ObjectiveCBridgeable`:
///   + if the dynamic type of `x` is not `T._ObjectiveCType`
///     or a subclass of it, trap;
///   + otherwise, returns the result of `T._forceBridgeFromObjectiveC(x)`;
/// - otherwise, trap.
@inlinable
public func _forceBridgeFromObjectiveC<T>(_ x: AnyObject, _: T.Type) -> T {
  if _fastPath(_isClassOrObjCExistential(T.self)) {
    return x as! T
  }

  var result: T?
  _bridgeNonVerbatimFromObjectiveC(x, T.self, &result)
  return result!
}

/// Convert `x` from its Objective-C representation to its Swift
/// representation.
// COMPILER_INTRINSIC
@inlinable
public func _forceBridgeFromObjectiveC_bridgeable<T:_ObjectiveCBridgeable> (
  _ x: T._ObjectiveCType,
  _: T.Type
) -> T {
  var result: T?
  T._forceBridgeFromObjectiveC(x, result: &result)
  return result!
}

/// Attempt to convert `x` from its Objective-C representation to its Swift
/// representation.
///
/// - If `T` is a class type:
///   - if the dynamic type of `x` is `T` or a subclass of it, it is bridged
///     verbatim, the function returns `x`;
/// - otherwise, if `T` conforms to `_ObjectiveCBridgeable`:
///   + otherwise, if the dynamic type of `x` is not `T._ObjectiveCType`
///     or a subclass of it, the result is empty;
///   + otherwise, returns the result of
///     `T._conditionallyBridgeFromObjectiveC(x)`;
/// - otherwise, the result is empty.
@inlinable
public func _conditionallyBridgeFromObjectiveC<T>(
  _ x: AnyObject,
  _: T.Type
) -> T? {
  if _fastPath(_isClassOrObjCExistential(T.self)) {
    return x as? T
  }

  var result: T?
  _ = _bridgeNonVerbatimFromObjectiveCConditional(x, T.self, &result)
  return result
}

/// Attempt to convert `x` from its Objective-C representation to its Swift
/// representation.
// COMPILER_INTRINSIC
@inlinable
public func _conditionallyBridgeFromObjectiveC_bridgeable<T:_ObjectiveCBridgeable>(
  _ x: T._ObjectiveCType,
  _: T.Type
) -> T? {
  var result: T?
  T._conditionallyBridgeFromObjectiveC (x, result: &result)
  return result
}

@_silgen_name("")
@usableFromInline
internal func _bridgeNonVerbatimFromObjectiveC<T>(
  _ x: AnyObject,
  _ nativeType: T.Type,
  _ result: inout T?
)

/// Helper stub to upcast to Any and store the result to an inout Any?
/// on the C++ runtime's behalf.
@_silgen_name("_bridgeNonVerbatimFromObjectiveCToAny")
internal func _bridgeNonVerbatimFromObjectiveCToAny(
    _ x: AnyObject,
    _ result: inout Any?
) {
  result = x as Any
}

/// Helper stub to upcast to Optional on the C++ runtime's behalf.
@_silgen_name("_bridgeNonVerbatimBoxedValue")
internal func _bridgeNonVerbatimBoxedValue<NativeType>(
    _ x: UnsafePointer<NativeType>,
    _ result: inout NativeType?
) {
  result = x.pointee
}

/// Runtime optional to conditionally perform a bridge from an object to a value
/// type.
///
/// - parameter result: Will be set to the resulting value if bridging succeeds, and
///   unchanged otherwise.
///
/// - Returns: `true` to indicate success, `false` to indicate failure.
@_silgen_name("")
public func _bridgeNonVerbatimFromObjectiveCConditional<T>(
  _ x: AnyObject,
  _ nativeType: T.Type,
  _ result: inout T?
) -> Bool

/// Determines if values of a given type can be converted to an Objective-C
/// representation.
///
/// - If `T` is a class type, returns `true`;
/// - otherwise, returns whether `T` conforms to `_ObjectiveCBridgeable`.
public func _isBridgedToObjectiveC<T>(_: T.Type) -> Bool {
  if _fastPath(_isClassOrObjCExistential(T.self)) {
    return true
  }
  return _isBridgedNonVerbatimToObjectiveC(T.self)
}

@_silgen_name("")
public func _isBridgedNonVerbatimToObjectiveC<T>(_: T.Type) -> Bool

/// A type that's bridged "verbatim" does not conform to
/// `_ObjectiveCBridgeable`, and can have its bits reinterpreted as an
/// `AnyObject`.  When this function returns true, the storage of an
/// `Array<T>` can be `unsafeBitCast` as an array of `AnyObject`.
@inlinable // FIXME(sil-serialize-all)
public func _isBridgedVerbatimToObjectiveC<T>(_: T.Type) -> Bool {
  return _isClassOrObjCExistential(T.self)
}

/// Retrieve the Objective-C type to which the given type is bridged.
@inlinable // FIXME(sil-serialize-all)
public func _getBridgedObjectiveCType<T>(_: T.Type) -> Any.Type? {
  if _fastPath(_isClassOrObjCExistential(T.self)) {
    return T.self
  }
  return _getBridgedNonVerbatimObjectiveCType(T.self)
}

@_silgen_name("")
public func _getBridgedNonVerbatimObjectiveCType<T>(_: T.Type) -> Any.Type?

// -- Pointer argument bridging

/// A mutable pointer addressing an Objective-C reference that doesn't own its
/// target.
///
/// `Pointee` must be a class type or `Optional<C>` where `C` is a class.
///
/// This type has implicit conversions to allow passing any of the following
/// to a C or ObjC API:
///
/// - `nil`, which gets passed as a null pointer,
/// - an inout argument of the referenced type, which gets passed as a pointer
///   to a writeback temporary with autoreleasing ownership semantics,
/// - an `UnsafeMutablePointer<Pointee>`, which is passed as-is.
///
/// Passing pointers to mutable arrays of ObjC class pointers is not
/// directly supported. Unlike `UnsafeMutablePointer<Pointee>`,
/// `AutoreleasingUnsafeMutablePointer<Pointee>` must reference storage that
/// does not own a reference count to the referenced
/// value. UnsafeMutablePointer's operations, by contrast, assume that
/// the referenced storage owns values loaded from or stored to it.
///
/// This type does not carry an owner pointer unlike the other C*Pointer types
/// because it only needs to reference the results of inout conversions, which
/// already have writeback-scoped lifetime.
@frozen
public struct AutoreleasingUnsafeMutablePointer<Pointee /* TODO : class */>
  :  _Pointer {

  public let _rawValue: Builtin.RawPointer

  @_transparent
  public // COMPILER_INTRINSIC
  init(_ _rawValue: Builtin.RawPointer) {
    self._rawValue = _rawValue
  }

  /// Retrieve or set the `Pointee` instance referenced by `self`.
  ///
  /// `AutoreleasingUnsafeMutablePointer` is assumed to reference a value with
  /// `__autoreleasing` ownership semantics, like `NSFoo **` declarations in
  /// ARC. Setting the pointee autoreleases the new value before trivially
  /// storing it in the referenced memory.
  ///
  /// - Precondition: the pointee has been initialized with an instance of type
  ///   `Pointee`.
  @inlinable
  public var pointee: Pointee {
    @_transparent get {
      // The memory addressed by this pointer contains a non-owning reference,
      // therefore we *must not* point an `UnsafePointer<AnyObject>` to
      // it---otherwise we would allow the compiler to assume it has a +1
      // refcount, enabling some optimizations that wouldn't be valid.
      //
      // Instead, we need to load the pointee as a +0 unmanaged reference. For
      // an extra twist, `Pointee` is allowed (but not required) to be an
      // optional type, so we actually need to load it as an optional, and
      // explicitly handle the nil case.
      let unmanaged =
        UnsafePointer<Optional<Unmanaged<AnyObject>>>(_rawValue).pointee
      return _unsafeReferenceCast(
        unmanaged?.takeUnretainedValue(),
        to: Pointee.self)
    }

    @_transparent nonmutating set {
      // Autorelease the object reference.
      let object = _unsafeReferenceCast(newValue, to: Optional<AnyObject>.self)
      Builtin.retain(object)
      Builtin.autorelease(object)

      // Convert it to an unmanaged reference and trivially assign it to the
      // memory addressed by this pointer.
      let unmanaged: Optional<Unmanaged<AnyObject>>
      if let object = object {
        unmanaged = Unmanaged.passUnretained(object)
      } else {
        unmanaged = nil
      }
      UnsafeMutablePointer<Optional<Unmanaged<AnyObject>>>(_rawValue).pointee =
        unmanaged
    }
  }

  /// Access the `i`th element of the raw array pointed to by
  /// `self`.
  ///
  /// - Precondition: `self != nil`.
  @inlinable // unsafe-performance
  public subscript(i: Int) -> Pointee {
    @_transparent
    get {
      return self.advanced(by: i).pointee
    }
  }

  /// Explicit construction from an UnsafeMutablePointer.
  ///
  /// This is inherently unsafe; UnsafeMutablePointer assumes the
  /// referenced memory has +1 strong ownership semantics, whereas
  /// AutoreleasingUnsafeMutablePointer implies +0 semantics.
  ///
  /// - Warning: Accessing `pointee` as a type that is unrelated to
  ///   the underlying memory's bound type is undefined.
  @_transparent
  public init<U>(@_nonEphemeral _ from: UnsafeMutablePointer<U>) {
   self._rawValue = from._rawValue
  }

  /// Explicit construction from an UnsafeMutablePointer.
  ///
  /// Returns nil if `from` is nil.
  ///
  /// This is inherently unsafe; UnsafeMutablePointer assumes the
  /// referenced memory has +1 strong ownership semantics, whereas
  /// AutoreleasingUnsafeMutablePointer implies +0 semantics.
  ///
  /// - Warning: Accessing `pointee` as a type that is unrelated to
  ///   the underlying memory's bound type is undefined.
  @_transparent
  public init?<U>(@_nonEphemeral _ from: UnsafeMutablePointer<U>?) {
   guard let unwrapped = from else { return nil }
   self.init(unwrapped)
  }
     
  /// Explicit construction from a UnsafePointer.
  ///
  /// This is inherently unsafe because UnsafePointers do not imply
  /// mutability.
  ///
  /// - Warning: Accessing `pointee` as a type that is unrelated to
  ///   the underlying memory's bound type is undefined.
  @usableFromInline @_transparent
  internal init<U>(
    @_nonEphemeral _ from: UnsafePointer<U>
  ) {
    self._rawValue = from._rawValue
  }

  /// Explicit construction from a UnsafePointer.
  ///
  /// Returns nil if `from` is nil.
  ///
  /// This is inherently unsafe because UnsafePointers do not imply
  /// mutability.
  ///
  /// - Warning: Accessing `pointee` as a type that is unrelated to
  ///   the underlying memory's bound type is undefined.
  @usableFromInline @_transparent
  internal init?<U>(
    @_nonEphemeral _ from: UnsafePointer<U>?
  ) {
    guard let unwrapped = from else { return nil }
    self.init(unwrapped)
  }
}

extension UnsafeMutableRawPointer {
  /// Creates a new raw pointer from an `AutoreleasingUnsafeMutablePointer`
  /// instance.
  ///
  /// - Parameter other: The pointer to convert.
  @_transparent
  public init<T>(
    @_nonEphemeral _ other: AutoreleasingUnsafeMutablePointer<T>
  ) {
    _rawValue = other._rawValue
  }

  /// Creates a new raw pointer from an `AutoreleasingUnsafeMutablePointer`
  /// instance.
  ///
  /// - Parameter other: The pointer to convert. If `other` is `nil`, the
  ///   result is `nil`.
  @_transparent
  public init?<T>(
    @_nonEphemeral _ other: AutoreleasingUnsafeMutablePointer<T>?
  ) {
    guard let unwrapped = other else { return nil }
    self.init(unwrapped)
  }
}

extension UnsafeRawPointer {
  /// Creates a new raw pointer from an `AutoreleasingUnsafeMutablePointer`
  /// instance.
  ///
  /// - Parameter other: The pointer to convert.
  @_transparent
  public init<T>(
    @_nonEphemeral _ other: AutoreleasingUnsafeMutablePointer<T>
  ) {
    _rawValue = other._rawValue
  }

  /// Creates a new raw pointer from an `AutoreleasingUnsafeMutablePointer`
  /// instance.
  ///
  /// - Parameter other: The pointer to convert. If `other` is `nil`, the
  ///   result is `nil`.
  @_transparent
  public init?<T>(
    @_nonEphemeral _ other: AutoreleasingUnsafeMutablePointer<T>?
  ) {
    guard let unwrapped = other else { return nil }
    self.init(unwrapped)
  }
}

@available(*, unavailable)
extension AutoreleasingUnsafeMutablePointer: Sendable { }

internal struct _CocoaFastEnumerationStackBuf {
  // Clang uses 16 pointers.  So do we.
  internal var _item0: UnsafeRawPointer?
  internal var _item1: UnsafeRawPointer?
  internal var _item2: UnsafeRawPointer?
  internal var _item3: UnsafeRawPointer?
  internal var _item4: UnsafeRawPointer?
  internal var _item5: UnsafeRawPointer?
  internal var _item6: UnsafeRawPointer?
  internal var _item7: UnsafeRawPointer?
  internal var _item8: UnsafeRawPointer?
  internal var _item9: UnsafeRawPointer?
  internal var _item10: UnsafeRawPointer?
  internal var _item11: UnsafeRawPointer?
  internal var _item12: UnsafeRawPointer?
  internal var _item13: UnsafeRawPointer?
  internal var _item14: UnsafeRawPointer?
  internal var _item15: UnsafeRawPointer?

  @_transparent
  internal var count: Int {
    return 16
  }

  internal init() {
    _item0 = nil
    _item1 = _item0
    _item2 = _item0
    _item3 = _item0
    _item4 = _item0
    _item5 = _item0
    _item6 = _item0
    _item7 = _item0
    _item8 = _item0
    _item9 = _item0
    _item10 = _item0
    _item11 = _item0
    _item12 = _item0
    _item13 = _item0
    _item14 = _item0
    _item15 = _item0

    _internalInvariant(MemoryLayout.size(ofValue: self) >=
                   MemoryLayout<Optional<UnsafeRawPointer>>.size * count)
  }
}

/// Get the ObjC type encoding for a type as a pointer to a C string.
///
/// This is used by the Foundation overlays. The compiler will error if the
/// passed-in type is generic or not representable in Objective-C
@_transparent
public func _getObjCTypeEncoding<T>(_ type: T.Type) -> UnsafePointer<Int8> {
  // This must be `@_transparent` because `Builtin.getObjCTypeEncoding` is
  // only supported by the compiler for concrete types that are representable
  // in ObjC.
  return UnsafePointer(Builtin.getObjCTypeEncoding(type))
}

#endif

//===--- Bridging without the ObjC runtime --------------------------------===//

#if !_runtime(_ObjC)

/// Convert `x` from its Objective-C representation to its Swift
/// representation.
// COMPILER_INTRINSIC
@inlinable
public func _forceBridgeFromObjectiveC_bridgeable<T:_ObjectiveCBridgeable> (
  _ x: T._ObjectiveCType,
  _: T.Type
) -> T {
  var result: T?
  T._forceBridgeFromObjectiveC(x, result: &result)
  return result!
}

/// Attempt to convert `x` from its Objective-C representation to its Swift
/// representation.
// COMPILER_INTRINSIC
@inlinable
public func _conditionallyBridgeFromObjectiveC_bridgeable<T:_ObjectiveCBridgeable>(
  _ x: T._ObjectiveCType,
  _: T.Type
) -> T? {
  var result: T?
  T._conditionallyBridgeFromObjectiveC (x, result: &result)
  return result
}

public // SPI(Foundation)
protocol _NSSwiftValue: AnyObject {
  init(_ value: Any)
  var value: Any { get }
  static var null: AnyObject { get }
}

@usableFromInline
internal class __SwiftValue {
  @usableFromInline
  let value: Any
  
  @usableFromInline
  init(_ value: Any) {
    self.value = value
  }
  
  @usableFromInline
  static let null = __SwiftValue(Optional<Any>.none as Any)
}

// Internal stdlib SPI
@_silgen_name("swift_unboxFromSwiftValueWithType")
public func swift_unboxFromSwiftValueWithType<T>(
  _ source: inout AnyObject,
  _ result: UnsafeMutablePointer<T>
  ) -> Bool {

  if source === _nullPlaceholder {
    if let unpacked = Optional<Any>.none as? T {
      result.initialize(to: unpacked)
      return true
    }
  }
    
  if let box = source as? __SwiftValue {
    if let value = box.value as? T {
      result.initialize(to: value)
      return true
    }
  } else if let box = source as? _NSSwiftValue {
    if let value = box.value as? T {
      result.initialize(to: value)
      return true
    }
  }
  
  return false
}

// Internal stdlib SPI
@_silgen_name("swift_swiftValueConformsTo")
public func _swiftValueConformsTo<T>(_ type: T.Type) -> Bool {
  if let foundationType = _foundationSwiftValueType {
    return foundationType is T.Type
  } else {
    return __SwiftValue.self is T.Type
  }
}

@_silgen_name("_swift_extractDynamicValue")
public func _extractDynamicValue<T>(_ value: T) -> AnyObject?

@_silgen_name("_swift_bridgeToObjectiveCUsingProtocolIfPossible")
public func _bridgeToObjectiveCUsingProtocolIfPossible<T>(_ value: T) -> AnyObject?

internal protocol _Unwrappable {
  func _unwrap() -> Any?
}

extension Optional: _Unwrappable {
  internal func _unwrap() -> Any? {
    return self
  }
}

private let _foundationSwiftValueType = _typeByName("Foundation.__SwiftValue") as? _NSSwiftValue.Type

@usableFromInline
internal var _nullPlaceholder: AnyObject {
  if let foundationType = _foundationSwiftValueType {
    return foundationType.null
  } else {
    return __SwiftValue.null
  }
}

@usableFromInline
func _makeSwiftValue(_ value: Any) -> AnyObject {
  if let foundationType = _foundationSwiftValueType {
    return foundationType.init(value)
  } else {
    return __SwiftValue(value)
  }
}

/// Bridge an arbitrary value to an Objective-C object.
///
/// - If `T` is a class type, it is always bridged verbatim, the function
///   returns `x`;
///
/// - otherwise, if `T` conforms to `_ObjectiveCBridgeable`,
///   returns the result of `x._bridgeToObjectiveC()`;
///
/// - otherwise, we use **boxing** to bring the value into Objective-C.
///   The value is wrapped in an instance of a private Objective-C class
///   that is `id`-compatible and dynamically castable back to the type of
///   the boxed value, but is otherwise opaque.
///
// COMPILER_INTRINSIC
public func _bridgeAnythingToObjectiveC<T>(_ x: T) -> AnyObject {
  var done = false
  var result: AnyObject!
  
  let source: Any = x
  
  if let dynamicSource = _extractDynamicValue(x) {
    result = dynamicSource as AnyObject
    done = true 
  }
  
  if !done, let wrapper = source as? _Unwrappable {
    if let value = wrapper._unwrap() {
      result = value as AnyObject
    } else {
      result = _nullPlaceholder
    }
    
    done = true
  }

  if !done {
    if type(of: source) as? AnyClass != nil {
      result = unsafeBitCast(x, to: AnyObject.self)
    } else if let object = _bridgeToObjectiveCUsingProtocolIfPossible(source) {
      result = object
    } else {
      result = _makeSwiftValue(source)
    }
  }
  
  return result
}

#endif // !_runtime(_ObjC)
